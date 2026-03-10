/*
 * vcodec_noflag.c - Test: NO per-AC flags, just 64 sequential VLC values per block
 *
 * Hypothesis: the per-AC flag model is WRONG. There are no flags.
 * All 64 coefficients (DC DPCM + 63 AC) are just sequential VLC values.
 * VLC 0 ("100") is literally the value 0, NOT an EOB marker.
 *
 * We know only ~327 blocks fit at 64 VLC per block. So:
 * Model A: All 432 blocks × 64 VLC (will overflow - see where it goes wrong)
 * Model B: 1-bit block flag + coded blocks get 64 VLC values
 * Model C: VLC skip count between coded blocks
 * Model D: All 64 VLC per block, but DC*8 to test MPEG-1 style
 * Model E: Sequential VLC with ESCAPE detection (111111XX = escape?)
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
    snprintf(path,sizeof(path),OUT_DIR "nf_%s.ppm",name);
    write_ppm(path, rgb, imgW, imgH);
    printf("  %s: Y[%.0f, %.0f]\n", name, pmin, pmax);
    free(rgb);
}

/* Model A: All 64 VLC sequential per block, no flags, no EOB */
static void test_model_a(const uint8_t *f, int fsize, int imgW, int imgH, const char *tag) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double *planeY = calloc(imgW*imgH, sizeof(double));
    double *planeCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr = calloc((imgW/2)*(imgH/2), sizeof(double));
    double dc_y=0, dc_cb=0, dc_cr=0;
    int blocks_ok = 0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[64]; memset(block,0,sizeof(block));
                dc_y += vlc_coeff(&br);
                block[0] = dc_y;
                for (int i = 1; i < 64 && !br_eof(&br); i++)
                    block[zz8[i]] = vlc_coeff(&br);
                double spatial[64];
                idct8x8(block, spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
                blocks_ok++;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double block[64]; memset(block,0,sizeof(block));
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                block[0] = *dc;
                for (int i=1;i<64&&!br_eof(&br);i++)
                    block[zz8[i]] = vlc_coeff(&br);
                double spatial[64];
                idct8x8(block, spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
                blocks_ok++;
            }
        }
    }

    printf("  %s: %d/%d blocks, bits %d/%d (%.1f%%)\n",
        tag, blocks_ok, 432, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render_frame(planeY, planeCb, planeCr, imgW, imgH, tag);
    free(planeY); free(planeCb); free(planeCr);
}

/* Model B: 1-bit block flag, coded blocks get all 64 VLC values */
static void test_model_b(const uint8_t *f, int fsize, int imgW, int imgH, const char *tag) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double *planeY = calloc(imgW*imgH, sizeof(double));
    double *planeCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr = calloc((imgW/2)*(imgH/2), sizeof(double));
    double dc_y=0, dc_cb=0, dc_cr=0;
    int coded=0, skipped=0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                int flag = br_get1(&br);
                double block[64]; memset(block,0,sizeof(block));
                if (flag) {
                    dc_y += vlc_coeff(&br);
                    block[0] = dc_y;
                    for (int i=1;i<64&&!br_eof(&br);i++)
                        block[zz8[i]] = vlc_coeff(&br);
                    coded++;
                } else {
                    block[0] = dc_y; /* repeat last DC */
                    skipped++;
                }
                double spatial[64];
                idct8x8(block, spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                int flag = br_get1(&br);
                double block[64]; memset(block,0,sizeof(block));
                double *dc=(c==0)?&dc_cb:&dc_cr;
                if (flag) {
                    *dc += vlc_coeff(&br);
                    block[0] = *dc;
                    for (int i=1;i<64&&!br_eof(&br);i++)
                        block[zz8[i]] = vlc_coeff(&br);
                    coded++;
                } else {
                    block[0] = *dc;
                    skipped++;
                }
                double spatial[64];
                idct8x8(block, spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
            }
        }
    }

    printf("  %s: coded=%d skipped=%d bits %d/%d (%.1f%%)\n",
        tag, coded, skipped, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render_frame(planeY, planeCb, planeCr, imgW, imgH, tag);
    free(planeY); free(planeCb); free(planeCr);
}

/* Model C: All 64 VLC with identity scan (no zigzag) */
static void test_model_c(const uint8_t *f, int fsize, int imgW, int imgH, const char *tag) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double *planeY = calloc(imgW*imgH, sizeof(double));
    double *planeCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr = calloc((imgW/2)*(imgH/2), sizeof(double));
    double dc_y=0, dc_cb=0, dc_cr=0;
    int blocks_ok = 0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[64]; memset(block,0,sizeof(block));
                dc_y += vlc_coeff(&br);
                block[0] = dc_y;
                for (int i = 1; i < 64 && !br_eof(&br); i++)
                    block[i] = vlc_coeff(&br); /* identity scan - no zigzag */
                double spatial[64];
                idct8x8(block, spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
                blocks_ok++;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double block[64]; memset(block,0,sizeof(block));
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                block[0] = *dc;
                for (int i=1;i<64&&!br_eof(&br);i++)
                    block[i] = vlc_coeff(&br);
                double spatial[64];
                idct8x8(block, spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
                blocks_ok++;
            }
        }
    }

    printf("  %s: %d blocks, bits %d/%d (%.1f%%)\n",
        tag, blocks_ok, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render_frame(planeY, planeCb, planeCr, imgW, imgH, tag);
    free(planeY); free(planeCb); free(planeCr);
}

/* Model D: DC + sequential VLC ACs with EOB-on-zero, then REMAINING bits
   used for enhancement/refinement pass */
static void test_model_d(const uint8_t *f, int fsize, int imgW, int imgH, const char *tag) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    /* First pass: DC + ACs until VLC=0 (EOB) */
    double blocks_coeff[432][64];
    memset(blocks_coeff, 0, sizeof(blocks_coeff));
    double dc_y=0, dc_cb=0, dc_cr=0;
    int total_acs = 0;

    int blk = 0;
    for (int mby = 0; mby < 9 && !br_eof(&br) && blk < 432; mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br) && blk < 432; mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br) && blk < 432; yb++, blk++) {
                dc_y += vlc_coeff(&br);
                blocks_coeff[blk][0] = dc_y;
                for (int i = 1; i < 64 && !br_eof(&br); i++) {
                    int v = vlc_coeff(&br);
                    if (v == 0) break; /* EOB */
                    blocks_coeff[blk][zz8[i]] = v;
                    total_acs++;
                }
            }
            for (int c=0;c<2&&!br_eof(&br)&&blk<432;c++,blk++) {
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                blocks_coeff[blk][0] = *dc;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    int v = vlc_coeff(&br);
                    if (v == 0) break;
                    blocks_coeff[blk][zz8[i]] = v;
                    total_acs++;
                }
            }
        }
    }

    int pass1_bits = br.total;
    printf("  %s: pass1 %d bits (%.1f%%), %d AC values\n",
        tag, pass1_bits, 100.0*pass1_bits/(bslen*8), total_acs);

    /* Second pass: read remaining bits as enhancement for blocks */
    int remaining_bits = bslen*8 - br.total;
    printf("  %s: %d remaining bits (%.1f%%)\n", tag, remaining_bits, 100.0*remaining_bits/(bslen*8));

    /* Try: remaining bits are per-AC flags + VLC for zero positions */
    int enhanced = 0;
    for (int b = 0; b < 432 && !br_eof(&br); b++) {
        for (int i = 1; i < 64 && !br_eof(&br); i++) {
            if (blocks_coeff[b][zz8[i]] == 0) { /* only enhance zero positions */
                if (br_get1(&br)) {
                    blocks_coeff[b][zz8[i]] = vlc_coeff(&br);
                    enhanced++;
                }
            }
        }
    }
    printf("  %s: enhanced %d positions, total bits %d/%d (%.1f%%)\n",
        tag, enhanced, br.total, bslen*8, 100.0*br.total/(bslen*8));

    /* Render */
    double *planeY = calloc(imgW*imgH, sizeof(double));
    double *planeCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr = calloc((imgW/2)*(imgH/2), sizeof(double));

    blk = 0;
    for (int mby = 0; mby < 9; mby++) {
        for (int mbx = 0; mbx < 8; mbx++) {
            for (int yb = 0; yb < 4; yb++, blk++) {
                double spatial[64];
                idct8x8(blocks_coeff[blk], spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
            }
            for (int c=0;c<2;c++,blk++) {
                double spatial[64];
                idct8x8(blocks_coeff[blk], spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
            }
        }
    }

    render_frame(planeY, planeCb, planeCr, imgW, imgH, tag);
    free(planeY); free(planeCb); free(planeCr);
}

/* Model E: per-AC flag but with AC values at HALF scale */
static void test_model_e(const uint8_t *f, int fsize, int imgW, int imgH,
    double ac_div, const char *tag) {
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
                dc_y += vlc_coeff(&br);
                block[0] = dc_y;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    if (br_get1(&br))
                        block[zz8[i]] = vlc_coeff(&br) / ac_div;
                }
                double spatial[64];
                idct8x8(block, spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double block[64]; memset(block,0,sizeof(block));
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                block[0] = *dc;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    if (br_get1(&br))
                        block[zz8[i]] = vlc_coeff(&br) / ac_div;
                }
                double spatial[64];
                idct8x8(block, spatial);
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

/* Model F: per-AC flag with quantization matrix dequant for AC */
static void test_model_f(const uint8_t *f, int fsize, int imgW, int imgH, const char *tag) {
    int qs = f[3];
    uint8_t qt[16]; memcpy(qt, f+4, 16);
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    /* Build 8x8 quantization matrix from 4x4 */
    double qm[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            qm[i*8+j] = qt[(i/2)*4 + (j/2)];

    double *planeY = calloc(imgW*imgH, sizeof(double));
    double *planeCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr = calloc((imgW/2)*(imgH/2), sizeof(double));
    double dc_y=0, dc_cb=0, dc_cr=0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[64]; memset(block,0,sizeof(block));
                dc_y += vlc_coeff(&br);
                block[0] = dc_y * 8.0; /* DC * 8 like MPEG-1 */
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    if (br_get1(&br)) {
                        int v = vlc_coeff(&br);
                        /* MPEG-1 style: level * qs * qm / 16 */
                        block[zz8[i]] = v * qs * qm[zz8[i]] / (16.0 * 64.0);
                    }
                }
                double spatial[64];
                idct8x8(block, spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double block[64]; memset(block,0,sizeof(block));
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                block[0] = *dc * 8.0;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    if (br_get1(&br)) {
                        int v = vlc_coeff(&br);
                        block[zz8[i]] = v * qs * qm[zz8[i]] / (16.0 * 64.0);
                    }
                }
                double spatial[64];
                idct8x8(block, spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
            }
        }
    }

    printf("  %s (qs=%d): bits %d/%d (%.1f%%)\n", tag, qs, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render_frame(planeY, planeCb, planeCr, imgW, imgH, tag);
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

    static uint8_t frames[4][MAX_FRAME]; int fsizes[4];
    int nf = assemble_frames(disc,tsec,slba,frames,fsizes,4);
    if (nf < 1) return 1;

    int imgW = 128, imgH = 144;
    printf("LBA %d: qs=%d type=%d fsize=%d\n", slba, frames[0][3], frames[0][39], fsizes[0]);

    test_model_a(frames[0],fsizes[0],imgW,imgH, "A_seq64");
    test_model_b(frames[0],fsizes[0],imgW,imgH, "B_flag64");
    test_model_c(frames[0],fsizes[0],imgW,imgH, "C_seq64_nozz");
    test_model_d(frames[0],fsizes[0],imgW,imgH, "D_eob_enhance");
    test_model_e(frames[0],fsizes[0],imgW,imgH, 4.0, "E_acflag_div4");
    test_model_e(frames[0],fsizes[0],imgW,imgH, 8.0, "E_acflag_div8");
    test_model_f(frames[0],fsizes[0],imgW,imgH, "F_mpeg_dequant");

    free(disc); zip_close(z);
    return 0;
}
