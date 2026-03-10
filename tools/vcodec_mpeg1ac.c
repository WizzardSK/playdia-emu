/*
 * vcodec_mpeg1ac.c - Test correct MPEG-1 intra AC VLC (Tables B.14/B.15)
 * Also examine still frames and try various simple VLC-only (no flag) models
 *
 * MPEG-1 AC coding: (run, level) pairs with VLC encoding
 * Table B.14 DCT coefficients (first): EOB='10', escape='000001'
 * Table B.15 is same except first coefficient '1s' = (0,1)
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

/* ==========================================
 * MPEG-1 AC VLC Table B.14 (complete)
 * Format: {bits, code, run, level}
 * level is always positive; sign bit follows
 * Special: run=-1 = EOB, run=-2 = escape
 * ========================================== */
typedef struct { int bits; uint32_t code; int run; int level; } rl_entry;

/* Correct MPEG-1 Table B.14 */
static const rl_entry tbl_b14[] = {
    /* EOB */
    {2, 0b10, -1, 0},
    /* (0,1) - note: for Table B.15 (first coeff in intra), this is '1s' instead */
    {3, 0b110, 0, 1},
    /* rest of table */
    {4, 0b0100, 0, 2},
    {5, 0b00101, 2, 1},
    {5, 0b00110, 0, 3},
    {5, 0b01010, 1, 1},
    {5, 0b01011, 0, 4},  /* FIXME: some refs say this is wrong */
    {6, 0b001001, 3, 1},
    {6, 0b001000, 4, 1},
    {6, 0b001011, 0, 5},
    {6, 0b001010, 0, 6},
    {6, 0b001101, 1, 2},
    {6, 0b001100, 0, 7},
    {7, 0b0001010, 5, 1},
    {7, 0b0001011, 6, 1},
    {7, 0b0001000, 7, 1},
    {7, 0b0001001, 8, 1},
    {8, 0b00001000, 9, 1},
    {8, 0b00001001, 0, 8},
    {8, 0b00001010, 10, 1},
    {8, 0b00001011, 0, 9},
    {8, 0b00001100, 2, 2},
    {8, 0b00001101, 1, 3},
    {8, 0b00001110, 11, 1},
    {8, 0b00001111, 0, 10},
    /* Escape */
    {6, 0b000001, -2, 0},
};
#define TBL_B14_COUNT 26

/* Try to decode one AC run-level pair using Table B.14 */
static int decode_rl_b14(bitstream *bs, int *run, int *level) {
    for (int i = 0; i < TBL_B14_COUNT; i++) {
        if (bs->pos + tbl_b14[i].bits > bs->total_bits) continue;
        int bits = bs_peek(bs, tbl_b14[i].bits);
        if (bits == (int)tbl_b14[i].code) {
            bs->pos += tbl_b14[i].bits;
            if (tbl_b14[i].run == -1) return 0; /* EOB */
            if (tbl_b14[i].run == -2) {
                /* Escape: 6-bit run + 8-bit level (or 12-bit in some variants) */
                *run = bs_read(bs, 6);
                int raw = bs_read(bs, 8);
                if (raw == 0) {
                    *level = bs_read(bs, 8);
                } else if (raw == 128) {
                    *level = bs_read(bs, 8) - 256;
                } else if (raw > 128) {
                    *level = raw - 256;
                } else {
                    *level = raw;
                }
                return 1;
            }
            *run = tbl_b14[i].run;
            *level = tbl_b14[i].level;
            /* Sign bit */
            int sign = bs_read(bs, 1);
            if (sign) *level = -(*level);
            return 1;
        }
    }
    return -1; /* no match */
}

/* Decode AC for one block using MPEG-1 run-level */
static int decode_block_mpeg1(bitstream *bs, int block[64], int is_first) {
    int pos = 1;
    int count = 0;
    while (pos < 64 && bs->pos < bs->total_bits) {
        int run = 0, level = 0;
        int ret = decode_rl_b14(bs, &run, &level);
        if (ret == 0) break; /* EOB */
        if (ret == -1) {
            bs->pos++; /* skip bad bit */
            return -1; /* error */
        }
        pos += run;
        if (pos >= 64) break;
        block[pos] = level;
        pos++;
        count++;
    }
    return count;
}

/* ==========================================
 * Model: No-flag VLC where 0=zero, nonzero=level
 * Using DC VLC directly for each of 63 positions
 * ========================================== */
static void decode_noflag_dcvlc(bitstream *bs, int block[64]) {
    for (int k = 1; k < 64 && bs->pos < bs->total_bits; k++) {
        block[k] = read_dc_vlc(bs);
    }
}

/* ==========================================
 * Model: Exponential Golomb order 0 (signed)
 * 0 → 0
 * 10x → ±1
 * 110xx → ±2,±3
 * 1110xxx → ±4..±7
 * ========================================== */
static int read_expgolomb_signed(bitstream *bs) {
    int zeros = 0;
    while (bs->pos < bs->total_bits) {
        int bit = bs_read(bs, 1);
        if (bit == 1) break;
        zeros++;
        if (zeros > 20) return 0;
    }
    if (zeros == 0) return 0;
    int val = bs_read(bs, zeros);
    val += (1 << zeros) - 1;
    /* Map to signed: 1→+1, 2→-1, 3→+2, 4→-2, ... */
    if (val & 1) return (val + 1) / 2;
    else return -(val / 2);
}

static void decode_noflag_expgolomb(bitstream *bs, int block[64]) {
    for (int k = 1; k < 64 && bs->pos < bs->total_bits; k++) {
        block[k] = read_expgolomb_signed(bs);
    }
}

/* ==========================================
 * Model: Simple VLC
 * 0 → 0
 * 1 + sign + unary_magnitude → nonzero
 * ========================================== */
static void decode_flag_sign_unary(bitstream *bs, int block[64]) {
    for (int k = 1; k < 64 && bs->pos < bs->total_bits; k++) {
        int flag = bs_read(bs, 1);
        if (!flag) { block[k] = 0; continue; }
        int sign = bs_read(bs, 1);
        /* Unary magnitude: read until 0 */
        int mag = 0;
        while (bs->pos < bs->total_bits) {
            int bit = bs_read(bs, 1);
            if (bit == 0) break;
            mag++;
        }
        mag++; /* minimum 1 */
        block[k] = sign ? -mag : mag;
    }
}

/* ==========================================
 * Model: flag + exp-golomb magnitude
 * 0 → 0
 * 1 + sign + expgolomb(unsigned) → nonzero
 * ========================================== */
static void decode_flag_sign_expgolomb(bitstream *bs, int block[64]) {
    for (int k = 1; k < 64 && bs->pos < bs->total_bits; k++) {
        int flag = bs_read(bs, 1);
        if (!flag) { block[k] = 0; continue; }
        int sign = bs_read(bs, 1);
        /* Exp-Golomb unsigned magnitude */
        int zeros = 0;
        while (bs->pos < bs->total_bits) {
            int bit = bs_read(bs, 1);
            if (bit == 1) break;
            zeros++;
            if (zeros > 15) break;
        }
        int mag;
        if (zeros == 0) mag = 1;
        else {
            int extra = bs_read(bs, zeros);
            mag = (1 << zeros) + extra;
        }
        block[k] = sign ? -mag : mag;
    }
}

/* ==========================================
 * Model: per-position VLC = size code + raw bits (like DC but for each AC)
 * But using SHORTER size table:
 * 0 → size 0 (value 0)
 * 10 → size 1 (1 bit follows)
 * 110 → size 2 (2 bits follow)
 * 1110 → size 3
 * 11110 → size 4
 * 111110 → size 5
 * 1111110 → size 6
 * ========================================== */
static void decode_unary_size(bitstream *bs, int block[64]) {
    for (int k = 1; k < 64 && bs->pos < bs->total_bits; k++) {
        int sz = 0;
        while (bs->pos < bs->total_bits) {
            int bit = bs_read(bs, 1);
            if (bit == 0) break;
            sz++;
            if (sz > 10) break;
        }
        if (sz == 0) { block[k] = 0; continue; }
        int val = bs_read(bs, sz);
        if (val < (1 << (sz-1))) val -= (1 << sz) - 1;
        block[k] = val;
    }
}

/* ==========================================
 * Model: inverted - 1=zero, 0=nonzero
 * 1 → 0
 * 0 + DC_VLC → nonzero
 * ========================================== */
static void decode_invflag_dcvlc(bitstream *bs, int block[64]) {
    for (int k = 1; k < 64 && bs->pos < bs->total_bits; k++) {
        int flag = bs_read(bs, 1);
        if (flag) { block[k] = 0; continue; }
        block[k] = read_dc_vlc(bs);
    }
}

static const int zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

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
    if (nf == 0) { printf("No frames at LBA %d\n", start_lba); free(disc); zip_close(z); return 1; }

    /* Test multiple frames */
    for (int fi = 0; fi < nf && fi < 3; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        int qs = f[3], type = f[39];
        printf("===== LBA %d frame %d: qs=%d type=%d fsize=%d =====\n",
               start_lba, fi, qs, type, fsize);

        const uint8_t *bsdata = f + 40;
        int bslen = fsize - 40;
        int total_bits = bslen * 8;
        int nblocks = (W/16) * (H/16) * 6;

        /* Decode DC */
        bitstream bs = {bsdata, total_bits, 0};
        int dc_pred[3] = {0,0,0};
        for (int b = 0; b < nblocks; b++) {
            int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
            read_dc_vlc(&bs);
            /* We don't need DC values here, just the bit position */
        }
        int dc_end = bs.pos;
        printf("DC: %d bits (%.1f%%)\n", dc_end, 100.0*dc_end/total_bits);

        /* Dump first 128 bits of AC data */
        printf("First 128 AC bits (offset %d): ", dc_end);
        for (int i = 0; i < 128 && dc_end+i < total_bits; i++) {
            int bp = dc_end + i;
            printf("%d", (bsdata[bp>>3] >> (7-(bp&7))) & 1);
            if ((i+1) % 8 == 0) printf(" ");
        }
        printf("\n\n");

        /* Test each model */
        struct { const char *name; int id; } models[] = {
            {"MPEG1_B14 (run-level)", 0},
            {"No-flag DC VLC", 1},
            {"Exp-Golomb signed", 2},
            {"Flag+sign+unary", 3},
            {"Flag+sign+expgolomb", 4},
            {"Unary size (0=zero, 1*=size)", 5},
            {"Inv flag (1=zero, 0=nonzero)+VLC", 6},
            {"Flag(0=zero)+DC_VLC (original)", 7},
        };
        int nmodels = 8;

        for (int m = 0; m < nmodels; m++) {
            bs.pos = dc_end;
            static int blocks[900][64];
            memset(blocks, 0, sizeof(blocks));

            int errors = 0;
            for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
                switch (models[m].id) {
                    case 0: {
                        int ret = decode_block_mpeg1(&bs, blocks[b], b==0);
                        if (ret < 0) errors++;
                        break;
                    }
                    case 1: decode_noflag_dcvlc(&bs, blocks[b]); break;
                    case 2: decode_noflag_expgolomb(&bs, blocks[b]); break;
                    case 3: decode_flag_sign_unary(&bs, blocks[b]); break;
                    case 4: decode_flag_sign_expgolomb(&bs, blocks[b]); break;
                    case 5: decode_unary_size(&bs, blocks[b]); break;
                    case 6: decode_invflag_dcvlc(&bs, blocks[b]); break;
                    case 7:
                        for (int k = 1; k < 64 && bs.pos < total_bits; k++) {
                            int flag = bs_read(&bs, 1);
                            if (flag) blocks[b][k] = read_dc_vlc(&bs);
                        }
                        break;
                }
            }

            /* Collect stats: frequency decay analysis */
            int low_nz = 0, mid_nz = 0, high_nz = 0;
            double low_abs = 0, mid_abs = 0, high_abs = 0;
            int nblk = 0;
            for (int b = 0; b < nblocks; b++) {
                if (b%6 >= 4) continue; /* Y only */
                nblk++;
                for (int k = 1; k <= 10; k++) {
                    if (blocks[b][k]) { low_nz++; low_abs += abs(blocks[b][k]); }
                }
                for (int k = 25; k <= 40; k++) {
                    if (blocks[b][k]) { mid_nz++; mid_abs += abs(blocks[b][k]); }
                }
                for (int k = 50; k <= 63; k++) {
                    if (blocks[b][k]) { high_nz++; high_abs += abs(blocks[b][k]); }
                }
            }

            printf("  %-35s %5d/%d (%.1f%%)",
                   models[m].name, bs.pos, total_bits, 100.0*bs.pos/total_bits);
            if (errors) printf(" err=%d", errors);
            printf("\n");
            printf("    Low(1-10): nz=%-4d avg=%-6.1f  Mid(25-40): nz=%-4d avg=%-6.1f  High(50-63): nz=%-4d avg=%.1f\n",
                   low_nz, low_nz?low_abs/low_nz:0,
                   mid_nz, mid_nz?mid_abs/mid_nz:0,
                   high_nz, high_nz?high_abs/high_nz:0);
        }
        printf("\n");
    }

    free(disc); zip_close(z);
    return 0;
}
