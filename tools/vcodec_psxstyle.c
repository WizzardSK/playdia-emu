/*
 * vcodec_psxstyle.c - Test PS1 MDEC-style 16-bit fixed-width AC coding
 *
 * PlayStation MDEC format:
 * - DC: first 16-bit halfword = [6-bit q_scale | 10-bit signed DC]
 * - AC: 16-bit halfwords = [6-bit run | 10-bit signed level]
 * - EOB: 0xFE00
 * - Block order: Cr, Cb, Y1, Y2, Y3, Y4
 *
 * Playdia might use:
 * - DC: MPEG-1 VLC (confirmed) OR 16-bit like PS1
 * - AC: 16-bit halfwords like PS1
 *
 * Tests:
 * 1. All-16bit (PS1 style): DC in 16-bit word, AC in 16-bit words
 * 2. VLC DC + 16-bit AC halfwords (after bit/byte/word alignment)
 * 3. Various bit widths for run+level pairs
 * 4. Different EOB markers
 * 5. Different block orders (PS1 vs standard)
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

static const struct { int len; uint32_t code; int size; } dc_vlc[] = {
    {3, 0b100, 0}, {2, 0b00, 1}, {2, 0b01, 2}, {3, 0b101, 3},
    {3, 0b110, 4}, {4, 0b1110, 5}, {5, 0b11110, 6},
    {6, 0b111110, 7}, {7, 0b1111110, 8},
};
#define DC_VLC_COUNT 9

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

static int read_dc_vlc(bitstream *bs) {
    for (int i = 0; i < DC_VLC_COUNT; i++) {
        int bits = bs_peek(bs, dc_vlc[i].len);
        if (bits == (int)dc_vlc[i].code) {
            bs->pos += dc_vlc[i].len;
            int sz = dc_vlc[i].size;
            if (sz == 0) return 0;
            int val = bs_read(bs, sz);
            if (val < (1 << (sz - 1))) val -= (1 << sz) - 1;
            return val;
        }
    }
    bs->pos += 2;
    return 0;
}

static int sign_extend(int val, int bits) {
    int mask = 1 << (bits - 1);
    return (val ^ mask) - mask;
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

static void write_ppm(const char *fn, uint8_t *rgb, int w, int h) {
    FILE *f = fopen(fn, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, w*h*3, f);
    fclose(f);
}

/* Decode frame with PS1-style 16-bit halfword coding */
/* run_bits + level_bits = total bits per halfword */
static int decode_psx_style(const uint8_t *bsdata, int bslen,
                            int qs, int dc_mode,
                            int run_bits, int level_bits,
                            int eob_marker, /* -1 = no EOB, use full 63 */
                            int block_order, /* 0=Y4CbCr, 1=CrCbY4 */
                            uint8_t *rgb) {
    int total_bits = bslen * 8;
    int mw = W/16, mh = H/16;
    int nmb = mw * mh;
    int nblocks = nmb * 6;
    bitstream bs = {bsdata, total_bits, 0};
    int hw_bits = run_bits + level_bits;

    static int blocks[900][64];
    memset(blocks, 0, sizeof(blocks));

    /* Block mapping for different orders */
    /* Standard: Y0,Y1,Y2,Y3,Cb,Cr (indices 0,1,2,3,4,5) */
    /* PS1: Cr,Cb,Y1,Y2,Y3,Y4 (indices 5,4,0,1,2,3) */
    int bmap_std[] = {0,1,2,3,4,5};
    int bmap_psx[] = {5,4,0,1,2,3};
    int *bmap = (block_order == 1) ? bmap_psx : bmap_std;

    int blocks_decoded = 0;
    int total_ac_pairs = 0;
    int eob_count = 0;

    for (int mb = 0; mb < nmb && bs.pos < total_bits; mb++) {
        for (int si = 0; si < 6 && bs.pos < total_bits; si++) {
            int bi = mb * 6 + bmap[si];
            if (bi >= 900) continue;

            /* DC */
            if (dc_mode == 0) {
                /* VLC DC (MPEG-1 style) */
                int sub = bmap[si];
                int comp = (sub < 4) ? 0 : (sub == 4) ? 1 : 2;
                /* Note: we need persistent DC prediction. Using static. */
                static int dc_pred[3];
                if (mb == 0 && si == 0) { dc_pred[0]=dc_pred[1]=dc_pred[2]=0; }
                int diff = read_dc_vlc(&bs);
                dc_pred[comp] += diff;
                blocks[bi][0] = dc_pred[comp] * 8; /* MPEG-1 DC scaling */
            } else {
                /* 16-bit DC halfword: [6-bit q_scale | 10-bit DC] */
                int hw = bs_read(&bs, 16);
                if (hw < 0) break;
                /* int q = (hw >> 10) & 0x3F; */
                int dc = sign_extend(hw & 0x3FF, 10);
                blocks[bi][0] = dc * 8;
            }

            /* AC: read halfwords */
            int pos = 1;
            while (pos < 64 && bs.pos + hw_bits <= total_bits) {
                int hw = bs_read(&bs, hw_bits);
                if (hw < 0) break;

                /* Check EOB */
                if (eob_marker >= 0 && hw == eob_marker) {
                    eob_count++;
                    break;
                }

                int run = (hw >> level_bits) & ((1 << run_bits) - 1);
                int level = sign_extend(hw & ((1 << level_bits) - 1), level_bits);

                if (level == 0 && eob_marker < 0) {
                    /* In no-EOB mode, level=0 with any run might mean skip */
                    pos += run + 1;
                    continue;
                }

                pos += run;
                if (pos >= 64) break;

                /* Dequantize: PS1-style */
                int qi = ((zigzag[pos]/8 >> 1) << 2) | ((zigzag[pos]%8) >> 1);
                int qval = default_qtable[qi] * qs;
                blocks[bi][zigzag[pos]] = (level * qval + 4) / 8;
                pos++;
                total_ac_pairs++;
            }
            blocks_decoded++;
        }
    }

    /* Convert blocks to image */
    memset(rgb, 128, W*H*3);
    int Y[H][W], Cb[H/2][W/2], Cr[H/2][W/2];
    memset(Y, 0, sizeof(Y)); memset(Cb, 0, sizeof(Cb)); memset(Cr, 0, sizeof(Cr));

    for (int mb = 0; mb < nmb && mb*6+5 < blocks_decoded; mb++) {
        int mx = mb % mw, my = mb / mw;
        int out[64];
        for (int s = 0; s < 4; s++) {
            idct8x8(blocks[mb*6+s], out);
            int bx = (s&1)*8, by = (s>>1)*8;
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++) {
                    int py = my*16+by+r, px = mx*16+bx+c;
                    if (py < H && px < W) Y[py][px] = out[r*8+c] + 128;
                }
        }
        idct8x8(blocks[mb*6+4], out);
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++) {
                int py = my*8+r, px = mx*8+c;
                if (py < H/2 && px < W/2) Cb[py][px] = out[r*8+c] + 128;
            }
        idct8x8(blocks[mb*6+5], out);
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++) {
                int py = my*8+r, px = mx*8+c;
                if (py < H/2 && px < W/2) Cr[py][px] = out[r*8+c] + 128;
            }
    }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int yv = Y[y][x], cb = Cb[y/2][x/2]-128, cr = Cr[y/2][x/2]-128;
            rgb[(y*W+x)*3+0] = clamp(yv + 1.402*cr);
            rgb[(y*W+x)*3+1] = clamp(yv - 0.344136*cb - 0.714136*cr);
            rgb[(y*W+x)*3+2] = clamp(yv + 1.772*cb);
        }

    /* Print stats */
    printf("    blocks=%d/%d bits=%d/%d (%.1f%%) ac_pairs=%d eobs=%d avg_ac=%.1f/blk\n",
           blocks_decoded, nblocks, bs.pos, total_bits, 100.0*bs.pos/total_bits,
           total_ac_pairs, eob_count,
           blocks_decoded > 0 ? (double)total_ac_pairs/blocks_decoded : 0);

    /* Calculate smoothness */
    double smooth = 0; int cnt = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W-1; x++) {
            smooth += abs(Y[y][x] - Y[y][x+1]); cnt++;
        }
    smooth /= cnt;

    /* Check AC stats: frequency decay */
    int low_nz = 0, high_nz = 0;
    for (int b = 0; b < nblocks && b < blocks_decoded; b++) {
        if (b%6 >= 4) continue;
        for (int k = 1; k <= 10; k++) if (blocks[b][zigzag[k]]) low_nz++;
        for (int k = 50; k <= 63; k++) if (blocks[b][zigzag[k]]) high_nz++;
    }
    printf("    smooth=%.1f low_nz=%d high_nz=%d\n", smooth, low_nz, high_nz);

    return blocks_decoded;
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
    if (nf == 0) { printf("No frames\n"); return 1; }

    uint8_t *f = frames[0];
    int fsize = fsizes[0];
    int qs = f[3];
    printf("LBA %d frame 0: qs=%d type=%d fsize=%d\n\n", start_lba, qs, f[39], fsize);

    const uint8_t *bsdata = f + 40;
    int bslen = fsize - 40;
    uint8_t *rgb = malloc(W * H * 3);

    /* Dump first 32 bytes as 16-bit big-endian halfwords */
    printf("First 16 halfwords (big-endian): ");
    for (int i = 0; i < 32 && i < bslen; i += 2)
        printf("%04X ", (bsdata[i]<<8)|bsdata[i+1]);
    printf("\n");

    /* And as little-endian */
    printf("First 16 halfwords (little-end): ");
    for (int i = 0; i < 32 && i < bslen; i += 2)
        printf("%04X ", bsdata[i]|(bsdata[i+1]<<8));
    printf("\n\n");

    /* === Test 1: Full PS1-style (16-bit DC+AC, various EOB markers) === */
    printf("=== PS1-style: 16-bit DC + 16-bit AC (6+10) ===\n");

    int eob_markers[] = {0xFE00, 0x0000, 0xFC00, 0x7C00, 0x3F00, -1};
    const char *eob_names[] = {"FE00", "0000", "FC00", "7C00", "3F00", "none"};
    for (int e = 0; e < 6; e++) {
        printf("  EOB=%s, std order: ", eob_names[e]);
        decode_psx_style(bsdata, bslen, qs, 1, 6, 10, eob_markers[e], 0, rgb);
    }

    /* === Test 2: VLC DC + 16-bit AC halfwords === */
    printf("\n=== VLC DC + 16-bit AC (6+10) ===\n");

    /* First, decode DC with VLC to find AC start */
    {
        bitstream bs = {bsdata, bslen*8, 0};
        int nblocks = (W/16)*(H/16)*6;
        int dc_pred[3] = {0,0,0};
        for (int b = 0; b < nblocks; b++) {
            int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
            read_dc_vlc(&bs);
        }
        int dc_end = bs.pos;
        printf("  DC VLC ends at bit %d (byte %d.%d)\n", dc_end, dc_end/8, dc_end%8);

        /* Try byte-aligned and halfword-aligned starts */
        int starts[] = {dc_end, (dc_end+7)&~7, (dc_end+15)&~15, dc_end+4, dc_end+8};
        for (int s = 0; s < 5; s++) {
            int ac_start_bit = starts[s];
            int ac_start_byte = ac_start_bit / 8;
            int ac_len = bslen - ac_start_byte;

            printf("  AC start bit %d: ", ac_start_bit);

            /* Read 16-bit halfwords from AC data */
            bitstream ac_bs = {bsdata, bslen*8, ac_start_bit};

            int nz_ac = 0, total_hw = 0, blocks_done = 0;
            int low_nz = 0, high_nz = 0;
            static int blk[64];

            for (int b = 0; b < nblocks && ac_bs.pos + 16 <= bslen*8; b++) {
                memset(blk, 0, sizeof(blk));
                int pos = 1;
                while (pos < 64 && ac_bs.pos + 16 <= bslen*8) {
                    int hw = bs_read(&ac_bs, 16);
                    total_hw++;

                    if (hw == 0xFE00) break; /* EOB */

                    int run = (hw >> 10) & 0x3F;
                    int level = sign_extend(hw & 0x3FF, 10);

                    pos += run;
                    if (pos >= 64) break;
                    blk[pos] = level;
                    nz_ac++;

                    /* Track freq decay */
                    int zz_pos = 0;
                    for (int k = 0; k < 64; k++) if (zigzag[k] == pos) { zz_pos = k; break; }
                    if (zz_pos <= 10) low_nz++;
                    if (zz_pos >= 50) high_nz++;
                    pos++;
                }
                blocks_done++;
            }

            printf("blocks=%d hw=%d nz=%d bits=%d/%d (%.1f%%) low=%d high=%d\n",
                   blocks_done, total_hw, nz_ac, ac_bs.pos, bslen*8,
                   100.0*ac_bs.pos/(bslen*8), low_nz, high_nz);
        }
    }

    /* === Test 3: Different bit widths for run+level === */
    printf("\n=== Different bit widths (VLC DC, byte-aligned AC) ===\n");
    {
        bitstream bs = {bsdata, bslen*8, 0};
        int nblocks = (W/16)*(H/16)*6;
        for (int b = 0; b < nblocks; b++) read_dc_vlc(&bs);
        int dc_end = (bs.pos + 7) & ~7; /* byte-align */

        int configs[][3] = {
            {4, 8, 12},   /* 4-bit run + 8-bit level = 12 bits */
            {5, 7, 12},   /* 5+7 = 12 */
            {4, 12, 16},  /* 4+12 = 16 */
            {5, 11, 16},  /* 5+11 = 16 */
            {6, 10, 16},  /* PS1 style */
            {4, 4, 8},    /* 4+4 = 8 bits */
            {3, 5, 8},    /* 3+5 = 8 bits */
        };
        int nconfigs = 7;

        for (int ci = 0; ci < nconfigs; ci++) {
            int rb = configs[ci][0], lb = configs[ci][1], tb = configs[ci][2];
            printf("  %d-bit run + %d-bit level (%d total): ", rb, lb, tb);

            bitstream ac_bs = {bsdata, bslen*8, dc_end};
            int nz_ac = 0, blocks_done = 0, eobs = 0;
            int low_nz = 0, high_nz = 0;

            for (int b = 0; b < nblocks && ac_bs.pos + tb <= bslen*8; b++) {
                int pos = 1;
                while (pos < 64 && ac_bs.pos + tb <= bslen*8) {
                    int hw = bs_read(&ac_bs, tb);
                    int max_run = (1 << rb) - 1;
                    int run = (hw >> lb) & max_run;
                    int level = sign_extend(hw & ((1 << lb) - 1), lb);

                    /* EOB: run=max, level=0 or specific pattern */
                    if (run == max_run && level == 0) { eobs++; break; }
                    /* Also try: level=0 as implicit EOB-like */
                    if (level == 0) { pos += run + 1; continue; }

                    pos += run;
                    if (pos >= 64) break;

                    int zz_pos = 0;
                    for (int k = 0; k < 64; k++) if (zigzag[k] == pos) { zz_pos = k; break; }
                    if (zz_pos <= 10) low_nz++;
                    if (zz_pos >= 50) high_nz++;
                    nz_ac++;
                    pos++;
                }
                blocks_done++;
            }
            printf("blk=%d nz=%d eobs=%d bits=%d/%d (%.1f%%) low=%d high=%d\n",
                   blocks_done, nz_ac, eobs, ac_bs.pos, bslen*8,
                   100.0*ac_bs.pos/(bslen*8), low_nz, high_nz);
        }
    }

    /* === Test 4: PS1 style with image output === */
    printf("\n=== Image output tests ===\n");

    /* 4a: PS1 full 16-bit, EOB=FE00, std order */
    printf("  PS1_full_FE00_std: ");
    decode_psx_style(bsdata, bslen, qs, 1, 6, 10, 0xFE00, 0, rgb);
    write_ppm("/tmp/psx_full.ppm", rgb, W, H);

    /* 4b: PS1 full 16-bit, no EOB, std order */
    printf("  PS1_full_noEOB_std: ");
    decode_psx_style(bsdata, bslen, qs, 1, 6, 10, -1, 0, rgb);
    write_ppm("/tmp/psx_full_noeob.ppm", rgb, W, H);

    /* 4c: VLC DC + 16-bit AC, byte-aligned */
    printf("  VLC_DC+16bit_AC: ");
    {
        /* Hybrid: VLC DC then 16-bit AC */
        bitstream bs = {bsdata, bslen*8, 0};
        int nblocks = (W/16)*(H/16)*6;
        static int hblocks[900][64];
        memset(hblocks, 0, sizeof(hblocks));

        int dc_pred[3] = {0,0,0};
        for (int b = 0; b < nblocks; b++) {
            int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
            int diff = read_dc_vlc(&bs);
            dc_pred[comp] += diff;
            hblocks[b][0] = dc_pred[comp] * 8;
        }
        /* Byte-align */
        bs.pos = (bs.pos + 7) & ~7;

        int total_nz = 0, total_eob = 0;
        for (int b = 0; b < nblocks && bs.pos + 16 <= bslen*8; b++) {
            int pos = 1;
            while (pos < 64 && bs.pos + 16 <= bslen*8) {
                int hw = bs_read(&bs, 16);
                if (hw == 0xFE00) { total_eob++; break; }
                int run = (hw >> 10) & 0x3F;
                int level = sign_extend(hw & 0x3FF, 10);
                if (level == 0) { pos += run + 1; continue; }
                pos += run;
                if (pos >= 64) break;
                int qi = ((zigzag[pos]/8 >> 1) << 2) | ((zigzag[pos]%8) >> 1);
                int qval = default_qtable[qi] * qs;
                hblocks[b][zigzag[pos]] = (level * qval + 4) / 8;
                pos++;
                total_nz++;
            }
        }
        printf("bits=%d/%d (%.1f%%) nz=%d eobs=%d\n",
               bs.pos, bslen*8, 100.0*bs.pos/(bslen*8), total_nz, total_eob);

        /* Build image from hblocks */
        memset(rgb, 128, W*H*3);
        int Yp[H][W], Cbp[H/2][W/2], Crp[H/2][W/2];
        memset(Yp,0,sizeof(Yp)); memset(Cbp,0,sizeof(Cbp)); memset(Crp,0,sizeof(Crp));
        int mw=W/16, mh=H/16;
        for (int mb = 0; mb < mw*mh; mb++) {
            int mx = mb%mw, my = mb/mw;
            int out[64];
            for (int s = 0; s < 4; s++) {
                idct8x8(hblocks[mb*6+s], out);
                int bx=(s&1)*8, by=(s>>1)*8;
                for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
                    int py=my*16+by+r, px=mx*16+bx+c;
                    if(py<H&&px<W) Yp[py][px]=out[r*8+c]+128;
                }
            }
            idct8x8(hblocks[mb*6+4], out);
            for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
                int py=my*8+r, px=mx*8+c;
                if(py<H/2&&px<W/2) Cbp[py][px]=out[r*8+c]+128;
            }
            idct8x8(hblocks[mb*6+5], out);
            for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
                int py=my*8+r, px=mx*8+c;
                if(py<H/2&&px<W/2) Crp[py][px]=out[r*8+c]+128;
            }
        }
        for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
            int yv=Yp[y][x], cb=Cbp[y/2][x/2]-128, cr=Crp[y/2][x/2]-128;
            rgb[(y*W+x)*3+0]=clamp(yv+1.402*cr);
            rgb[(y*W+x)*3+1]=clamp(yv-0.344136*cb-0.714136*cr);
            rgb[(y*W+x)*3+2]=clamp(yv+1.772*cb);
        }
        write_ppm("/tmp/psx_vlcdc_16ac.ppm", rgb, W, H);
    }

    free(rgb); free(disc); zip_close(z);
    return 0;
}
