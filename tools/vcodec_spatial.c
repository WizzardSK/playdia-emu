/*
 * vcodec_spatial.c - Test spatial (non-DCT) interpretation of coefficients
 *
 * Hypothesis: AK8000 does NOT use DCT. Instead:
 * - DC = base pixel value for block (DPCM)
 * - AC values = pixel-level deltas within 8x8 block (various scan orders)
 *
 * Also test: maybe it's a WHT (Walsh-Hadamard Transform) not DCT
 * Also test: maybe it's scaled DCT (integer approximation)
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
    snprintf(path,sizeof(path),OUT_DIR "sp_%s.ppm",name);
    write_ppm(path, rgb, imgW, imgH);
    printf("  %s: Y[%.0f, %.0f]\n", name, pmin, pmax);
    free(rgb);
}

/* 8x8 Walsh-Hadamard Transform (inverse) */
static void iwht8(double *in, double *out) {
    /* 1D WHT via butterfly */
    double tmp[8];
    for (int i = 0; i < 8; i++) {
        /* Stage 1: pairs */
        double a[8];
        a[0] = in[i*8+0] + in[i*8+1];
        a[1] = in[i*8+0] - in[i*8+1];
        a[2] = in[i*8+2] + in[i*8+3];
        a[3] = in[i*8+2] - in[i*8+3];
        a[4] = in[i*8+4] + in[i*8+5];
        a[5] = in[i*8+4] - in[i*8+5];
        a[6] = in[i*8+6] + in[i*8+7];
        a[7] = in[i*8+6] - in[i*8+7];
        /* Stage 2 */
        double b[8];
        b[0] = a[0] + a[2];
        b[1] = a[1] + a[3];
        b[2] = a[0] - a[2];
        b[3] = a[1] - a[3];
        b[4] = a[4] + a[6];
        b[5] = a[5] + a[7];
        b[6] = a[4] - a[6];
        b[7] = a[5] - a[7];
        /* Stage 3 */
        tmp[0] = b[0] + b[4];
        tmp[1] = b[1] + b[5];
        tmp[2] = b[2] + b[6];
        tmp[3] = b[3] + b[7];
        tmp[4] = b[0] - b[4];
        tmp[5] = b[1] - b[5];
        tmp[6] = b[2] - b[6];
        tmp[7] = b[3] - b[7];
        for (int j = 0; j < 8; j++)
            out[i*8+j] = tmp[j] / 8.0;
    }
    /* Second dimension */
    double tmp2[64];
    for (int j = 0; j < 8; j++) {
        double col[8];
        for (int i = 0; i < 8; i++) col[i] = out[i*8+j];
        double a[8];
        a[0] = col[0] + col[1]; a[1] = col[0] - col[1];
        a[2] = col[2] + col[3]; a[3] = col[2] - col[3];
        a[4] = col[4] + col[5]; a[5] = col[4] - col[5];
        a[6] = col[6] + col[7]; a[7] = col[6] - col[7];
        double b[8];
        b[0] = a[0]+a[2]; b[1] = a[1]+a[3]; b[2] = a[0]-a[2]; b[3] = a[1]-a[3];
        b[4] = a[4]+a[6]; b[5] = a[5]+a[7]; b[6] = a[4]-a[6]; b[7] = a[5]-a[7];
        tmp2[0*8+j] = (b[0]+b[4])/8.0; tmp2[1*8+j] = (b[1]+b[5])/8.0;
        tmp2[2*8+j] = (b[2]+b[6])/8.0; tmp2[3*8+j] = (b[3]+b[7])/8.0;
        tmp2[4*8+j] = (b[0]-b[4])/8.0; tmp2[5*8+j] = (b[1]-b[5])/8.0;
        tmp2[6*8+j] = (b[2]-b[6])/8.0; tmp2[7*8+j] = (b[3]-b[7])/8.0;
    }
    memcpy(out, tmp2, sizeof(double)*64);
}

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

static const int zz8[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

/* Sequency order for WHT (natural order for Walsh functions) */
static const int whseq[64] = {
     0, 1, 3, 2, 7, 6, 4, 5,
    15,14,12,13, 8, 9,11,10,
    31,30,28,29,24,25,27,26,
    16,17,19,18,23,22,20,21,
    63,62,60,61,56,57,59,58,
    48,49,51,50,55,54,52,53,
    32,33,35,34,39,38,36,37,
    47,46,44,45,40,41,43,42};

typedef void (*Transform)(double *in, double *out);

static void decode_and_render(const uint8_t *f, int fsize, int imgW, int imgH,
    Transform xform, const int *scan, int use_flags, double offset,
    const char *name) {
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
                for (int i = 1; i < 64 && !br_eof(&br); i++) {
                    if (use_flags) {
                        if (br_get1(&br))
                            block[scan[i]] = vlc_coeff(&br);
                    } else {
                        block[scan[i]] = vlc_coeff(&br);
                    }
                }
                double spatial[64];
                if (xform)
                    xform(block, spatial);
                else
                    memcpy(spatial, block, sizeof(double)*64); /* no transform */
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x] + offset;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double block[64]; memset(block,0,sizeof(block));
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                block[0] = *dc;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    if (use_flags) {
                        if (br_get1(&br))
                            block[scan[i]] = vlc_coeff(&br);
                    } else {
                        block[scan[i]] = vlc_coeff(&br);
                    }
                }
                double spatial[64];
                if (xform)
                    xform(block, spatial);
                else
                    memcpy(spatial, block, sizeof(double)*64);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
            }
        }
    }

    printf("  %s: bits %d/%d (%.1f%%)\n", name, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render_frame(planeY, planeCb, planeCr, imgW, imgH, name);
    free(planeY); free(planeCb); free(planeCr);
}

/* Spatial delta model: DC = base, AC values = pixel deltas in raster order */
static void decode_spatial_delta(const uint8_t *f, int fsize, int imgW, int imgH,
    const char *name) {
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
                dc_y += vlc_coeff(&br);
                double pixels[64];
                /* DC is the average/base value for all pixels */
                for (int i = 0; i < 64; i++) pixels[i] = dc_y + 128.0;
                /* AC values are deltas applied to specific pixels */
                for (int i = 1; i < 64 && !br_eof(&br); i++) {
                    if (br_get1(&br)) {
                        int v = vlc_coeff(&br);
                        pixels[i] += v;
                    }
                }
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = pixels[y*8+x];
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                double pixels[64];
                for (int i = 0; i < 64; i++) pixels[i] = *dc;
                for (int i = 1; i < 64 && !br_eof(&br); i++) {
                    if (br_get1(&br)) {
                        int v = vlc_coeff(&br);
                        pixels[i] += v;
                    }
                }
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = pixels[y*8+x];
            }
        }
    }

    printf("  %s: bits %d/%d (%.1f%%)\n", name, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render_frame(planeY, planeCb, planeCr, imgW, imgH, name);
    free(planeY); free(planeCb); free(planeCr);
}

/* Spatial DPCM model: DC = first pixel, then each pixel is delta from previous */
static void decode_spatial_dpcm(const uint8_t *f, int fsize, int imgW, int imgH,
    const char *name) {
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
                dc_y += vlc_coeff(&br);
                double pixels[64];
                pixels[0] = dc_y + 128.0;
                for (int i = 1; i < 64 && !br_eof(&br); i++) {
                    if (br_get1(&br))
                        pixels[i] = pixels[i-1] + vlc_coeff(&br);
                    else
                        pixels[i] = pixels[i-1]; /* no change */
                }
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = pixels[y*8+x];
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                double pixels[64];
                pixels[0] = *dc;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    if (br_get1(&br))
                        pixels[i] = pixels[i-1] + vlc_coeff(&br);
                    else
                        pixels[i] = pixels[i-1];
                }
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = pixels[y*8+x];
            }
        }
    }

    printf("  %s: bits %d/%d (%.1f%%)\n", name, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render_frame(planeY, planeCb, planeCr, imgW, imgH, name);
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
    printf("LBA %d: qs=%d type=%d\n", slba, frames[0][3], frames[0][39]);

    /* Identity scan (raster order, no zigzag) */
    int identity[64];
    for (int i = 0; i < 64; i++) identity[i] = i;

    /* Test 1: WHT with per-AC flags + zigzag */
    printf("\n--- Walsh-Hadamard Transform ---\n");
    decode_and_render(frames[0],fsizes[0],imgW,imgH, iwht8, zz8, 1, 128.0, "wht_zz_flag");

    /* Test 2: WHT with per-AC flags + identity scan */
    decode_and_render(frames[0],fsizes[0],imgW,imgH, iwht8, identity, 1, 128.0, "wht_id_flag");

    /* Test 3: DCT with per-AC flags but TRANSPOSED zigzag */
    int zz_transposed[64];
    for (int i = 0; i < 64; i++) {
        int r = zz8[i] / 8, c = zz8[i] % 8;
        zz_transposed[i] = c * 8 + r; /* swap row/col */
    }
    decode_and_render(frames[0],fsizes[0],imgW,imgH, idct8x8, zz_transposed, 1, 128.0, "dct_zzT_flag");

    /* Test 4: No transform (spatial delta) with per-AC flags */
    printf("\n--- Spatial Models ---\n");
    decode_spatial_delta(frames[0],fsizes[0],imgW,imgH, "spatial_delta");
    decode_spatial_dpcm(frames[0],fsizes[0],imgW,imgH, "spatial_dpcm");

    /* Test 5: No transform, per-AC flags, raster scan */
    decode_and_render(frames[0],fsizes[0],imgW,imgH, NULL, identity, 1, 128.0, "notransform_flag");

    /* Test 6: DCT but DC*8 (MPEG style) with per-AC flags */
    /* Custom: decode with DC multiplied by 8 */
    {
        const uint8_t *bs = frames[0] + 40;
        int bslen = fsizes[0] - 40;
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
                    block[0] = dc_y * 8.0; /* MPEG-1 style DC scaling */
                    for (int i=1;i<64&&!br_eof(&br);i++) {
                        if (br_get1(&br))
                            block[zz8[i]] = vlc_coeff(&br);
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
                    block[0] = *dc * 8.0;
                    for (int i=1;i<64&&!br_eof(&br);i++) {
                        if (br_get1(&br))
                            block[zz8[i]] = vlc_coeff(&br);
                    }
                    double spatial[64]; idct8x8(block, spatial);
                    double *plane=(c==0)?planeCb:planeCr;
                    for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                        plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
                }
            }
        }
        printf("  dc8_flag: bits %d/%d (%.1f%%)\n", br.total, bslen*8, 100.0*br.total/(bslen*8));
        render_frame(planeY, planeCb, planeCr, imgW, imgH, "dc8_acflag");
        free(planeY); free(planeCb); free(planeCr);
    }

    free(disc); zip_close(z);
    return 0;
}
