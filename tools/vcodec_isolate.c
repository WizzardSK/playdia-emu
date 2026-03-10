/*
 * vcodec_isolate.c - Isolate the problem: try per-AC flag model with
 * different dequant, zigzag, and block orderings to find what's wrong.
 *
 * Tests:
 * A. No dequant (raw VLC values → IDCT)
 * B. DC*8 only, AC unscaled (no qm multiplication)
 * C. Standard dequant but NO zigzag (linear scan)
 * D. Standard dequant, column-major zigzag
 * E. Different Y block order (TL,BL,TR,BR vs TL,TR,BL,BR)
 * F. Row-major scan order
 * G. Reduced AC: multiply by smaller factor
 * H. JPEG-style: DC*8, AC * qm[i] (no qscale division)
 * I. Try DC without DPCM accumulation (absolute DC)
 * J. No IDCT - raw coefficients placed directly
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
static const int zz8[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

/* Column-major zigzag (transpose of standard) */
static const int zz8_col[64] = {
     0, 8, 1,16, 9, 2,24,17,10, 3,32,25,18,11, 4,40,
    33,26,19,12, 5,48,41,34,27,20,13, 6,56,49,42,35,
    28,21,14, 7,57,50,43,36,29,22,15,58,51,44,37,30,
    23,59,52,45,38,31,60,53,46,39,61,54,47,62,55,63};

/* Row-major (no zigzag) */
static int zz8_row[64];

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

static void place_block8(int *plane, int pw, int bx, int by, double spatial[64]) {
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            plane[(by*8+y)*pw + bx*8+x] = (int)round(spatial[y*8+x]);
}

static void output_image(int *planeY, int imgW, int imgH, const char *name) {
    int pmin=99999, pmax=-99999;
    for(int i=0;i<imgW*imgH;i++){
        if(planeY[i]<pmin)pmin=planeY[i];
        if(planeY[i]>pmax)pmax=planeY[i];
    }
    uint8_t *yimg = malloc(imgW*imgH);
    if (pmax > pmin) {
        for(int i=0;i<imgW*imgH;i++)
            yimg[i] = clamp8(255*(planeY[i]-pmin)/(pmax-pmin));
    } else {
        memset(yimg, 128, imgW*imgH);
    }
    char path[256];
    snprintf(path,sizeof(path),OUT_DIR "%s.pgm",name);
    write_pgm(path, yimg, imgW, imgH);
    printf("    [%d,%d] -> %s\n", pmin, pmax, path);
    free(yimg);
}

typedef struct {
    int dequant_mode; /* 0=none, 1=DC*8+AC*qm*qs/16, 2=AC only(no qm), 3=AC*qm(no qs), 4=reduced */
    const int *zigzag; /* zigzag table */
    int yblock_order; /* 0=TL,TR,BL,BR  1=TL,BL,TR,BR  2=TL,TR,BR,BL */
    int dc_mode; /* 0=DPCM, 1=absolute, 2=DPCM reset per row */
    int do_idct; /* 0=no IDCT, 1=IDCT */
} TestConfig;

static void decode_frame(const uint8_t *f, int fsize, int imgW, int imgH,
    TestConfig *cfg, const char *name) {
    int qscale = f[3];
    uint8_t qt[16]; memcpy(qt, f+4, 16);
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;

    int qm[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            qm[i*8+j] = qt[(i/2)*4 + (j/2)];

    BR br; br_init(&br, bs, bslen);
    int *planeY = calloc(imgW*imgH, sizeof(int));
    int dc_y = 0;

    /* Y block positions within macroblock for different orderings */
    int yb_bx[3][4] = {{0,1,0,1}, {0,0,1,1}, {0,1,1,0}};
    int yb_by[3][4] = {{0,0,1,1}, {0,1,0,1}, {0,0,1,1}};

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        if (cfg->dc_mode == 2) dc_y = 0;
        int dc_cb = 0, dc_cr = 0;
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                int block[64];
                memset(block, 0, sizeof(block));

                /* DC */
                int dv = vlc_coeff(&br);
                if (cfg->dc_mode == 0 || cfg->dc_mode == 2)
                    dc_y += dv;
                else
                    dc_y = dv;

                switch (cfg->dequant_mode) {
                    case 0: block[0] = dc_y; break;
                    case 1: block[0] = dc_y * 8; break;
                    case 2: block[0] = dc_y * 8; break;
                    case 3: block[0] = dc_y * 8; break;
                    case 4: block[0] = dc_y * 8; break;
                }

                /* AC with per-bit flags */
                for (int i = 1; i < 64 && !br_eof(&br); i++) {
                    if (br_get1(&br)) {
                        int v = vlc_coeff(&br);
                        int zi = cfg->zigzag[i];
                        switch (cfg->dequant_mode) {
                            case 0: block[zi] = v; break;
                            case 1: block[zi] = v * qm[zi] * qscale / 16; break;
                            case 2: block[zi] = v; break; /* no qm */
                            case 3: block[zi] = v * qm[zi]; break; /* no qscale */
                            case 4: block[zi] = v * 2; break; /* reduced */
                        }
                    }
                }

                int bx_off = yb_bx[cfg->yblock_order][yb];
                int by_off = yb_by[cfg->yblock_order][yb];

                if (cfg->do_idct) {
                    double spatial[64];
                    idct8x8(block, spatial);
                    place_block8(planeY, imgW, mbx*2+bx_off, mby*2+by_off, spatial);
                } else {
                    /* Direct placement (no IDCT) */
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x++)
                            planeY[(mby*2+by_off)*8*imgW + (mbx*2+bx_off)*8 + y*imgW + x] = block[y*8+x];
                }
            }
            /* Skip Cb, Cr */
            for (int c = 0; c < 2 && !br_eof(&br); c++) {
                int dv = vlc_coeff(&br);
                if (c == 0) dc_cb += dv; else dc_cr += dv;
                for (int i = 1; i < 64 && !br_eof(&br); i++)
                    if (br_get1(&br)) vlc_coeff(&br);
            }
        }
    }

    printf("  %s: bits %d/%d (%.1f%%)\n", name, br.total, bslen*8, 100.0*br.total/(bslen*8));
    output_image(planeY, imgW, imgH, name);
    free(planeY);
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    int slba = argc > 2 ? atoi(argv[2]) : 5232;

    /* Init row-major zigzag */
    for (int i = 0; i < 64; i++) zz8_row[i] = i;

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
    if (nf < 1) { printf("No frames at LBA %d\n", slba); free(disc); zip_close(z); return 1; }

    int imgW = 128, imgH = 144;
    printf("LBA %d: qs=%d type=%d fsize=%d\n", slba, frames[0][3], frames[0][39], fsizes[0]);

    TestConfig tests[] = {
        /* A: No dequant, standard zigzag, DPCM DC, IDCT */
        {0, zz8, 0, 0, 1},
        /* B: DC*8, AC unscaled, standard zigzag, DPCM, IDCT */
        {2, zz8, 0, 0, 1},
        /* C: Standard dequant, NO zigzag (row-major), DPCM, IDCT */
        {1, zz8_row, 0, 0, 1},
        /* D: Standard dequant, column-major zigzag, DPCM, IDCT */
        {1, zz8_col, 0, 0, 1},
        /* E: Standard dequant, standard zigzag, alt Y order, DPCM, IDCT */
        {1, zz8, 1, 0, 1},
        /* F: No dequant, standard zigzag, DPCM, NO IDCT */
        {0, zz8, 0, 0, 0},
        /* G: AC*qm only (no qscale), standard zigzag, DPCM, IDCT */
        {3, zz8, 0, 0, 1},
        /* H: Reduced AC(*2), standard zigzag, DPCM, IDCT */
        {4, zz8, 0, 0, 1},
        /* I: Standard dequant, standard zigzag, absolute DC, IDCT */
        {1, zz8, 0, 1, 1},
        /* J: Standard dequant, standard zigzag, DPCM DC reset per row, IDCT */
        {1, zz8, 0, 2, 1},
        /* K: No dequant, row-major, DPCM, NO IDCT (raw spatial) */
        {0, zz8_row, 0, 0, 0},
        /* L: No dequant, standard zigzag, absolute DC, IDCT */
        {0, zz8, 0, 1, 1},
    };
    const char *names[] = {
        "iso_A_nodequant", "iso_B_dconly8", "iso_C_rowmajor",
        "iso_D_colzz", "iso_E_altYord", "iso_F_noidct",
        "iso_G_acqm", "iso_H_reduced", "iso_I_absdc",
        "iso_J_dcreset", "iso_K_raw_rowmajor", "iso_L_nodq_absdc"
    };
    int ntests = sizeof(tests)/sizeof(tests[0]);

    for (int t = 0; t < ntests; t++) {
        decode_frame(frames[0], fsizes[0], imgW, imgH, &tests[t], names[t]);
    }

    free(disc); zip_close(z);
    return 0;
}
