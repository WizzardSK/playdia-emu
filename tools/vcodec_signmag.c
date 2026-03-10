/*
 * vcodec_signmag.c - Test sign+magnitude VLC for AC coefficients
 *
 * KEY INSIGHT: Most frames use 100% of bitstream (no padding). This means
 * the encoding IS variable-length, and fixed-width's "clean" images are
 * just from value clamping.
 *
 * The "flag=1, VLC=0" anomaly (wastes 4 bits for zero) suggests the
 * VLC interpretation is wrong. What if flag=1 means:
 * - Read 1 sign bit (0=+, 1=-)
 * - Read unsigned VLC for magnitude (0="100"=mag 0? No, use different VLC)
 *
 * Or: what if "flag" isn't really a flag but the FIRST BIT of a VLC that
 * has a different tree structure for AC?
 *
 * Test: what if the per-AC coding is actually a 2-BIT field:
 * 00 = zero (skip)
 * 01 = +1
 * 10 = -1
 * 11 = read full VLC for value (larger than ±1)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <zip.h>
#include <math.h>

#define SECTOR_RAW 2352
#define MAX_FRAME  65536
#define PI 3.14159265358979323846
#define OUT_DIR "/home/wizzard/share/GitHub/playdia-emu/tools/test_output/"

static void write_ppm(const char *p, const uint8_t *rgb, int w, int h) {
    FILE *f=fopen(p,"wb"); if(!f)return;
    fprintf(f,"P6\n%d %d\n255\n",w,h); fwrite(rgb,1,w*h*3,f); fclose(f);
}

typedef struct { const uint8_t *data; int len,pos,bit,total; } BR;
static void br_init(BR *b, const uint8_t *d, int l) { b->data=d;b->len=l;b->pos=0;b->bit=7;b->total=0; }
static int br_eof(BR *b) { return b->pos>=b->len; }
static int br_get1(BR *b) {
    if(b->pos>=b->len) return 0;
    int v=(b->data[b->pos]>>b->bit)&1;
    if(--b->bit<0){b->bit=7;b->pos++;}
    b->total++; return v;
}
static int br_get(BR *b, int n) { int v=0; for(int i=0;i<n;i++) v=(v<<1)|br_get1(b); return v; }

static int vlc_dc(BR *b) {
    int size;
    if (br_get1(b) == 0) { size = br_get1(b) ? 2 : 1; }
    else {
        if (br_get1(b) == 0) { size = br_get1(b) ? 3 : 0; }
        else {
            if (br_get1(b) == 0) size = 4;
            else if (br_get1(b) == 0) size = 5;
            else if (br_get1(b) == 0) size = 6;
            else size = br_get1(b) ? 8 : 7;
        }
    }
    if (size == 0) return 0;
    int val = br_get(b, size);
    if (val < (1 << (size-1))) val = val - (1 << size) + 1;
    return val;
}

static const int zz8[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

static void idct8x8(double block[64], double out[64]) {
    double tmp[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) {
            double sum = 0;
            for (int k = 0; k < 8; k++) {
                double ck = (k == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += ck * block[i*8+k] * cos((2*j+1)*k*PI/16.0);
            }
            tmp[i*8+j] = sum * 0.5;
        }
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) {
            double sum = 0;
            for (int k = 0; k < 8; k++) {
                double ck = (k == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += ck * tmp[k*8+j] * cos((2*i+1)*k*PI/16.0);
            }
            out[i*8+j] = sum * 0.5;
        }
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

static int clamp8(int v) { return v<0?0:v>255?255:v; }

static void render(double *planeY, double *planeCb, double *planeCr,
    int imgW, int imgH, const char *name) {
    uint8_t *rgb = malloc(imgW*imgH*3);
    double pmin=1e9, pmax=-1e9;
    for(int i=0;i<imgW*imgH;i++){if(planeY[i]<pmin)pmin=planeY[i];if(planeY[i]>pmax)pmax=planeY[i];}
    for (int y=0;y<imgH;y++) for(int x=0;x<imgW;x++) {
        double yv=planeY[y*imgW+x], cb=planeCb[(y/2)*(imgW/2)+x/2], cr=planeCr[(y/2)*(imgW/2)+x/2];
        rgb[(y*imgW+x)*3+0]=clamp8((int)round(yv+1.402*cr));
        rgb[(y*imgW+x)*3+1]=clamp8((int)round(yv-0.344*cb-0.714*cr));
        rgb[(y*imgW+x)*3+2]=clamp8((int)round(yv+1.772*cb));
    }
    char path[256];
    snprintf(path,sizeof(path),OUT_DIR "sm_%s.ppm",name);
    write_ppm(path, rgb, imgW, imgH);
    printf("  %s: Y[%.0f, %.0f]\n", name, pmin, pmax);
    free(rgb);
}

/* Model 1: 2-bit AC code: 00=0, 01=+1, 10=-1, 11=read VLC */
static void test_2bit_code(const uint8_t *f, int fsize, int imgW, int imgH, const char *tag) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double *planeY = calloc(imgW*imgH, sizeof(double));
    double *planeCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr = calloc((imgW/2)*(imgH/2), sizeof(double));
    double dc_y=0, dc_cb=0, dc_cr=0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[64]; memset(block,0,sizeof(block));
                dc_y += vlc_dc(&br);
                block[0] = dc_y;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    int code = br_get(&br, 2);
                    if (code == 0) block[zz8[i]] = 0;
                    else if (code == 1) block[zz8[i]] = 1;
                    else if (code == 2) block[zz8[i]] = -1;
                    else block[zz8[i]] = vlc_dc(&br); /* 11 = full VLC */
                }
                double spatial[64]; idct8x8(block, spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double block[64]; memset(block,0,sizeof(block));
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_dc(&br);
                block[0] = *dc;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    int code = br_get(&br, 2);
                    if (code == 0) block[zz8[i]] = 0;
                    else if (code == 1) block[zz8[i]] = 1;
                    else if (code == 2) block[zz8[i]] = -1;
                    else block[zz8[i]] = vlc_dc(&br);
                }
                double spatial[64]; idct8x8(block, spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
            }
        }
    }
    printf("  %s: bits %d/%d (%.1f%%)\n", tag, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render(planeY, planeCb, planeCr, imgW, imgH, tag);
    free(planeY); free(planeCb); free(planeCr);
}

/* Model 2: flag=0: 0, flag=1: sign bit + unsigned magnitude VLC
   Unsigned mag VLC: 1=mag1, 01=mag2, 001=mag3, etc. (unary) */
static void test_sign_unary(const uint8_t *f, int fsize, int imgW, int imgH, const char *tag) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double *planeY = calloc(imgW*imgH, sizeof(double));
    double *planeCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr = calloc((imgW/2)*(imgH/2), sizeof(double));
    double dc_y=0, dc_cb=0, dc_cr=0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[64]; memset(block,0,sizeof(block));
                dc_y += vlc_dc(&br);
                block[0] = dc_y;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    if (br_get1(&br)) { /* nonzero */
                        int sign = br_get1(&br); /* 0=+, 1=- */
                        /* Unary magnitude: count zeros before 1 */
                        int mag = 1;
                        while (!br_get1(&br) && mag < 32 && !br_eof(&br))
                            mag++;
                        block[zz8[i]] = sign ? -mag : mag;
                    }
                }
                double spatial[64]; idct8x8(block, spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double block[64]; memset(block,0,sizeof(block));
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_dc(&br);
                block[0] = *dc;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    if (br_get1(&br)) {
                        int sign = br_get1(&br);
                        int mag = 1;
                        while (!br_get1(&br) && mag < 32 && !br_eof(&br))
                            mag++;
                        block[zz8[i]] = sign ? -mag : mag;
                    }
                }
                double spatial[64]; idct8x8(block, spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
            }
        }
    }
    printf("  %s: bits %d/%d (%.1f%%)\n", tag, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render(planeY, planeCb, planeCr, imgW, imgH, tag);
    free(planeY); free(planeCb); free(planeCr);
}

/* Model 3: Same VLC as DC but reinterpreted —
   "0" prefix = short value, "1" prefix = longer
   0 + 0 = +1 (2 bits)
   0 + 1 = -1 (2 bits) OR size=2 values
   Actually, let me try: what if AC VLC is just sign + DC_magnitude_VLC?
   Where magnitude VLC (no zero): 0=1, 10=2, 110=3, 1110=4...
   So: read sign bit, then unary-coded magnitude starting from 1 */
static void test_sign_dcmag(const uint8_t *f, int fsize, int imgW, int imgH, const char *tag) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double *planeY = calloc(imgW*imgH, sizeof(double));
    double *planeCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr = calloc((imgW/2)*(imgH/2), sizeof(double));
    double dc_y=0, dc_cb=0, dc_cr=0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[64]; memset(block,0,sizeof(block));
                dc_y += vlc_dc(&br);
                block[0] = dc_y;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    if (br_get1(&br)) { /* flag=1: nonzero */
                        int sign = br_get1(&br); /* 0=+, 1=- */
                        /* Read DC magnitude VLC for unsigned magnitude */
                        int size;
                        if (br_get1(&br) == 0) { size = br_get1(&br) ? 2 : 1; }
                        else {
                            if (br_get1(&br) == 0) { size = br_get1(&br) ? 3 : 0; }
                            else {
                                if (br_get1(&br) == 0) size = 4;
                                else if (br_get1(&br) == 0) size = 5;
                                else if (br_get1(&br) == 0) size = 6;
                                else size = br_get1(&br) ? 8 : 7;
                            }
                        }
                        int mag;
                        if (size == 0) mag = 0; /* shouldn't happen, but... */
                        else {
                            mag = br_get(&br, size);
                            /* For unsigned magnitude, no sign extension needed */
                            /* Just use the raw value + 1 to avoid zero */
                            if (mag == 0) mag = 1 << (size-1); /* min value for this size */
                        }
                        block[zz8[i]] = sign ? -mag : mag;
                    }
                }
                double spatial[64]; idct8x8(block, spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double block[64]; memset(block,0,sizeof(block));
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_dc(&br);
                block[0] = *dc;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    if (br_get1(&br)) {
                        int sign = br_get1(&br);
                        int size;
                        if (br_get1(&br) == 0) { size = br_get1(&br) ? 2 : 1; }
                        else {
                            if (br_get1(&br) == 0) { size = br_get1(&br) ? 3 : 0; }
                            else {
                                if (br_get1(&br) == 0) size = 4;
                                else if (br_get1(&br) == 0) size = 5;
                                else if (br_get1(&br) == 0) size = 6;
                                else size = br_get1(&br) ? 8 : 7;
                            }
                        }
                        int mag;
                        if (size == 0) mag = 0;
                        else {
                            mag = br_get(&br, size);
                            if (mag == 0) mag = 1 << (size-1);
                        }
                        block[zz8[i]] = sign ? -mag : mag;
                    }
                }
                double spatial[64]; idct8x8(block, spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
            }
        }
    }
    printf("  %s: bits %d/%d (%.1f%%)\n", tag, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render(planeY, planeCb, planeCr, imgW, imgH, tag);
    free(planeY); free(planeCb); free(planeCr);
}

/* Model 4: Verify AC distribution with the ORIGINAL flag model,
   but check if low freq has different stats than high freq
   when looking at consecutive INTER frames */
static void test_ac_dist_detail(const uint8_t *f, int fsize) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double dc_y=0;
    /* Track per-zigzag-position statistics */
    int nz_count[64] = {0};
    int sum_abs[64] = {0};
    int max_abs[64] = {0};
    int zero_vlc_count[64] = {0}; /* flag=1 but VLC=0 */
    int total_y_blocks = 0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                dc_y += vlc_dc(&br);
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    if (br_get1(&br)) {
                        int v = vlc_dc(&br);
                        if (v == 0) {
                            zero_vlc_count[i]++;
                        }
                        nz_count[i]++;
                        sum_abs[i] += abs(v);
                        if (abs(v) > max_abs[i]) max_abs[i] = abs(v);
                    }
                }
                total_y_blocks++;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                vlc_dc(&br);
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    if (br_get1(&br)) vlc_dc(&br);
                }
            }
        }
    }

    printf("\n  Detailed AC stats (Y only, %d blocks):\n", total_y_blocks);
    printf("  ZZpos | Freq | NZ%% | AvgAbs | MaxAbs | Flag1Val0%%\n");
    for (int i = 1; i < 64; i++) {
        int r = zz8[i] / 8, c = zz8[i] % 8;
        double nzpct = 100.0*nz_count[i]/total_y_blocks;
        double avg = nz_count[i] ? (double)sum_abs[i]/nz_count[i] : 0;
        double zvpct = nz_count[i] ? 100.0*zero_vlc_count[i]/nz_count[i] : 0;
        printf("  %2d    | (%d,%d)| %4.1f | %5.1f  | %4d   | %4.1f\n",
            i, r, c, nzpct, avg, max_abs[i], zvpct);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    int slba = argc > 2 ? atoi(argv[2]) : 5232;

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

    static uint8_t frames[4][MAX_FRAME]; int fsizes[4];
    int nf = assemble_frames(disc,tsec,slba,frames,fsizes,4);
    if (nf < 1) return 1;

    int imgW = 128, imgH = 144;
    printf("LBA %d: qs=%d type=%d\n", slba, frames[0][3], frames[0][39]);

    printf("\n--- 2-bit AC code (00=0, 01=+1, 10=-1, 11=VLC) ---\n");
    test_2bit_code(frames[0],fsizes[0],imgW,imgH, "2bit_code");

    printf("\n--- Sign + unary magnitude ---\n");
    test_sign_unary(frames[0],fsizes[0],imgW,imgH, "sign_unary");

    printf("\n--- Sign + DC magnitude VLC ---\n");
    test_sign_dcmag(frames[0],fsizes[0],imgW,imgH, "sign_dcmag");

    printf("\n--- Detailed AC distribution ---\n");
    test_ac_dist_detail(frames[0],fsizes[0]);

    free(disc); zip_close(z);
    return 0;
}
