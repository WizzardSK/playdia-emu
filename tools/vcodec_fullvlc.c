/*
 * vcodec_fullvlc.c - Full MPEG-1 AC VLC table (111 entries) test
 *
 * Tests:
 * A: Standard MPEG-1 with EOB='10' (full table)
 * B: No EOB: '1s'=(0,±1) for ALL coefficients, blocks end at pos 64
 * C: Interleaved DC+AC per block (not all DC first)
 * D: No EOB, but with escape code
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

/* DC VLC */
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

/* Full AC VLC table (111 entries from jpsxdec/MPEG-1) */
/* Format: {code_bits, code_len, run, level} */
/* code_bits is the code WITHOUT the sign bit */
typedef struct { uint32_t code; int len; int run; int level; } ac_entry;

static const ac_entry ac_table[] = {
    /* 2-bit codes */
    {0b11, 2, 0, 1},           /* 11s → (0,±1) */
    /* 3-bit codes */
    {0b011, 3, 1, 1},          /* 011s → (1,±1) */
    /* 4-bit codes */
    {0b0100, 4, 0, 2},         /* 0100s → (0,±2) */
    {0b0101, 4, 2, 1},         /* 0101s → (2,±1) */
    /* 5-bit codes */
    {0b00101, 5, 0, 3},        /* 00101s → (0,±3) */
    {0b00110, 5, 4, 1},        /* 00110s → (4,±1) */
    {0b00111, 5, 3, 1},        /* 00111s → (3,±1) */
    /* 6-bit codes */
    {0b000100, 6, 7, 1},       /* 000100s → (7,±1) */
    {0b000101, 6, 6, 1},       /* 000101s → (6,±1) */
    {0b000110, 6, 1, 2},       /* 000110s → (1,±2) */
    {0b000111, 6, 5, 1},       /* 000111s → (5,±1) */
    /* 7-bit codes */
    {0b0000100, 7, 2, 2},      /* 0000100s → (2,±2) */
    {0b0000101, 7, 9, 1},      /* 0000101s → (9,±1) */
    {0b0000110, 7, 0, 4},      /* 0000110s → (0,±4) */
    {0b0000111, 7, 8, 1},      /* 0000111s → (8,±1) */
    /* 8-bit codes */
    {0b00100000, 8, 13, 1},
    {0b00100001, 8, 0, 6},
    {0b00100010, 8, 12, 1},
    {0b00100011, 8, 11, 1},
    {0b00100100, 8, 3, 2},
    {0b00100101, 8, 1, 3},
    {0b00100110, 8, 0, 5},
    {0b00100111, 8, 10, 1},
    /* 10-bit codes */
    {0b0000001000, 10, 16, 1},
    {0b0000001001, 10, 5, 2},
    {0b0000001010, 10, 0, 7},
    {0b0000001011, 10, 2, 3},
    {0b0000001100, 10, 1, 4},
    {0b0000001101, 10, 15, 1},
    {0b0000001110, 10, 14, 1},
    {0b0000001111, 10, 4, 2},
    /* 12-bit codes */
    {0b000000010000, 12, 0, 11},
    {0b000000010001, 12, 8, 2},
    {0b000000010010, 12, 4, 3},
    {0b000000010011, 12, 0, 10},
    {0b000000010100, 12, 2, 4},
    {0b000000010101, 12, 7, 2},
    {0b000000010110, 12, 21, 1},
    {0b000000010111, 12, 20, 1},
    {0b000000011000, 12, 0, 9},
    {0b000000011001, 12, 19, 1},
    {0b000000011010, 12, 18, 1},
    {0b000000011011, 12, 1, 5},
    {0b000000011100, 12, 3, 3},
    {0b000000011101, 12, 0, 8},
    {0b000000011110, 12, 6, 2},
    {0b000000011111, 12, 17, 1},
    /* 13-bit codes */
    {0b0000000010000, 13, 10, 2},
    {0b0000000010001, 13, 9, 2},
    {0b0000000010010, 13, 5, 3},
    {0b0000000010011, 13, 3, 4},
    {0b0000000010100, 13, 2, 5},
    {0b0000000010101, 13, 1, 7},
    {0b0000000010110, 13, 1, 6},
    {0b0000000010111, 13, 0, 15},
    {0b0000000011000, 13, 0, 14},
    {0b0000000011001, 13, 0, 13},
    {0b0000000011010, 13, 0, 12},
    {0b0000000011011, 13, 26, 1},
    {0b0000000011100, 13, 25, 1},
    {0b0000000011101, 13, 24, 1},
    {0b0000000011110, 13, 23, 1},
    {0b0000000011111, 13, 22, 1},
    /* 14-bit codes */
    {0b00000000010000, 14, 0, 31},
    {0b00000000010001, 14, 0, 30},
    {0b00000000010010, 14, 0, 29},
    {0b00000000010011, 14, 0, 28},
    {0b00000000010100, 14, 0, 27},
    {0b00000000010101, 14, 0, 26},
    {0b00000000010110, 14, 0, 25},
    {0b00000000010111, 14, 0, 24},
    {0b00000000011000, 14, 0, 23},
    {0b00000000011001, 14, 0, 22},
    {0b00000000011010, 14, 0, 21},
    {0b00000000011011, 14, 0, 20},
    {0b00000000011100, 14, 0, 19},
    {0b00000000011101, 14, 0, 18},
    {0b00000000011110, 14, 0, 17},
    {0b00000000011111, 14, 0, 16},
    /* 15-bit codes */
    {0b000000000010000, 15, 0, 40},
    {0b000000000010001, 15, 0, 39},
    {0b000000000010010, 15, 0, 38},
    {0b000000000010011, 15, 0, 37},
    {0b000000000010100, 15, 0, 36},
    {0b000000000010101, 15, 0, 35},
    {0b000000000010110, 15, 0, 34},
    {0b000000000010111, 15, 0, 33},
    {0b000000000011000, 15, 0, 32},
    {0b000000000011001, 15, 1, 14},
    {0b000000000011010, 15, 1, 13},
    {0b000000000011011, 15, 1, 12},
    {0b000000000011100, 15, 1, 11},
    {0b000000000011101, 15, 1, 10},
    {0b000000000011110, 15, 1, 9},
    {0b000000000011111, 15, 1, 8},
    /* 16-bit codes */
    {0b0000000000010000, 16, 1, 18},
    {0b0000000000010001, 16, 1, 17},
    {0b0000000000010010, 16, 1, 16},
    {0b0000000000010011, 16, 1, 15},
    {0b0000000000010100, 16, 6, 3},
    {0b0000000000010101, 16, 16, 2},
    {0b0000000000010110, 16, 15, 2},
    {0b0000000000010111, 16, 14, 2},
    {0b0000000000011000, 16, 13, 2},
    {0b0000000000011001, 16, 12, 2},
    {0b0000000000011010, 16, 11, 2},
    {0b0000000000011011, 16, 31, 1},
    {0b0000000000011100, 16, 30, 1},
    {0b0000000000011101, 16, 29, 1},
    {0b0000000000011110, 16, 28, 1},
    {0b0000000000011111, 16, 27, 1},
};
#define AC_TABLE_SIZE 111

/* Decode one AC coefficient using the full table */
/* Returns: 1=got run/level, 0=EOB, -1=error */
static int decode_ac_std(bitstream *bs, int *run, int *level) {
    if (bs->pos + 2 > bs->total_bits) return -1;

    /* Check EOB first: '10' */
    if (bs_peek(bs, 2) == 0b10) {
        bs->pos += 2;
        return 0; /* EOB */
    }

    /* Check escape: '000001' */
    if (bs->pos + 6 <= bs->total_bits && bs_peek(bs, 6) == 0b000001) {
        bs->pos += 6;
        *run = bs_read(bs, 6);
        int raw = bs_read(bs, 10);
        *level = (raw >= 512) ? raw - 1024 : raw;
        return 1;
    }

    /* Search table (longest match first for efficiency, but codes are prefix-free) */
    for (int i = 0; i < AC_TABLE_SIZE; i++) {
        int len = ac_table[i].len;
        if (bs->pos + len + 1 > bs->total_bits) continue;
        int bits = bs_peek(bs, len);
        if (bits == (int)ac_table[i].code) {
            bs->pos += len;
            int sign = bs_read(bs, 1);
            *run = ac_table[i].run;
            *level = ac_table[i].level;
            if (sign) *level = -(*level);
            return 1;
        }
    }
    return -1; /* no match */
}

/* Decode AC without EOB: '1s'=(0,±1) replaces '10'=EOB */
static int decode_ac_noeob(bitstream *bs, int *run, int *level) {
    if (bs->pos + 2 > bs->total_bits) return -1;

    /* '1' followed by sign bit = (0, ±1) */
    int peek1 = bs_peek(bs, 1);
    if (peek1 == 1) {
        bs->pos += 1; /* consume '1' */
        int sign = bs_read(bs, 1);
        *run = 0;
        *level = sign ? -1 : 1;
        return 1;
    }

    /* Check escape: '000001' */
    if (bs->pos + 6 <= bs->total_bits && bs_peek(bs, 6) == 0b000001) {
        bs->pos += 6;
        *run = bs_read(bs, 6);
        int raw = bs_read(bs, 10);
        *level = (raw >= 512) ? raw - 1024 : raw;
        return 1;
    }

    /* Search table for codes starting with '0' */
    for (int i = 1; i < AC_TABLE_SIZE; i++) { /* skip first entry (0,1) since handled above */
        int len = ac_table[i].len;
        if (bs->pos + len + 1 > bs->total_bits) continue;
        int bits = bs_peek(bs, len);
        if (bits == (int)ac_table[i].code) {
            bs->pos += len;
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

static const int default_qtable[16] = {10,20,14,13,18,37,22,28,15,24,15,18,18,31,17,20};

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

static void write_ppm(const char *fn, uint8_t *rgb, int w, int h) {
    FILE *f = fopen(fn, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, w*h*3, f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) return 1;
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
    if (nf == 0) { printf("No frames\n"); return 1; }

    uint8_t *f = frames[0];
    int fsize = fsizes[0];
    int qs = f[3], type = f[39];
    printf("LBA %d: qs=%d type=%d fsize=%d\n", start_lba, qs, type, fsize);

    const uint8_t *bsdata = f + 40;
    int bslen = fsize - 40;
    int total_bits = bslen * 8;
    int mw = W/16, mh = H/16;
    int nblocks = mw*mh*6;

    int qm[64];
    for (int i = 0; i < 64; i++) {
        int r = i/8, c = i%8;
        int qi = ((r>>1)<<2)|(c>>1);
        qm[i] = default_qtable[qi] * qs;
    }

    uint8_t *rgb = malloc(W*H*3);

    /* === Model A: All DC first, then MPEG-1 AC VLC with EOB === */
    printf("\n=== Model A: All DC first + MPEG-1 AC VLC (EOB='10') ===\n");
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
        int dc_end = bs.pos;

        int eob_count = 0, nz_count = 0, err_count = 0;
        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int pos = 1;
            while (pos < 64 && bs.pos < total_bits) {
                int run, level;
                int ret = decode_ac_std(&bs, &run, &level);
                if (ret == 0) { eob_count++; break; }
                if (ret == -1) { err_count++; bs.pos++; break; }
                pos += run;
                if (pos >= 64) break;
                blocks[b][zigzag[pos]] = (level * qm[zigzag[pos]] + 4) / 8;
                nz_count++;
                pos++;
            }
        }
        printf("  dc=%d bits, total=%d/%d (%.1f%%), eobs=%d, nz=%d, errors=%d\n",
               dc_end, bs.pos, total_bits, 100.0*bs.pos/total_bits,
               eob_count, nz_count, err_count);
    }

    /* === Model B: All DC first + NO EOB VLC ('1s'=(0,±1)) === */
    printf("\n=== Model B: All DC first + NO EOB VLC ('1s'=(0,±1)) ===\n");
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
        int dc_end = bs.pos;

        int nz_count = 0, err_count = 0;
        int low_nz = 0, high_nz = 0;
        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int pos = 1;
            while (pos < 64 && bs.pos < total_bits) {
                int run, level;
                int ret = decode_ac_noeob(&bs, &run, &level);
                if (ret == -1) { err_count++; bs.pos++; continue; }
                pos += run;
                if (pos >= 64) break;
                blocks[b][zigzag[pos]] = (level * qm[zigzag[pos]] + 4) / 8;
                nz_count++;
                if (pos <= 10) low_nz++;
                if (pos >= 50) high_nz++;
                pos++;
            }
        }
        printf("  dc=%d bits, total=%d/%d (%.1f%%), nz=%d, errors=%d\n",
               dc_end, bs.pos, total_bits, 100.0*bs.pos/total_bits,
               nz_count, err_count);
        printf("  low_nz(1-10)=%d, high_nz(50-63)=%d\n", low_nz, high_nz);
    }

    /* === Model C: INTERLEAVED DC+AC per block (MPEG-1 style) === */
    printf("\n=== Model C: Interleaved DC+AC (MPEG-1 style with EOB) ===\n");
    {
        static int blocks[900][64];
        memset(blocks, 0, sizeof(blocks));
        bitstream bs = {bsdata, total_bits, 0};

        int dc_pred[3] = {0,0,0};
        int eob_count = 0, nz_count = 0, err_count = 0;
        int blocks_done = 0;

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
            /* DC */
            int diff = read_dc_vlc(&bs);
            dc_pred[comp] += diff;
            blocks[b][0] = dc_pred[comp] * 8;
            /* AC */
            int pos = 1;
            while (pos < 64 && bs.pos < total_bits) {
                int run, level;
                int ret = decode_ac_std(&bs, &run, &level);
                if (ret == 0) { eob_count++; break; }
                if (ret == -1) { err_count++; bs.pos++; break; }
                pos += run;
                if (pos >= 64) break;
                blocks[b][zigzag[pos]] = (level * qm[zigzag[pos]] + 4) / 8;
                nz_count++;
                pos++;
            }
            blocks_done++;
        }
        printf("  blocks=%d total=%d/%d (%.1f%%), eobs=%d, nz=%d, errors=%d\n",
               blocks_done, bs.pos, total_bits, 100.0*bs.pos/total_bits,
               eob_count, nz_count, err_count);
        printf("  avg AC pairs/block: %.1f\n",
               blocks_done > 0 ? (double)nz_count/blocks_done : 0);
    }

    /* === Model D: Interleaved DC+AC, NO EOB === */
    printf("\n=== Model D: Interleaved DC+AC, NO EOB ===\n");
    {
        static int blocks[900][64];
        memset(blocks, 0, sizeof(blocks));
        bitstream bs = {bsdata, total_bits, 0};

        int dc_pred[3] = {0,0,0};
        int nz_count = 0, err_count = 0;
        int blocks_done = 0;
        int low_nz = 0, high_nz = 0;

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
            int diff = read_dc_vlc(&bs);
            dc_pred[comp] += diff;
            blocks[b][0] = dc_pred[comp] * 8;

            int pos = 1;
            while (pos < 64 && bs.pos < total_bits) {
                int run, level;
                int ret = decode_ac_noeob(&bs, &run, &level);
                if (ret == -1) { err_count++; bs.pos++; continue; }
                pos += run;
                if (pos >= 64) break;
                blocks[b][zigzag[pos]] = (level * qm[zigzag[pos]] + 4) / 8;
                nz_count++;
                if (pos <= 10) low_nz++;
                if (pos >= 50) high_nz++;
                pos++;
            }
            blocks_done++;
        }
        printf("  blocks=%d total=%d/%d (%.1f%%), nz=%d, errors=%d\n",
               blocks_done, bs.pos, total_bits, 100.0*bs.pos/total_bits,
               nz_count, err_count);
        printf("  avg AC/block: %.1f, low_nz=%d, high_nz=%d\n",
               blocks_done > 0 ? (double)nz_count/blocks_done : 0, low_nz, high_nz);

        /* Build image */
        int Yp[H][W];
        memset(Yp, 0, sizeof(Yp));
        for (int mb = 0; mb < mw*mh && mb*6+3 < blocks_done; mb++) {
            int mx = mb%mw, my = mb/mw;
            int out[64];
            for (int s = 0; s < 4; s++) {
                idct8x8(blocks[mb*6+s], out);
                int bx=(s&1)*8, by=(s>>1)*8;
                for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
                    int py=my*16+by+r, px=mx*16+bx+c;
                    if(py<H&&px<W) Yp[py][px]=clamp(out[r*8+c]+128);
                }
            }
        }
        /* Smoothness */
        double smooth = 0; int cnt = 0;
        for (int y=0;y<H;y++) for (int x=0;x<W-1;x++) { smooth+=abs(Yp[y][x]-Yp[y][x+1]); cnt++; }
        printf("  smoothness: %.1f\n", smooth/cnt);
    }

    /* === Model E: Interleaved DC+AC with EOB + output image === */
    printf("\n=== Model E: Interleaved DC+AC (EOB) + image output ===\n");
    {
        static int blocks[900][64];
        memset(blocks, 0, sizeof(blocks));
        bitstream bs = {bsdata, total_bits, 0};

        int dc_pred[3] = {0,0,0};
        int blocks_done = 0;

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
            int diff = read_dc_vlc(&bs);
            dc_pred[comp] += diff;
            blocks[b][0] = dc_pred[comp] * 8;

            int pos = 1;
            while (pos < 64 && bs.pos < total_bits) {
                int run, level;
                int ret = decode_ac_std(&bs, &run, &level);
                if (ret == 0) break; /* EOB */
                if (ret == -1) { bs.pos++; break; }
                pos += run;
                if (pos >= 64) break;
                blocks[b][zigzag[pos]] = (level * qm[zigzag[pos]] + 4) / 8;
                pos++;
            }
            blocks_done++;
        }
        printf("  blocks=%d bits=%d/%d (%.1f%%)\n",
               blocks_done, bs.pos, total_bits, 100.0*bs.pos/total_bits);

        /* Build RGB image */
        int Yp[H][W], Cb[H/2][W/2], Cr[H/2][W/2];
        memset(Yp,0,sizeof(Yp)); memset(Cb,0,sizeof(Cb)); memset(Cr,0,sizeof(Cr));
        for (int mb = 0; mb < mw*mh && mb*6+5 < blocks_done; mb++) {
            int mx = mb%mw, my = mb/mw;
            int out[64];
            for (int s = 0; s < 4; s++) {
                idct8x8(blocks[mb*6+s], out);
                int bx=(s&1)*8, by=(s>>1)*8;
                for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
                    int py=my*16+by+r, px=mx*16+bx+c;
                    if(py<H&&px<W) Yp[py][px]=out[r*8+c]+128;
                }
            }
            idct8x8(blocks[mb*6+4], out);
            for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
                int py=my*8+r, px=mx*8+c;
                if(py<H/2&&px<W/2) Cb[py][px]=out[r*8+c]+128;
            }
            idct8x8(blocks[mb*6+5], out);
            for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
                int py=my*8+r, px=mx*8+c;
                if(py<H/2&&px<W/2) Cr[py][px]=out[r*8+c]+128;
            }
        }
        for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
            int yv=Yp[y][x], cb=Cb[y/2][x/2]-128, cr=Cr[y/2][x/2]-128;
            rgb[(y*W+x)*3+0]=clamp(yv+1.402*cr);
            rgb[(y*W+x)*3+1]=clamp(yv-0.344136*cb-0.714136*cr);
            rgb[(y*W+x)*3+2]=clamp(yv+1.772*cb);
        }
        write_ppm("/tmp/vlc_interleaved_eob.ppm", rgb, W, H);

        double smooth = 0; int cnt = 0;
        for (int y=0;y<H;y++) for (int x=0;x<W-1;x++) {
            int yv = clamp(Yp[y][x]);
            int yv2 = clamp(Yp[y][x+1]);
            smooth += abs(yv-yv2); cnt++;
        }
        printf("  smoothness: %.1f\n", smooth/cnt);
    }

    free(rgb); free(disc); zip_close(z);
    return 0;
}
