/*
 * vcodec_diag.c - Comprehensive diagnostic for Playdia video codec
 * Tests: DC stats, luminance-only, different dequant, resolution, AC stats
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

/* Chrominance DC VLC (MPEG-1 Table B.11) */
static const struct { int len; uint32_t code; int size; } dc_chroma_vlc[] = {
    {2, 0b00, 0}, {2, 0b01, 1}, {2, 0b10, 2}, {3, 0b110, 3},
    {4, 0b1110, 4}, {5, 0b11110, 5}, {6, 0b111110, 6},
    {7, 0b1111110, 7}, {8, 0b11111110, 8},
};
#define DC_CHROMA_VLC_COUNT 9

static int read_dc_chroma_vlc(bitstream *bs) {
    for (int i = 0; i < DC_CHROMA_VLC_COUNT; i++) {
        int bits = bs_peek(bs, dc_chroma_vlc[i].len);
        if (bits == (int)dc_chroma_vlc[i].code) {
            bs->pos += dc_chroma_vlc[i].len;
            int sz = dc_chroma_vlc[i].size;
            if (sz == 0) return 0;
            int val = bs_read(bs, sz);
            if (val < (1 << (sz - 1))) val -= (1 << sz) - 1;
            return val;
        }
    }
    bs->pos += 2;
    return 0;
}

typedef struct { uint32_t code; int len; int run; int level; } ac_entry;
static const ac_entry ac_table[] = {
    {0b11, 2, 0, 1}, {0b011, 3, 1, 1}, {0b0100, 4, 0, 2}, {0b0101, 4, 2, 1},
    {0b00101, 5, 0, 3}, {0b00110, 5, 4, 1}, {0b00111, 5, 3, 1},
    {0b000100, 6, 7, 1}, {0b000101, 6, 6, 1}, {0b000110, 6, 1, 2}, {0b000111, 6, 5, 1},
    {0b0000100, 7, 2, 2}, {0b0000101, 7, 9, 1}, {0b0000110, 7, 0, 4}, {0b0000111, 7, 8, 1},
    {0b00100000, 8, 13, 1}, {0b00100001, 8, 0, 6}, {0b00100010, 8, 12, 1},
    {0b00100011, 8, 11, 1}, {0b00100100, 8, 3, 2}, {0b00100101, 8, 1, 3},
    {0b00100110, 8, 0, 5}, {0b00100111, 8, 10, 1},
    {0b0000001000, 10, 16, 1}, {0b0000001001, 10, 5, 2}, {0b0000001010, 10, 0, 7},
    {0b0000001011, 10, 2, 3}, {0b0000001100, 10, 1, 4}, {0b0000001101, 10, 15, 1},
    {0b0000001110, 10, 14, 1}, {0b0000001111, 10, 4, 2},
    {0b000000010000, 12, 0, 11}, {0b000000010001, 12, 8, 2}, {0b000000010010, 12, 4, 3},
    {0b000000010011, 12, 0, 10}, {0b000000010100, 12, 2, 4}, {0b000000010101, 12, 7, 2},
    {0b000000010110, 12, 21, 1}, {0b000000010111, 12, 20, 1}, {0b000000011000, 12, 0, 9},
    {0b000000011001, 12, 19, 1}, {0b000000011010, 12, 18, 1}, {0b000000011011, 12, 1, 5},
    {0b000000011100, 12, 3, 3}, {0b000000011101, 12, 0, 8}, {0b000000011110, 12, 6, 2},
    {0b000000011111, 12, 17, 1},
    {0b0000000010000, 13, 10, 2}, {0b0000000010001, 13, 9, 2}, {0b0000000010010, 13, 5, 3},
    {0b0000000010011, 13, 3, 4}, {0b0000000010100, 13, 2, 5}, {0b0000000010101, 13, 1, 7},
    {0b0000000010110, 13, 1, 6}, {0b0000000010111, 13, 0, 15}, {0b0000000011000, 13, 0, 14},
    {0b0000000011001, 13, 0, 13}, {0b0000000011010, 13, 0, 12}, {0b0000000011011, 13, 26, 1},
    {0b0000000011100, 13, 25, 1}, {0b0000000011101, 13, 24, 1}, {0b0000000011110, 13, 23, 1},
    {0b0000000011111, 13, 22, 1},
    {0b00000000010000, 14, 0, 31}, {0b00000000010001, 14, 0, 30}, {0b00000000010010, 14, 0, 29},
    {0b00000000010011, 14, 0, 28}, {0b00000000010100, 14, 0, 27}, {0b00000000010101, 14, 0, 26},
    {0b00000000010110, 14, 0, 25}, {0b00000000010111, 14, 0, 24}, {0b00000000011000, 14, 0, 23},
    {0b00000000011001, 14, 0, 22}, {0b00000000011010, 14, 0, 21}, {0b00000000011011, 14, 0, 20},
    {0b00000000011100, 14, 0, 19}, {0b00000000011101, 14, 0, 18}, {0b00000000011110, 14, 0, 17},
    {0b00000000011111, 14, 0, 16},
    {0b000000000010000, 15, 0, 40}, {0b000000000010001, 15, 0, 39}, {0b000000000010010, 15, 0, 38},
    {0b000000000010011, 15, 0, 37}, {0b000000000010100, 15, 0, 36}, {0b000000000010101, 15, 0, 35},
    {0b000000000010110, 15, 0, 34}, {0b000000000010111, 15, 0, 33}, {0b000000000011000, 15, 0, 32},
    {0b000000000011001, 15, 1, 14}, {0b000000000011010, 15, 1, 13}, {0b000000000011011, 15, 1, 12},
    {0b000000000011100, 15, 1, 11}, {0b000000000011101, 15, 1, 10}, {0b000000000011110, 15, 1, 9},
    {0b000000000011111, 15, 1, 8},
    {0b0000000000010000, 16, 1, 18}, {0b0000000000010001, 16, 1, 17}, {0b0000000000010010, 16, 1, 16},
    {0b0000000000010011, 16, 1, 15}, {0b0000000000010100, 16, 6, 3}, {0b0000000000010101, 16, 16, 2},
    {0b0000000000010110, 16, 15, 2}, {0b0000000000010111, 16, 14, 2}, {0b0000000000011000, 16, 13, 2},
    {0b0000000000011001, 16, 12, 2}, {0b0000000000011010, 16, 11, 2}, {0b0000000000011011, 16, 31, 1},
    {0b0000000000011100, 16, 30, 1}, {0b0000000000011101, 16, 29, 1}, {0b0000000000011110, 16, 28, 1},
    {0b0000000000011111, 16, 27, 1},
};
#define AC_TABLE_SIZE 111

static int decode_ac(bitstream *bs, int *run, int *level) {
    if (bs->pos + 2 > bs->total_bits) return -1;
    if (bs_peek(bs, 2) == 0b10) { bs->pos += 2; return 0; }
    if (bs->pos + 6 <= bs->total_bits && bs_peek(bs, 6) == 0b000001) {
        bs->pos += 6;
        *run = bs_read(bs, 6);
        int raw = bs_read(bs, 10);
        *level = (raw >= 512) ? raw - 1024 : raw;
        return 1;
    }
    for (int i = 0; i < AC_TABLE_SIZE; i++) {
        if (bs->pos + ac_table[i].len + 1 > bs->total_bits) continue;
        if (bs_peek(bs, ac_table[i].len) == (int)ac_table[i].code) {
            bs->pos += ac_table[i].len;
            int sign = bs_read(bs, 1);
            *run = ac_table[i].run;
            *level = ac_table[i].level;
            if (sign) *level = -(*level);
            return 1;
        }
    }
    return -1;
}

static const int zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

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

static void write_ppm(const char *fn, uint8_t *img, int w, int h, int ch) {
    FILE *f = fopen(fn, "wb");
    if (ch == 1) {
        fprintf(f, "P5\n%d %d\n255\n", w, h);
        fwrite(img, 1, w*h, f);
    } else {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        fwrite(img, 1, w*h*3, f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) { printf("Usage: %s <zip> <lba>\n", argv[0]); return 1; }
    int start_lba = atoi(argv[2]);

    int err; zip_t *z = zip_open(argv[1], ZIP_RDONLY, &err); if (!z) return 1;
    int bi2=-1; zip_uint64_t bs2=0;
    for (int i=0; i<(int)zip_get_num_entries(z,0); i++) {
        zip_stat_t st; if(zip_stat_index(z,i,0,&st)==0 && st.size>bs2){bs2=st.size;bi2=i;}}
    zip_stat_t st; zip_stat_index(z,bi2,0,&st);
    zip_file_t *zf = zip_fopen_index(z,bi2,0);
    uint8_t *disc = malloc(st.size);
    zip_int64_t rd=0;
    while(rd<(zip_int64_t)st.size){zip_int64_t r=zip_fread(zf,disc+rd,st.size-rd);if(r<=0)break;rd+=r;}
    zip_fclose(zf);
    int tsec = (int)(st.size/SECTOR_RAW);

    static uint8_t frames[8][MAX_FRAME]; int fsizes[8];
    int nf = assemble_frames(disc, tsec, start_lba, frames, fsizes, 8);
    if (nf == 0) { printf("No frames found\n"); return 1; }

    printf("=== FRAME DIAGNOSTICS LBA %d ===\n", start_lba);
    printf("Found %d frames\n\n", nf);

    for (int fi = 0; fi < nf && fi < 3; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        int qs = f[3], type = f[39];
        const uint8_t *bsdata = f + 40;
        int bslen = fsize - 40;

        printf("--- Frame %d: fsize=%d qs=%d type=%d ---\n", fi, fsize, qs, type);
        printf("Header bytes 0-39:\n");
        for (int i = 0; i < 40; i++) {
            printf("%02X ", f[i]);
            if ((i & 15) == 15) printf("\n");
        }
        if (40 % 16) printf("\n");

        /* Find real data length (non-padding) */
        int last_nonff = bslen - 1;
        while (last_nonff >= 0 && bsdata[last_nonff] == 0xFF) last_nonff--;
        int last_nonzero = bslen - 1;
        while (last_nonzero >= 0 && bsdata[last_nonzero] == 0x00) last_nonzero--;

        printf("Bitstream: %d bytes, last non-FF at %d, last non-00 at %d\n",
               bslen, last_nonff, last_nonzero);

        int real_len = last_nonff + 1; /* assume FF padding */
        if (last_nonzero < last_nonff) real_len = bslen; /* no FF padding */
        int total_bits = bslen * 8;
        int real_bits = real_len * 8;
        printf("Real data: %d bytes = %d bits (%.1f%% of frame)\n\n",
               real_len, real_bits, 100.0*real_len/bslen);

        /* ===== TEST 1: DC statistics ===== */
        printf("== DC Statistics (luma-only VLC) ==\n");
        {
            int W = 256, H = 144;
            int mw = W/16, mh = H/16, nblocks = mw*mh*6;
            bitstream bs = {bsdata, total_bits, 0};
            int dc_pred[3] = {0,0,0};
            int dc_min = 99999, dc_max = -99999;
            int dc_vals[900];

            for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
                int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
                int diff = read_dc_vlc(&bs);
                dc_pred[comp] += diff;
                dc_vals[b] = dc_pred[comp];
                if (dc_pred[comp] < dc_min) dc_min = dc_pred[comp];
                if (dc_pred[comp] > dc_max) dc_max = dc_pred[comp];
            }
            printf("  All same VLC: DC range [%d, %d], bits used = %d (%.1f%%)\n",
                   dc_min, dc_max, bs.pos, 100.0*bs.pos/total_bits);
            printf("  First 12 DC: ");
            for (int i = 0; i < 12 && i < nblocks; i++)
                printf("%d ", dc_vals[i]);
            printf("\n");
        }

        /* ===== TEST 2: DC with chroma VLC ===== */
        printf("\n== DC with luma + chroma VLC ==\n");
        {
            int W = 256, H = 144;
            int mw = W/16, mh = H/16, nblocks = mw*mh*6;
            bitstream bs = {bsdata, total_bits, 0};
            int dc_pred[3] = {0,0,0};
            int y_min = 99999, y_max = -99999;
            int cb_min = 99999, cb_max = -99999;
            int cr_min = 99999, cr_max = -99999;

            for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
                int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
                int diff;
                if (comp == 0)
                    diff = read_dc_vlc(&bs);
                else
                    diff = read_dc_chroma_vlc(&bs);
                dc_pred[comp] += diff;
                if (comp == 0) { if (dc_pred[0]<y_min) y_min=dc_pred[0]; if (dc_pred[0]>y_max) y_max=dc_pred[0]; }
                if (comp == 1) { if (dc_pred[1]<cb_min) cb_min=dc_pred[1]; if (dc_pred[1]>cb_max) cb_max=dc_pred[1]; }
                if (comp == 2) { if (dc_pred[2]<cr_min) cr_min=dc_pred[2]; if (dc_pred[2]>cr_max) cr_max=dc_pred[2]; }
            }
            printf("  Y  DC range: [%d, %d]\n", y_min, y_max);
            printf("  Cb DC range: [%d, %d]\n", cb_min, cb_max);
            printf("  Cr DC range: [%d, %d]\n", cr_min, cr_max);
            printf("  Bits: %d (%.1f%%)\n", bs.pos, 100.0*bs.pos/total_bits);
        }

        /* ===== TEST 3: VLC+EOB bit position vs real data length ===== */
        printf("\n== VLC+EOB endpoint analysis ==\n");
        {
            int W = 256, H = 144;
            int mw = W/16, mh = H/16, nblocks = mw*mh*6;
            bitstream bs = {bsdata, total_bits, 0};
            int dc_pred[3] = {0,0,0};
            int eobs = 0, nz_total = 0, errs = 0;
            int run_hist[64] = {0};
            int level_hist[64] = {0}; /* levels 0-63 */

            for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
                int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
                int diff = read_dc_vlc(&bs);
                dc_pred[comp] += diff;
                int pos = 1;
                while (pos < 64 && bs.pos < total_bits) {
                    int run, level;
                    int ret = decode_ac(&bs, &run, &level);
                    if (ret == 0) { eobs++; break; }
                    if (ret == -1) { errs++; bs.pos++; break; }
                    if (run < 64) run_hist[run]++;
                    int al = abs(level);
                    if (al < 64) level_hist[al]++;
                    nz_total++;
                    pos += run;
                    if (pos >= 64) break;
                    pos++;
                }
            }
            int end_byte = (bs.pos + 7) / 8;
            printf("  Stopped at bit %d (byte %d of %d)\n", bs.pos, end_byte, bslen);
            printf("  EOBs=%d, NZ=%d, errors=%d\n", eobs, nz_total, errs);
            printf("  Avg AC/block: %.1f\n", eobs > 0 ? (double)nz_total/eobs : 0);
            printf("  Distance from real end: %d bytes\n", real_len - end_byte);

            printf("  Run histogram: ");
            for (int i = 0; i < 16; i++) printf("r%d=%d ", i, run_hist[i]);
            printf("\n");
            printf("  Level histogram: ");
            for (int i = 1; i < 16; i++) printf("l%d=%d ", i, level_hist[i]);
            printf("\n");

            /* What's at the end position? */
            printf("  Bytes at VLC end: ");
            for (int i = end_byte-2; i < end_byte+8 && i < bslen; i++)
                printf("%02X ", bsdata[i]);
            printf("\n");
        }

        /* ===== TEST 4: Output images at 256x144 ===== */
        if (fi == 0) {
            int W = 256, H = 144;
            int mw = W/16, mh = H/16, nblocks = mw*mh*6;
            uint8_t *gray = malloc(W*H);
            uint8_t *rgb = malloc(W*H*3);

            int default_qtable[16] = {10,20,14,13,18,37,22,28,15,24,15,18,18,31,17,20};
            int qm[64];
            for (int i = 0; i < 64; i++) {
                int qi = ((i/8/2)*4 + (i%8/2));
                qm[i] = default_qtable[qi] * qs;
            }

            /* DC-only grayscale */
            printf("\n== Output: DC-only luminance ==\n");
            {
                static int blocks[900][64];
                memset(blocks, 0, sizeof(blocks));
                bitstream bs = {bsdata, total_bits, 0};
                int dc_pred[3] = {0,0,0};
                for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
                    int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
                    int diff = read_dc_vlc(&bs);
                    dc_pred[comp] += diff;
                    blocks[b][0] = dc_pred[comp] * 8;
                }
                /* Y only */
                int Y[H][W];
                memset(Y, 0, sizeof(Y));
                for (int mb = 0; mb < mw*mh && mb*6+3 < nblocks; mb++) {
                    int mx2 = mb%mw, my2 = mb/mw;
                    int out[64];
                    for (int s = 0; s < 4; s++) {
                        idct8x8(blocks[mb*6+s], out);
                        int bx=(s&1)*8, by=(s>>1)*8;
                        for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
                            int py=my2*16+by+r, px=mx2*16+bx+c;
                            if(py<H&&px<W) Y[py][px]=clamp(out[r*8+c]+128);
                        }
                    }
                }
                for (int y=0;y<H;y++) for (int x=0;x<W;x++) gray[y*W+x]=Y[y][x];
                write_ppm("/tmp/diag_dc_gray.ppm", gray, W, H, 1);
                printf("  → /tmp/diag_dc_gray.ppm\n");
            }

            /* VLC+EOB with different dequant scales */
            int scales[] = {1, 2, 4, 8};
            for (int si = 0; si < 4; si++) {
                int scale = scales[si];
                printf("\n== Output: VLC+EOB, dequant=level*%d ==\n", scale);
                static int blocks[900][64];
                memset(blocks, 0, sizeof(blocks));
                bitstream bs = {bsdata, total_bits, 0};
                int dc_pred[3] = {0,0,0};
                for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
                    int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
                    int diff = read_dc_vlc(&bs);
                    dc_pred[comp] += diff;
                    blocks[b][0] = dc_pred[comp] * 8;
                    int pos = 1;
                    while (pos < 64 && bs.pos < total_bits) {
                        int run, level;
                        int ret = decode_ac(&bs, &run, &level);
                        if (ret == 0) break;
                        if (ret == -1) { bs.pos++; break; }
                        pos += run;
                        if (pos >= 64) break;
                        blocks[b][zigzag[pos]] = level * scale;
                        pos++;
                    }
                }
                /* Y only */
                int Y[H][W];
                memset(Y, 0, sizeof(Y));
                for (int mb = 0; mb < mw*mh && mb*6+3 < nblocks; mb++) {
                    int mx2 = mb%mw, my2 = mb/mw;
                    int out[64];
                    for (int s = 0; s < 4; s++) {
                        idct8x8(blocks[mb*6+s], out);
                        int bx=(s&1)*8, by=(s>>1)*8;
                        for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
                            int py=my2*16+by+r, px=mx2*16+bx+c;
                            if(py<H&&px<W) Y[py][px]=clamp(out[r*8+c]+128);
                        }
                    }
                }
                for (int y=0;y<H;y++) for (int x=0;x<W;x++) gray[y*W+x]=Y[y][x];
                char fn[64];
                snprintf(fn, sizeof(fn), "/tmp/diag_vlc_s%d.ppm", scale);
                write_ppm(fn, gray, W, H, 1);

                /* Smoothness */
                double smooth = 0; int cnt = 0;
                for (int y=0;y<H;y++) for (int x=0;x<W-1;x++) {
                    smooth += abs(Y[y][x]-Y[y][x+1]); cnt++;
                }
                printf("  smoothness=%.1f → %s\n", smooth/cnt, fn);
            }

            /* VLC+EOB with MPEG-1 dequant formula */
            printf("\n== Output: VLC+EOB, MPEG-1 dequant=(2*level+1)*qm/16 ==\n");
            {
                static int blocks[900][64];
                memset(blocks, 0, sizeof(blocks));
                bitstream bs = {bsdata, total_bits, 0};
                int dc_pred[3] = {0,0,0};
                for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
                    int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
                    int diff = read_dc_vlc(&bs);
                    dc_pred[comp] += diff;
                    blocks[b][0] = dc_pred[comp] * 8;
                    int pos = 1;
                    while (pos < 64 && bs.pos < total_bits) {
                        int run, level;
                        int ret = decode_ac(&bs, &run, &level);
                        if (ret == 0) break;
                        if (ret == -1) { bs.pos++; break; }
                        pos += run;
                        if (pos >= 64) break;
                        /* MPEG-1 intra dequant: (2*level+sign)*qscale*W[i]/16 */
                        int sign = (level < 0) ? -1 : 1;
                        int al = abs(level);
                        int val = ((2*al+1) * qm[zigzag[pos]]) / 16;
                        blocks[b][zigzag[pos]] = sign * val;
                        pos++;
                    }
                }
                int Y[H][W];
                memset(Y, 0, sizeof(Y));
                for (int mb = 0; mb < mw*mh && mb*6+3 < nblocks; mb++) {
                    int mx2 = mb%mw, my2 = mb/mw;
                    int out[64];
                    for (int s = 0; s < 4; s++) {
                        idct8x8(blocks[mb*6+s], out);
                        int bx=(s&1)*8, by=(s>>1)*8;
                        for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
                            int py=my2*16+by+r, px=mx2*16+bx+c;
                            if(py<H&&px<W) Y[py][px]=clamp(out[r*8+c]+128);
                        }
                    }
                }
                for (int y=0;y<H;y++) for (int x=0;x<W;x++) gray[y*W+x]=Y[y][x];
                write_ppm("/tmp/diag_vlc_mpeg1.ppm", gray, W, H, 1);
                double smooth = 0; int cnt = 0;
                for (int y=0;y<H;y++) for (int x=0;x<W-1;x++) {
                    smooth += abs(Y[y][x]-Y[y][x+1]); cnt++;
                }
                printf("  smoothness=%.1f → /tmp/diag_vlc_mpeg1.ppm\n", smooth/cnt);
            }

            /* ===== TEST 5: What if AC uses JPEG-style size+value per coefficient? ===== */
            printf("\n== Test: DC-VLC for each AC (flag + size-value) ==\n");
            {
                static int blocks[900][64];
                memset(blocks, 0, sizeof(blocks));
                bitstream bs = {bsdata, total_bits, 0};
                int dc_pred[3] = {0,0,0};
                int nz_total = 0, zero_total = 0;
                bool ok = true;

                for (int b = 0; b < nblocks && bs.pos < real_bits && ok; b++) {
                    int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
                    int diff = read_dc_vlc(&bs);
                    dc_pred[comp] += diff;
                    blocks[b][0] = dc_pred[comp] * 8;
                    /* AC: each position coded as DC-VLC (0 = zero coeff) */
                    for (int pos = 1; pos < 64 && bs.pos < real_bits; pos++) {
                        int val = read_dc_vlc(&bs);
                        if (val != 0) {
                            blocks[b][zigzag[pos]] = val * 4;
                            nz_total++;
                        } else {
                            zero_total++;
                        }
                    }
                }
                printf("  NZ=%d, Zero=%d (%.1f%% zero)\n", nz_total, zero_total,
                       100.0*zero_total/(nz_total+zero_total));
                printf("  Bits used: %d of %d real (%.1f%%)\n",
                       bs.pos, real_bits, 100.0*bs.pos/real_bits);

                int Y[W*H];
                memset(Y, 0, sizeof(int)*W*H);
                int W2 = 256, H2 = 144;
                int mw2 = W2/16, mh2 = H2/16;
                for (int mb = 0; mb < mw2*mh2 && mb*6+3 < nblocks; mb++) {
                    int mx2 = mb%mw2, my2 = mb/mw2;
                    int out[64];
                    for (int s = 0; s < 4; s++) {
                        idct8x8(blocks[mb*6+s], out);
                        int bx=(s&1)*8, by=(s>>1)*8;
                        for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
                            int py=my2*16+by+r, px=mx2*16+bx+c;
                            if(py<H2&&px<W2) Y[py*W2+px]=clamp(out[r*8+c]+128);
                        }
                    }
                }
                for (int y=0;y<H2;y++) for (int x=0;x<W2;x++) gray[y*W2+x]=Y[y*W2+x];
                write_ppm("/tmp/diag_dcvlc_ac.ppm", gray, W, H, 1);
                double smooth = 0; int cnt = 0;
                for (int y=0;y<H;y++) for (int x=0;x<W-1;x++) {
                    smooth += abs(Y[y*W+x]-Y[y*W+x+1]); cnt++;
                }
                printf("  smoothness=%.1f → /tmp/diag_dcvlc_ac.ppm\n", smooth/cnt);
            }

            /* ===== TEST 6: Try without zigzag (raster order) ===== */
            printf("\n== Test: VLC+EOB without zigzag (raster order) ==\n");
            {
                static int blocks[900][64];
                memset(blocks, 0, sizeof(blocks));
                bitstream bs = {bsdata, total_bits, 0};
                int dc_pred[3] = {0,0,0};
                for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
                    int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
                    int diff = read_dc_vlc(&bs);
                    dc_pred[comp] += diff;
                    blocks[b][0] = dc_pred[comp] * 8;
                    int pos = 1;
                    while (pos < 64 && bs.pos < total_bits) {
                        int run, level;
                        int ret = decode_ac(&bs, &run, &level);
                        if (ret == 0) break;
                        if (ret == -1) { bs.pos++; break; }
                        pos += run;
                        if (pos >= 64) break;
                        blocks[b][pos] = level * 4; /* NO zigzag, direct position */
                        pos++;
                    }
                }
                int Y[H][W];
                memset(Y, 0, sizeof(Y));
                for (int mb = 0; mb < mw*mh && mb*6+3 < nblocks; mb++) {
                    int mx2 = mb%mw, my2 = mb/mw;
                    int out[64];
                    for (int s = 0; s < 4; s++) {
                        idct8x8(blocks[mb*6+s], out);
                        int bx=(s&1)*8, by=(s>>1)*8;
                        for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
                            int py=my2*16+by+r, px=mx2*16+bx+c;
                            if(py<H&&px<W) Y[py][px]=clamp(out[r*8+c]+128);
                        }
                    }
                }
                for (int y=0;y<H;y++) for (int x=0;x<W;x++) gray[y*W+x]=Y[y][x];
                write_ppm("/tmp/diag_vlc_nozz.ppm", gray, W, H, 1);
                double smooth = 0; int cnt = 0;
                for (int y=0;y<H;y++) for (int x=0;x<W-1;x++) {
                    smooth += abs(Y[y][x]-Y[y][x+1]); cnt++;
                }
                printf("  smoothness=%.1f → /tmp/diag_vlc_nozz.ppm\n", smooth/cnt);
            }

            free(gray);
            free(rgb);
        }
        printf("\n");
    }

    free(disc); zip_close(z);
    return 0;
}
