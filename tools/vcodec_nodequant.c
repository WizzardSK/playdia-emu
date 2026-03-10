/*
 * vcodec_nodequant.c - Test hypothesis: VLC values ARE the actual DCT
 * coefficients (no dequantization needed). Try with DC level shift +128.
 *
 * Also test: DC * small_factor (2, 4, 8) to see which gives best contrast.
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

static void idct8x8(int block[64], double out[64]) {
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

static void decode_and_output(const uint8_t *f, int fsize, int imgW, int imgH,
    int dc_scale, int ac_scale_num, int ac_scale_den, int dc_offset,
    const char *name, int use_chroma) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;

    BR br; br_init(&br, bs, bslen);
    int *planeY = calloc(imgW*imgH, sizeof(int));
    int *planeCb = calloc((imgW/2)*(imgH/2), sizeof(int));
    int *planeCr = calloc((imgW/2)*(imgH/2), sizeof(int));
    int dc_y = 0, dc_cb = 0, dc_cr = 0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            /* 4 Y blocks */
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                int block[64];
                memset(block, 0, sizeof(block));
                dc_y += vlc_coeff(&br);
                block[0] = dc_y * dc_scale;
                for (int i = 1; i < 64 && !br_eof(&br); i++)
                    if (br_get1(&br))
                        block[zz8[i]] = vlc_coeff(&br) * ac_scale_num / ac_scale_den;

                double spatial[64];
                idct8x8(block, spatial);

                int bx = mbx*2 + (yb&1);
                int by = mby*2 + (yb>>1);
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                        planeY[(by*8+y)*imgW + bx*8+x] = (int)round(spatial[y*8+x]) + dc_offset;
            }
            /* Cb block */
            {
                int block[64];
                memset(block, 0, sizeof(block));
                dc_cb += vlc_coeff(&br);
                block[0] = dc_cb * dc_scale;
                for (int i = 1; i < 64 && !br_eof(&br); i++)
                    if (br_get1(&br))
                        block[zz8[i]] = vlc_coeff(&br) * ac_scale_num / ac_scale_den;
                if (use_chroma) {
                    double spatial[64];
                    idct8x8(block, spatial);
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x++)
                            planeCb[(mby*8+y)*(imgW/2) + mbx*8+x] = (int)round(spatial[y*8+x]);
                }
            }
            /* Cr block */
            {
                int block[64];
                memset(block, 0, sizeof(block));
                dc_cr += vlc_coeff(&br);
                block[0] = dc_cr * dc_scale;
                for (int i = 1; i < 64 && !br_eof(&br); i++)
                    if (br_get1(&br))
                        block[zz8[i]] = vlc_coeff(&br) * ac_scale_num / ac_scale_den;
                if (use_chroma) {
                    double spatial[64];
                    idct8x8(block, spatial);
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x++)
                            planeCr[(mby*8+y)*(imgW/2) + mbx*8+x] = (int)round(spatial[y*8+x]);
                }
            }
        }
    }
    printf("  %s: bits %d/%d (%.1f%%)\n", name, br.total, bslen*8, 100.0*br.total/(bslen*8));

    /* Y output - direct clamp */
    {
        uint8_t *img = malloc(imgW*imgH);
        for (int i = 0; i < imgW*imgH; i++) img[i] = clamp8(planeY[i]);
        char path[256];
        snprintf(path, sizeof(path), OUT_DIR "%s.pgm", name);
        write_pgm(path, img, imgW, imgH);

        int pmin=99999,pmax=-99999;
        for(int i=0;i<imgW*imgH;i++){if(planeY[i]<pmin)pmin=planeY[i];if(planeY[i]>pmax)pmax=planeY[i];}
        printf("    Y range [%d,%d]\n", pmin, pmax);
        free(img);
    }

    /* Y auto-scaled */
    {
        int pmin=99999,pmax=-99999;
        for(int i=0;i<imgW*imgH;i++){if(planeY[i]<pmin)pmin=planeY[i];if(planeY[i]>pmax)pmax=planeY[i];}
        uint8_t *img = malloc(imgW*imgH);
        if (pmax > pmin)
            for(int i=0;i<imgW*imgH;i++) img[i]=clamp8(255*(planeY[i]-pmin)/(pmax-pmin));
        else memset(img, 128, imgW*imgH);
        char path[256];
        snprintf(path, sizeof(path), OUT_DIR "%s_auto.pgm", name);
        write_pgm(path, img, imgW, imgH);
        free(img);
    }

    /* YCbCr → RGB if chroma */
    if (use_chroma) {
        uint8_t *rgb = malloc(imgW*imgH*3);
        for (int y = 0; y < imgH; y++)
            for (int x = 0; x < imgW; x++) {
                int yv = planeY[y*imgW+x];
                int cb = planeCb[(y/2)*(imgW/2)+x/2];
                int cr = planeCr[(y/2)*(imgW/2)+x/2];
                rgb[(y*imgW+x)*3+0] = clamp8(yv + (int)(1.402*cr));
                rgb[(y*imgW+x)*3+1] = clamp8(yv - (int)(0.344*cb) - (int)(0.714*cr));
                rgb[(y*imgW+x)*3+2] = clamp8(yv + (int)(1.772*cb));
            }
        char path[256];
        snprintf(path, sizeof(path), OUT_DIR "%s_rgb.ppm", name);
        write_ppm(path, rgb, imgW, imgH);
        free(rgb);
    }

    free(planeY); free(planeCb); free(planeCr);
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

    int lbas[] = {5232, 1872, 502, 3072};
    for (int li = 0; li < 4; li++) {
        int lba = lbas[li];
        static uint8_t frames[8][MAX_FRAME]; int fsizes[8];
        int nf = assemble_frames(disc,tsec,lba,frames,fsizes,8);
        if (nf < 1) continue;

        printf("\n=== LBA %d: qs=%d type=%d fsize=%d ===\n", lba, frames[0][3], frames[0][39], fsizes[0]);

        char name[64];
        /* Test: no dequant, DC*1, offset +128 */
        snprintf(name,sizeof(name),"ndq_dc1_lba%d",lba);
        decode_and_output(frames[0],fsizes[0],128,144, 1,1,1, 128, name, 0);

        /* Test: DC*2, AC*1, offset +128 */
        snprintf(name,sizeof(name),"ndq_dc2_lba%d",lba);
        decode_and_output(frames[0],fsizes[0],128,144, 2,1,1, 128, name, 0);

        /* Test: DC*4, AC*1, offset +128 */
        snprintf(name,sizeof(name),"ndq_dc4_lba%d",lba);
        decode_and_output(frames[0],fsizes[0],128,144, 4,1,1, 128, name, 0);

        /* Test: DC*8, AC*1, offset +128 */
        snprintf(name,sizeof(name),"ndq_dc8_lba%d",lba);
        decode_and_output(frames[0],fsizes[0],128,144, 8,1,1, 128, name, 1);

        /* Test: DC*1, AC*1, offset 0 (pure no-offset) */
        snprintf(name,sizeof(name),"ndq_nooff_lba%d",lba);
        decode_and_output(frames[0],fsizes[0],128,144, 1,1,1, 0, name, 0);

        /* Test: DC*8, AC*2, offset +128 */
        snprintf(name,sizeof(name),"ndq_dc8ac2_lba%d",lba);
        decode_and_output(frames[0],fsizes[0],128,144, 8,2,1, 128, name, 0);

        /* Test frame 1 (inter frame) if available */
        if (nf > 1) {
            snprintf(name,sizeof(name),"ndq_dc8_f1_lba%d",lba);
            decode_and_output(frames[1],fsizes[1],128,144, 8,1,1, 128, name, 0);
        }
    }

    free(disc); zip_close(z);
    return 0;
}
