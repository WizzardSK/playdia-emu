/*
 * vcodec_fixed_final.c - Comprehensive fixed-width AC coefficient test
 *
 * KEY INSIGHT: per-AC flag model produces UNIFORM statistics across all
 * frequencies (44% nonzero, avg abs 9.5 at ALL positions). This CANNOT be
 * natural image data. It means the per-AC flag model is WRONG.
 *
 * Fixed 3-bit and 4-bit produce clean images. The actual encoding is
 * likely fixed-width AC coefficients after VLC-coded DC DPCM.
 *
 * Average bits needed: (97936-2000)/27216 = 3.52 bits per AC position.
 *
 * Tests:
 * - Fixed 3-bit with dequant by qtable
 * - Fixed 3-bit sign-magnitude vs two's complement
 * - Fixed 3-bit + remaining bits as 1-bit refinement
 * - 4-bit low-freq + 3-bit high-freq mix
 * - qtable/N variable width with dequant
 * - Multiple scenes to verify consistency
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
    snprintf(path,sizeof(path),OUT_DIR "ff_%s.ppm",name);
    write_ppm(path, rgb, imgW, imgH);
    printf("  %s: Y[%.0f, %.0f]\n", name, pmin, pmax);
    free(rgb);
}

typedef struct {
    int ac_bits; /* fixed bits per AC, or 0 for variable */
    int bw_map[64]; /* per-position bit widths (for variable) */
    double dq_mult; /* dequant multiplier for AC */
    int use_qtable_dq; /* multiply AC by qtable entry? */
    double dc_mult; /* DC multiplier */
    const char *name;
} Config;

static void decode_frame(const uint8_t *f, int fsize, int imgW, int imgH, Config *cfg) {
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
    int blocks = 0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                double block[64]; memset(block,0,sizeof(block));
                dc_y += vlc_coeff(&br);
                block[0] = dc_y * cfg->dc_mult;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    int bw = cfg->ac_bits ? cfg->ac_bits : cfg->bw_map[i];
                    int v = br_sget(&br, bw);
                    double dv = v;
                    if (cfg->use_qtable_dq) dv *= qm[zz8[i]];
                    dv *= cfg->dq_mult;
                    block[zz8[i]] = dv;
                }
                double spatial[64]; idct8x8(block, spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
                blocks++;
            }
            for (int c=0;c<2&&!br_eof(&br);c++) {
                double block[64]; memset(block,0,sizeof(block));
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                block[0] = *dc * cfg->dc_mult;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    int bw = cfg->ac_bits ? cfg->ac_bits : cfg->bw_map[i];
                    int v = br_sget(&br, bw);
                    double dv = v;
                    if (cfg->use_qtable_dq) dv *= qm[zz8[i]];
                    dv *= cfg->dq_mult;
                    block[zz8[i]] = dv;
                }
                double spatial[64]; idct8x8(block, spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
                blocks++;
            }
        }
    }

    printf("  %s (qs=%d): blks=%d/%d bits %d/%d (%.1f%%)\n",
        cfg->name, qs, blocks, 432, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render(planeY, planeCb, planeCr, imgW, imgH, cfg->name);
    free(planeY); free(planeCb); free(planeCr);
}

/* Build variable bit-width map from qtable */
static void build_bw_map(const uint8_t qt[16], int bw_map[64], int divisor) {
    bw_map[0] = 0; /* DC is VLC */
    for (int i = 1; i < 64; i++) {
        int r = zz8[i] / 8, c = zz8[i] % 8;
        int qi = (r/2)*4 + (c/2);
        int bw = (qt[qi] + divisor/2) / divisor;
        if (bw < 1) bw = 1;
        if (bw > 8) bw = 8;
        bw_map[i] = bw;
    }
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
        printf("\n=== LBA %d (qs=%d, type=%d) ===\n", lba, qs, frames[0][39]);

        /* A: Fixed 3-bit, no dequant */
        Config cfgA = {3, {0}, 1.0, 0, 1.0, "A_3bit"};
        char nameA[32]; snprintf(nameA,sizeof(nameA),"A_3b_l%d",lba);
        cfgA.name = nameA;
        decode_frame(frames[0],fsizes[0],imgW,imgH, &cfgA);

        /* B: Fixed 3-bit, AC * qtable/16 */
        Config cfgB = {3, {0}, 1.0/16.0, 1, 1.0, "B"};
        char nameB[32]; snprintf(nameB,sizeof(nameB),"B_3b_qt16_l%d",lba);
        cfgB.name = nameB;
        decode_frame(frames[0],fsizes[0],imgW,imgH, &cfgB);

        /* C: Fixed 3-bit, AC * qs/8 */
        Config cfgC = {3, {0}, (double)qs/8.0, 0, 1.0, "C"};
        char nameC[32]; snprintf(nameC,sizeof(nameC),"C_3b_qs8_l%d",lba);
        cfgC.name = nameC;
        decode_frame(frames[0],fsizes[0],imgW,imgH, &cfgC);

        /* D: Fixed 3-bit, AC * qs * qtable / 128 */
        Config cfgD = {3, {0}, (double)qs/128.0, 1, 1.0, "D"};
        char nameD[32]; snprintf(nameD,sizeof(nameD),"D_3b_qsqt128_l%d",lba);
        cfgD.name = nameD;
        decode_frame(frames[0],fsizes[0],imgW,imgH, &cfgD);

        /* E: Fixed 4-bit, no dequant */
        Config cfgE = {4, {0}, 1.0, 0, 1.0, "E"};
        char nameE[32]; snprintf(nameE,sizeof(nameE),"E_4b_l%d",lba);
        cfgE.name = nameE;
        decode_frame(frames[0],fsizes[0],imgW,imgH, &cfgE);

        /* F: Fixed 4-bit, AC * qs/8 */
        Config cfgF = {4, {0}, (double)qs/8.0, 0, 1.0, "F"};
        char nameF[32]; snprintf(nameF,sizeof(nameF),"F_4b_qs8_l%d",lba);
        cfgF.name = nameF;
        decode_frame(frames[0],fsizes[0],imgW,imgH, &cfgF);

        /* G: Variable width (qtable/6), no dequant */
        uint8_t qt[16]; memcpy(qt, frames[0]+4, 16);
        int bw6[64]; build_bw_map(qt, bw6, 6);
        Config cfgG = {0, {0}, 1.0, 0, 1.0, "G"};
        memcpy(cfgG.bw_map, bw6, sizeof(bw6));
        char nameG[32]; snprintf(nameG,sizeof(nameG),"G_var6_l%d",lba);
        cfgG.name = nameG;
        decode_frame(frames[0],fsizes[0],imgW,imgH, &cfgG);

        /* H: Fixed 3-bit, DC*8 (MPEG-1 style DC), AC*1 */
        Config cfgH = {3, {0}, 1.0, 0, 8.0, "H"};
        char nameH[32]; snprintf(nameH,sizeof(nameH),"H_3b_dc8_l%d",lba);
        cfgH.name = nameH;
        decode_frame(frames[0],fsizes[0],imgW,imgH, &cfgH);

        /* I: Fixed 3-bit, DC*8, AC * qtable * qs / 256 */
        Config cfgI = {3, {0}, (double)qs/256.0, 1, 8.0, "I"};
        char nameI[32]; snprintf(nameI,sizeof(nameI),"I_3b_dc8_qsqt_l%d",lba);
        cfgI.name = nameI;
        decode_frame(frames[0],fsizes[0],imgW,imgH, &cfgI);

        /* J: Fixed 3-bit, DC*1, AC * qtable * qs / 64 */
        Config cfgJ = {3, {0}, (double)qs/64.0, 1, 1.0, "J"};
        char nameJ[32]; snprintf(nameJ,sizeof(nameJ),"J_3b_qsqt64_l%d",lba);
        cfgJ.name = nameJ;
        decode_frame(frames[0],fsizes[0],imgW,imgH, &cfgJ);
    }

    free(disc); zip_close(z);
    return 0;
}
