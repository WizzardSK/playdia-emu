/*
 * vcodec_structure.c - Deep structural analysis of the bitstream
 *
 * Look for:
 * 1. Embedded 00 80 XX command patterns in the bitstream
 * 2. Repeating patterns at various periods
 * 3. Cross-frame byte-level correlation
 * 4. Whether DC is really all at the beginning (or interleaved)
 * 5. Entropy changes at fine granularity
 * 6. Whether the data might be arithmetic coded
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
    uint8_t *f=frames[0]; int fsize=fsizes[0]; int qs=f[3];

    printf("Frame 0: qs=%d fsize=%d\n\n", qs, fsize);

    /* ============================================================
     * Test 1: Search for 00 80 XX patterns in bitstream
     * ============================================================ */
    printf("=== Test 1: Search for 00 80 XX patterns ===\n");
    {
        const uint8_t *data = f;  /* entire frame including header */
        int count = 0;
        for (int i = 0; i < fsize - 2; i++) {
            if (data[i] == 0x00 && data[i+1] == 0x80) {
                printf("  Offset %5d (0x%04X): 00 80 %02X %02X\n",
                       i, i, data[i+2], i+3<fsize?data[i+3]:0);
                count++;
                if (count > 20) { printf("  ... (truncated)\n"); break; }
            }
        }
        printf("  Total 00 80 patterns: ");
        count = 0;
        for (int i = 0; i < fsize - 1; i++)
            if (data[i] == 0x00 && data[i+1] == 0x80) count++;
        printf("%d\n", count);
    }

    /* ============================================================
     * Test 2: What's REALLY in the header? Dump full 40 bytes
     * ============================================================ */
    printf("\n=== Test 2: Full header dump ===\n");
    {
        printf("  Raw hex: ");
        for (int i = 0; i < 40; i++) printf("%02X ", f[i]);
        printf("\n");

        printf("  Words (16-bit BE): ");
        for (int i = 0; i < 40; i += 2)
            printf("%04X ", (f[i]<<8)|f[i+1]);
        printf("\n");

        printf("  Words (32-bit BE): ");
        for (int i = 0; i < 40; i += 4)
            printf("%08X ", (f[i]<<24)|(f[i+1]<<16)|(f[i+2]<<8)|f[i+3]);
        printf("\n");
    }

    /* ============================================================
     * Test 3: Cross-frame correlation
     * Compare byte values at same position across I-frames
     * ============================================================ */
    printf("\n=== Test 3: Cross-frame byte correlation ===\n");
    {
        /* Compare frame 0 and frame 4 (both I-frames) at specific offsets */
        printf("  Comparing F00 vs F04 (both I-frames) at various offsets:\n");
        if (nf > 4) {
            int offsets[] = {40, 50, 100, 200, 500, 513, 1000, 2000, 4000, 6000};
            for (int oi = 0; oi < 10; oi++) {
                int off = offsets[oi];
                printf("    Offset %4d: F00=", off);
                for (int j = 0; j < 8 && off+j < fsizes[0]; j++) printf("%02X", frames[0][off+j]);
                printf("  F04=");
                for (int j = 0; j < 8 && off+j < fsizes[4]; j++) printf("%02X", frames[4][off+j]);
                printf("\n");
            }
        }
    }

    /* ============================================================
     * Test 4: Fine-grained entropy analysis (64-byte windows)
     * ============================================================ */
    printf("\n=== Test 4: Fine-grained entropy (64-byte windows) ===\n");
    {
        const uint8_t *bsdata = f + 40;
        int bslen = fsize - 40;

        for (int w = 0; w < 30 && (w+1)*64 <= bslen; w++) {
            int hist[256] = {0};
            for (int i = 0; i < 64; i++)
                hist[bsdata[w*64 + i]]++;
            double ent = 0;
            for (int i = 0; i < 256; i++) {
                if (hist[i] == 0) continue;
                double p = hist[i] / 64.0;
                ent -= p * log2(p);
            }
            int zeros = 0, ones = 0;
            for (int i = 0; i < 64; i++)
                for (int b = 0; b < 8; b++)
                    if ((bsdata[w*64+i] >> b) & 1) ones++; else zeros++;
            printf("  W%02d (byte %4d): ent=%.2f 0s=%.1f%% 1s=%.1f%%",
                   w, w*64, ent, 100.0*zeros/512, 100.0*ones/512);
            if (w*64 < 520) printf(" [DC region]");
            printf("\n");
        }
    }

    /* ============================================================
     * Test 5: Try verifying DC decode by checking if AC region
     * has lower entropy than DC region (it shouldn't if DC is correct)
     * ============================================================ */
    printf("\n=== Test 5: DC vs AC region statistics ===\n");
    {
        const uint8_t *bsdata = f + 40;
        int bslen = fsize - 40;

        bitstream bs = {bsdata, bslen*8, 0};
        int dp[3]={0,0,0};
        for(int b=0;b<nblocks&&bs.pos<bslen*8;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dp[comp]+=read_dc(&bs);
        }
        int dc_end_byte = (bs.pos + 7) / 8;

        /* Count byte values in DC region */
        int dc_hist[256]={0}, ac_hist[256]={0};
        for (int i = 0; i < dc_end_byte && i < bslen; i++) dc_hist[bsdata[i]]++;
        for (int i = dc_end_byte; i < bslen; i++) ac_hist[bsdata[i]]++;

        double dc_ent=0, ac_ent=0;
        for (int i = 0; i < 256; i++) {
            if (dc_hist[i] > 0) { double p = (double)dc_hist[i]/dc_end_byte; dc_ent -= p*log2(p); }
            int ac_total = bslen - dc_end_byte;
            if (ac_hist[i] > 0) { double p = (double)ac_hist[i]/ac_total; ac_ent -= p*log2(p); }
        }
        printf("  DC region: %d bytes, entropy=%.3f bits/byte\n", dc_end_byte, dc_ent);
        printf("  AC region: %d bytes, entropy=%.3f bits/byte\n", bslen-dc_end_byte, ac_ent);

        /* Most common bytes in DC region */
        printf("  DC top bytes: ");
        for (int rank = 0; rank < 5; rank++) {
            int best = -1, bestc = 0;
            for (int i = 0; i < 256; i++) {
                if (dc_hist[i] > bestc) { bestc = dc_hist[i]; best = i; }
            }
            if (best >= 0) { printf("%02X(%d) ", best, bestc); dc_hist[best] = 0; }
        }
        printf("\n");
    }

    /* ============================================================
     * Test 6: Arithmetic coding detection
     * If the data is arithmetic coded, it would have very specific properties:
     * - No zero-run patterns
     * - High entropy throughout
     * - The "carrier" bit pattern would show specific properties
     * Try to detect: does the bitstream look like a single long number?
     * ============================================================ */
    printf("\n=== Test 6: Arithmetic coding indicators ===\n");
    {
        const uint8_t *bsdata = f + 40;
        int bslen = fsize - 40;
        int dc_end_byte = 513;  /* approximate */

        /* Check for long runs of 0 or 1 bits */
        int max_run0 = 0, max_run1 = 0;
        int cur_run = 0;
        int prev_bit = -1;
        int run_hist[20] = {0};

        for (int i = dc_end_byte; i < bslen; i++) {
            for (int b = 7; b >= 0; b--) {
                int bit = (bsdata[i] >> b) & 1;
                if (bit == prev_bit) {
                    cur_run++;
                } else {
                    if (cur_run < 20) run_hist[cur_run]++;
                    if (prev_bit == 0 && cur_run > max_run0) max_run0 = cur_run;
                    if (prev_bit == 1 && cur_run > max_run1) max_run1 = cur_run;
                    cur_run = 1;
                }
                prev_bit = bit;
            }
        }
        printf("  Max run of 0s: %d, Max run of 1s: %d\n", max_run0, max_run1);
        printf("  Run length histogram: ");
        for (int i = 1; i < 15; i++) printf("r%d=%d ", i, run_hist[i]);
        printf("\n");

        /* Expected for random: run of N has probability 0.5^N
         * For N=10: should see ~100 in 100K bits
         * For N=15: should see ~3
         * For N=20: should see ~0.1
         */
        int total_bits_checked = (bslen - dc_end_byte) * 8;
        printf("  Total bits: %d\n", total_bits_checked);
        printf("  Expected max run for random: ~%.0f bits\n", log2(total_bits_checked));
    }

    /* ============================================================
     * Test 7: Try reading the ENTIRE frame as interleaved DC+AC
     * What if our assumption about "DC first for all blocks" is wrong?
     * What if each block has its own header byte followed by coded data?
     * ============================================================ */
    printf("\n=== Test 7: Per-block structure hypothesis ===\n");
    {
        /* What if each macroblock has a header?
         * Try: for each of 144 macroblocks, read some header info
         * then 6 blocks of data */
        const uint8_t *bsdata = f + 40;
        int bslen = fsize - 40;
        bitstream bs = {bsdata, bslen*8, 0};

        /* Hypothesis: each macroblock has a QS-override or mode byte */
        /* Try reading 4 bits as MB header, then 6 DC values */
        printf("  Hypothesis A: 4-bit MB header + 6 DCs\n");
        int mb_headers[20];
        for (int mb = 0; mb < 10; mb++) {
            mb_headers[mb] = bs_read(&bs, 4);
            int dp[3]={0,0,0};
            for (int s = 0; s < 6; s++) {
                int comp = (s < 4) ? 0 : (s == 4) ? 1 : 2;
                dp[comp] += read_dc(&bs);
            }
            printf("    MB%d: hdr=%d, Y_dc=%d,%d,%d,%d Cb=%d Cr=%d  @bit %d\n",
                   mb, mb_headers[mb],
                   dp[0], dp[0], dp[0], dp[0], dp[1], dp[2], bs.pos);
        }

        /* Hypothesis B: 8-bit MB header */
        printf("  Hypothesis B: 8-bit MB header + 6 DCs\n");
        bs.pos = 0;
        for (int mb = 0; mb < 5; mb++) {
            int hdr = bs_read(&bs, 8);
            int dp[3]={0,0,0};
            for (int s = 0; s < 6; s++) {
                int comp = (s < 4) ? 0 : (s == 4) ? 1 : 2;
                dp[comp] += read_dc(&bs);
            }
            printf("    MB%d: hdr=0x%02X, first_Y_dc=%d  @bit %d\n",
                   mb, hdr, dp[0], bs.pos);
        }
    }

    /* ============================================================
     * Test 8: Does the DC decode actually EXACTLY match expectations?
     * Verify by checking the very first few DC codes in detail
     * ============================================================ */
    printf("\n=== Test 8: Detailed DC decode verification ===\n");
    {
        const uint8_t *bsdata = f + 40;
        int bslen = fsize - 40;
        bitstream bs = {bsdata, bslen*8, 0};

        printf("  First 80 bits: ");
        for (int i = 0; i < 80 && i < bslen*8; i++) {
            printf("%d", (bsdata[i/8] >> (7-(i%8))) & 1);
            if (i % 8 == 7) printf(" ");
        }
        printf("\n");

        printf("  First 10 bytes hex: ");
        for (int i = 0; i < 10; i++) printf("%02X ", bsdata[i]);
        printf("\n");

        /* Decode DC for first 2 macroblocks, showing every bit */
        int dp[3]={0,0,0};
        for (int b = 0; b < 12; b++) {
            int comp = (b%6<4)?0:(b%6==4)?1:2;
            int start = bs.pos;

            /* Manually find which VLC entry matches */
            int matched = -1;
            for (int i = 0; i < 9; i++) {
                int bits = bs_peek(&bs, dc_vlc[i].len);
                if (bits == (int)dc_vlc[i].code) { matched = i; break; }
            }

            if (matched >= 0) {
                int vlc_len = dc_vlc[matched].len;
                int sz = dc_vlc[matched].size;
                bs.pos += vlc_len;
                int val = 0;
                if (sz > 0) {
                    val = bs_read(&bs, sz);
                    if (val < (1 << (sz - 1))) val -= (1 << sz) - 1;
                }
                dp[comp] += val;

                printf("  Block %2d (comp=%d): bits@%d '", b, comp, start);
                for (int i = start; i < bs.pos; i++)
                    printf("%d", (bsdata[i/8]>>(7-(i%8)))&1);
                printf("' vlc=%d sz=%d diff=%+d dc=%d\n",
                       matched, sz, val, dp[comp]);
            } else {
                printf("  Block %2d: NO MATCH at bit %d\n", b, start);
                bs.pos += 2;
            }
        }
    }

    /* ============================================================
     * Test 9: Check if frame data is scrambled with a known pattern
     * XOR with QS, XOR with byte position, etc.
     * ============================================================ */
    printf("\n=== Test 9: Descrambling attempts ===\n");
    {
        const uint8_t *bsdata = f + 40;
        int bslen = fsize - 40;
        int dc_end = 513;

        /* XOR with QS */
        printf("  XOR with QS (%d): first AC bytes: ", qs);
        for (int i = dc_end; i < dc_end+16 && i < bslen; i++)
            printf("%02X ", bsdata[i] ^ qs);
        printf("\n");

        /* XOR with byte index */
        printf("  XOR with index: first AC bytes: ");
        for (int i = dc_end; i < dc_end+16 && i < bslen; i++)
            printf("%02X ", bsdata[i] ^ (i & 0xFF));
        printf("\n");

        /* XOR with 0xFF (complement) */
        printf("  Complement: first AC bytes: ");
        for (int i = dc_end; i < dc_end+16 && i < bslen; i++)
            printf("%02X ", bsdata[i] ^ 0xFF);
        printf("\n");

        /* Check if it could be a Linear Feedback Shift Register (LFSR) pattern
         * by looking at consecutive XOR */
        printf("  Consecutive XOR: ");
        for (int i = dc_end; i < dc_end+16 && i+1 < bslen; i++)
            printf("%02X ", bsdata[i] ^ bsdata[i+1]);
        printf("\n");
    }

    /* ============================================================
     * Test 10: What if we have the WRONG sector assembly?
     * What if the sectors overlap or there's sector header data mixed in?
     * Dump raw sector boundaries
     * ============================================================ */
    printf("\n=== Test 10: Raw sector structure ===\n");
    {
        /* Find the actual sectors used for frame 0 */
        int sectors_found = 0;
        int sector_lbas[6];
        for (int l = start_lba; l < tsec && sectors_found < 10; l++) {
            const uint8_t *s = disc + (long)l * SECTOR_RAW;
            if (s[0]!=0||s[1]!=0xFF||s[15]!=2||(s[18]&4)) continue;
            if (s[24] == 0xF1 || s[24] == 0xF2) {
                printf("  Sector at LBA %d: submode=%02X, coding=%02X, type=%02X",
                       l, s[18], s[20], s[24]);
                printf("  header bytes 16-24: ");
                for (int i = 16; i < 25; i++) printf("%02X ", s[i]);
                printf("  data[0-7]: ");
                for (int i = 25; i < 33; i++) printf("%02X ", s[i]);
                printf("\n");
                if (sectors_found < 6) sector_lbas[sectors_found] = l;
                sectors_found++;
                if (s[24] == 0xF2) break;
            }
        }

        /* Check for hidden data in sector headers */
        printf("\n  XA subheader details for frame sectors:\n");
        for (int si = 0; si < sectors_found && si < 6; si++) {
            const uint8_t *s = disc + (long)sector_lbas[si] * SECTOR_RAW;
            printf("    Sector %d (LBA %d): ", si, sector_lbas[si]);
            printf("file=%02X chan=%02X submode=%02X coding=%02X ",
                   s[16], s[17], s[18], s[20]);
            printf("(dup: %02X %02X %02X %02X)\n", s[20], s[21], s[22], s[23]);
        }
    }

    free(disc); zip_close(z);
    return 0;
}
