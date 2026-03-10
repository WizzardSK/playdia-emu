/*
 * vcodec_runlevel.c - Test run-level AC coding models
 *
 * The per-AC flag model produces UNIFORM statistics across all zigzag positions,
 * proving it's wrong. Run-level coding (like MPEG-1) would explain this because
 * positions are specified by (run, level) pairs, not sequentially.
 *
 * Models tested:
 * A: MPEG-1 style - 0=EOB, 1+run(unary)+level(DC VLC w/sign)
 * B: Simple run-level - flag: 0=EOB, 1=pair; run=unary; level=DC VLC
 * C: No EOB - just run+level pairs until 63 positions filled
 * D: MPEG-1 Table B.14 (actual MPEG-1 AC VLC table)
 * E: Flag per position BUT with different zigzag orders
 * F: JPEG-style: read DC VLC for level, if 0=EOB, else unary run before it
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

/* MPEG-1 DC luminance VLC (modified for Playdia: sizes 7,8 use shorter codes) */
static const struct { int len; uint32_t code; int size; } dc_vlc[] = {
    {3, 0b100, 0}, {2, 0b00, 1}, {2, 0b01, 2}, {3, 0b101, 3},
    {3, 0b110, 4}, {4, 0b1110, 5}, {5, 0b11110, 6},
    {6, 0b111110, 7}, {7, 0b1111110, 8},
};
#define DC_VLC_COUNT 9

/* Zigzag orders to test */
static const int zigzag_std[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

/* Row-major order */
static const int row_major[64] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
    32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,
    48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
};

/* Alternate zigzag (MPEG-2 style) */
static const int zigzag_alt[64] = {
    0, 8, 16, 24, 1, 9, 2, 10, 17, 25, 32, 40, 48, 56, 57, 49,
    41, 33, 26, 18, 3, 11, 4, 12, 19, 27, 34, 42, 50, 58, 35, 43,
    51, 59, 20, 28, 5, 13, 6, 14, 21, 29, 36, 44, 52, 60, 37, 45,
    53, 61, 22, 30, 7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63
};

/* Diagonal order */
static const int zigzag_diag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

/* --- MPEG-1 AC VLC Table B.14 (subset) --- */
/* (run, level) pairs encoded as VLC */
typedef struct { int bits; uint32_t code; int run; int level; } ac_vlc_entry;
static const ac_vlc_entry mpeg1_ac_vlc[] = {
    /* EOB */
    {2, 0b10, -1, 0},       /* EOB marker */
    /* run=0 */
    {2, 0b11, 0, 1},
    {4, 0b0110, 0, 2},
    {5, 0b01010, 0, 3},
    {7, 0b0010110, 0, 4},
    {8, 0b00100110, 0, 5},
    {8, 0b00100001, 0, 6},
    {10, 0b0000001010, 0, 7},
    /* run=1 */
    {3, 0b011, 1, 1},
    {6, 0b000110, 1, 2},
    {8, 0b00100101, 1, 3},
    {10, 0b0000001001, 1, 4},
    /* run=2 */
    {4, 0b0101, 2, 1},
    {7, 0b0010010, 2, 2},
    /* run=3 */
    {5, 0b00111, 3, 1},
    {8, 0b00100100, 3, 2},
    /* run=4 */
    {5, 0b00110, 4, 1},
    /* run=5 */
    {6, 0b000111, 5, 1},
    /* run=6 */
    {7, 0b0010111, 6, 1},
    /* run=7 */
    {7, 0b0010011, 7, 1},
    /* run=8 */
    {8, 0b00100000, 8, 1},
    /* run=9 */
    {8, 0b00100011, 9, 1},
    /* run=10 */
    {8, 0b00100010, 10, 1},
    /* run=11-14 */
    {10, 0b0000001000, 11, 1},
    {12, 0b000000010010, 12, 1},
    {12, 0b000000010011, 13, 1},
    {12, 0b000000010001, 14, 1},
    /* Escape */
    {6, 0b000001, -2, 0},   /* Escape: 6-bit run + 8-bit level follows */
};
#define MPEG1_AC_VLC_COUNT 28

/* Bitstream reader */
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

/* Read DC VLC but return unsigned magnitude (for use as AC level) */
static int read_dc_vlc_unsigned(bitstream *bs) {
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

/* Quantization table */
static const int default_qtable[16] = {10,20,14,13,18,37,22,28,15,24,15,18,18,31,17,20};

static void get_quant_matrix(int qs, const int qtab[16], int qm[64]) {
    for (int i = 0; i < 64; i++) {
        int r = i / 8, c = i % 8;
        int qi = ((r >> 1) << 2) | (c >> 1);
        qm[i] = qtab[qi] * qs;
    }
}

/* Simple IDCT */
static void idct8x8(int block[64], int out[64]) {
    double tmp[64];
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            double sum = 0;
            for (int u = 0; u < 8; u++) {
                double cu = (u == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += cu * block[i*8+u] * cos((2*j+1)*u*M_PI/16.0);
            }
            tmp[i*8+j] = sum * 0.5;
        }
    }
    for (int j = 0; j < 8; j++) {
        for (int i = 0; i < 8; i++) {
            double sum = 0;
            for (int v = 0; v < 8; v++) {
                double cv = (v == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += cv * tmp[v*8+j] * cos((2*i+1)*v*M_PI/16.0);
            }
            out[i*8+j] = (int)(sum * 0.5);
        }
    }
}

static int clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void write_ppm(const char *fn, uint8_t *rgb, int w, int h) {
    FILE *f = fopen(fn, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, w*h*3, f);
    fclose(f);
}

/* ====== Model A: EOB + unary run + DC VLC level ====== */
static void decode_model_a(bitstream *bs, int blocks[][64], int nblocks,
                           const int *zz, int qm[64], int qs) {
    for (int b = 0; b < nblocks; b++) {
        /* DC already decoded outside */
        int pos = 1; /* next zigzag position */
        while (pos < 64 && bs->pos < bs->total_bits) {
            int bit = bs_read(bs, 1);
            if (bit == 0) break; /* EOB */
            /* Read run: unary (count of 0 bits, then 1) */
            int run = 0;
            while (bs->pos < bs->total_bits) {
                int rb = bs_read(bs, 1);
                if (rb == 1) break;
                run++;
            }
            pos += run;
            if (pos >= 64) break;
            /* Read level: DC VLC (signed) */
            int level = read_dc_vlc(bs);
            blocks[b][zz[pos]] = level * qm[zz[pos]] / 8;
            pos++;
        }
    }
}

/* ====== Model B: flag=0 EOB, flag=1 → run(2bit) + level(DC VLC) ====== */
static void decode_model_b(bitstream *bs, int blocks[][64], int nblocks,
                           const int *zz, int qm[64], int qs) {
    for (int b = 0; b < nblocks; b++) {
        int pos = 1;
        while (pos < 64 && bs->pos < bs->total_bits) {
            int flag = bs_read(bs, 1);
            if (flag == 0) break; /* EOB */
            /* 2-bit run */
            int run = bs_read(bs, 2);
            pos += run;
            if (pos >= 64) break;
            int level = read_dc_vlc(bs);
            blocks[b][zz[pos]] = level * qm[zz[pos]] / 8;
            pos++;
        }
    }
}

/* ====== Model C: No EOB, run+level pairs until pos>=63 ====== */
static void decode_model_c(bitstream *bs, int blocks[][64], int nblocks,
                           const int *zz, int qm[64], int qs) {
    for (int b = 0; b < nblocks; b++) {
        int pos = 1;
        while (pos < 64 && bs->pos < bs->total_bits) {
            /* Unary run */
            int run = 0;
            while (bs->pos < bs->total_bits) {
                int rb = bs_read(bs, 1);
                if (rb == 1) break;
                run++;
                if (run > 63) break;
            }
            if (run > 62) break; /* all zeros */
            pos += run;
            if (pos >= 64) break;
            int level = read_dc_vlc(bs);
            blocks[b][zz[pos]] = level * qm[zz[pos]] / 8;
            pos++;
        }
    }
}

/* ====== Model D: MPEG-1 Table B.14 AC VLC ====== */
static int read_mpeg1_ac(bitstream *bs, int *run_out, int *level_out) {
    /* Try to match MPEG-1 AC VLC */
    for (int i = 0; i < MPEG1_AC_VLC_COUNT; i++) {
        if (bs->pos + mpeg1_ac_vlc[i].bits > bs->total_bits) continue;
        int bits = bs_peek(bs, mpeg1_ac_vlc[i].bits);
        if (bits == (int)mpeg1_ac_vlc[i].code) {
            bs->pos += mpeg1_ac_vlc[i].bits;
            if (mpeg1_ac_vlc[i].run == -1) return 0; /* EOB */
            if (mpeg1_ac_vlc[i].run == -2) {
                /* Escape: 6-bit run, 8-bit signed level */
                *run_out = bs_read(bs, 6);
                int lv = bs_read(bs, 8);
                if (lv > 127) lv -= 256;
                *level_out = lv;
                return 1;
            }
            *run_out = mpeg1_ac_vlc[i].run;
            *level_out = mpeg1_ac_vlc[i].level;
            /* Sign bit */
            int sign = bs_read(bs, 1);
            if (sign) *level_out = -(*level_out);
            return 1;
        }
    }
    /* No match - skip bit */
    bs->pos++;
    return -1;
}

static void decode_model_d(bitstream *bs, int blocks[][64], int nblocks,
                           const int *zz, int qm[64], int qs) {
    for (int b = 0; b < nblocks; b++) {
        int pos = 1;
        while (pos < 64 && bs->pos < bs->total_bits) {
            int run = 0, level = 0;
            int ret = read_mpeg1_ac(bs, &run, &level);
            if (ret == 0) break; /* EOB */
            if (ret == -1) continue; /* error, skip */
            pos += run;
            if (pos >= 64) break;
            blocks[b][zz[pos]] = level * qm[zz[pos]] / 8;
            pos++;
        }
    }
}

/* ====== Model E: Original flag model but with DIFFERENT zigzag ====== */
static void decode_model_e(bitstream *bs, int blocks[][64], int nblocks,
                           const int *zz, int qm[64], int qs) {
    for (int b = 0; b < nblocks; b++) {
        for (int k = 1; k < 64; k++) {
            if (bs->pos >= bs->total_bits) break;
            int flag = bs_read(bs, 1);
            if (flag) {
                int val = read_dc_vlc(bs);
                blocks[b][zz[k]] = val * qm[zz[k]] / 8;
            }
        }
    }
}

/* ====== Model F: JPEG-style (VLC=0 means EOB) ====== */
static void decode_model_f(bitstream *bs, int blocks[][64], int nblocks,
                           const int *zz, int qm[64], int qs) {
    for (int b = 0; b < nblocks; b++) {
        int pos = 1;
        while (pos < 64 && bs->pos < bs->total_bits) {
            /* Read 4-bit run/size like JPEG: high=run, low=size */
            int rs = bs_read(bs, 8);
            if (rs < 0) break;
            int run = (rs >> 4) & 0xF;
            int sz = rs & 0xF;
            if (rs == 0x00) break; /* EOB */
            pos += run;
            if (pos >= 64) break;
            if (sz > 0) {
                int val = bs_read(bs, sz);
                if (val < (1 << (sz - 1))) val -= (1 << sz) - 1;
                blocks[b][zz[pos]] = val * qm[zz[pos]] / 8;
            }
            pos++;
        }
    }
}

/* ====== Model G: flag + VLC but with EOB = specific VLC pattern ====== */
/* What if after flag=1, reading the DC VLC and getting size=0 means EOB? */
static void decode_model_g(bitstream *bs, int blocks[][64], int nblocks,
                           const int *zz, int qm[64], int qs) {
    for (int b = 0; b < nblocks; b++) {
        int pos = 1;
        while (pos < 64 && bs->pos < bs->total_bits) {
            int flag = bs_read(bs, 1);
            if (flag == 0) {
                pos++; /* zero coefficient */
            } else {
                /* Try reading DC VLC - if size code matches "100" (size=0), it's EOB */
                int saved_pos = bs->pos;
                /* Check for size=0 code (100) */
                int peek3 = bs_peek(bs, 3);
                if (peek3 == 0b100) {
                    bs->pos += 3;
                    break; /* EOB */
                }
                /* Otherwise read normal VLC */
                int val = read_dc_vlc(bs);
                blocks[b][zz[pos]] = val * qm[zz[pos]] / 8;
                pos++;
            }
        }
    }
}

/* ====== Model H: 2-bit code per position: 00=0, 01=+1, 10=-1, 11=VLC ======
 * But with EOB: if we see 00 followed by 00 = EOB */
/* Actually let's try: unary run of zeros + signed VLC for nonzero */
static void decode_model_h(bitstream *bs, int blocks[][64], int nblocks,
                           const int *zz, int qm[64], int qs) {
    for (int b = 0; b < nblocks; b++) {
        int pos = 1;
        while (pos < 64 && bs->pos < bs->total_bits) {
            /* Count zero flags until we hit 1 */
            int zeros = 0;
            while (pos + zeros < 64 && bs->pos < bs->total_bits) {
                int bit = bs_read(bs, 1);
                if (bit == 1) break;
                zeros++;
            }
            pos += zeros;
            if (pos >= 64) break;
            /* Sign bit */
            int sign = bs_read(bs, 1);
            /* Magnitude: unary (0s then 1) + 1 */
            int mag = 1;
            while (bs->pos < bs->total_bits) {
                int bit = bs_read(bs, 1);
                if (bit == 1) break;
                mag++;
            }
            int level = sign ? -mag : mag;
            blocks[b][zz[pos]] = level * qm[zz[pos]] / 8;
            pos++;
        }
    }
}

/* Decode DC for all blocks */
static void decode_all_dc(bitstream *bs, int blocks[][64], int nblocks, int qm[64]) {
    int dc_pred[3] = {0, 0, 0};
    int mw = W / 16; /* macroblocks per row */
    for (int b = 0; b < nblocks; b++) {
        memset(blocks[b], 0, 64 * sizeof(int));
        /* Determine component */
        int mb = b / 6;
        int sub = b % 6;
        int comp = (sub < 4) ? 0 : (sub == 4) ? 1 : 2;
        int diff = read_dc_vlc(bs);
        dc_pred[comp] += diff;
        blocks[b][0] = dc_pred[comp] * qm[0] / 8;
    }
}

/* Convert blocks to image */
static void blocks_to_image(int blocks[][64], int nblocks, uint8_t *rgb) {
    int mw = W / 16, mh = H / 16;
    int nmb = mw * mh; /* should be nblocks/6 */

    /* Allocate YCbCr planes */
    int Y[H][W], Cb[H/2][W/2], Cr[H/2][W/2];
    memset(Y, 0, sizeof(Y));
    memset(Cb, 0, sizeof(Cb));
    memset(Cr, 0, sizeof(Cr));

    for (int mb = 0; mb < nmb && mb*6+5 < nblocks; mb++) {
        int mx = mb % mw, my = mb / mw;
        int out[64];

        /* 4 Y blocks */
        for (int s = 0; s < 4; s++) {
            idct8x8(blocks[mb*6+s], out);
            int bx = (s & 1) * 8, by = (s >> 1) * 8;
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++) {
                    int py = my*16+by+r, px = mx*16+bx+c;
                    if (py < H && px < W) Y[py][px] = out[r*8+c] + 128;
                }
        }
        /* Cb */
        idct8x8(blocks[mb*6+4], out);
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++) {
                int py = my*8+r, px = mx*8+c;
                if (py < H/2 && px < W/2) Cb[py][px] = out[r*8+c] + 128;
            }
        /* Cr */
        idct8x8(blocks[mb*6+5], out);
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++) {
                int py = my*8+r, px = mx*8+c;
                if (py < H/2 && px < W/2) Cr[py][px] = out[r*8+c] + 128;
            }
    }

    /* YCbCr to RGB */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int yv = Y[y][x];
            int cb = Cb[y/2][x/2] - 128;
            int cr = Cr[y/2][x/2] - 128;
            int r = yv + 1.402*cr;
            int g = yv - 0.344136*cb - 0.714136*cr;
            int bv = yv + 1.772*cb;
            rgb[(y*W+x)*3+0] = clamp(r);
            rgb[(y*W+x)*3+1] = clamp(g);
            rgb[(y*W+x)*3+2] = clamp(bv);
        }
}

/* Print AC stats for a decoded set of blocks */
static void print_ac_stats(int blocks[][64], int nblocks) {
    /* Check Y blocks only (first 4 of each MB) */
    int count[64] = {0};
    double sum_abs[64] = {0};
    int max_abs[64] = {0};
    int nblk = 0;

    for (int b = 0; b < nblocks; b++) {
        if (b % 6 >= 4) continue; /* Y only */
        nblk++;
        for (int k = 1; k < 64; k++) {
            int v = blocks[b][k];
            if (v != 0) {
                count[k]++;
                sum_abs[k] += abs(v);
                if (abs(v) > max_abs[k]) max_abs[k] = abs(v);
            }
        }
    }

    /* Print summary: low freq vs high freq */
    int low_nz = 0, high_nz = 0;
    double low_abs = 0, high_abs = 0;
    for (int k = 1; k <= 10; k++) { low_nz += count[k]; low_abs += sum_abs[k]; }
    for (int k = 54; k <= 63; k++) { high_nz += count[k]; high_abs += sum_abs[k]; }
    printf("  Low freq (1-10): nz=%d, avg_abs=%.1f\n", low_nz, low_nz ? low_abs/low_nz : 0);
    printf("  High freq (54-63): nz=%d, avg_abs=%.1f\n", high_nz, high_nz ? high_abs/high_nz : 0);
}

int main(int argc, char **argv) {
    if (argc < 3) { printf("Usage: %s <zip> <lba>\n", argv[0]); return 1; }
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
    if (nf == 0) { printf("No frames found\n"); free(disc); zip_close(z); return 1; }

    uint8_t *f = frames[0];
    int fsize = fsizes[0];
    int qs = f[3], type = f[39];
    printf("LBA %d: qs=%d type=%d fsize=%d\n\n", start_lba, qs, type, fsize);

    /* Parse qtable from header */
    int qtable[16];
    for (int i = 0; i < 16; i++) qtable[i] = f[4 + i];
    int qm[64];
    get_quant_matrix(qs, qtable, qm);

    const uint8_t *bsdata = f + 40;
    int bslen = fsize - 40;
    int total_bits = bslen * 8;

    int mw = W / 16, mh = H / 16;
    int nmb = mw * mh;
    int nblocks = nmb * 6;

    uint8_t *rgb = malloc(W * H * 3);

    /* ====== Test each model ====== */
    typedef struct { const char *name; int model; const int *zz; } test_config;
    test_config tests[] = {
        {"A: EOB+unary_run+VLC", 0, zigzag_std},
        {"B: flag_EOB+2bit_run+VLC", 1, zigzag_std},
        {"C: no_EOB+unary_run+VLC", 2, zigzag_std},
        {"D: MPEG1_TableB14", 3, zigzag_std},
        {"E_std: flag+VLC (std zigzag)", 4, zigzag_std},
        {"E_alt: flag+VLC (alt zigzag)", 4, zigzag_alt},
        {"E_row: flag+VLC (row major)", 4, row_major},
        {"F: JPEG_style_RS", 5, zigzag_std},
        {"G: flag+VLC_EOB_on_size0", 6, zigzag_std},
        {"H: skip_zeros+sign+unary_mag", 7, zigzag_std},
    };
    int ntests = sizeof(tests) / sizeof(tests[0]);

    for (int t = 0; t < ntests; t++) {
        static int blocks[900][64];
        bitstream bsr = {bsdata, total_bits, 0};

        /* Decode DC */
        decode_all_dc(&bsr, blocks, nblocks, qm);
        int dc_bits = bsr.pos;

        /* Decode AC */
        switch (tests[t].model) {
            case 0: decode_model_a(&bsr, blocks, nblocks, tests[t].zz, qm, qs); break;
            case 1: decode_model_b(&bsr, blocks, nblocks, tests[t].zz, qm, qs); break;
            case 2: decode_model_c(&bsr, blocks, nblocks, tests[t].zz, qm, qs); break;
            case 3: decode_model_d(&bsr, blocks, nblocks, tests[t].zz, qm, qs); break;
            case 4: decode_model_e(&bsr, blocks, nblocks, tests[t].zz, qm, qs); break;
            case 5: decode_model_f(&bsr, blocks, nblocks, tests[t].zz, qm, qs); break;
            case 6: decode_model_g(&bsr, blocks, nblocks, tests[t].zz, qm, qs); break;
            case 7: decode_model_h(&bsr, blocks, nblocks, tests[t].zz, qm, qs); break;
        }

        int used_bits = bsr.pos;
        printf("%-35s bits %d/%d (%.1f%%) dc=%d\n", tests[t].name,
               used_bits, total_bits, 100.0*used_bits/total_bits, dc_bits);
        print_ac_stats(blocks, nblocks);

        /* Save image for most promising models */
        blocks_to_image(blocks, nblocks, rgb);

        /* Calculate Y range */
        int ymin=255, ymax=0;
        for (int i = 0; i < W*H*3; i += 3) {
            int y = (int)(0.299*rgb[i] + 0.587*rgb[i+1] + 0.114*rgb[i+2]);
            if (y < ymin) ymin = y;
            if (y > ymax) ymax = y;
        }
        printf("  Y range: [%d, %d]\n", ymin, ymax);

        char fn[256];
        snprintf(fn, sizeof(fn), "/tmp/rl_%c.ppm", 'a' + t);
        write_ppm(fn, rgb, W, H);
        printf("\n");
    }

    free(rgb); free(disc); zip_close(z);
    return 0;
}
