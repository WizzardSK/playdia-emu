/*
 * vcodec_flagval.c - Test 1-bit flag + fixed-width value AC coding
 *
 * Hypothesis: AC coefficients coded as:
 *   0 = zero coefficient at current zigzag position
 *   1 + N-bit signed value = non-zero coefficient
 *
 * Evidence:
 * - r6 excess for 1-runs: flag(1) + value(-1)=11111 → 111111 (6 ones)
 * - r12 excess for 0-runs: 12 consecutive zero flags
 * - Math: 54432 × 1bit + ~9 NZ/block × (1+5) ≈ 93312 ≈ 93833 available
 *
 * Also test with EOB optimization and different value widths (4,5,6,7,8)
 * Also test interleaved DC+AC vs separate DC
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

static void decode_and_image(const uint8_t *bsdata, int data_end, int total_bits,
    int qs, int nblocks, int mw, int mh, int val_bits, const char *label, const char *fn,
    int interleaved) {
    static int coeffs[900][64];
    memset(coeffs, 0, sizeof(coeffs));
    bitstream bs = {bsdata, total_bits, 0};
    int dp[3]={0,0,0};
    int total_nz=0, total_zeros=0;
    int band_nz[8]={0};
    int val_hist[64]={0};

    if (!interleaved) {
        /* Separate DC pass first */
        for(int b=0;b<nblocks&&bs.pos<total_bits;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dp[comp]+=read_dc(&bs);
            coeffs[b][0]=dp[comp]*8;
        }
    }

    int dc_end = bs.pos;

    for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
        if (interleaved) {
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dp[comp]+=read_dc(&bs);
            coeffs[b][0]=dp[comp]*8;
        }

        for (int p = 1; p < 64 && bs.pos < total_bits; p++) {
            int flag = bs_read(&bs, 1);
            if (flag < 0) break;
            if (flag == 0) {
                total_zeros++;
                continue;
            }
            /* Non-zero: read val_bits for signed value */
            int raw = bs_read(&bs, val_bits);
            if (raw < 0) break;
            /* Sign extend */
            int half = 1 << (val_bits - 1);
            int val = (raw >= half) ? raw - (1 << val_bits) : raw;
            if (val == 0) val = half;  /* avoid zero for non-zero flag (map 0 → +half) */

            int zpos = zigzag[p];
            int qi = zpos % 16;
            coeffs[b][zpos] = val * qt[qi] * qs / 8;
            int band = (p-1)/8;
            if (band < 8) band_nz[band]++;
            total_nz++;
            if (abs(val) < 64) val_hist[abs(val)]++;
        }
    }

    int used = bs.pos;
    printf("  %s: used %d/%d (%.1f%%), NZ=%d, zeros=%d, NZ/blk=%.1f\n",
           label, used, total_bits, 100.0*used/total_bits,
           total_nz, total_zeros, (double)total_nz/nblocks);
    printf("    Bands: ");
    for(int i=0;i<8;i++) printf("b%d=%d ",i,band_nz[i]); printf("\n");
    printf("    Values: ");
    for(int i=0;i<10;i++) printf("|%d|=%d ",i,val_hist[i]); printf("\n");

    /* Write image */
    if (fn) {
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
        write_ppm(fn, Yp, Cbp, Crp);
        printf("    → %s\n", fn);
    }
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
    int bslen=fsize-40;

    /* Find real data end (before padding) */
    int data_end = bslen;
    for (int i = bslen-1; i >= 0; i--)
        if (bsdata[i] != 0xFF) { data_end = i+1; break; }
    int total_bits = data_end * 8;

    printf("Frame 0: qs=%d, data=%d bytes (%d bits), pad=%d bytes\n\n",
           qs, data_end, total_bits, bslen-data_end);

    /* ============================================================
     * Test different value bit widths (separate DC pass)
     * ============================================================ */
    printf("=== Separate DC + flag-value AC ===\n");
    for (int vb = 3; vb <= 8; vb++) {
        char label[32], fn[64];
        snprintf(label, sizeof(label), "%d-bit val (sep DC)", vb);
        snprintf(fn, sizeof(fn), "/tmp/ac_fv_sep_%d.ppm", vb);
        decode_and_image(bsdata, data_end, total_bits, qs, nblocks, mw, mh,
                        vb, label, (vb==5||vb==6) ? fn : NULL, 0);
    }

    /* ============================================================
     * Test interleaved DC+AC
     * ============================================================ */
    printf("\n=== Interleaved DC + flag-value AC ===\n");
    for (int vb = 3; vb <= 8; vb++) {
        char label[32], fn[64];
        snprintf(label, sizeof(label), "%d-bit val (interl)", vb);
        snprintf(fn, sizeof(fn), "/tmp/ac_fv_int_%d.ppm", vb);
        decode_and_image(bsdata, data_end, total_bits, qs, nblocks, mw, mh,
                        vb, label, (vb==5||vb==6) ? fn : NULL, 1);
    }

    /* ============================================================
     * Random data control for N=5
     * ============================================================ */
    printf("\n=== Random data control (5-bit, separate DC) ===\n");
    {
        uint8_t *rnd = malloc(bslen);
        srand(12345);
        for (int i = 0; i < bslen; i++) rnd[i] = rand() & 0xFF;
        decode_and_image(rnd, data_end, total_bits, qs, nblocks, mw, mh,
                        5, "5-bit RANDOM", NULL, 0);
        free(rnd);
    }

    /* ============================================================
     * Cross-frame consistency test with N=5
     * ============================================================ */
    printf("\n=== Cross-frame (5-bit separate DC) ===\n");
    for (int fi = 0; fi < nf && fi < 8; fi++) {
        uint8_t *ff=frames[fi]; int ffs=fsizes[fi];
        int fqs=ff[3], ft=ff[39];
        const uint8_t *fbd=ff+40;
        int fbl=ffs-40;

        int de = fbl;
        for(int i=fbl-1;i>=0;i--) if(fbd[i]!=0xFF){de=i+1;break;}
        int ftb=de*8;

        static int coeffs[900][64];
        memset(coeffs,0,sizeof(coeffs));
        bitstream bs={fbd,ftb,0};
        int dp[3]={0,0,0};
        for(int b=0;b<nblocks&&bs.pos<ftb;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dp[comp]+=read_dc(&bs);
            coeffs[b][0]=dp[comp]*8;
        }

        int nz=0, zeros=0;
        int band_nz[8]={0};
        for(int b=0;b<nblocks&&bs.pos<ftb;b++){
            for(int p=1;p<64&&bs.pos<ftb;p++){
                int flag=bs_read(&bs,1);
                if(flag<0) break;
                if(flag==0){zeros++;continue;}
                int raw=bs_read(&bs,5);
                if(raw<0) break;
                int half=1<<4;
                int val=(raw>=half)?raw-32:raw;
                if(val==0)val=half;
                int band=(p-1)/8; if(band<8)band_nz[band]++;
                nz++;
            }
        }
        printf("  F%02d qs=%2d t=%d: %d/%d (%.1f%%) NZ=%d z=%d NZ/b=%.1f bands=%d,%d,%d,%d,%d,%d,%d,%d\n",
               fi,fqs,ft,bs.pos,ftb,100.0*bs.pos/ftb,nz,zeros,(double)nz/nblocks,
               band_nz[0],band_nz[1],band_nz[2],band_nz[3],
               band_nz[4],band_nz[5],band_nz[6],band_nz[7]);
    }

    /* ============================================================
     * Test with DC VLC for value instead of fixed-width
     * 0 = zero, 1 + DC VLC = non-zero value
     * ============================================================ */
    printf("\n=== Flag + DC VLC value ===\n");
    {
        static int coeffs[900][64];
        memset(coeffs,0,sizeof(coeffs));
        bitstream bs={bsdata,total_bits,0};
        int dp[3]={0,0,0};
        for(int b=0;b<nblocks&&bs.pos<total_bits;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dp[comp]+=read_dc(&bs);
            coeffs[b][0]=dp[comp]*8;
        }

        int nz=0, zeros=0;
        int band_nz[8]={0};
        int val_hist[20]={0};

        for(int b=0;b<nblocks&&bs.pos<total_bits;b++){
            for(int p=1;p<64&&bs.pos<total_bits;p++){
                int flag=bs_read(&bs,1);
                if(flag<0) break;
                if(flag==0){zeros++;continue;}
                int val=read_dc(&bs);
                if(val==-9999) break;
                if(val==0){zeros++;continue;} /* size=0 → zero? */
                int zpos=zigzag[p];
                int qi=zpos%16;
                coeffs[b][zpos]=val*qt[qi]*qs/8;
                int band=(p-1)/8; if(band<8)band_nz[band]++;
                if(abs(val)<20) val_hist[abs(val)]++;
                nz++;
            }
        }
        printf("  Used %d/%d (%.1f%%), NZ=%d, zeros=%d, NZ/blk=%.1f\n",
               bs.pos,total_bits,100.0*bs.pos/total_bits,nz,zeros,(double)nz/nblocks);
        printf("  Bands: ");for(int i=0;i<8;i++)printf("b%d=%d ",i,band_nz[i]);printf("\n");
        printf("  Values: ");for(int i=0;i<10;i++)printf("|%d|=%d ",i,val_hist[i]);printf("\n");

        static int Yp[H][W], Cbp[H/2][W/2], Crp[H/2][W/2];
        memset(Yp,128,sizeof(Yp));memset(Cbp,128,sizeof(Cbp));memset(Crp,128,sizeof(Crp));
        for(int mb=0;mb<mw*mh;mb++){
            int mx2=mb%mw,my2=mb/mw;
            for(int s=0;s<6;s++){
                int out[64];
                idct8x8(coeffs[mb*6+s],out);
                if(s<4){int bx=(s&1)*8,by=(s>>1)*8;
                    for(int r=0;r<8;r++)for(int c=0;c<8;c++){
                        int py=my2*16+by+r,px=mx2*16+bx+c;
                        if(py<H&&px<W)Yp[py][px]=clamp(out[r*8+c]+128);}}
                else if(s==4){for(int r=0;r<8;r++)for(int c=0;c<8;c++){
                        int py=my2*8+r,px=mx2*8+c;
                        if(py<H/2&&px<W/2)Cbp[py][px]=clamp(out[r*8+c]+128);}}
                else{for(int r=0;r<8;r++)for(int c=0;c<8;c++){
                        int py=my2*8+r,px=mx2*8+c;
                        if(py<H/2&&px<W/2)Crp[py][px]=clamp(out[r*8+c]+128);}}
            }
        }
        write_ppm("/tmp/ac_flagvlc.ppm",Yp,Cbp,Crp);
        printf("  → /tmp/ac_flagvlc.ppm\n");
    }

    free(disc); zip_close(z);
    return 0;
}
