/*
 * vcodec_flageob.c - Test: per-AC flag where flag=1,VLC=0 means EOB
 *
 * Models:
 * A: Standard per-AC flag (baseline)
 * B: flag=1,VLC=0 → EOB (rest of block is zero)
 * C: flag=1,VLC=0 → skip position, don't place value (treat as run marker)
 * D: 4x4 blocks with per-AC flag (16 entries per block, 1728 blocks)
 * E: 4x4 blocks with flag=1,VLC=0 → EOB
 * F: Separate DC pass then AC pass (all DCs first for all blocks, then ACs)
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

static const int zz4[16] = {
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};

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

static void idct4x4(double block[16], double out[16]) {
    double tmp[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double sum = 0;
            for (int k = 0; k < 4; k++) {
                double ck = (k == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += ck * block[i*4+k] * cos((2*j+1)*k*PI/8.0);
            }
            tmp[i*4+j] = sum * 0.5;
        }
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            double sum = 0;
            for (int k = 0; k < 4; k++) {
                double ck = (k == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += ck * tmp[k*4+j] * cos((2*i+1)*k*PI/8.0);
            }
            out[i*4+j] = sum * 0.5;
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

static void output_rgb(double *planeY, double *planeCb, double *planeCr,
    int imgW, int imgH, const char *name) {
    double pmin=1e9, pmax=-1e9;
    for(int i=0;i<imgW*imgH;i++){if(planeY[i]<pmin)pmin=planeY[i];if(planeY[i]>pmax)pmax=planeY[i];}

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
    char path[256];
    snprintf(path,sizeof(path),OUT_DIR "fe_%s.ppm",name);
    write_ppm(path, rgb, imgW, imgH);
    printf("  %s: Y[%.0f,%.0f]\n", name, pmin, pmax);
    free(rgb);
}

/* Model B: per-AC flag, flag=1+VLC=0 → EOB */
static void test_model_B(const uint8_t *f, int fsize, int imgW, int imgH) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);
    double *pY = calloc(imgW*imgH, sizeof(double));
    double *pCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *pCr = calloc((imgW/2)*(imgH/2), sizeof(double));
    double dc_y=0, dc_cb=0, dc_cr=0;
    int total_eob = 0, total_blocks = 0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[64]; memset(block, 0, sizeof(block));
                dc_y += vlc_coeff(&br);
                block[0] = dc_y;
                int eob = 0;
                for (int i = 1; i < 64 && !br_eof(&br) && !eob; i++) {
                    if (br_get1(&br)) {
                        int v = vlc_coeff(&br);
                        if (v == 0) { eob = 1; total_eob++; }
                        else block[zz8[i]] = v;
                    }
                }
                double spatial[64]; idct8x8(block, spatial);
                int bx = mbx*2 + (yb&1), by = mby*2 + (yb>>1);
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                        pY[(by*8+y)*imgW + bx*8+x] = spatial[y*8+x] + 128.0;
                total_blocks++;
            }
            for (int c = 0; c < 2 && !br_eof(&br); c++) {
                double block[64]; memset(block, 0, sizeof(block));
                double *dc = (c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                block[0] = *dc;
                int eob = 0;
                for (int i = 1; i < 64 && !br_eof(&br) && !eob; i++) {
                    if (br_get1(&br)) {
                        int v = vlc_coeff(&br);
                        if (v == 0) { eob = 1; total_eob++; }
                        else block[zz8[i]] = v;
                    }
                }
                double spatial[64]; idct8x8(block, spatial);
                double *plane = (c==0)?pCb:pCr;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                        plane[(mby*8+y)*(imgW/2) + mbx*8+x] = spatial[y*8+x];
                total_blocks++;
            }
        }
    }
    printf("  Model B: bits %d/%d (%.1f%%) blocks=%d eob=%d\n",
        br.total, bslen*8, 100.0*br.total/(bslen*8), total_blocks, total_eob);
    output_rgb(pY, pCb, pCr, imgW, imgH, "modelB");
    free(pY); free(pCb); free(pCr);
}

/* Model D: 4x4 blocks, 288 MBs (8x8 pixel MBs), 6 blocks/MB */
static void test_model_D(const uint8_t *f, int fsize, int imgW, int imgH) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);
    double *pY = calloc(imgW*imgH, sizeof(double));
    double *pCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *pCr = calloc((imgW/2)*(imgH/2), sizeof(double));
    double dc_y=0, dc_cb=0, dc_cr=0;

    /* 8x8 pixel macroblocks: 16×18 MBs for 128×144 */
    int mb_cols = imgW / 8;  /* 16 */
    int mb_rows = imgH / 8;  /* 18 */

    for (int mby = 0; mby < mb_rows && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < mb_cols && !br_eof(&br); mbx++) {
            /* 4 Y blocks (each 4x4) in 8x8 pixel area */
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[16]; memset(block, 0, sizeof(block));
                dc_y += vlc_coeff(&br);
                block[0] = dc_y;
                for (int i = 1; i < 16 && !br_eof(&br); i++) {
                    if (br_get1(&br))
                        block[zz4[i]] = vlc_coeff(&br);
                }
                double spatial[16]; idct4x4(block, spatial);
                int bx = (yb & 1), by = (yb >> 1);
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++)
                        pY[(mby*8+by*4+y)*imgW + mbx*8+bx*4+x] = spatial[y*4+x] + 128.0;
            }
            /* 1 Cb, 1 Cr (each 4x4 covering 8x8 pixel area) */
            for (int c = 0; c < 2 && !br_eof(&br); c++) {
                double block[16]; memset(block, 0, sizeof(block));
                double *dc = (c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                block[0] = *dc;
                for (int i = 1; i < 16 && !br_eof(&br); i++) {
                    if (br_get1(&br))
                        block[zz4[i]] = vlc_coeff(&br);
                }
                double spatial[16]; idct4x4(block, spatial);
                double *plane = (c==0)?pCb:pCr;
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++)
                        plane[(mby*4+y)*(imgW/2) + mbx*4+x] = spatial[y*4+x];
            }
        }
    }
    printf("  Model D (4x4): bits %d/%d (%.1f%%)\n",
        br.total, bslen*8, 100.0*br.total/(bslen*8));
    output_rgb(pY, pCb, pCr, imgW, imgH, "modelD_4x4");
    free(pY); free(pCb); free(pCr);
}

/* Model E: 4x4 blocks with flag=1,VLC=0 → EOB */
static void test_model_E(const uint8_t *f, int fsize, int imgW, int imgH) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);
    double *pY = calloc(imgW*imgH, sizeof(double));
    double *pCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *pCr = calloc((imgW/2)*(imgH/2), sizeof(double));
    double dc_y=0, dc_cb=0, dc_cr=0;
    int eob_count = 0;

    int mb_cols = imgW / 8, mb_rows = imgH / 8;

    for (int mby = 0; mby < mb_rows && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < mb_cols && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[16]; memset(block, 0, sizeof(block));
                dc_y += vlc_coeff(&br);
                block[0] = dc_y;
                int eob = 0;
                for (int i = 1; i < 16 && !br_eof(&br) && !eob; i++) {
                    if (br_get1(&br)) {
                        int v = vlc_coeff(&br);
                        if (v == 0) { eob = 1; eob_count++; }
                        else block[zz4[i]] = v;
                    }
                }
                double spatial[16]; idct4x4(block, spatial);
                int bx = (yb & 1), by = (yb >> 1);
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++)
                        pY[(mby*8+by*4+y)*imgW + mbx*8+bx*4+x] = spatial[y*4+x] + 128.0;
            }
            for (int c = 0; c < 2 && !br_eof(&br); c++) {
                double block[16]; memset(block, 0, sizeof(block));
                double *dc = (c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                block[0] = *dc;
                int eob = 0;
                for (int i = 1; i < 16 && !br_eof(&br) && !eob; i++) {
                    if (br_get1(&br)) {
                        int v = vlc_coeff(&br);
                        if (v == 0) { eob = 1; eob_count++; }
                        else block[zz4[i]] = v;
                    }
                }
                double spatial[16]; idct4x4(block, spatial);
                double *plane = (c==0)?pCb:pCr;
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++)
                        plane[(mby*4+y)*(imgW/2) + mbx*4+x] = spatial[y*4+x];
            }
        }
    }
    printf("  Model E (4x4+EOB): bits %d/%d (%.1f%%) eobs=%d\n",
        br.total, bslen*8, 100.0*br.total/(bslen*8), eob_count);
    output_rgb(pY, pCb, pCr, imgW, imgH, "modelE_4x4eob");
    free(pY); free(pCb); free(pCr);
}

/* Model F: All DCs first (plane-sequential DC), then all ACs */
static void test_model_F(const uint8_t *f, int fsize, int imgW, int imgH) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);
    double *pY = calloc(imgW*imgH, sizeof(double));
    double *pCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *pCr = calloc((imgW/2)*(imgH/2), sizeof(double));

    /* First pass: read all DCs */
    double blocks[432][64];
    memset(blocks, 0, sizeof(blocks));
    double dc_y=0, dc_cb=0, dc_cr=0;
    int blk = 0;
    for (int mby = 0; mby < 9; mby++) {
        for (int mbx = 0; mbx < 8; mbx++) {
            for (int yb = 0; yb < 4; yb++) {
                dc_y += vlc_coeff(&br);
                blocks[blk][0] = dc_y;
                blk++;
            }
            dc_cb += vlc_coeff(&br);
            blocks[blk][0] = dc_cb; blk++;
            dc_cr += vlc_coeff(&br);
            blocks[blk][0] = dc_cr; blk++;
        }
    }
    int dc_bits = br.total;
    printf("  Model F: DC pass used %d bits (%.1f%%)\n", dc_bits, 100.0*dc_bits/(bslen*8));

    /* Second pass: read all ACs with per-flag */
    blk = 0;
    for (int b = 0; b < 432 && !br_eof(&br); b++) {
        for (int i = 1; i < 64 && !br_eof(&br); i++) {
            if (br_get1(&br))
                blocks[b][zz8[i]] = vlc_coeff(&br);
        }
    }
    printf("  Model F: total bits %d/%d (%.1f%%)\n",
        br.total, bslen*8, 100.0*br.total/(bslen*8));

    /* Render */
    blk = 0;
    for (int mby = 0; mby < 9; mby++) {
        for (int mbx = 0; mbx < 8; mbx++) {
            for (int yb = 0; yb < 4; yb++) {
                double spatial[64]; idct8x8(blocks[blk], spatial);
                int bx = mbx*2 + (yb&1), by = mby*2 + (yb>>1);
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                        pY[(by*8+y)*imgW + bx*8+x] = spatial[y*8+x] + 128.0;
                blk++;
            }
            double spatCb[64], spatCr[64];
            idct8x8(blocks[blk], spatCb); blk++;
            idct8x8(blocks[blk], spatCr); blk++;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    pCb[(mby*8+y)*(imgW/2) + mbx*8+x] = spatCb[y*8+x];
                    pCr[(mby*8+y)*(imgW/2) + mbx*8+x] = spatCr[y*8+x];
                }
        }
    }
    output_rgb(pY, pCb, pCr, imgW, imgH, "modelF_dcsep");
    free(pY); free(pCb); free(pCr);
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

    test_model_B(frames[0],fsizes[0],imgW,imgH);
    test_model_D(frames[0],fsizes[0],imgW,imgH);
    test_model_E(frames[0],fsizes[0],imgW,imgH);
    test_model_F(frames[0],fsizes[0],imgW,imgH);

    free(disc); zip_close(z);
    return 0;
}
