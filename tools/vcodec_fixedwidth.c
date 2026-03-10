/*
 * vcodec_fixedwidth.c - Test fixed-width AC coding schemes
 *
 * Key insight: all VLC approaches self-calibrate on random data.
 * Fixed-width coding would NOT self-calibrate — it would consume
 * a predictable amount of data. This is a strong discriminator.
 *
 * Tests:
 * 1. PS1 MDEC-style but smaller: 8, 10, 12-bit run-level pairs
 * 2. Per-block byte count (variable block sizes)
 * 3. Bitmap + values
 * 4. Look at padded frame boundaries for end markers
 * 5. Try reading bitstream as interleaved DC+AC with fixed-width AC
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
    bs->pos += 2;
    return 0;
}

static const int zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static const uint8_t qt[16] = {10,20,14,13,18,37,22,28,15,24,15,18,18,31,17,20};

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

static void write_ppm(const char *fn, int Y[H][W], int Cb[H/2][W/2], int Cr[H/2][W/2]) {
    FILE *fp = fopen(fn, "wb");
    fprintf(fp, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int yv = Y[y][x], cb = Cb[y/2][x/2]-128, cr = Cr[y/2][x/2]-128;
            uint8_t rgb[3];
            rgb[0] = clamp(yv + 1.402*cr);
            rgb[1] = clamp(yv - 0.344136*cb - 0.714136*cr);
            rgb[2] = clamp(yv + 1.772*cb);
            fwrite(rgb, 1, 3, fp);
        }
    fclose(fp);
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

    /* ============================================================
     * Analyze ALL padded frames to find end-of-data patterns
     * ============================================================ */
    printf("=== Padded frame boundary analysis ===\n");
    for (int fi = 0; fi < nf; fi++) {
        uint8_t *ff = frames[fi];
        int ffs = fsizes[fi];
        const uint8_t *fbd = ff+40;
        int fbl = ffs-40;

        int pad = 0;
        for (int i = fbl-1; i >= 0; i--) {
            if (fbd[i] == 0xFF) pad++; else break;
        }
        if (pad < 10) continue;  /* Only analyze significantly padded frames */

        int de = fbl - pad;  /* data end byte offset */
        printf("  F%02d qs=%2d: data_end=byte %d (%d bits), pad=%d bytes\n",
               fi, ff[3], de, de*8, pad);
        printf("    Last 20 bytes: ");
        for (int i = de-20; i < de; i++) {
            if (i >= 0) printf("%02X ", fbd[i]);
        }
        printf("\n");
        printf("    Last 20 bytes binary:\n");
        for (int i = de-5; i < de; i++) {
            if (i >= 0) {
                printf("    byte %d: ", i);
                for (int b = 7; b >= 0; b--) printf("%d", (fbd[i]>>b)&1);
                printf(" (%02X)\n", fbd[i]);
            }
        }

        /* Check DC bit position in this frame */
        bitstream bs = {fbd, fbl*8, 0};
        int dp[3]={0,0,0};
        for(int b=0;b<nblocks&&bs.pos<fbl*8;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dp[comp]+=read_dc(&bs);
        }
        printf("    DC ends at bit %d (byte %.1f)\n", bs.pos, bs.pos/8.0);
        printf("    AC data: %d bytes (%d bits)\n", de - bs.pos/8, de*8 - bs.pos);
    }

    /* Use first frame for remaining tests */
    uint8_t *f=frames[0]; int fsize=fsizes[0]; int qs=f[3];
    const uint8_t *bsdata=f+40;
    int bslen=fsize-40, total_bits=bslen*8;

    bitstream bs0 = {bsdata, total_bits, 0};
    int dc_vals[900]; int dc_pred[3]={0,0,0};
    for(int b=0;b<nblocks&&bs0.pos<total_bits;b++){
        int comp=(b%6<4)?0:(b%6==4)?1:2;
        dc_pred[comp]+=read_dc(&bs0);
        dc_vals[b]=dc_pred[comp];
    }
    int dc_end=bs0.pos;
    int ac_bits=total_bits-dc_end;
    printf("\nFrame 0: qs=%d, dc=%d bits, ac=%d bits (%.1f bits/block)\n\n",
           qs, dc_end, ac_bits, (double)ac_bits/nblocks);

    /* ============================================================
     * Test 1: Fixed-width run-level pairs (various bit widths)
     * EOB: all-zeros pair (run=0, level=0)
     * ============================================================ */
    printf("=== Test 1: Fixed-width run-level (with EOB=all-zeros) ===\n");
    {
        struct { int run_bits; int level_bits; const char *name; } configs[] = {
            {4, 4, "4+4=8"},
            {4, 6, "4+6=10"},
            {6, 6, "6+6=12"},
            {4, 8, "4+8=12b"},
            {6, 4, "6+4=10b"},
            {6, 8, "6+8=14"},
            {6, 10, "6+10=16 (MDEC)"},
        };
        int ncfg = sizeof(configs)/sizeof(configs[0]);

        for (int ci = 0; ci < ncfg; ci++) {
            int rb = configs[ci].run_bits;
            int lb = configs[ci].level_bits;
            int pw = rb + lb;

            bitstream bs = {bsdata, total_bits, dc_end};
            int total_nz = 0, total_eob = 0;
            int completed = 0;
            int run_max = 0, level_max = 0;
            int band_nz[8] = {0};

            for (int b = 0; b < nblocks && bs.pos + pw <= total_bits; b++) {
                int pos = 1;
                while (pos < 64 && bs.pos + pw <= total_bits) {
                    int run = bs_read(&bs, rb);
                    int level_raw = bs_read(&bs, lb);
                    if (run == 0 && level_raw == 0) { total_eob++; break; } /* EOB */

                    /* Sign-extend level */
                    int half = 1 << (lb - 1);
                    int level = (level_raw >= half) ? level_raw - (1 << lb) : level_raw;

                    pos += run;
                    if (pos >= 64) break;
                    if (pos > 0) {
                        int band = (pos-1)/8;
                        if (band < 8) band_nz[band]++;
                    }
                    total_nz++;
                    if (run > run_max) run_max = run;
                    if (abs(level) > level_max) level_max = abs(level);
                    pos++;
                }
                completed++;
            }
            int used = bs.pos - dc_end;
            printf("  %s: %d blocks, NZ=%d, EOB=%d, used=%d/%d (%.1f%%) rmax=%d lmax=%d\n",
                   configs[ci].name, completed, total_nz, total_eob, used, ac_bits,
                   100.0*used/ac_bits, run_max, level_max);
            printf("    Bands: ");
            for(int i=0;i<8;i++) printf("%d ",band_nz[i]); printf("\n");
        }
    }

    /* ============================================================
     * Test 2: Fixed-width on RANDOM data (control)
     * Only test the most promising widths from Test 1
     * ============================================================ */
    printf("\n=== Test 2: Fixed-width on RANDOM data (control) ===\n");
    {
        uint8_t *rnd = malloc(bslen);
        srand(12345);
        for (int i = 0; i < bslen; i++) rnd[i] = rand() & 0xFF;

        int configs[][2] = {{4,4},{4,6},{6,6},{6,10}};
        int ncfg = 4;
        for (int ci = 0; ci < ncfg; ci++) {
            int rb = configs[ci][0], lb = configs[ci][1];
            int pw = rb + lb;

            bitstream bs = {rnd, ac_bits, 0};
            int nz=0, eob=0, completed=0;
            int band_nz[8]={0};

            for (int b = 0; b < nblocks && bs.pos + pw <= ac_bits; b++) {
                int pos = 1;
                while (pos < 64 && bs.pos + pw <= ac_bits) {
                    int run = bs_read(&bs, rb);
                    int level_raw = bs_read(&bs, lb);
                    if (run == 0 && level_raw == 0) { eob++; break; }
                    int half = 1 << (lb - 1);
                    int level = (level_raw >= half) ? level_raw - (1 << lb) : level_raw;
                    pos += run;
                    if (pos >= 64) break;
                    if (pos > 0) { int band=(pos-1)/8; if(band<8) band_nz[band]++; }
                    nz++;
                    pos++;
                }
                completed++;
            }
            int used = bs.pos;
            printf("  %d+%d: %d blocks, NZ=%d, EOB=%d, used=%d/%d (%.1f%%)\n",
                   rb, lb, completed, nz, eob, used, ac_bits, 100.0*used/ac_bits);
            printf("    Bands: ");
            for(int i=0;i<8;i++) printf("%d ",band_nz[i]); printf("\n");
        }
        free(rnd);
    }

    /* ============================================================
     * Test 3: Interleaved DC+AC with fixed-width run-level
     * Instead of separate DC pass, read DC then AC per block
     * ============================================================ */
    printf("\n=== Test 3: Interleaved DC + fixed-width AC ===\n");
    {
        int configs[][2] = {{4,4},{4,6},{6,6},{6,10}};
        int ncfg = 4;
        for (int ci = 0; ci < ncfg; ci++) {
            int rb = configs[ci][0], lb = configs[ci][1];
            int pw = rb + lb;

            bitstream bs = {bsdata, total_bits, 0};
            int dp[3]={0,0,0};
            int nz=0, eob=0, completed=0;
            int band_nz[8]={0};

            for (int b = 0; b < nblocks && bs.pos + pw <= total_bits; b++) {
                int comp=(b%6<4)?0:(b%6==4)?1:2;
                dp[comp]+=read_dc(&bs);

                int pos = 1;
                while (pos < 64 && bs.pos + pw <= total_bits) {
                    int run = bs_read(&bs, rb);
                    int level_raw = bs_read(&bs, lb);
                    if (run == 0 && level_raw == 0) { eob++; break; }
                    int half = 1 << (lb - 1);
                    int level = (level_raw >= half) ? level_raw - (1 << lb) : level_raw;
                    pos += run;
                    if (pos >= 64) break;
                    if (pos > 0) { int band=(pos-1)/8; if(band<8) band_nz[band]++; }
                    nz++;
                    pos++;
                }
                completed++;
            }
            int used = bs.pos;
            printf("  %d+%d: %d blocks, NZ=%d, EOB=%d, used=%d/%d (%.1f%%)\n",
                   rb, lb, completed, nz, eob, used, total_bits, 100.0*used/total_bits);
            printf("    Bands: ");
            for(int i=0;i<8;i++) printf("%d ",band_nz[i]); printf("\n");
        }
    }

    /* ============================================================
     * Test 4: Image output for most promising fixed-width scheme
     * Try 10-bit (6+4) interleaved DC+AC with dequantization
     * ============================================================ */
    printf("\n=== Test 4: Image outputs for fixed-width candidates ===\n");
    {
        /* Test multiple configurations */
        struct { int rb; int lb; const char *fn; } tests[] = {
            {4, 4, "/tmp/ac_fw_4_4.ppm"},
            {4, 6, "/tmp/ac_fw_4_6.ppm"},
            {6, 6, "/tmp/ac_fw_6_6.ppm"},
            {6, 10, "/tmp/ac_fw_6_10.ppm"},
        };

        for (int ti = 0; ti < 4; ti++) {
            int rb = tests[ti].rb, lb = tests[ti].lb;
            int pw = rb + lb;
            static int coeffs[900][64];
            memset(coeffs, 0, sizeof(coeffs));

            bitstream bs = {bsdata, total_bits, 0};
            int dp[3]={0,0,0};

            for (int b = 0; b < nblocks && bs.pos + pw <= total_bits; b++) {
                int comp=(b%6<4)?0:(b%6==4)?1:2;
                dp[comp]+=read_dc(&bs);
                coeffs[b][0] = dp[comp] * 8;

                int pos = 1;
                while (pos < 64 && bs.pos + pw <= total_bits) {
                    int run = bs_read(&bs, rb);
                    int level_raw = bs_read(&bs, lb);
                    if (run == 0 && level_raw == 0) break;  /* EOB */
                    int half = 1 << (lb - 1);
                    int level = (level_raw >= half) ? level_raw - (1 << lb) : level_raw;
                    pos += run;
                    if (pos >= 64) break;
                    int zpos = zigzag[pos];
                    int qi = zpos % 16;
                    coeffs[b][zpos] = level * qt[qi] * qs / 8;
                    pos++;
                }
            }

            /* IDCT and write image */
            static int Yp[H][W], Cbp[H/2][W/2], Crp[H/2][W/2];
            memset(Yp,128,sizeof(Yp)); memset(Cbp,128,sizeof(Cbp)); memset(Crp,128,sizeof(Crp));
            for (int mb = 0; mb < mw*mh; mb++) {
                int mx2=mb%mw, my2=mb/mw;
                for (int s = 0; s < 6; s++) {
                    int out[64];
                    idct8x8(coeffs[mb*6+s], out);
                    if (s < 4) {
                        int bx=(s&1)*8, by=(s>>1)*8;
                        for(int r=0;r<8;r++) for(int c=0;c<8;c++){
                            int py=my2*16+by+r, px=mx2*16+bx+c;
                            if(py<H&&px<W) Yp[py][px]=clamp(out[r*8+c]+128);
                        }
                    } else if (s == 4) {
                        for(int r=0;r<8;r++) for(int c=0;c<8;c++){
                            int py=my2*8+r, px=mx2*8+c;
                            if(py<H/2&&px<W/2) Cbp[py][px]=clamp(out[r*8+c]+128);
                        }
                    } else {
                        for(int r=0;r<8;r++) for(int c=0;c<8;c++){
                            int py=my2*8+r, px=mx2*8+c;
                            if(py<H/2&&px<W/2) Crp[py][px]=clamp(out[r*8+c]+128);
                        }
                    }
                }
            }
            write_ppm(tests[ti].fn, Yp, Cbp, Crp);
            printf("  %d+%d → %s\n", rb, lb, tests[ti].fn);
        }
    }

    /* ============================================================
     * Test 5: What if AC uses byte-aligned data?
     * Each block's AC data starts at a byte boundary
     * and the first byte indicates the AC data length in bytes
     * ============================================================ */
    printf("\n=== Test 5: Byte-aligned blocks with length byte ===\n");
    {
        /* Align AC start to byte boundary */
        int ac_byte_start = (dc_end + 7) / 8;
        int pos = ac_byte_start;
        int block_count = 0;
        int total_ac_bytes = 0;
        int min_len = 999, max_len = 0;

        printf("  Starting at byte %d\n", pos);
        for (int b = 0; b < nblocks && pos < bslen; b++) {
            int len = bsdata[pos];
            if (len == 0 || len == 0xFF) break;  /* end marker? */
            total_ac_bytes += len;
            if (len < min_len) min_len = len;
            if (len > max_len) max_len = len;
            pos += 1 + len;  /* skip length byte + data */
            block_count++;
        }
        printf("  Blocks: %d, total AC bytes: %d, len range: [%d, %d]\n",
               block_count, total_ac_bytes, min_len, max_len);
        printf("  Used: %d/%d bytes (%.1f%%)\n", pos - ac_byte_start,
               bslen - ac_byte_start, 100.0*(pos - ac_byte_start)/(bslen - ac_byte_start));

        /* Try with 2-byte length prefix */
        pos = ac_byte_start;
        block_count = 0;
        for (int b = 0; b < nblocks && pos + 1 < bslen; b++) {
            int len = (bsdata[pos] << 8) | bsdata[pos+1];
            if (len > 500) break;  /* unreasonable */
            pos += 2 + len;
            block_count++;
        }
        printf("  2-byte prefix: %d blocks at byte %d\n", block_count, pos);
    }

    /* ============================================================
     * Test 6: Bit-counted blocks
     * What if each block has a bit count prefix, not byte count?
     * ============================================================ */
    printf("\n=== Test 6: Bit-counted blocks ===\n");
    for (int prefix_bits = 7; prefix_bits <= 10; prefix_bits++) {
        bitstream bs = {bsdata, total_bits, dc_end};
        int dp[3]={0,0,0};
        int block_count = 0;
        int total_block_bits = 0;
        int min_bl = 9999, max_bl = 0;

        for (int b = 0; b < nblocks && bs.pos + prefix_bits <= total_bits; b++) {
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dp[comp]+=read_dc(&bs);

            int bl = bs_read(&bs, prefix_bits);
            if (bl < 0) break;
            total_block_bits += bl;
            if (bl < min_bl) min_bl = bl;
            if (bl > max_bl) max_bl = bl;
            bs.pos += bl;  /* skip the block's AC data */
            if (bs.pos > total_bits) break;
            block_count++;
        }
        printf("  %d-bit prefix: %d blocks, total=%d bits, range=[%d,%d], endpos=%d/%d\n",
               prefix_bits, block_count, total_block_bits, min_bl, max_bl, bs.pos, total_bits);
    }

    /* ============================================================
     * Test 7: Sector-aligned data
     * Each CD sector is 2047 bytes. Does AC data align to sector boundaries?
     * ============================================================ */
    printf("\n=== Test 7: Sector boundary analysis ===\n");
    {
        printf("  Bytes per sector: 2047\n");
        printf("  Header: 40 bytes (in sector 0)\n");
        printf("  Sector 0 data: 2047-40 = 2007 bytes\n");
        printf("  Sectors 1-5: 5 × 2047 = 10235 bytes\n");
        printf("  Total: 2007 + 10235 = 12242 bytes\n");
        printf("  DC ends at byte %.1f (sector %.2f)\n",
               dc_end/8.0, (40 + dc_end/8.0)/2047.0);

        /* Check if any structure appears at sector boundaries */
        for (int s = 0; s < 6; s++) {
            int byte_off;
            if (s == 0) byte_off = 0;
            else byte_off = 2007 + (s-1)*2047;

            printf("  Sector %d start (byte %d): ", s, byte_off);
            for (int i = 0; i < 8 && byte_off+i < bslen; i++)
                printf("%02X ", bsdata[byte_off+i]);
            printf("\n");
        }
    }

    free(disc); zip_close(z);
    return 0;
}
