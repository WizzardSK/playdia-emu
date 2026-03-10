/*
 * vcodec_actest.c - Test AC coefficient placement
 * Try DC-only, then gradually add AC coefficients
 * Also test: no zigzag, reversed zigzag, H.261 zigzag
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

/* Standard MPEG zigzag */
static const int zz_std[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

/* Identity (row-major, no zigzag) */
static int zz_id[64];

/* MPEG-2 alternate scan */
static const int zz_alt[64] = {
     0, 8,16,24, 1, 9, 2,10,17,25,32,40,48,56,57,49,
    41,33,26,18, 3,11, 4,12,19,27,34,42,50,58,35,43,
    51,59,20,28, 5,13, 6,14,21,29,36,44,52,60,37,45,
    53,61,22,30, 7,15,23,31,38,46,54,62,39,47,55,63};

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

static void full_decode(const uint8_t *f, int fsize, int imgW, int imgH,
    const int *zigzag, int max_ac, int dc_mult, int ac_mult_num, int ac_mult_den,
    const char *name, int do_color) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;

    BR br; br_init(&br, bs, bslen);
    int *planeY = calloc(imgW*imgH, sizeof(int));
    int *planeCb = calloc((imgW/2)*(imgH/2), sizeof(int));
    int *planeCr = calloc((imgW/2)*(imgH/2), sizeof(int));
    int dc_y = 0, dc_cb = 0, dc_cr = 0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                int block[64]; memset(block, 0, sizeof(block));
                dc_y += vlc_coeff(&br);
                block[0] = dc_y * dc_mult;
                for (int i = 1; i < 64 && !br_eof(&br); i++) {
                    if (br_get1(&br)) {
                        int v = vlc_coeff(&br);
                        if (i <= max_ac)
                            block[zigzag[i]] = v * ac_mult_num / ac_mult_den;
                    }
                }
                double spatial[64];
                idct8x8(block, spatial);
                int bx = mbx*2 + (yb&1), by = mby*2 + (yb>>1);
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                        planeY[(by*8+y)*imgW + bx*8+x] = (int)round(spatial[y*8+x]) + 128;
            }
            for (int c = 0; c < 2 && !br_eof(&br); c++) {
                int block[64]; memset(block, 0, sizeof(block));
                int *dc = (c==0) ? &dc_cb : &dc_cr;
                *dc += vlc_coeff(&br);
                block[0] = *dc * dc_mult;
                for (int i = 1; i < 64 && !br_eof(&br); i++) {
                    if (br_get1(&br)) {
                        int v = vlc_coeff(&br);
                        if (i <= max_ac)
                            block[zigzag[i]] = v * ac_mult_num / ac_mult_den;
                    }
                }
                if (do_color) {
                    double spatial[64];
                    idct8x8(block, spatial);
                    int *plane = (c==0) ? planeCb : planeCr;
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x++)
                            plane[(mby*8+y)*(imgW/2) + mbx*8+x] = (int)round(spatial[y*8+x]);
                }
            }
        }
    }

    printf("  %s: bits %d/%d (%.1f%%)\n", name, br.total, bslen*8, 100.0*br.total/(bslen*8));

    /* Y clamped output */
    uint8_t *img = malloc(imgW*imgH);
    int pmin=99999,pmax=-99999;
    for(int i=0;i<imgW*imgH;i++){if(planeY[i]<pmin)pmin=planeY[i];if(planeY[i]>pmax)pmax=planeY[i];}
    for (int i = 0; i < imgW*imgH; i++) img[i] = clamp8(planeY[i]);
    char path[256];
    snprintf(path,sizeof(path),OUT_DIR "%s.pgm",name);
    write_pgm(path, img, imgW, imgH);
    printf("    Y [%d,%d]\n", pmin, pmax);

    /* RGB if color */
    if (do_color) {
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
        snprintf(path,sizeof(path),OUT_DIR "%s_rgb.ppm",name);
        write_ppm(path, rgb, imgW, imgH);
        free(rgb);
    }

    free(img); free(planeY); free(planeCb); free(planeCr);
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    int slba = argc > 2 ? atoi(argv[2]) : 5232;

    for (int i = 0; i < 64; i++) zz_id[i] = i;

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

    /* DC-only (no AC) - clean thumbnail */
    full_decode(frames[0],fsizes[0],imgW,imgH, zz_std,0, 1,1,1, "act_dconly",1);

    /* Standard zigzag, first 3 AC */
    full_decode(frames[0],fsizes[0],imgW,imgH, zz_std,3, 1,1,1, "act_ac3_std",0);

    /* Standard zigzag, first 10 AC */
    full_decode(frames[0],fsizes[0],imgW,imgH, zz_std,10, 1,1,1, "act_ac10_std",0);

    /* Standard zigzag, all AC */
    full_decode(frames[0],fsizes[0],imgW,imgH, zz_std,63, 1,1,1, "act_ac63_std",1);

    /* Identity zigzag (no reorder), all AC */
    full_decode(frames[0],fsizes[0],imgW,imgH, zz_id,63, 1,1,1, "act_ac63_id",0);

    /* Alt scan zigzag, all AC */
    full_decode(frames[0],fsizes[0],imgW,imgH, zz_alt,63, 1,1,1, "act_ac63_alt",0);

    /* Standard zigzag, all AC, DC*2 AC*1 */
    full_decode(frames[0],fsizes[0],imgW,imgH, zz_std,63, 2,1,1, "act_dc2ac1",1);

    /* Standard zigzag, all AC, DC*4 AC*1 */
    full_decode(frames[0],fsizes[0],imgW,imgH, zz_std,63, 4,1,1, "act_dc4ac1",0);

    /* Standard zigzag, all AC, with qtable dequant on AC only */
    /* Actually test: DC*1, AC * qt[zigzag_pos]/16 */
    /* This needs custom decode - skip for now */

    /* Try with both games */
    if (argc > 3 && strcmp(argv[3], "multi") == 0) {
        int lbas[] = {502, 1872, 3072};
        for (int i = 0; i < 3; i++) {
            nf = assemble_frames(disc,tsec,lbas[i],frames,fsizes,4);
            if (nf < 1) continue;
            printf("\nLBA %d: qs=%d type=%d\n", lbas[i], frames[0][3], frames[0][39]);
            char name[64];
            snprintf(name,sizeof(name),"act_dconly_lba%d",lbas[i]);
            full_decode(frames[0],fsizes[0],imgW,imgH, zz_std,0, 1,1,1, name,1);
            snprintf(name,sizeof(name),"act_full_lba%d",lbas[i]);
            full_decode(frames[0],fsizes[0],imgW,imgH, zz_std,63, 1,1,1, name,1);
        }
    }

    free(disc); zip_close(z);
    return 0;
}
