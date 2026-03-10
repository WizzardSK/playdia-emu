/*
 * vcodec_fixedvar.c - Variable-width fixed-length AC coefficients
 *
 * Key finding: fixed 4-bit AC produces clean images (no checkerboard)
 * while VLC-based approaches all produce noise.
 *
 * Test: qtable values determine bit width per position.
 * The qtable might encode the number of bits for each 2x2 region of the 8×8 block.
 *
 * Also test: fixed width with dequantization
 * Also test: different divisors for qtable → bit width mapping
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
static int br_sget(BR *b, int n) {
    if (n == 0) return 0;
    int v = br_get(b, n);
    if (v >= (1 << (n-1))) v -= (1 << n);
    return v;
}
/* Sign-magnitude reading */
static int br_smget(BR *b, int n) {
    if (n == 0) return 0;
    if (n == 1) return br_get1(b) ? -1 : 1; /* 0=+1, 1=-1? or just 0/1 */
    int mag = br_get(b, n-1);
    if (mag == 0) return 0;
    int sign = br_get1(b);
    return sign ? -mag : mag;
}

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
    snprintf(path,sizeof(path),OUT_DIR "fv_%s.ppm",name);
    write_ppm(path, rgb, imgW, imgH);
    printf("  %s: Y[%.0f, %.0f]\n", name, pmin, pmax);
    free(rgb);
}

/* Get bit width for 8×8 position from 4×4 qtable */
static int get_bitwidth(const uint8_t qt[16], int row, int col, int divisor) {
    int qi = (row/2)*4 + (col/2);
    int bw = (qt[qi] + divisor/2) / divisor; /* round */
    if (bw < 1) bw = 1;
    if (bw > 8) bw = 8;
    return bw;
}

/* Test variable bit width based on qtable */
static void test_var_width(const uint8_t *f, int fsize, int imgW, int imgH, int divisor) {
    uint8_t qt[16]; memcpy(qt, f+4, 16);
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    /* Calculate total bits needed */
    int bits_per_block = 0;
    int bw_map[64];
    for (int i = 1; i < 64; i++) {
        int r = zz8[i] / 8, c = zz8[i] % 8;
        bw_map[i] = get_bitwidth(qt, r, c, divisor);
        bits_per_block += bw_map[i];
    }

    double *planeY = calloc(imgW*imgH, sizeof(double));
    double *planeCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr = calloc((imgW/2)*(imgH/2), sizeof(double));
    double dc_y=0, dc_cb=0, dc_cr=0;
    int blocks_done = 0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[64]; memset(block,0,sizeof(block));
                dc_y += vlc_coeff(&br);
                block[0] = dc_y;
                for (int i=1;i<64&&!br_eof(&br);i++)
                    block[zz8[i]] = br_sget(&br, bw_map[i]);
                double spatial[64]; idct8x8(block, spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
                blocks_done++;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double block[64]; memset(block,0,sizeof(block));
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                block[0] = *dc;
                for (int i=1;i<64&&!br_eof(&br);i++)
                    block[zz8[i]] = br_sget(&br, bw_map[i]);
                double spatial[64]; idct8x8(block, spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
                blocks_done++;
            }
        }
    }

    char name[64]; snprintf(name,sizeof(name),"var_d%d", divisor);
    printf("  %s: bpb=%d blks=%d/%d bits %d/%d (%.1f%%)\n",
        name, bits_per_block, blocks_done, 432, br.total, bslen*8, 100.0*br.total/(bslen*8));
    /* Print bit width map for first row of qtable */
    printf("    bw: ");
    for (int i = 0; i < 8; i++) printf("%d ", get_bitwidth(qt, 0, i, divisor));
    printf("/ ");
    for (int i = 0; i < 8; i++) printf("%d ", get_bitwidth(qt, 2, i, divisor));
    printf("\n");
    render(planeY, planeCb, planeCr, imgW, imgH, name);
    free(planeY); free(planeCb); free(planeCr);
}

/* Test fixed width with dequantization: coeff = fixed_val * qtable[pos] * factor */
static void test_fixed_dequant(const uint8_t *f, int fsize, int imgW, int imgH,
    int ac_bits, double dq_factor) {
    uint8_t qt[16]; memcpy(qt, f+4, 16);
    int qs = f[3];
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    /* Build 8x8 quant matrix */
    double qm[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            qm[i*8+j] = qt[(i/2)*4 + (j/2)];

    double *planeY = calloc(imgW*imgH, sizeof(double));
    double *planeCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr = calloc((imgW/2)*(imgH/2), sizeof(double));
    double dc_y=0, dc_cb=0, dc_cr=0;
    int blocks_done = 0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[64]; memset(block,0,sizeof(block));
                dc_y += vlc_coeff(&br);
                block[0] = dc_y;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    int v = br_sget(&br, ac_bits);
                    block[zz8[i]] = v * qm[zz8[i]] * dq_factor;
                }
                double spatial[64]; idct8x8(block, spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
                blocks_done++;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double block[64]; memset(block,0,sizeof(block));
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                block[0] = *dc;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    int v = br_sget(&br, ac_bits);
                    block[zz8[i]] = v * qm[zz8[i]] * dq_factor;
                }
                double spatial[64]; idct8x8(block, spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
                blocks_done++;
            }
        }
    }

    char name[64]; snprintf(name,sizeof(name),"fdq_%db_%.3f", ac_bits, dq_factor);
    printf("  %s: qs=%d blks=%d/%d bits %d/%d (%.1f%%)\n",
        name, qs, blocks_done, 432, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render(planeY, planeCb, planeCr, imgW, imgH, name);
    free(planeY); free(planeCb); free(planeCr);
}

/* Test: VLC magnitude/sign for AC but with value clamped to ±N */
static void test_vlc_clamped(const uint8_t *f, int fsize, int imgW, int imgH, int maxval) {
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
                    if (br_get1(&br)) {
                        int v = vlc_coeff(&br);
                        if (v > maxval) v = maxval;
                        if (v < -maxval) v = -maxval;
                        block[zz8[i]] = v;
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
                *dc += vlc_coeff(&br);
                block[0] = *dc;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    if (br_get1(&br)) {
                        int v = vlc_coeff(&br);
                        if (v > maxval) v = maxval;
                        if (v < -maxval) v = -maxval;
                        block[zz8[i]] = v;
                    }
                }
                double spatial[64]; idct8x8(block, spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
            }
        }
    }

    char name[64]; snprintf(name,sizeof(name),"vlc_clamp%d", maxval);
    printf("  %s: bits %d/%d (%.1f%%)\n", name, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render(planeY, planeCb, planeCr, imgW, imgH, name);
    free(planeY); free(planeCb); free(planeCr);
}

/* Test: magnitude/sign VLC for AC but with MPEG-1 style
   VLC gives "size", then read "size" bits as the value.
   Like DC but: 0 gives VALUE 0 (not EOB). All 63 positions. No flags. */
static void test_all63_vlc(const uint8_t *f, int fsize, int imgW, int imgH) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double *planeY = calloc(imgW*imgH, sizeof(double));
    double *planeCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr = calloc((imgW/2)*(imgH/2), sizeof(double));
    double dc_y=0, dc_cb=0, dc_cr=0;
    int blks = 0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[64]; memset(block,0,sizeof(block));
                dc_y += vlc_coeff(&br);
                block[0] = dc_y;
                for (int i=1;i<64&&!br_eof(&br);i++)
                    block[zz8[i]] = vlc_coeff(&br);
                double spatial[64]; idct8x8(block, spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
                blks++;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double block[64]; memset(block,0,sizeof(block));
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                block[0] = *dc;
                for (int i=1;i<64&&!br_eof(&br);i++)
                    block[zz8[i]] = vlc_coeff(&br);
                double spatial[64]; idct8x8(block, spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
                blks++;
            }
        }
    }

    printf("  all63vlc: blks=%d bits %d/%d (%.1f%%)\n",
        blks, br.total, bslen*8, 100.0*br.total/(bslen*8));

    /* Now render DC-only for comparison - shows if block alignment is right */
    /* Re-decode but only keep low-freq AC (positions 1-3 in zigzag) */
    br_init(&br, bs, bslen);
    dc_y=0; dc_cb=0; dc_cr=0;
    double *planeY2 = calloc(imgW*imgH, sizeof(double));
    double *planeCb2 = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr2 = calloc((imgW/2)*(imgH/2), sizeof(double));

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[64]; memset(block,0,sizeof(block));
                dc_y += vlc_coeff(&br);
                block[0] = dc_y;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    int v = vlc_coeff(&br);
                    if (i <= 6) block[zz8[i]] = v; /* only low freq */
                }
                double spatial[64]; idct8x8(block, spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY2[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double block[64]; memset(block,0,sizeof(block));
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                block[0] = *dc;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    int v = vlc_coeff(&br);
                    if (i <= 6) block[zz8[i]] = v;
                }
                double spatial[64]; idct8x8(block, spatial);
                double *plane=(c==0)?planeCb2:planeCr2;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
            }
        }
    }
    render(planeY2, planeCb2, planeCr2, imgW, imgH, "all63_lowac");
    free(planeY2); free(planeCb2); free(planeCr2);

    render(planeY, planeCb, planeCr, imgW, imgH, "all63vlc");
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
    int qs = frames[0][3];
    printf("LBA %d: qs=%d type=%d\n", slba, qs, frames[0][39]);

    printf("\n--- Variable bit width (qtable/divisor) ---\n");
    for (int div = 3; div <= 8; div++)
        test_var_width(frames[0],fsizes[0],imgW,imgH, div);

    printf("\n--- Fixed-width with dequant ---\n");
    test_fixed_dequant(frames[0],fsizes[0],imgW,imgH, 3, 1.0/(16.0));
    test_fixed_dequant(frames[0],fsizes[0],imgW,imgH, 3, 1.0/(32.0));
    test_fixed_dequant(frames[0],fsizes[0],imgW,imgH, 4, 1.0/(16.0));
    test_fixed_dequant(frames[0],fsizes[0],imgW,imgH, 4, 1.0/(32.0));

    printf("\n--- VLC with value clamping ---\n");
    test_vlc_clamped(frames[0],fsizes[0],imgW,imgH, 3);
    test_vlc_clamped(frames[0],fsizes[0],imgW,imgH, 7);
    test_vlc_clamped(frames[0],fsizes[0],imgW,imgH, 15);

    printf("\n--- All 63 sequential VLC (no flags) + low-AC-only ---\n");
    test_all63_vlc(frames[0],fsizes[0],imgW,imgH);

    free(disc); zip_close(z);
    return 0;
}
