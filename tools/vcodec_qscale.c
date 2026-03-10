/*
 * vcodec_qscale.c - Test qscale-based dequantization formulas
 * Key insight: qs=8 gives perfect range without dequant.
 * Hypothesis: coefficients are pre-multiplied by qscale/8.
 * Test: divide by qscale/8 to normalize.
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

static void write_pgm(const char *p, const uint8_t *g, int w, int h) {
    FILE *f=fopen(p,"wb"); if(!f)return;
    fprintf(f,"P5\n%d %d\n255\n",w,h); fwrite(g,1,w*h,f); fclose(f);
}
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

static int vlc_coeff(BR *b) {
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

typedef struct {
    double dc_scale;
    double ac_scale;
    const char *desc;
} DequantMode;

static void test_dequant(const uint8_t *f, int fsize, int imgW, int imgH,
    DequantMode *mode, const char *tag) {
    int qscale = f[3];
    uint8_t qt[16]; memcpy(qt, f+4, 16);
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;

    /* Build 8x8 quantization matrix from 4x4 */
    double qm[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            qm[i*8+j] = qt[(i/2)*4 + (j/2)];

    BR br; br_init(&br, bs, bslen);
    double *planeY = calloc(imgW*imgH, sizeof(double));
    double *planeCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr = calloc((imgW/2)*(imgH/2), sizeof(double));
    double dc_y = 0, dc_cb = 0, dc_cr = 0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[64]; memset(block, 0, sizeof(block));
                dc_y += vlc_coeff(&br);
                block[0] = dc_y * mode->dc_scale;
                for (int i = 1; i < 64 && !br_eof(&br); i++) {
                    if (br_get1(&br)) {
                        double v = vlc_coeff(&br);
                        block[zz8[i]] = v * mode->ac_scale;
                    }
                }
                double spatial[64];
                idct8x8(block, spatial);
                int bx = mbx*2 + (yb&1), by = mby*2 + (yb>>1);
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                        planeY[(by*8+y)*imgW + bx*8+x] = spatial[y*8+x] + 128.0;
            }
            for (int c = 0; c < 2 && !br_eof(&br); c++) {
                double block[64]; memset(block, 0, sizeof(block));
                double *dc = (c==0) ? &dc_cb : &dc_cr;
                *dc += vlc_coeff(&br);
                block[0] = *dc * mode->dc_scale;
                for (int i = 1; i < 64 && !br_eof(&br); i++) {
                    if (br_get1(&br)) {
                        double v = vlc_coeff(&br);
                        block[zz8[i]] = v * mode->ac_scale;
                    }
                }
                double spatial[64];
                idct8x8(block, spatial);
                double *plane = (c==0) ? planeCb : planeCr;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                        plane[(mby*8+y)*(imgW/2) + mbx*8+x] = spatial[y*8+x];
            }
        }
    }

    /* Output Y */
    double pmin=1e9, pmax=-1e9;
    for(int i=0;i<imgW*imgH;i++){if(planeY[i]<pmin)pmin=planeY[i];if(planeY[i]>pmax)pmax=planeY[i];}

    uint8_t *img = malloc(imgW*imgH);
    for (int i = 0; i < imgW*imgH; i++) img[i] = clamp8((int)round(planeY[i]));
    char path[256];
    snprintf(path,sizeof(path),OUT_DIR "qs_%s.pgm",tag);
    write_pgm(path, img, imgW, imgH);

    /* RGB */
    uint8_t *rgb = malloc(imgW*imgH*3);
    for (int y = 0; y < imgH; y++)
        for (int x = 0; x < imgW; x++) {
            double yv = planeY[y*imgW+x];
            double cb = planeCb[(y/2)*(imgW/2)+x/2];
            double cr = planeCr[(y/2)*(imgW/2)+x/2];
            rgb[(y*imgW+x)*3+0] = clamp8((int)round(yv + 1.402*cr));
            rgb[(y*imgW+x)*3+1] = clamp8((int)round(yv - 0.344*cb - 0.714*cr));
            rgb[(y*imgW+x)*3+2] = clamp8((int)round(yv + 1.772*cb));
        }
    snprintf(path,sizeof(path),OUT_DIR "qs_%s_rgb.ppm",tag);
    write_ppm(path, rgb, imgW, imgH);

    printf("  %s (qs=%d): Y[%.0f, %.0f] %s\n", tag, qscale, pmin, pmax, mode->desc);
    free(img); free(rgb); free(planeY); free(planeCb); free(planeCr);
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;

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

    int lbas[] = {502, 1872, 3072, 5232};
    int imgW = 128, imgH = 144;

    for (int li = 0; li < 4; li++) {
        int lba = lbas[li];
        static uint8_t frames[4][MAX_FRAME]; int fsizes[4];
        int nf = assemble_frames(disc,tsec,lba,frames,fsizes,4);
        if (nf < 1) continue;

        int qs = frames[0][3];
        printf("\nLBA %d (qs=%d, type=%d):\n", lba, qs, frames[0][39]);

        /* Mode A: DC*1, AC*1 (baseline no-dequant) */
        DequantMode mA = {1.0, 1.0, "no dequant"};
        char tag[64]; snprintf(tag,sizeof(tag),"A_lba%d",lba);
        test_dequant(frames[0],fsizes[0],imgW,imgH, &mA, tag);

        /* Mode B: DC*1, AC * 8/qs */
        DequantMode mB = {1.0, 8.0/qs, "AC*8/qs"};
        snprintf(tag,sizeof(tag),"B_lba%d",lba);
        test_dequant(frames[0],fsizes[0],imgW,imgH, &mB, tag);

        /* Mode C: DC * qs/8, AC * 1 */
        DequantMode mC = {(double)qs/8.0, 1.0, "DC*qs/8"};
        snprintf(tag,sizeof(tag),"C_lba%d",lba);
        test_dequant(frames[0],fsizes[0],imgW,imgH, &mC, tag);

        /* Mode D: DC * qs/8, AC * 8/qs */
        DequantMode mD = {(double)qs/8.0, 8.0/qs, "DC*qs/8 AC*8/qs"};
        snprintf(tag,sizeof(tag),"D_lba%d",lba);
        test_dequant(frames[0],fsizes[0],imgW,imgH, &mD, tag);

        /* Mode E: all * qs/8 */
        DequantMode mE = {(double)qs/8.0, (double)qs/8.0, "all*qs/8"};
        snprintf(tag,sizeof(tag),"E_lba%d",lba);
        test_dequant(frames[0],fsizes[0],imgW,imgH, &mE, tag);

        /* Mode F: DC * 1, AC * 0 (DC only) */
        DequantMode mF = {1.0, 0.0, "DC only"};
        snprintf(tag,sizeof(tag),"F_lba%d",lba);
        test_dequant(frames[0],fsizes[0],imgW,imgH, &mF, tag);

        /* Mode G: DC * 1, AC * 0.5 (attenuated AC) */
        DequantMode mG = {1.0, 0.5, "AC*0.5"};
        snprintf(tag,sizeof(tag),"G_lba%d",lba);
        test_dequant(frames[0],fsizes[0],imgW,imgH, &mG, tag);

        /* Mode H: DC * 1, AC * 0.25 */
        DequantMode mH = {1.0, 0.25, "AC*0.25"};
        snprintf(tag,sizeof(tag),"H_lba%d",lba);
        test_dequant(frames[0],fsizes[0],imgW,imgH, &mH, tag);
    }

    free(disc); zip_close(z);
    return 0;
}
