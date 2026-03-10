/*
 * vcodec_twostage.c - Two-stage AC coding hypothesis
 *
 * Stage 1: Interleaved DC + MPEG-1 VLC run-level with EOB (~25%)
 * Stage 2: Per-position refinement for remaining AC positions
 *
 * Hypothesis: the VLC codes main AC coefficients, then a second pass
 * adds detail using a simpler coding (flag+VLC or similar).
 *
 * Also test: what if after the VLC pass, remaining data is ANOTHER
 * set of blocks (like a second field or different component)?
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

static int decode_ac_eob(bitstream *bs, int *run, int *level) {
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
        int len = ac_table[i].len;
        if (bs->pos + len + 1 > bs->total_bits) continue;
        if (bs_peek(bs, len) == (int)ac_table[i].code) {
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
    FILE *f = fopen(fn, "wb"); fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, w*h*3, f); fclose(f);
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

    /* Test on multiple LBAs */
    for (int fi = 0; fi < nf && fi < 2; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        int qs = f[3], type = f[39];
        const uint8_t *bsdata = f + 40;
        int bslen = fsize - 40;
        int total_bits = bslen * 8;
        int mw = W/16, mh = H/16, nblocks = mw*mh*6;

        int qm[64];
        for (int i = 0; i < 64; i++) {
            int r2 = i/8, c2 = i%8;
            int qi = ((r2>>1)<<2)|(c2>>1);
            qm[i] = default_qtable[qi] * qs;
        }

        printf("\n===== LBA %d frame %d: qs=%d type=%d =====\n", start_lba, fi, qs, type);

        /* Stage 1: Interleaved DC + VLC with EOB */
        static int blocks[900][64];
        memset(blocks, 0, sizeof(blocks));
        bitstream bs = {bsdata, total_bits, 0};

        int dc_pred[3] = {0,0,0};
        int eob_at[900]; /* bit position where EOB fires for each block */
        int ac_coded[900]; /* bitmap of which AC positions have values */
        memset(eob_at, 0, sizeof(eob_at));
        memset(ac_coded, 0, sizeof(ac_coded));

        int nz_s1 = 0;
        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
            int diff = read_dc_vlc(&bs);
            dc_pred[comp] += diff;
            blocks[b][0] = dc_pred[comp] * 8;

            int pos = 1;
            while (pos < 64 && bs.pos < total_bits) {
                int run, level;
                int ret = decode_ac_eob(&bs, &run, &level);
                if (ret == 0) { eob_at[b] = pos; break; }
                if (ret == -1) { bs.pos++; eob_at[b] = pos; break; }
                pos += run;
                if (pos >= 64) break;
                blocks[b][zigzag[pos]] = (level * qm[zigzag[pos]] + 4) / 8;
                ac_coded[b] |= (1ULL << pos);
                nz_s1++;
                pos++;
            }
            if (eob_at[b] == 0) eob_at[b] = 64;
        }
        int stage1_end = bs.pos;
        printf("Stage 1: %d bits (%.1f%%), nz=%d, avg=%.1f/block\n",
               stage1_end, 100.0*stage1_end/total_bits, nz_s1, (double)nz_s1/nblocks);

        int remaining_bits = total_bits - stage1_end;
        printf("Remaining: %d bits (%.1f%%)\n", remaining_bits, 100.0*remaining_bits/total_bits);

        /* Examine remaining data */
        printf("First 128 remaining bits: ");
        for (int i = 0; i < 128 && stage1_end+i < total_bits; i++) {
            int bp = stage1_end + i;
            printf("%d", (bsdata[bp>>3] >> (7-(bp&7))) & 1);
            if ((i+1)%8==0) printf(" ");
        }
        printf("\n");

        /* === Stage 2 test A: flag+VLC for remaining positions === */
        {
            bitstream bs2 = {bsdata, total_bits, stage1_end};
            int nz_s2 = 0;
            for (int b = 0; b < nblocks && bs2.pos < total_bits; b++) {
                int start_pos = eob_at[b]; /* first uncoded position */
                for (int k = start_pos; k < 64 && bs2.pos < total_bits; k++) {
                    int flag = bs_read(&bs2, 1);
                    if (flag) {
                        int val = read_dc_vlc(&bs2);
                        blocks[b][zigzag[k]] += val; /* add to existing */
                        nz_s2++;
                    }
                }
            }
            printf("Stage 2A (flag+VLC remaining): %d/%d bits (%.1f%%), nz=%d\n",
                   bs2.pos, total_bits, 100.0*bs2.pos/total_bits, nz_s2);
        }

        /* === Stage 2 test B: Another round of VLC with EOB === */
        {
            bitstream bs2 = {bsdata, total_bits, stage1_end};
            int nz_s2 = 0, eobs2 = 0;
            for (int b = 0; b < nblocks && bs2.pos < total_bits; b++) {
                int pos = 1;
                while (pos < 64 && bs2.pos < total_bits) {
                    int run, level;
                    int ret = decode_ac_eob(&bs2, &run, &level);
                    if (ret == 0) { eobs2++; break; }
                    if (ret == -1) { bs2.pos++; break; }
                    pos += run;
                    if (pos >= 64) break;
                    blocks[b][zigzag[pos]] += level; /* add refinement */
                    nz_s2++;
                    pos++;
                }
            }
            printf("Stage 2B (another VLC pass): %d/%d bits (%.1f%%), nz=%d, eobs=%d\n",
                   bs2.pos, total_bits, 100.0*bs2.pos/total_bits, nz_s2, eobs2);
        }

        /* === Stage 2 test C: Repeat interleaved DC+AC for SECOND field === */
        {
            bitstream bs2 = {bsdata, total_bits, stage1_end};
            int dc_pred2[3] = {0,0,0};
            int nz_s2 = 0, eobs2 = 0, blocks2 = 0;
            for (int b = 0; b < nblocks && bs2.pos < total_bits; b++) {
                int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
                int diff = read_dc_vlc(&bs2);
                dc_pred2[comp] += diff;
                int pos = 1;
                while (pos < 64 && bs2.pos < total_bits) {
                    int run, level;
                    int ret = decode_ac_eob(&bs2, &run, &level);
                    if (ret == 0) { eobs2++; break; }
                    if (ret == -1) { bs2.pos++; break; }
                    pos += run;
                    if (pos >= 64) break;
                    nz_s2++;
                    pos++;
                }
                blocks2++;
            }
            printf("Stage 2C (second DC+AC pass): %d/%d bits (%.1f%%), blocks=%d, nz=%d, eobs=%d\n",
                   bs2.pos, total_bits, 100.0*bs2.pos/total_bits, blocks2, nz_s2, eobs2);
        }

        /* === Stage 2 test D: Fixed 1-bit per remaining position === */
        {
            int fb = stage1_end;
            for (int b = 0; b < nblocks; b++) {
                fb += (64 - eob_at[b]); /* 1 bit per uncoded position */
            }
            printf("Stage 2D (1-bit per remaining): %d bits (%.1f%%)\n",
                   fb, 100.0*fb/total_bits);
        }

        /* Distribution of eob_at positions */
        int eob_hist[65] = {0};
        for (int b = 0; b < nblocks; b++) eob_hist[eob_at[b]]++;
        printf("\nEOB position histogram (first 20):\n");
        for (int i = 1; i <= 64; i++) {
            if (eob_hist[i] > 0) printf("  pos %2d: %d blocks\n", i, eob_hist[i]);
        }

        /* EOB at position 1 means the block had ONLY DC (no ACs) */
        printf("Blocks with no ACs: %d/%d\n", eob_hist[1], nblocks);
    }

    free(disc); zip_close(z);
    return 0;
}
