/*
 * vcodec_gr05_image.c - Generate images from Playdia AK8000 video frames
 *
 * Codec structure (discovered via reverse engineering):
 * - DC: MPEG-1 luma DC VLC (Table B.12), DPCM per component, 864 blocks
 * - AC: Multiple passes of 864 blocks each
 *   - For each block: for each zigzag pos 1-63:
 *     read 1 flag bit; if 0 = coefficient is 0
 *     if 1: read VLC value using same MPEG-1 luma DC table
 *       if VLC value == 0: EOB (end this block's scan)
 *       else: dequantized coeff = v * qs / (qtable[zz] * 16)
 *   - Coefficients accumulate across all passes
 *   - On VLC fail: skip to next block (stream truncation)
 * - type=0 (intra): 2 AC passes fill ~100% of AC bits
 * - type=1 (inter): 4 AC passes fill ~100% of AC bits
 *
 * Dequantization: coeff = vlc_value * qs / (qtable[zigzag_pos] * 16)
 *   where qtable is a 4x4 grid tiled over the 8x8 frequency matrix:
 *   index qi = (row/2)*4 + (col/2)
 *
 * Frame layout: 256x144, 4:2:0, 16x9 MBs, each MB = Y0,Y1,Y2,Y3,Cb,Cr
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static uint8_t fdata[16384];
static int fdatalen;

static int get_bit(const uint8_t *d, int bp) { return (d[bp>>3]>>(7-(bp&7)))&1; }
static uint32_t get_bits(const uint8_t *d, int bp, int n) {
    uint32_t v=0; for(int i=0;i<n;i++) v=(v<<1)|get_bit(d,bp+i); return v;
}

/* MPEG-1 luminance DC VLC (Table B.12) */
static const struct{int len;uint32_t code;} dcv[]={
    {3,0x4},{2,0x0},{2,0x1},{3,0x5},{3,0x6},
    {4,0xE},{5,0x1E},{6,0x3E},{7,0x7E},
    {8,0xFE},{9,0x1FE},{10,0x3FE}};

static int dec_dc(const uint8_t *d, int bp, int *val, int tb) {
    for(int i=0;i<12;i++){
        if(bp+dcv[i].len>tb) continue;
        uint32_t b=get_bits(d,bp,dcv[i].len);
        if(b==dcv[i].code){
            int sz=i,c=dcv[i].len;
            if(sz==0){*val=0;}
            else{if(bp+c+sz>tb)return-1;uint32_t r=get_bits(d,bp+c,sz);c+=sz;
                *val=(r<(1u<<(sz-1)))?(int)r-(1<<sz)+1:(int)r;}
            return c;}}
    return -1;
}

/* Zigzag scan order for 8x8 block (JPEG standard) */
static const int zigzag[64] = {
    0,  1,  8,  16,  9,  2,  3, 10,
   17, 24, 32, 25, 18, 11,  4,  5,
   12, 19, 26, 33, 40, 48, 41, 34,
   27, 20, 13,  6,  7, 14, 21, 28,
   35, 42, 49, 56, 57, 50, 43, 36,
   29, 22, 15, 23, 30, 37, 44, 51,
   58, 59, 52, 45, 38, 31, 39, 46,
   53, 60, 61, 54, 47, 55, 62, 63
};

/* Quantization table (hardcoded in AK8000) */
static const uint8_t qtable[16] = {
    0x0A, 0x14, 0x0E, 0x0D, 0x12, 0x25, 0x16, 0x1C,
    0x0F, 0x18, 0x0F, 0x12, 0x12, 0x1F, 0x11, 0x14
};

/* Load a frame from a CD-ROM BIN file at the given LBA */
static int load_frame_lba(const char *binfile, int target_lba) {
    FILE *fp = fopen(binfile, "rb");
    if(!fp) return 0;
    int f1c = 0;
    uint8_t tmpf[16384];
    for(int s = 0; s < 20; s++) {
        long off = (long)(target_lba + s) * 2352;
        uint8_t sec[2352];
        fseek(fp, off, SEEK_SET);
        if(fread(sec, 1, 2352, fp) != 2352) break;
        uint8_t t = sec[24];
        if(t == 0xF1) {
            if(f1c < 6) memcpy(tmpf + f1c*2047, sec+25, 2047);
            f1c++;
        } else if(t == 0xF2) {
            if(f1c == 6 && tmpf[0]==0x00 && tmpf[1]==0x80 && tmpf[2]==0x04) {
                fdatalen = 6*2047;
                memcpy(fdata, tmpf, fdatalen);
                fclose(fp);
                return 1;
            }
            f1c = 0;
        } else if(t == 0xF3) f1c = 0;
    }
    fclose(fp);
    return 0;
}

/* IDCT for 8x8 block */
static void idct8x8(double *blk) {
    /* Row IDCT */
    for(int i = 0; i < 8; i++) {
        double *row = blk + i*8;
        double tmp[8];
        for(int x = 0; x < 8; x++) {
            double s = 0;
            for(int u = 0; u < 8; u++) {
                double cu = (u == 0) ? 1.0/sqrt(2.0) : 1.0;
                s += cu * row[u] * cos(M_PI*(2*x+1)*u/16.0);
            }
            tmp[x] = s * 0.5;
        }
        for(int x = 0; x < 8; x++) row[x] = tmp[x];
    }
    /* Column IDCT */
    for(int j = 0; j < 8; j++) {
        double col[8];
        for(int i = 0; i < 8; i++) col[i] = blk[i*8+j];
        double tmp[8];
        for(int y = 0; y < 8; y++) {
            double s = 0;
            for(int v = 0; v < 8; v++) {
                double cv = (v == 0) ? 1.0/sqrt(2.0) : 1.0;
                s += cv * col[v] * cos(M_PI*(2*y+1)*v/16.0);
            }
            tmp[y] = s * 0.5;
        }
        for(int y = 0; y < 8; y++) blk[y*8+j] = tmp[y];
    }
}

static int iclamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Number of AC passes: type=0 uses 2, type=1 uses 4 */
#define AC_PASSES_INTRA 2
#define AC_PASSES_INTER 4

/* Decode one AC pass for all 864 blocks.
 * For each block: flag+VLC per position 1-63, stop at EOB (flag=1, v=0).
 * Dequantization: coeff = v * qs / (qtable[zigzag_pos] * 4)
 * Accumulates into coeff_store[864][64] (adds to existing values).
 * Returns new bit position. On VLC failure, skips to next block. */
static int decode_ac_pass(int bp, int tb, double coeff_store[864][64],
                           int qs, const double qscale[64]) {
    for(int mb = 0; mb < 144; mb++) {
        for(int bl = 0; bl < 6; bl++) {
            int blk_idx = mb*6 + bl;
            for(int pos = 1; pos <= 63; pos++) {
                if(bp >= tb) return bp; /* stream ended */
                int flag = get_bit(fdata, bp); bp++;
                if(!flag) continue; /* coefficient is zero */
                int v;
                int used = dec_dc(fdata, bp, &v, tb);
                if(used < 0) goto next_block; /* VLC decode failed - skip block */
                bp += used;
                if(v == 0) goto next_block; /* EOB token */
                int zz = zigzag[pos];
                coeff_store[blk_idx][zz] += v * qs / (qscale[zz] * 16.0);
            }
            next_block:;
        }
    }
    return bp;
}

/* Render the frame currently in fdata/fdatalen and write PPM files */
static void render_frame(const char *name) {
    int qs = fdata[3];
    int frame_type = fdata[39];
    int de = fdatalen;
    while(de > 0 && fdata[de-1] == 0xFF) de--;
    int tb_bits = de * 8;
    printf("Frame: %s (QS=%d, type=%d, %d bytes)\n", name, qs, frame_type, de);

    /* Decode all DC coefficients first (MPEG-1 luma DC VLC, DPCM) */
    int bp = 40 * 8;
    int dc_pred[3] = {0, 0, 0};
    int dc_vals[864];
    int dc_ok = 1;

    for(int mb = 0; mb < 144 && dc_ok; mb++) {
        for(int bl = 0; bl < 6 && dc_ok; bl++) {
            int comp = (bl < 4) ? 0 : (bl == 4 ? 1 : 2);
            int blk_idx = mb*6 + bl;
            int dv;
            int used = dec_dc(fdata, bp, &dv, tb_bits);
            if(used < 0) { printf("  DC FAIL at blk %d\n", blk_idx); dc_ok=0; break; }
            dc_pred[comp] += dv;
            dc_vals[blk_idx] = dc_pred[comp];
            bp += used;
        }
    }
    if(!dc_ok) return;

    int ac_start = bp;
    int ac_bits = tb_bits - ac_start;
    printf("  DC: %d bits, AC: %d bits\n", ac_start - 40*8, ac_bits);

    /* Build per-position quantization scale from 4x4 qtable tiled over 8x8 */
    double qscale[64];
    for(int i = 0; i < 64; i++) {
        int row = i / 8, col = i % 8;
        int qi = (row/2)*4 + (col/2);
        qscale[i] = (double)qtable[qi];
    }

    /* Coefficient storage for all 864 blocks [64 coefficients each] */
    static double coeff_store[864][64];
    memset(coeff_store, 0, sizeof(coeff_store));

    /* Set DC coefficients */
    for(int i = 0; i < 864; i++) coeff_store[i][0] = dc_vals[i];

    /* AC passes: 2 for intra (type=0), 4 for inter (type=1) */
    int n_passes = (frame_type == 0) ? AC_PASSES_INTRA : AC_PASSES_INTER;
    bp = ac_start;
    for(int p = 0; p < n_passes && bp < tb_bits; p++) {
        int prev = bp;
        bp = decode_ac_pass(bp, tb_bits, coeff_store, qs, qscale);
        printf("  AC pass %d: %d bits\n", p+1, bp - prev);
    }
    int total_nz = 0;
    for(int i = 0; i < 864; i++)
        for(int j = 1; j < 64; j++)
            if(coeff_store[i][j] != 0.0) total_nz++;
    printf("  AC total: %d bits (%.1f%%), NZ=%d\n",
           bp - ac_start, ac_bits > 0 ? 100.0*(bp-ac_start)/ac_bits : 0, total_nz);

    /* Image buffers for 256x144 YCbCr */
    static double Y[256*144], Cb[128*72], Cr[128*72];
    memset(Y, 0, sizeof(Y));
    memset(Cb, 0, sizeof(Cb));
    memset(Cr, 0, sizeof(Cr));

    /* IDCT and spatial layout */
    for(int mb = 0; mb < 144; mb++) {
        int mb_x = mb % 16;
        int mb_y = mb / 16;

        for(int bl = 0; bl < 6; bl++) {
            int blk_idx = mb*6 + bl;
            int comp = (bl < 4) ? 0 : (bl == 4 ? 1 : 2);

            int bx, by;
            if(comp == 0) {
                bx = mb_x*2 + (bl & 1);
                by = mb_y*2 + ((bl >> 1) & 1);
            } else {
                bx = mb_x;
                by = mb_y;
            }

            double coeff[64];
            memcpy(coeff, coeff_store[blk_idx], sizeof(coeff));
            idct8x8(coeff);

            if(comp == 0) {
                for(int y = 0; y < 8; y++)
                    for(int x = 0; x < 8; x++) {
                        int py = by*8+y, px = bx*8+x;
                        if(py < 144 && px < 256) Y[py*256+px] = coeff[y*8+x];
                    }
            } else if(comp == 1) {
                for(int y = 0; y < 8; y++)
                    for(int x = 0; x < 8; x++) {
                        int py = by*8+y, px = bx*8+x;
                        if(py < 72 && px < 128) Cb[py*128+px] = coeff[y*8+x];
                    }
            } else {
                for(int y = 0; y < 8; y++)
                    for(int x = 0; x < 8; x++) {
                        int py = by*8+y, px = bx*8+x;
                        if(py < 72 && px < 128) Cr[py*128+px] = coeff[y*8+x];
                    }
            }
        }
    }

    /* Write RGB PPM */
    char outname[512];
    snprintf(outname, sizeof(outname), "/home/wizzard/share/GitHub/playdia-emu/tools/test_output/flag_%s.ppm", name);
    {
        FILE *fp2 = fopen(outname, "wb");
        if(fp2) {
            fprintf(fp2, "P6\n256 144\n255\n");
            for(int py = 0; py < 144; py++) {
                for(int px = 0; px < 256; px++) {
                    double y_val = Y[py*256 + px] + 128.0;
                    double cb_val = Cb[(py/2)*128 + px/2];
                    double cr_val = Cr[(py/2)*128 + px/2];
                    int R = iclamp((int)(y_val + 1.402 * cr_val), 0, 255);
                    int G = iclamp((int)(y_val - 0.344 * cb_val - 0.714 * cr_val), 0, 255);
                    int B = iclamp((int)(y_val + 1.772 * cb_val), 0, 255);
                    fputc(R, fp2); fputc(G, fp2); fputc(B, fp2);
                }
            }
            fclose(fp2);
            printf("  Wrote %s\n", outname);
        }

        /* Write Y-only PGM */
        snprintf(outname, sizeof(outname), "/home/wizzard/share/GitHub/playdia-emu/tools/test_output/flag_%s_Y.pgm", name);
        {
            FILE *fp3 = fopen(outname, "wb");
            if(fp3) {
                fprintf(fp3, "P5\n256 144\n255\n");
                for(int py = 0; py < 144; py++)
                    for(int px = 0; px < 256; px++) {
                        int yv = iclamp((int)(Y[py*256+px] + 128.0), 0, 255);
                        fputc(yv, fp3);
                    }
                fclose(fp3);
                printf("  Wrote Y-only: %s\n", outname);
            }
        }
    }
    printf("\n");
}

int main() {
    printf("=== Playdia AK8000 Video Decode: flag+VLC AC, two-pass ===\n\n");

    const char *dbz = "/home/wizzard/share/GitHub/playdia-roms/Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku - Chikyuu-hen (Japan) (Track 2).bin";
    const char *dbzU = "/home/wizzard/share/GitHub/playdia-roms/Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku - Uchuu-hen (Japan) (Track 2).bin";
    const char *smoon = "/home/wizzard/share/GitHub/playdia-roms/Bishoujo Senshi Sailor Moon S - Quiz Taiketsu! Sailor Power Shuketsu!! (Japan) (Track 2).bin";
    const char *marinee = "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin";
    struct { const char *rom; int lba; const char *name; } tests[] = {
        { dbz, 137, "dbz_lba137" },   /* static title (grass/sky) */
        { dbz, 452, "dbz_lba452" },   /* first changing frame */
        { dbz, 466, "dbz_lba466" },   /* different scene */
        { dbz, 473, "dbz_lba473" },   /* balanced brightness */
        { dbz, 503, "dbz_lba503" },   /* inter frame */
        { dbz, 536, "dbz_lba536" },   /* new intra */
        { dbz, 549, "dbz_lba549" },   /* inter */
        { dbz, 706, "dbz_lba706" },   /* purple indoor scene */
        { dbzU, 137, "dbzU_lba137" }, /* DBZ Space chapter */
        { smoon, 137, "smoon_lba137" },
        { smoon, 500, "smoon_lba500" },
        { marinee, 148, "marinee_lba148" },
        { NULL, 0, NULL }
    };

    for(int fi = 0; tests[fi].rom; fi++) {
        if(!load_frame_lba(tests[fi].rom, tests[fi].lba)) {
            printf("Failed to load %s LBA=%d\n", tests[fi].name, tests[fi].lba);
            continue;
        }
        render_frame(tests[fi].name);
    }

    return 0;
}
