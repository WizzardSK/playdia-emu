/*
 * vcodec_dcvlc_ac.c - Test DC luminance VLC table for AC coefficients
 *
 * Evidence: AC bitstream run-length fingerprint matches DC VLC table:
 * - 1-runs excess at r6 (matches 1111110 code's 6 leading 1s)
 * - 0-runs excess at r12 (two 000000 prefixes adjacent)
 * - Max run = 12 (no code has >6 consecutive same bits)
 *
 * Test approaches:
 * 1. DC VLC for run + DC VLC for level (interleaved per block)
 * 2. DC VLC as run-level combined code (like MPEG but with DC table)
 * 3. DC VLC for level only (with run coded differently)
 * 4. Entire bitstream as continuous DC VLC codes
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

/* MPEG-1 DC luminance VLC */
static const struct { int len; uint32_t code; int size; } dc_vlc[] = {
    {3, 0b100, 0}, {2, 0b00, 1}, {2, 0b01, 2}, {3, 0b101, 3},
    {3, 0b110, 4}, {4, 0b1110, 5}, {5, 0b11110, 6},
    {6, 0b111110, 7}, {7, 0b1111110, 8},
};
#define DC_VLC_COUNT 9

/* Read a value using DC luminance VLC
 * Returns: the decoded value (signed difference)
 * Sets *size_out to the size category (0-8)
 */
static int read_dc_vlc(bitstream *bs, int *size_out) {
    for (int i = 0; i < DC_VLC_COUNT; i++) {
        int bits = bs_peek(bs, dc_vlc[i].len);
        if (bits < 0) return -9999;
        if (bits == (int)dc_vlc[i].code) {
            bs->pos += dc_vlc[i].len;
            int sz = dc_vlc[i].size;
            if (size_out) *size_out = sz;
            if (sz == 0) return 0;
            int val = bs_read(bs, sz);
            if (val < 0) return -9999;
            if (val < (1 << (sz - 1))) val -= (1 << sz) - 1;
            return val;
        }
    }
    /* No match — shouldn't happen if the table is correct */
    if (size_out) *size_out = -1;
    bs->pos += 1;
    return -9999;
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
    uint8_t *f=frames[0]; int fsize=fsizes[0]; int qs=f[3];
    const uint8_t *bsdata=f+40;
    int bslen=fsize-40, total_bits=bslen*8;

    printf("Frame 0: qs=%d fsize=%d bits=%d\n\n", qs, fsize, total_bits);

    /* ============================================================
     * Approach 1: Interleaved DC+AC, both using DC luminance VLC
     * For each block: DC VLC (DPCM diff), then repeated:
     *   DC VLC gives value; if size=0, it's EOB
     *   Otherwise the value is the AC coefficient
     * ============================================================ */
    printf("=== Approach 1: DC VLC for DC and AC (size=0 → EOB) ===\n");
    {
        static int coeffs[900][64];
        memset(coeffs, 0, sizeof(coeffs));
        bitstream bs = {bsdata, total_bits, 0};
        int dp[3]={0,0,0};
        int total_nz=0, total_eob=0;
        int band_nz[8]={0};

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dp[comp] += read_dc_vlc(&bs, NULL);
            coeffs[b][0] = dp[comp] * 8;

            for (int p = 1; p < 64 && bs.pos < total_bits; p++) {
                int sz;
                int val = read_dc_vlc(&bs, &sz);
                if (val == -9999) break;
                if (sz == 0) { total_eob++; break; }  /* size=0 = EOB */
                coeffs[b][zigzag[p]] = val;
                if (val != 0) {
                    int band = (p-1)/8;
                    if (band < 8) band_nz[band]++;
                    total_nz++;
                }
            }
        }
        printf("  Used: %d/%d bits (%.1f%%)\n", bs.pos, total_bits, 100.0*bs.pos/total_bits);
        printf("  NZ=%d, EOB=%d, NZ/block=%.1f\n", total_nz, total_eob, (double)total_nz/nblocks);
        printf("  Bands: ");
        for(int i=0;i<8;i++) printf("b%d=%d ",i,band_nz[i]); printf("\n");

        /* Write image */
        static int Yp[H][W], Cbp[H/2][W/2], Crp[H/2][W/2];
        memset(Yp,128,sizeof(Yp)); memset(Cbp,128,sizeof(Cbp)); memset(Crp,128,sizeof(Crp));
        for (int mb = 0; mb < mw*mh; mb++) {
            int mx2=mb%mw, my2=mb/mw;
            for (int s = 0; s < 6; s++) {
                int out[64];
                /* Dequantize */
                int dq[64]; memset(dq, 0, sizeof(dq));
                dq[0] = coeffs[mb*6+s][0];
                for (int i = 1; i < 64; i++) {
                    int qi = i % 16;
                    dq[i] = coeffs[mb*6+s][i] * qt[qi] * qs / 8;
                }
                idct8x8(dq, out);
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
        write_ppm("/tmp/ac_dcvlc1.ppm", Yp, Cbp, Crp);
        printf("  → /tmp/ac_dcvlc1.ppm\n");
    }

    /* ============================================================
     * Approach 2: DC VLC for run + DC VLC for level
     * run_vlc gives # of zeros before next NZ
     * level_vlc gives the NZ value
     * size=0 on run = EOB
     * ============================================================ */
    printf("\n=== Approach 2: DC VLC(run) + DC VLC(level), size=0 run = EOB ===\n");
    {
        static int coeffs[900][64];
        memset(coeffs, 0, sizeof(coeffs));
        bitstream bs = {bsdata, total_bits, 0};
        int dp[3]={0,0,0};
        int total_nz=0, total_eob=0;
        int band_nz[8]={0};
        int run_hist[20]={0}, level_hist[20]={0};

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dp[comp] += read_dc_vlc(&bs, NULL);
            coeffs[b][0] = dp[comp] * 8;

            int pos = 1;
            while (pos < 64 && bs.pos < total_bits) {
                int rsz;
                int run = read_dc_vlc(&bs, &rsz);
                if (run == -9999) break;
                if (rsz == 0) { total_eob++; break; }  /* EOB */
                if (run < 0) run = -run;  /* run should be positive */
                pos += run;
                if (pos >= 64) break;

                int lsz;
                int level = read_dc_vlc(&bs, &lsz);
                if (level == -9999) break;
                if (lsz == 0) { /* level=0, skip? */ pos++; continue; }

                coeffs[b][zigzag[pos]] = level;
                int band = (pos-1)/8;
                if (band < 8) band_nz[band]++;
                if (run < 20) run_hist[run]++;
                if (abs(level) < 20) level_hist[abs(level)]++;
                total_nz++;
                pos++;
            }
        }
        printf("  Used: %d/%d bits (%.1f%%)\n", bs.pos, total_bits, 100.0*bs.pos/total_bits);
        printf("  NZ=%d, EOB=%d\n", total_nz, total_eob);
        printf("  Bands: ");
        for(int i=0;i<8;i++) printf("b%d=%d ",i,band_nz[i]); printf("\n");
        printf("  Runs: "); for(int i=0;i<10;i++) printf("r%d=%d ",i,run_hist[i]); printf("\n");
        printf("  Levels: "); for(int i=0;i<10;i++) printf("l%d=%d ",i,level_hist[i]); printf("\n");
    }

    /* ============================================================
     * Approach 3: Separate DC pass, then AC uses DC VLC per coefficient
     * Same as Approach 1 but with separate DC
     * ============================================================ */
    printf("\n=== Approach 3: Separate DC, then DC VLC per AC coeff (size=0=EOB) ===\n");
    {
        static int coeffs[900][64];
        memset(coeffs, 0, sizeof(coeffs));
        bitstream bs = {bsdata, total_bits, 0};
        int dp[3]={0,0,0};
        /* First pass: all DC */
        for(int b=0;b<nblocks&&bs.pos<total_bits;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dp[comp]+=read_dc_vlc(&bs,NULL);
            coeffs[b][0]=dp[comp]*8;
        }
        int dc_end = bs.pos;

        int total_nz=0, total_eob=0;
        int band_nz[8]={0};

        /* Second pass: AC using DC VLC */
        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            for (int p = 1; p < 64 && bs.pos < total_bits; p++) {
                int sz;
                int val = read_dc_vlc(&bs, &sz);
                if (val == -9999) break;
                if (sz == 0) { total_eob++; break; }
                coeffs[b][zigzag[p]] = val;
                if (val != 0) {
                    int band = (p-1)/8;
                    if (band<8) band_nz[band]++;
                    total_nz++;
                }
            }
        }
        printf("  DC: %d bits, AC used: %d bits, total: %d/%d (%.1f%%)\n",
               dc_end, bs.pos-dc_end, bs.pos, total_bits, 100.0*bs.pos/total_bits);
        printf("  NZ=%d, EOB=%d\n", total_nz, total_eob);
        printf("  Bands: ");
        for(int i=0;i<8;i++) printf("b%d=%d ",i,band_nz[i]); printf("\n");

        /* Write image */
        static int Yp[H][W], Cbp[H/2][W/2], Crp[H/2][W/2];
        memset(Yp,128,sizeof(Yp)); memset(Cbp,128,sizeof(Cbp)); memset(Crp,128,sizeof(Crp));
        for (int mb = 0; mb < mw*mh; mb++) {
            int mx2=mb%mw, my2=mb/mw;
            for (int s = 0; s < 6; s++) {
                int out[64];
                int dq[64]; memset(dq, 0, sizeof(dq));
                dq[0] = coeffs[mb*6+s][0];
                for (int i = 1; i < 64; i++) {
                    int qi = i % 16;
                    dq[i] = coeffs[mb*6+s][i] * qt[qi] * qs / 8;
                }
                idct8x8(dq, out);
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
        write_ppm("/tmp/ac_dcvlc3.ppm", Yp, Cbp, Crp);
        printf("  → /tmp/ac_dcvlc3.ppm\n");
    }

    /* ============================================================
     * Approach 4: DC VLC for run, then fixed-width level
     * For each NZ: read DC VLC as run, then read N bits for level
     * ============================================================ */
    printf("\n=== Approach 4: DC VLC(run) + fixed-width level ===\n");
    for (int level_bits = 2; level_bits <= 8; level_bits += 2) {
        bitstream bs = {bsdata, total_bits, 0};
        int dp[3]={0,0,0};
        for(int b=0;b<nblocks&&bs.pos<total_bits;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dp[comp]+=read_dc_vlc(&bs,NULL);
        }

        int total_nz=0, total_eob=0;
        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int pos = 1;
            while (pos < 64 && bs.pos < total_bits) {
                int rsz;
                int run = read_dc_vlc(&bs, &rsz);
                if (run == -9999) break;
                if (rsz == 0) { total_eob++; break; }
                if (run < 0) run = -run;
                pos += run;
                if (pos >= 64) break;
                int lv = bs_read(&bs, level_bits);
                if (lv < 0) break;
                total_nz++;
                pos++;
            }
        }
        printf("  %d-bit level: used %d/%d (%.1f%%), NZ=%d, EOB=%d\n",
               level_bits, bs.pos, total_bits, 100.0*bs.pos/total_bits,
               total_nz, total_eob);
    }

    /* ============================================================
     * Approach 5: MPEG chrominance DC VLC for AC
     * Different table: 00→0, 01→1, 10→2, 110→3, 1110→4, etc.
     * ============================================================ */
    printf("\n=== Approach 5: MPEG chrominance DC VLC for AC ===\n");
    {
        /* Chrominance DC table */
        static const struct { int len; uint32_t code; int size; } chroma_vlc[] = {
            {2, 0b00, 0}, {2, 0b01, 1}, {2, 0b10, 2},
            {3, 0b110, 3}, {4, 0b1110, 4}, {5, 0b11110, 5},
            {6, 0b111110, 6}, {7, 0b1111110, 7}, {8, 0b11111110, 8},
        };

        bitstream bs = {bsdata, total_bits, 0};
        int dp[3]={0,0,0};
        /* DC with luminance VLC */
        for(int b=0;b<nblocks&&bs.pos<total_bits;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dp[comp]+=read_dc_vlc(&bs,NULL);
        }
        int dc_end = bs.pos;

        /* AC with chrominance VLC */
        int total_nz=0, total_eob=0;
        int band_nz[8]={0};

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            for (int p = 1; p < 64 && bs.pos < total_bits; p++) {
                int matched = 0;
                for (int i = 0; i < 9; i++) {
                    int bits = bs_peek(&bs, chroma_vlc[i].len);
                    if (bits < 0) goto done5;
                    if (bits == (int)chroma_vlc[i].code) {
                        bs.pos += chroma_vlc[i].len;
                        int sz = chroma_vlc[i].size;
                        if (sz == 0) { total_eob++; matched=1; goto eob5; }
                        int val = bs_read(&bs, sz);
                        if (val < 0) goto done5;
                        if (val < (1 << (sz-1))) val -= (1<<sz)-1;
                        if (val != 0) {
                            int band=(p-1)/8;
                            if(band<8) band_nz[band]++;
                            total_nz++;
                        }
                        matched = 1;
                        break;
                    }
                }
                if (!matched) { bs.pos++; }
            }
            eob5:;
        }
        done5:;
        printf("  DC=%d bits, AC used %d/%d (%.1f%%), NZ=%d, EOB=%d\n",
               dc_end, bs.pos-dc_end, total_bits-dc_end,
               100.0*(bs.pos-dc_end)/(total_bits-dc_end), total_nz, total_eob);
        printf("  Bands: ");
        for(int i=0;i<8;i++) printf("b%d=%d ",i,band_nz[i]); printf("\n");
    }

    /* ============================================================
     * Approach 6: Verify with random data
     * ============================================================ */
    printf("\n=== Control: DC VLC per AC on RANDOM data ===\n");
    {
        uint8_t *rnd = malloc(bslen);
        srand(12345);
        for (int i = 0; i < bslen; i++) rnd[i] = rand() & 0xFF;

        bitstream bs = {rnd, total_bits, 0};
        /* Skip DC-equivalent bits */
        bs.pos = 4103;  /* approximate DC end */

        int total_nz=0, total_eob=0;
        int band_nz[8]={0};

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            for (int p = 1; p < 64 && bs.pos < total_bits; p++) {
                int sz;
                int val = read_dc_vlc(&bs, &sz);
                if (val == -9999) break;
                if (sz == 0) { total_eob++; break; }
                if (val != 0) {
                    int band=(p-1)/8; if(band<8) band_nz[band]++;
                    total_nz++;
                }
            }
        }
        printf("  Used %d/%d (%.1f%%), NZ=%d, EOB=%d\n",
               bs.pos, total_bits, 100.0*bs.pos/total_bits, total_nz, total_eob);
        printf("  Bands: ");
        for(int i=0;i<8;i++) printf("b%d=%d ",i,band_nz[i]); printf("\n");
        free(rnd);
    }

    /* ============================================================
     * Approach 7: Cross-frame consistency
     * ============================================================ */
    printf("\n=== Cross-frame: Approach 1 on multiple frames ===\n");
    for (int fi = 0; fi < nf && fi < 6; fi++) {
        uint8_t *ff=frames[fi]; int ffs=fsizes[fi];
        int fqs=ff[3], ft=ff[39];
        const uint8_t *fbd=ff+40;
        int fbl=ffs-40, ftb=fbl*8;

        bitstream bs = {fbd, ftb, 0};
        int dp[3]={0,0,0};
        int nz=0, eob=0;

        for (int b = 0; b < nblocks && bs.pos < ftb; b++) {
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dp[comp]+=read_dc_vlc(&bs,NULL);
            for (int p = 1; p < 64 && bs.pos < ftb; p++) {
                int sz;
                int val = read_dc_vlc(&bs, &sz);
                if (val == -9999) break;
                if (sz == 0) { eob++; break; }
                if (val != 0) nz++;
            }
        }
        printf("  F%02d qs=%2d t=%d: %d/%d bits (%.1f%%), NZ=%d, EOB=%d, NZ/blk=%.1f\n",
               fi, fqs, ft, bs.pos, ftb, 100.0*bs.pos/ftb, nz, eob, (double)nz/nblocks);
    }

    free(disc); zip_close(z);
    return 0;
}
