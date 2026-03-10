/*
 * vcodec_chromdc.c - Test chrominance DC VLC and inter-frame consistency
 *
 * Key idea 1: MPEG-1 uses DIFFERENT DC VLC for chrominance vs luminance.
 * If we use the wrong table for Cb/Cr, the AC start position is wrong.
 *
 * Key idea 2: Correct AC model should produce images where consecutive
 * frames have higher inter-frame correlation than incorrect models.
 *
 * Key idea 3: Try using chrominance DC VLC for ALL blocks (maybe the
 * Playdia uses a non-standard table).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <zip.h>

#define SECTOR_RAW 2352
#define MAX_FRAME  65536
#define W 256
#define H 144

/* Luminance DC VLC */
static const struct { int len; uint32_t code; int size; } luma_dc_vlc[] = {
    {3, 0b100, 0}, {2, 0b00, 1}, {2, 0b01, 2}, {3, 0b101, 3},
    {3, 0b110, 4}, {4, 0b1110, 5}, {5, 0b11110, 6},
    {6, 0b111110, 7}, {7, 0b1111110, 8},
};

/* Chrominance DC VLC */
static const struct { int len; uint32_t code; int size; } chroma_dc_vlc[] = {
    {2, 0b00, 0}, {2, 0b01, 1}, {2, 0b10, 2}, {3, 0b110, 3},
    {4, 0b1110, 4}, {5, 0b11110, 5}, {6, 0b111110, 6},
    {7, 0b1111110, 7}, {8, 0b11111110, 8},
};

typedef struct { const uint8_t *data; int total_bits; int pos; } bitstream;

static int bs_read(bitstream *bs, int n) {
    if (n <= 0 || bs->pos + n > bs->total_bits) return -1;
    int v = 0;
    for (int i = 0; i < n; i++) {
        int bp = bs->pos + i;
        v = (v << 1) | ((bs->data[bp >> 3] >> (7 - (bp & 7))) & 1);
    }
    bs->pos += n;
    return v;
}

static int bs_peek(bitstream *bs, int n) {
    if (n <= 0 || bs->pos + n > bs->total_bits) return -1;
    int v = 0;
    for (int i = 0; i < n; i++) {
        int bp = bs->pos + i;
        v = (v << 1) | ((bs->data[bp >> 3] >> (7 - (bp & 7))) & 1);
    }
    return v;
}

static int read_vlc(bitstream *bs, int use_chroma) {
    int count = use_chroma ? 9 : 9;
    for (int i = 0; i < count; i++) {
        int len, size;
        uint32_t code;
        if (use_chroma) {
            len = chroma_dc_vlc[i].len;
            code = chroma_dc_vlc[i].code;
            size = chroma_dc_vlc[i].size;
        } else {
            len = luma_dc_vlc[i].len;
            code = luma_dc_vlc[i].code;
            size = luma_dc_vlc[i].size;
        }
        int bits = bs_peek(bs, len);
        if (bits == (int)code) {
            bs->pos += len;
            if (size == 0) return 0;
            int val = bs_read(bs, size);
            if (val < (1 << (size - 1))) val -= (1 << size) - 1;
            return val;
        }
    }
    bs->pos += 2;
    return 0;
}

static int assemble_frames(const uint8_t *disc, int tsec, int slba,
    uint8_t fr[][MAX_FRAME], int fs[], int mx) {
    int n=0,c=0; bool inf=false;
    for(int l=slba;l<tsec&&n<mx;l++){
        const uint8_t *s=disc+(long)l*SECTOR_RAW;
        if(s[0]!=0||s[1]!=0xFF||s[15]!=2||(s[18]&4)) continue;
        if(s[24]==0xF1){if(!inf){inf=true;c=0;}if(c+2047<MAX_FRAME){memcpy(fr[n]+c,s+25,2047);c+=2047;}}
        else if(s[24]==0xF2){if(inf&&c>0){fs[n]=c;n++;inf=false;c=0;}}
    } return n;
}

static const int zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static const int default_qtable[16] = {10,20,14,13,18,37,22,28,15,24,15,18,18,31,17,20};

static void idct8x8(int block[64], int out[64]) {
    double tmp[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) {
            double sum = 0;
            for (int u = 0; u < 8; u++) {
                double cu = (u == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += cu * block[i*8+u] * cos((2*j+1)*u*M_PI/16.0);
            }
            tmp[i*8+j] = sum * 0.5;
        }
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) {
            double sum = 0;
            for (int v = 0; v < 8; v++) {
                double cv = (v == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += cv * tmp[v*8+j] * cos((2*i+1)*v*M_PI/16.0);
            }
            out[i*8+j] = (int)(sum * 0.5);
        }
}

static int clamp(int v) { return v<0?0:v>255?255:v; }

/* Decode a frame and return Y plane */
static void decode_frame(const uint8_t *bsdata, int bslen, int qs,
                         int dc_mode, /* 0=all luma, 1=luma+chroma, 2=all chroma */
                         int ac_mode, /* 0=flag+luma_vlc, 1=flag+chroma_vlc, 2=no-flag-vlc, 3=DC-only */
                         int Y_plane[H][W]) {
    int total_bits = bslen * 8;
    int mw = W/16, mh = H/16;
    int nblocks = mw * mh * 6;
    bitstream bs = {bsdata, total_bits, 0};

    int qm[64];
    for (int i = 0; i < 64; i++) {
        int r = i/8, c = i%8;
        int qi = ((r>>1)<<2)|(c>>1);
        qm[i] = default_qtable[qi] * qs;
    }

    static int blocks[900][64];
    memset(blocks, 0, sizeof(blocks));

    /* Decode DC */
    int dc_pred[3] = {0,0,0};
    for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
        int sub = b % 6;
        int comp = (sub < 4) ? 0 : (sub == 4) ? 1 : 2;
        int use_chroma;
        switch (dc_mode) {
            case 0: use_chroma = 0; break;
            case 1: use_chroma = (comp > 0); break;
            case 2: use_chroma = 1; break;
            default: use_chroma = 0;
        }
        int diff = read_vlc(&bs, use_chroma);
        dc_pred[comp] += diff;
        blocks[b][0] = dc_pred[comp] * qm[0] / 8;
    }

    /* Decode AC */
    if (ac_mode < 3) {
        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int sub = b % 6;
            int comp = (sub < 4) ? 0 : (sub == 4) ? 1 : 2;
            for (int k = 1; k < 64 && bs.pos < total_bits; k++) {
                int val;
                if (ac_mode == 2) {
                    /* No flag, just VLC */
                    val = read_vlc(&bs, 0);
                } else {
                    int flag = bs_read(&bs, 1);
                    if (flag) {
                        int use_chroma_ac = (ac_mode == 1 && comp > 0);
                        val = read_vlc(&bs, use_chroma_ac);
                    } else {
                        val = 0;
                    }
                }
                blocks[b][zigzag[k]] = val * qm[zigzag[k]] / 8;
            }
        }
    }

    /* IDCT and extract Y */
    int mw2 = W/16, mh2 = H/16;
    memset(Y_plane, 0, sizeof(int)*H*W);
    for (int mb = 0; mb < mw2*mh2; mb++) {
        int mx = mb % mw2, my = mb / mw2;
        for (int s = 0; s < 4; s++) {
            int out[64];
            idct8x8(blocks[mb*6+s], out);
            int bx = (s&1)*8, by = (s>>1)*8;
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++) {
                    int py = my*16+by+r, px = mx*16+bx+c;
                    if (py < H && px < W) Y_plane[py][px] = out[r*8+c] + 128;
                }
        }
    }
}

/* Calculate PSNR between two Y planes */
static double calc_psnr(int a[H][W], int b[H][W]) {
    double mse = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            double d = a[y][x] - b[y][x];
            mse += d * d;
        }
    mse /= (H * W);
    if (mse < 0.001) return 99.0;
    return 10.0 * log10(255.0*255.0 / mse);
}

/* Calculate spatial smoothness (avg abs diff between adjacent pixels) */
static double calc_smoothness(int Y[H][W]) {
    double sum = 0;
    int count = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W-1; x++) {
            sum += abs(Y[y][x] - Y[y][x+1]);
            count++;
        }
    for (int y = 0; y < H-1; y++)
        for (int x = 0; x < W; x++) {
            sum += abs(Y[y][x] - Y[y+1][x]);
            count++;
        }
    return sum / count;
}

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    int start_lba = atoi(argv[2]);

    int err; zip_t *z = zip_open(argv[1], ZIP_RDONLY, &err); if (!z) return 1;
    int bi=-1; zip_uint64_t bs2=0;
    for (int i=0; i<(int)zip_get_num_entries(z,0); i++) {
        zip_stat_t st; if(zip_stat_index(z,i,0,&st)==0 && st.size>bs2){bs2=st.size;bi=i;}}
    zip_stat_t st; zip_stat_index(z,bi,0,&st);
    zip_file_t *zf = zip_fopen_index(z,bi,0);
    uint8_t *disc = malloc(st.size);
    zip_int64_t rd=0;
    while(rd<(zip_int64_t)st.size){zip_int64_t r=zip_fread(zf,disc+rd,st.size-rd);if(r<=0)break;rd+=r;}
    zip_fclose(zf);
    int tsec = (int)(st.size/SECTOR_RAW);

    static uint8_t frames[8][MAX_FRAME]; int fsizes[8];
    int nf = assemble_frames(disc, tsec, start_lba, frames, fsizes, 8);
    if (nf < 2) { printf("Need at least 2 frames\n"); return 1; }

    printf("LBA %d: %d frames\n", start_lba, nf);
    for (int fi = 0; fi < nf && fi < 4; fi++)
        printf("  Frame %d: qs=%d type=%d fsize=%d\n",
               fi, frames[fi][3], frames[fi][39], fsizes[fi]);

    /* === Part 1: Test DC VLC variations === */
    printf("\n=== DC VLC table comparison ===\n");
    struct { const char *name; int dc_mode; } dc_tests[] = {
        {"All luminance DC", 0},
        {"Luma+Chroma DC", 1},
        {"All chrominance DC", 2},
    };

    for (int t = 0; t < 3; t++) {
        /* Decode DC only for frame 0 */
        uint8_t *f = frames[0];
        int bslen = fsizes[0] - 40;
        int qs = f[3];
        const uint8_t *bsdata = f + 40;
        int total_bits = bslen * 8;
        bitstream bs = {bsdata, total_bits, 0};

        int nblocks = (W/16)*(H/16)*6;
        int dc_pred[3] = {0,0,0};
        int dc_vals[900];
        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int sub = b % 6;
            int comp = (sub < 4) ? 0 : (sub == 4) ? 1 : 2;
            int use_chroma;
            switch (dc_tests[t].dc_mode) {
                case 0: use_chroma = 0; break;
                case 1: use_chroma = (comp > 0); break;
                case 2: use_chroma = 1; break;
                default: use_chroma = 0;
            }
            int diff = read_vlc(&bs, use_chroma);
            dc_pred[comp] += diff;
            dc_vals[b] = dc_pred[comp];
        }
        int dc_bits = bs.pos;
        int ac_bits = total_bits - dc_bits;

        /* Check DC value ranges */
        int y_min=9999, y_max=-9999, cb_min=9999, cb_max=-9999;
        for (int b = 0; b < nblocks; b++) {
            int sub = b % 6;
            if (sub < 4) { if(dc_vals[b]<y_min)y_min=dc_vals[b]; if(dc_vals[b]>y_max)y_max=dc_vals[b]; }
            else { if(dc_vals[b]<cb_min)cb_min=dc_vals[b]; if(dc_vals[b]>cb_max)cb_max=dc_vals[b]; }
        }
        printf("  %-20s: dc=%d bits (%.1f%%), Y_dc[%d,%d] C_dc[%d,%d]\n",
               dc_tests[t].name, dc_bits, 100.0*dc_bits/total_bits,
               y_min, y_max, cb_min, cb_max);
    }

    /* === Part 2: Inter-frame consistency === */
    printf("\n=== Inter-frame consistency (lower spatial_diff = better) ===\n");

    struct { const char *name; int dc_mode; int ac_mode; } models[] = {
        {"DC-only (luma DC)", 0, 3},
        {"DC-only (luma+chroma DC)", 1, 3},
        {"flag+luma_vlc (all luma DC)", 0, 0},
        {"flag+luma_vlc (luma+chroma DC)", 1, 0},
        {"flag+chroma_vlc_for_AC", 0, 1},
        {"no-flag VLC", 0, 2},
    };
    int nmodels = 6;

    static int Y0[H][W], Y1[H][W];

    for (int m = 0; m < nmodels; m++) {
        /* Decode frames 0 and 1 */
        int qs0 = frames[0][3], qs1 = frames[1][3];
        decode_frame(frames[0]+40, fsizes[0]-40, qs0,
                     models[m].dc_mode, models[m].ac_mode, Y0);
        decode_frame(frames[1]+40, fsizes[1]-40, qs1,
                     models[m].dc_mode, models[m].ac_mode, Y1);

        double psnr = calc_psnr(Y0, Y1);
        double smooth0 = calc_smoothness(Y0);
        double smooth1 = calc_smoothness(Y1);

        printf("  %-35s PSNR=%.1f dB smooth=%.1f,%.1f\n",
               models[m].name, psnr, smooth0, smooth1);
    }

    /* === Part 3: Try AC with different starting offsets === */
    printf("\n=== AC with different header sizes (frame 0) ===\n");
    int qs0 = frames[0][3];
    for (int hdr = 36; hdr <= 48; hdr += 2) {
        if (hdr > fsizes[0]) continue;
        const uint8_t *bsdata = frames[0] + hdr;
        int bslen = fsizes[0] - hdr;

        decode_frame(bsdata, bslen, qs0, 0, 0, Y0);
        double smooth = calc_smoothness(Y0);

        /* Also count how many AC bits consumed */
        int total_bits = bslen * 8;
        bitstream bs = {bsdata, total_bits, 0};
        int dc_pred[3] = {0,0,0};
        int nblocks = (W/16)*(H/16)*6;
        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
            int diff = read_vlc(&bs, 0);
            dc_pred[comp] += diff;
        }
        int dc_bits = bs.pos;

        printf("  hdr=%2d: dc=%d bits, smooth=%.1f\n", hdr, dc_bits, smooth);
    }

    free(disc); zip_close(z);
    return 0;
}
