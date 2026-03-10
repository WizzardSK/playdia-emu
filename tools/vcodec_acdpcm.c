/*
 * vcodec_acdpcm.c - Test DPCM for ALL coefficients (not just DC)
 *
 * Hypothesis: ALL 64 coefficients use DPCM across blocks.
 * Each zigzag position maintains its own accumulator.
 * The VLC values are differences, not absolute values.
 *
 * Also test: DPCM per MB row (reset at each row)
 * Also test: DPCM per plane (Y separate from Cb, Cr)
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

static void render_frame(double *planeY, double *planeCb, double *planeCr,
    int imgW, int imgH, const char *name) {
    uint8_t *rgb = malloc(imgW*imgH*3);
    double pmin=1e9, pmax=-1e9;
    for(int i=0;i<imgW*imgH;i++){if(planeY[i]<pmin)pmin=planeY[i];if(planeY[i]>pmax)pmax=planeY[i];}

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
    snprintf(path,sizeof(path),OUT_DIR "dp_%s.ppm",name);
    write_ppm(path, rgb, imgW, imgH);
    printf("  %s: Y[%.0f, %.0f]\n", name, pmin, pmax);
    free(rgb);
}

/* Model 1: All 64 coefficients DPCM, per-AC flags */
static void test_dpcm_all(const uint8_t *f, int fsize, int imgW, int imgH, const char *tag) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double *planeY = calloc(imgW*imgH, sizeof(double));
    double *planeCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr = calloc((imgW/2)*(imgH/2), sizeof(double));

    /* DPCM accumulators per plane per coefficient */
    double dpcm_y[64] = {0};
    double dpcm_cb[64] = {0};
    double dpcm_cr[64] = {0};

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[64]; memset(block,0,sizeof(block));
                dpcm_y[0] += vlc_coeff(&br);
                block[0] = dpcm_y[0];
                for (int i = 1; i < 64 && !br_eof(&br); i++) {
                    if (br_get1(&br)) {
                        dpcm_y[i] += vlc_coeff(&br);
                    }
                    block[zz8[i]] = dpcm_y[i];
                }
                double spatial[64]; idct8x8(block, spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double *dpcm = (c==0) ? dpcm_cb : dpcm_cr;
                double block[64]; memset(block,0,sizeof(block));
                dpcm[0] += vlc_coeff(&br);
                block[0] = dpcm[0];
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    if (br_get1(&br)) {
                        dpcm[i] += vlc_coeff(&br);
                    }
                    block[zz8[i]] = dpcm[i];
                }
                double spatial[64]; idct8x8(block, spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
            }
        }
    }

    printf("  %s: bits %d/%d (%.1f%%)\n", tag, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render_frame(planeY, planeCb, planeCr, imgW, imgH, tag);
    free(planeY); free(planeCb); free(planeCr);
}

/* Model 2: DPCM reset per MB row */
static void test_dpcm_row(const uint8_t *f, int fsize, int imgW, int imgH, const char *tag) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double *planeY = calloc(imgW*imgH, sizeof(double));
    double *planeCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr = calloc((imgW/2)*(imgH/2), sizeof(double));

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        double dpcm_y[64] = {0};
        double dpcm_cb[64] = {0};
        double dpcm_cr[64] = {0};

        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[64]; memset(block,0,sizeof(block));
                dpcm_y[0] += vlc_coeff(&br);
                block[0] = dpcm_y[0];
                for (int i = 1; i < 64 && !br_eof(&br); i++) {
                    if (br_get1(&br)) {
                        dpcm_y[i] += vlc_coeff(&br);
                    }
                    block[zz8[i]] = dpcm_y[i];
                }
                double spatial[64]; idct8x8(block, spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double *dpcm = (c==0) ? dpcm_cb : dpcm_cr;
                double block[64]; memset(block,0,sizeof(block));
                dpcm[0] += vlc_coeff(&br);
                block[0] = dpcm[0];
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    if (br_get1(&br)) {
                        dpcm[i] += vlc_coeff(&br);
                    }
                    block[zz8[i]] = dpcm[i];
                }
                double spatial[64]; idct8x8(block, spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
            }
        }
    }

    printf("  %s: bits %d/%d (%.1f%%)\n", tag, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render_frame(planeY, planeCb, planeCr, imgW, imgH, tag);
    free(planeY); free(planeCb); free(planeCr);
}

/* Model 3: No IDCT, all 64 DPCM values are PIXEL values directly (raster scan) */
static void test_pixel_dpcm(const uint8_t *f, int fsize, int imgW, int imgH, const char *tag) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double *planeY = calloc(imgW*imgH, sizeof(double));
    double *planeCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr = calloc((imgW/2)*(imgH/2), sizeof(double));

    /* Each pixel in a block is DPCM from the SAME POSITION in the previous block */
    double prev_y[4][64]; memset(prev_y,0,sizeof(prev_y));
    double prev_cb[64] = {0};
    double prev_cr[64] = {0};

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        memset(prev_y,0,sizeof(prev_y));
        memset(prev_cb,0,sizeof(prev_cb));
        memset(prev_cr,0,sizeof(prev_cr));
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                /* First value = delta for pixel 0 */
                prev_y[yb][0] += vlc_coeff(&br);
                for (int i = 1; i < 64 && !br_eof(&br); i++) {
                    if (br_get1(&br))
                        prev_y[yb][i] += vlc_coeff(&br);
                }
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = prev_y[yb][y*8+x] + 128.0;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double *prev = (c==0) ? prev_cb : prev_cr;
                prev[0] += vlc_coeff(&br);
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    if (br_get1(&br))
                        prev[i] += vlc_coeff(&br);
                }
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = prev[y*8+x];
            }
        }
    }

    printf("  %s: bits %d/%d (%.1f%%)\n", tag, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render_frame(planeY, planeCb, planeCr, imgW, imgH, tag);
    free(planeY); free(planeCb); free(planeCr);
}

/* Model 4: DPCM coefficient coding, block sequential
 * DC DPCM + AC DPCM, with the DPCM accumulator shared across
 * ALL Y blocks (not per-subblock position) */
static void test_dpcm_global(const uint8_t *f, int fsize, int imgW, int imgH, const char *tag) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double *planeY = calloc(imgW*imgH, sizeof(double));
    double *planeCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr = calloc((imgW/2)*(imgH/2), sizeof(double));

    /* Single set of 64 DPCM accumulators for ALL Y blocks */
    double dpcm_y[64] = {0};
    double dpcm_cb[64] = {0};
    double dpcm_cr[64] = {0};

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[64]; memset(block,0,sizeof(block));
                dpcm_y[0] += vlc_coeff(&br);
                block[0] = dpcm_y[0];
                for (int i = 1; i < 64 && !br_eof(&br); i++) {
                    if (br_get1(&br))
                        dpcm_y[i] += vlc_coeff(&br);
                    block[zz8[i]] = dpcm_y[i];
                }
                double spatial[64]; idct8x8(block, spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double *dpcm = (c==0) ? dpcm_cb : dpcm_cr;
                double block[64]; memset(block,0,sizeof(block));
                dpcm[0] += vlc_coeff(&br);
                block[0] = dpcm[0];
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    if (br_get1(&br))
                        dpcm[i] += vlc_coeff(&br);
                    block[zz8[i]] = dpcm[i];
                }
                double spatial[64]; idct8x8(block, spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
            }
        }
    }

    printf("  %s: bits %d/%d (%.1f%%)\n", tag, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render_frame(planeY, planeCb, planeCr, imgW, imgH, tag);
    free(planeY); free(planeCb); free(planeCr);
}

/* Model 5: Verify DC alignment - decode with per-AC flags, dump AC statistics */
static void test_ac_stats(const uint8_t *f, int fsize) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double dc_y=0;
    int ac_nonzero_by_pos[64] = {0};
    int ac_sum_abs_by_pos[64] = {0};
    int total_blocks = 0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                dc_y += vlc_coeff(&br);
                for (int i = 1; i < 64 && !br_eof(&br); i++) {
                    if (br_get1(&br)) {
                        int v = vlc_coeff(&br);
                        ac_nonzero_by_pos[i]++;
                        ac_sum_abs_by_pos[i] += abs(v);
                    }
                }
                total_blocks++;
            }
            /* Skip chroma */
            for (int c=0;c<2&&!br_eof(&br);c++) {
                vlc_coeff(&br);
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    if (br_get1(&br)) vlc_coeff(&br);
                }
            }
        }
    }

    printf("\n  AC statistics (Y blocks only, %d blocks):\n", total_blocks);
    printf("  Pos | Freq(row,col) | NonZero | AvgAbs\n");
    printf("  ----|---------------|---------|-------\n");
    for (int i = 1; i < 64; i++) {
        int r = zz8[i] / 8, c = zz8[i] % 8;
        double avg = ac_nonzero_by_pos[i] ? (double)ac_sum_abs_by_pos[i]/ac_nonzero_by_pos[i] : 0;
        if (i <= 15 || (i % 8 == 0))
            printf("  %3d | (%d,%d)         | %3d/%3d | %.1f\n",
                i, r, c, ac_nonzero_by_pos[i], total_blocks, avg);
    }

    /* Check: do low frequencies have more energy than high? */
    int lo_nz = 0, hi_nz = 0, lo_sum = 0, hi_sum = 0;
    for (int i = 1; i < 64; i++) {
        int r = zz8[i] / 8, c = zz8[i] % 8;
        if (r + c <= 4) { lo_nz += ac_nonzero_by_pos[i]; lo_sum += ac_sum_abs_by_pos[i]; }
        else { hi_nz += ac_nonzero_by_pos[i]; hi_sum += ac_sum_abs_by_pos[i]; }
    }
    printf("\n  Low freq (r+c<=4): %d nonzero, avg abs %.1f\n",
        lo_nz, lo_nz ? (double)lo_sum/lo_nz : 0);
    printf("  High freq (r+c>4): %d nonzero, avg abs %.1f\n",
        hi_nz, hi_nz ? (double)hi_sum/hi_nz : 0);
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

    test_dpcm_all(frames[0],fsizes[0],imgW,imgH, "dpcm_all");
    test_dpcm_row(frames[0],fsizes[0],imgW,imgH, "dpcm_row");
    test_dpcm_global(frames[0],fsizes[0],imgW,imgH, "dpcm_global");
    test_pixel_dpcm(frames[0],fsizes[0],imgW,imgH, "pixel_dpcm");

    /* AC frequency distribution analysis */
    test_ac_stats(frames[0],fsizes[0]);

    free(disc); zip_close(z);
    return 0;
}
