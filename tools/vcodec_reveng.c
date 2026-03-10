/*
 * vcodec_reveng.c - Reverse engineer VLC from bit-run characteristics
 *
 * Key observation: AC bitstream has max run = 12 and anomalous
 * excess at r6 and r12. This is a coding fingerprint.
 *
 * Approach:
 * 1. Look for the most common short bit patterns after DC
 * 2. Try unary-based coding with max 12 bits
 * 3. Try truncated codes
 * 4. Use padded frame to find the EOB code
 * 5. Statistical analysis of bit transitions
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

typedef struct { const uint8_t *data; int total_bits; int pos; } bitstream;
static int bs_read(bitstream *bs, int n) {
    if (n <= 0 || bs->pos + n > bs->total_bits) return -1;
    int v = 0;
    for (int i = 0; i < n; i++) {
        int bp = bs->pos + i;
        v = (v << 1) | ((bs->data[bp >> 3] >> (7 - (bp & 7))) & 1);
    }
    bs->pos += n; return v;
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
static const struct { int len; uint32_t code; int size; } dc_vlc[] = {
    {3, 0b100, 0}, {2, 0b00, 1}, {2, 0b01, 2}, {3, 0b101, 3},
    {3, 0b110, 4}, {4, 0b1110, 5}, {5, 0b11110, 6},
    {6, 0b111110, 7}, {7, 0b1111110, 8},
};
static int read_dc(bitstream *bs) {
    for (int i = 0; i < 9; i++) {
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
    bs->pos += 2; return 0;
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

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    int start_lba = atoi(argv[2]);

    int err; zip_t *z = zip_open(argv[1], ZIP_RDONLY, &err); if (!z) return 1;
    int bi=-1; zip_uint64_t bsz=0;
    for(int i=0;i<(int)zip_get_num_entries(z,0);i++){
        zip_stat_t st;if(zip_stat_index(z,i,0,&st)==0&&st.size>bsz){bsz=st.size;bi=i;}}
    zip_stat_t st; zip_stat_index(z,bi,0,&st);
    zip_file_t *zf=zip_fopen_index(z,bi,0);
    uint8_t *disc=malloc(st.size);
    zip_int64_t rd=0;
    while(rd<(zip_int64_t)st.size){zip_int64_t r=zip_fread(zf,disc+rd,st.size-rd);if(r<=0)break;rd+=r;}
    zip_fclose(zf);
    int tsec=(int)(st.size/SECTOR_RAW);

    static uint8_t frames[32][MAX_FRAME]; int fsizes[32];
    int nf=assemble_frames(disc,tsec,start_lba,frames,fsizes,32);
    if(nf==0){printf("No frames\n");return 1;}

    int mw=W/16, mh=H/16, nblocks=mw*mh*6;

    /* Find the best padded frame */
    int pad_fi = -1, max_pad = 0;
    for (int fi = 0; fi < nf; fi++) {
        uint8_t *ff = frames[fi];
        int fbl = fsizes[fi] - 40;
        const uint8_t *fbd = ff + 40;
        int pad = 0;
        for (int i = fbl-1; i >= 0; i--)
            if (fbd[i] == 0xFF) pad++; else break;
        if (pad > max_pad) { max_pad = pad; pad_fi = fi; }
    }

    printf("Using padded frame %d (pad=%d bytes)\n\n", pad_fi, max_pad);

    /* Analyze BOTH a padded and non-padded frame */
    for (int pass = 0; pass < 2; pass++) {
        int fi = (pass == 0) ? pad_fi : 0;
        if (fi < 0) continue;
        uint8_t *ff = frames[fi];
        int fsize = fsizes[fi], fqs = ff[3];
        const uint8_t *bsdata = ff + 40;
        int bslen = fsize - 40;

        /* Find data end */
        int data_end_byte = bslen;
        for (int i = bslen-1; i >= 0; i--)
            if (bsdata[i] != 0xFF) { data_end_byte = i+1; break; }

        bitstream bs = {bsdata, data_end_byte*8, 0};
        int dp[3]={0,0,0};
        for(int b=0;b<nblocks&&bs.pos<data_end_byte*8;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dp[comp]+=read_dc(&bs);
        }
        int dc_end = bs.pos;
        int ac_start_byte = (dc_end + 7) / 8;
        int ac_bits = data_end_byte * 8 - dc_end;

        printf("=== Frame %d (qs=%d, pass=%d) ===\n", fi, fqs, pass);
        printf("  Data: %d bytes, DC=%d bits, AC=%d bits\n", data_end_byte, dc_end, ac_bits);

        /* ============================================================
         * Analysis 1: N-gram frequency at the AC start
         * Count all bit patterns of length 1-12
         * ============================================================ */
        printf("\n  Bit pattern frequency (first 10000 AC bits):\n");
        for (int plen = 1; plen <= 6; plen++) {
            int ncodes = 1 << plen;
            int *counts = calloc(ncodes, sizeof(int));
            bitstream bs2 = {bsdata, data_end_byte*8, dc_end};
            int total = 0;
            while (bs2.pos + plen <= dc_end + 10000 && bs2.pos + plen <= data_end_byte*8) {
                int pat = bs_peek(&bs2, plen);
                if (pat >= 0) counts[pat]++;
                bs2.pos++;  /* slide by 1 bit */
                total++;
            }
            printf("    %d-bit: ", plen);
            for (int i = 0; i < ncodes && i < 16; i++) {
                printf("%d(", i);
                for (int b = plen-1; b >= 0; b--) printf("%d", (i>>b)&1);
                printf(")=%d ", counts[i]);
            }
            printf("\n");
            free(counts);
        }

        /* ============================================================
         * Analysis 2: Transition probabilities
         * P(0|0), P(1|0), P(0|1), P(1|1)
         * ============================================================ */
        printf("\n  Bit transition probabilities:\n");
        {
            int t[2][2] = {{0,0},{0,0}};
            int prev = -1;
            for (int bit_pos = dc_end; bit_pos < dc_end + ac_bits && bit_pos < data_end_byte*8; bit_pos++) {
                int bp = bit_pos;
                int bit = (bsdata[bp/8] >> (7-(bp%8))) & 1;
                if (prev >= 0) t[prev][bit]++;
                prev = bit;
            }
            printf("    P(0|0)=%.3f P(1|0)=%.3f P(0|1)=%.3f P(1|1)=%.3f\n",
                   (double)t[0][0]/(t[0][0]+t[0][1]),
                   (double)t[0][1]/(t[0][0]+t[0][1]),
                   (double)t[1][0]/(t[1][0]+t[1][1]),
                   (double)t[1][1]/(t[1][0]+t[1][1]));
        }

        /* ============================================================
         * Analysis 3: Run-length distribution with separate 0s and 1s
         * ============================================================ */
        printf("\n  Run-length by bit value:\n");
        {
            int run0[20]={0}, run1[20]={0};
            int cur_run = 0, prev = -1;
            for (int bp = dc_end; bp < dc_end + ac_bits && bp < data_end_byte*8; bp++) {
                int bit = (bsdata[bp/8] >> (7-(bp%8))) & 1;
                if (bit == prev) cur_run++;
                else {
                    if (prev == 0 && cur_run < 20) run0[cur_run]++;
                    if (prev == 1 && cur_run < 20) run1[cur_run]++;
                    cur_run = 1;
                }
                prev = bit;
            }
            printf("    0-runs: ");
            for (int i = 1; i < 15; i++) printf("r%d=%d ", i, run0[i]);
            printf("\n    1-runs: ");
            for (int i = 1; i < 15; i++) printf("r%d=%d ", i, run1[i]);
            printf("\n");

            /* Expected for biased random (p0=0.562) */
            double p0 = 0.562, p1 = 0.438;
            printf("    Expected 0-runs (p0=%.3f): ", p0);
            for (int i = 1; i < 15; i++) {
                double exp = ac_bits * pow(p0,i) * p1 / 2.0;
                printf("r%d=%.0f ", i, exp);
            }
            printf("\n    Expected 1-runs (p1=%.3f): ", p1);
            for (int i = 1; i < 15; i++) {
                double exp = ac_bits * pow(p1,i) * p0 / 2.0;
                printf("r%d=%.0f ", i, exp);
            }
            printf("\n");
        }

        /* ============================================================
         * Analysis 4: Look at the LAST 200 bits before padding
         * This should show the EOB pattern repeated
         * ============================================================ */
        if (pass == 0 && max_pad > 100) {
            printf("\n  Last 200 bits before padding:\n  ");
            int start = data_end_byte * 8 - 200;
            if (start < dc_end) start = dc_end;
            for (int bp = start; bp < data_end_byte * 8; bp++) {
                printf("%d", (bsdata[bp/8] >> (7-(bp%8))) & 1);
                if ((bp - start + 1) % 40 == 0) printf("\n  ");
            }
            printf("\n");

            /* Also show as hex */
            printf("  Last 25 bytes before padding: ");
            for (int i = data_end_byte - 25; i < data_end_byte; i++)
                if (i >= 0) printf("%02X ", bsdata[i]);
            printf("\n");

            /* Check if there's a repeating pattern in last 100 bits */
            printf("  Pattern detection in last 100 bits:\n");
            for (int period = 1; period <= 8; period++) {
                int matches = 0, total = 0;
                for (int bp = data_end_byte*8 - 100; bp < data_end_byte*8 - period; bp++) {
                    int b1 = (bsdata[bp/8] >> (7-(bp%8))) & 1;
                    int b2 = (bsdata[(bp+period)/8] >> (7-((bp+period)%8))) & 1;
                    if (b1 == b2) matches++;
                    total++;
                }
                printf("    Period %d: %.1f%% match\n", period, 100.0*matches/total);
            }
        }

        /* ============================================================
         * Analysis 5: Compare DC region bit-run distribution with AC region
         * If they use the same coding, the patterns should be similar
         * ============================================================ */
        printf("\n  Run-length comparison DC vs AC:\n");
        {
            int dc_runs[20]={0}, ac_runs[20]={0};
            int cur_run, prev;

            /* DC region */
            cur_run=0; prev=-1;
            for (int bp = 0; bp < dc_end; bp++) {
                int bit = (bsdata[bp/8] >> (7-(bp%8))) & 1;
                if (bit == prev) cur_run++;
                else {
                    if (cur_run > 0 && cur_run < 20) dc_runs[cur_run]++;
                    cur_run = 1;
                }
                prev = bit;
            }

            /* AC region */
            cur_run=0; prev=-1;
            for (int bp = dc_end; bp < data_end_byte*8; bp++) {
                int bit = (bsdata[bp/8] >> (7-(bp%8))) & 1;
                if (bit == prev) cur_run++;
                else {
                    if (cur_run > 0 && cur_run < 20) ac_runs[cur_run]++;
                    cur_run = 1;
                }
                prev = bit;
            }

            printf("    DC: ");
            for (int i = 1; i < 10; i++) printf("r%d=%d ", i, dc_runs[i]);
            printf("\n    AC: ");
            for (int i = 1; i < 15; i++) printf("r%d=%d ", i, ac_runs[i]);
            printf("\n");

            /* Normalize and compare ratios */
            printf("    AC/DC ratio (normalized): ");
            double dc_total = 0, ac_total = 0;
            for (int i = 1; i < 10; i++) { dc_total += dc_runs[i]; ac_total += ac_runs[i]; }
            for (int i = 1; i < 10; i++) {
                double dc_frac = dc_runs[i] / dc_total;
                double ac_frac = ac_runs[i] / ac_total;
                printf("r%d=%.2f ", i, dc_frac > 0 ? ac_frac/dc_frac : 0);
            }
            printf("\n");
        }

        /* ============================================================
         * Analysis 6: 6-bit and 12-bit pattern frequency
         * Since r6 and r12 are anomalous, look for specific 6/12-bit patterns
         * ============================================================ */
        printf("\n  Most common 6-bit patterns in AC:\n");
        {
            int hist[64] = {0};
            for (int bp = dc_end; bp + 6 <= data_end_byte*8; bp++) {
                bitstream bs2 = {bsdata, data_end_byte*8, bp};
                int pat = bs_peek(&bs2, 6);
                if (pat >= 0) hist[pat]++;
            }
            /* Sort and show top 10 */
            for (int rank = 0; rank < 10; rank++) {
                int best = -1, bestc = 0;
                for (int i = 0; i < 64; i++)
                    if (hist[i] > bestc) { bestc = hist[i]; best = i; }
                if (best >= 0) {
                    printf("    ");
                    for (int b = 5; b >= 0; b--) printf("%d", (best>>b)&1);
                    printf(" (%02X) = %d times (%.2f%%)\n", best, bestc,
                           100.0*bestc/((data_end_byte*8-dc_end-5)));
                    hist[best] = 0;
                }
            }
        }

        /* ============================================================
         * Analysis 7: Byte frequency in AC region
         * Looking for structural bytes
         * ============================================================ */
        printf("\n  Most/least common bytes in AC:\n");
        {
            int hist[256]={0};
            for (int i = ac_start_byte; i < data_end_byte; i++) hist[bsdata[i]]++;
            int ac_bytes = data_end_byte - ac_start_byte;

            printf("    Top 5: ");
            int hcopy[256]; memcpy(hcopy, hist, sizeof(hist));
            for (int rank = 0; rank < 5; rank++) {
                int best=-1,bestc=0;
                for (int i=0;i<256;i++) if (hcopy[i]>bestc){bestc=hcopy[i];best=i;}
                if(best>=0){printf("%02X=%d(%.1f%%) ",best,bestc,100.0*bestc/ac_bytes);hcopy[best]=0;}
            }
            printf("\n    Bottom 5: ");
            memcpy(hcopy, hist, sizeof(hist));
            for (int rank = 0; rank < 5; rank++) {
                int worst=-1,worstc=99999;
                for (int i=0;i<256;i++) if (hcopy[i]<worstc){worstc=hcopy[i];worst=i;}
                if(worst>=0){printf("%02X=%d(%.1f%%) ",worst,worstc,100.0*worstc/ac_bytes);hcopy[worst]=99999;}
            }
            printf("\n");

            /* Byte value vs frequency: do bytes with more 0-bits appear more often? */
            printf("    Bytes by popcount:\n");
            for (int pc = 0; pc <= 8; pc++) {
                int count = 0, total_freq = 0;
                for (int i = 0; i < 256; i++) {
                    int bits_set = 0;
                    for (int b = 0; b < 8; b++) if ((i>>b)&1) bits_set++;
                    if (bits_set == pc) { count++; total_freq += hist[i]; }
                }
                printf("      popcount=%d: %d byte values, total freq=%d, avg=%.1f\n",
                       pc, count, total_freq, (double)total_freq/count);
            }
        }
    }

    free(disc); zip_close(z);
    return 0;
}
