/*
 * vcodec_acvlc.c - Test different VLC tables for AC coefficients
 *
 * Key insight: the per-AC flag model uses 1 bit for zero and 1+VLC for nonzero.
 * But what if there's no separate flag? What if AC uses a different VLC where:
 *   - "0" = zero (1 bit)
 *   - "10" + N bits = size N value (unary prefix)
 *
 * Also test: what if AC uses MPEG-1 DC chrominance VLC?
 * Also test: what if AC VLC has a different tree structure entirely?
 *
 * And a radical test: what if the "per-AC flag" bits are actually a
 * SIGNIFICANCE MAP (like in H.264 CABAC) read separately from values?
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

/* DC VLC (confirmed MPEG-1 DC luminance, slightly modified) */
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

/* AC VLC type 1: "0" = zero, "1" + DC_VLC = nonzero value */
static int vlc_ac1(BR *b) {
    if (!br_get1(b)) return 0;
    return vlc_dc(b);
}

/* AC VLC type 2: MPEG-1 DC chrominance table */
static int vlc_ac_chrom(BR *b) {
    int size;
    /* 00=0, 01=1, 10=2, 110=3, 1110=4, 11110=5, 111110=6, 1111110=7, 11111110=8 */
    int b1 = br_get1(b);
    int b2 = br_get1(b);
    if (b1 == 0) {
        if (b2 == 0) return 0; /* 00 = size 0 = value 0 */
        size = 1; /* 01 = size 1 */
    } else {
        if (b2 == 0) size = 2; /* 10 = size 2 */
        else {
            /* 11... */
            if (!br_get1(b)) size = 3;
            else if (!br_get1(b)) size = 4;
            else if (!br_get1(b)) size = 5;
            else if (!br_get1(b)) size = 6;
            else size = br_get1(b) ? 8 : 7;
        }
    }
    if (size == 0) return 0;
    int val = br_get(b, size);
    if (val < (1 << (size-1))) val = val - (1 << size) + 1;
    return val;
}

/* AC VLC type 3: unary prefix for size
 * 0=0, 10=size1, 110=size2, 1110=size3, etc. */
static int vlc_ac_unary(BR *b) {
    int size = 0;
    while (br_get1(b) && size < 12 && !br_eof(b))
        size++;
    if (size == 0) return 0;
    int val = br_get(b, size);
    if (val < (1 << (size-1))) val = val - (1 << size) + 1;
    return val;
}

/* AC VLC type 4: Exp-Golomb order 0 (like H.264)
 * 0=0, 010=1, 011=-1, 00100=2, 00101=-2, 00110=3, 00111=-3, etc. */
static int vlc_ac_expgolomb(BR *b) {
    int zeros = 0;
    while (!br_get1(b) && zeros < 16 && !br_eof(b))
        zeros++;
    if (zeros == 0) return 0;
    int val = br_get(b, zeros);
    int code = (1 << zeros) - 1 + val;
    /* Map to signed: even = positive, odd = negative */
    if (code & 1)
        return -((code + 1) / 2);
    else
        return code / 2;
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
    snprintf(path,sizeof(path),OUT_DIR "av_%s.ppm",name);
    write_ppm(path, rgb, imgW, imgH);
    printf("  %s: Y[%.0f, %.0f]\n", name, pmin, pmax);
    free(rgb);
}

typedef int (*AcVlcFunc)(BR *b);

static void test_ac_vlc(const uint8_t *f, int fsize, int imgW, int imgH,
    AcVlcFunc ac_vlc, int use_eob, const char *name) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double *planeY = calloc(imgW*imgH, sizeof(double));
    double *planeCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr = calloc((imgW/2)*(imgH/2), sizeof(double));
    double dc_y=0, dc_cb=0, dc_cr=0;
    int total_blocks = 0, eob_count = 0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[64]; memset(block,0,sizeof(block));
                dc_y += vlc_dc(&br);
                block[0] = dc_y;
                for (int i = 1; i < 64 && !br_eof(&br); i++) {
                    int v = ac_vlc(&br);
                    if (use_eob && v == 0) { eob_count++; break; }
                    block[zz8[i]] = v;
                }
                double spatial[64]; idct8x8(block, spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
                total_blocks++;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double block[64]; memset(block,0,sizeof(block));
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_dc(&br);
                block[0] = *dc;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    int v = ac_vlc(&br);
                    if (use_eob && v == 0) { eob_count++; break; }
                    block[zz8[i]] = v;
                }
                double spatial[64]; idct8x8(block, spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
                total_blocks++;
            }
        }
    }

    printf("  %s: blocks=%d eob=%d bits=%d/%d (%.1f%%)\n",
        name, total_blocks, eob_count, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render_frame(planeY, planeCb, planeCr, imgW, imgH, name);
    free(planeY); free(planeCb); free(planeCr);
}

/* Special test: per-AC flag with chrominance VLC for values */
static void test_flag_chrom(const uint8_t *f, int fsize, int imgW, int imgH, const char *name) {
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
                    if (br_get1(&br))
                        block[zz8[i]] = vlc_ac_chrom(&br);
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
                    if (br_get1(&br))
                        block[zz8[i]] = vlc_ac_chrom(&br);
                }
                double spatial[64]; idct8x8(block, spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
            }
        }
    }

    printf("  %s: bits=%d/%d (%.1f%%)\n", name, br.total, bslen*8, 100.0*br.total/(bslen*8));
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

    /* Test 1: AC VLC type 1 — "0"=zero, "1"+DC_VLC=value (no separate flag) */
    printf("\n--- AC VLC type 1 (0=zero, 1+DC_VLC=value) ---\n");
    test_ac_vlc(frames[0],fsizes[0],imgW,imgH, vlc_ac1, 0, "ac1_noeob");

    /* Test 2: Same with 0 as EOB */
    printf("\n--- AC VLC type 1 with EOB ---\n");
    /* This is same as flag model + EOB on value 0 */

    /* Test 3: Chrominance VLC for all coefficients (no flags) */
    printf("\n--- Chrominance VLC (no flags, sequential, no EOB) ---\n");
    test_ac_vlc(frames[0],fsizes[0],imgW,imgH, vlc_ac_chrom, 0, "chrom_noeob");

    /* Test 4: Chrominance VLC with EOB on zero */
    printf("\n--- Chrominance VLC with EOB ---\n");
    test_ac_vlc(frames[0],fsizes[0],imgW,imgH, vlc_ac_chrom, 1, "chrom_eob");

    /* Test 5: Unary prefix VLC (no flags, sequential) */
    printf("\n--- Unary prefix VLC ---\n");
    test_ac_vlc(frames[0],fsizes[0],imgW,imgH, vlc_ac_unary, 0, "unary_noeob");
    test_ac_vlc(frames[0],fsizes[0],imgW,imgH, vlc_ac_unary, 1, "unary_eob");

    /* Test 6: Exp-Golomb VLC */
    printf("\n--- Exp-Golomb VLC ---\n");
    test_ac_vlc(frames[0],fsizes[0],imgW,imgH, vlc_ac_expgolomb, 0, "expg_noeob");

    /* Test 7: Per-AC flag with chrominance VLC for values */
    printf("\n--- Per-AC flag + chrominance VLC for values ---\n");
    test_flag_chrom(frames[0],fsizes[0],imgW,imgH, "flag_chrom");

    free(disc); zip_close(z);
    return 0;
}
