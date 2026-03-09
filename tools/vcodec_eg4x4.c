/*
 * Playdia video - Exp-Golomb coded 4×4 DCT blocks
 * Each block: 16 signed Exp-Golomb coded coefficients in zigzag order
 * Dequantize with QTable, then 4×4 IDCT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <zip.h>

#define SECTOR_RAW 2352
#define MAX_FRAME  65536
#define PI 3.14159265358979323846

static void write_pgm(const char *p, const uint8_t *g, int w, int h) {
    FILE *f=fopen(p,"wb"); if(!f)return;
    fprintf(f,"P5\n%d %d\n255\n",w,h); fwrite(g,1,w*h,f); fclose(f);
    printf("  -> %s (%dx%d)\n",p,w,h);
}

typedef struct { const uint8_t *data; int len,pos,bit,total; } BR;
static void br_init(BR *b, const uint8_t *d, int l) { b->data=d;b->len=l;b->pos=0;b->bit=7;b->total=0; }
static int br_get1(BR *b) {
    if(b->pos>=b->len) return -1;
    int v=(b->data[b->pos]>>b->bit)&1;
    if(--b->bit<0){b->bit=7;b->pos++;}
    b->total++; return v;
}
static int br_get(BR *b, int n) { int v=0; for(int i=0;i<n;i++){int x=br_get1(b);if(x<0)return 0;v=(v<<1)|x;} return v; }
static bool br_eof(BR *b) { return b->pos>=b->len; }

// Signed Exp-Golomb decode
static int eg_decode(BR *b) {
    int lz = 0;
    while (!br_eof(b)) {
        int bit = br_get1(b);
        if (bit < 0) return 0;
        if (bit == 1) break;
        lz++;
        if (lz > 24) return 0;
    }
    int suffix = lz > 0 ? br_get(b, lz) : 0;
    int code_num = (1 << lz) - 1 + suffix;
    // Signed mapping: 0→0, 1→1, 2→-1, 3→2, 4→-2, ...
    return (code_num & 1) ? -((code_num + 1) / 2) : (code_num / 2);
}

// Unsigned Exp-Golomb
static int eg_decode_u(BR *b) {
    int lz = 0;
    while (!br_eof(b)) {
        int bit = br_get1(b);
        if (bit < 0) return 0;
        if (bit == 1) break;
        lz++;
        if (lz > 24) return 0;
    }
    int suffix = lz > 0 ? br_get(b, lz) : 0;
    return (1 << lz) - 1 + suffix;
}

static const int zigzag4[16] = { 0,1,4,8, 5,2,3,6, 9,12,13,10, 7,11,14,15 };
static const int rowscan[16] = { 0,1,2,3, 4,5,6,7, 8,9,10,11, 12,13,14,15 };

static void idct4x4(const double c[16], int out[16]) {
    for(int y=0;y<4;y++) for(int x=0;x<4;x++) {
        double s=0;
        for(int v=0;v<4;v++){double cv=v==0?0.5:sqrt(0.5);
        for(int u=0;u<4;u++){double cu=u==0?0.5:sqrt(0.5);
        s+=cu*cv*c[v*4+u]*cos((2*x+1)*u*PI/8.0)*cos((2*y+1)*v*PI/8.0);}}
        out[y*4+x]=(int)round(s);
    }
}

// 4×4 inverse Walsh-Hadamard
static void iwht4x4(const double c[16], int out[16]) {
    double tmp[16];
    for(int r=0;r<4;r++){
        double a=c[r*4],b=c[r*4+1],cc=c[r*4+2],d=c[r*4+3];
        tmp[r*4]=a+b+cc+d; tmp[r*4+1]=a-b+cc-d;
        tmp[r*4+2]=a+b-cc-d; tmp[r*4+3]=a-b-cc+d;
    }
    for(int co=0;co<4;co++){
        double a=tmp[co],b=tmp[4+co],cc2=tmp[8+co],d=tmp[12+co];
        out[co]=(int)round((a+b+cc2+d)/4.0);
        out[4+co]=(int)round((a-b+cc2-d)/4.0);
        out[8+co]=(int)round((a+b-cc2-d)/4.0);
        out[12+co]=(int)round((a-b-cc2+d)/4.0);
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

int main(int argc, char **argv) {
    if(argc<2){fprintf(stderr,"Usage: %s <zip> [lba]\n",argv[0]);return 1;}
    int slba = argc>2 ? atoi(argv[2]) : 502;

    int err; zip_t *z=zip_open(argv[1],ZIP_RDONLY,&err);
    if(!z){return 1;}
    int bi=-1;zip_uint64_t bs2=0;
    for(int i=0;i<(int)zip_get_num_entries(z,0);i++){
        zip_stat_t st;if(zip_stat_index(z,i,0,&st)==0&&st.size>bs2){bs2=st.size;bi=i;}}
    zip_stat_t st;zip_stat_index(z,bi,0,&st);
    zip_file_t *zf=zip_fopen_index(z,bi,0);
    uint8_t *disc=malloc(st.size);
    zip_int64_t rd=0;
    while(rd<(zip_int64_t)st.size){zip_int64_t r=zip_fread(zf,disc+rd,st.size-rd);if(r<=0)break;rd+=r;}
    zip_fclose(zf);
    int tsec=(int)(st.size/SECTOR_RAW);

    static uint8_t frames[8][MAX_FRAME]; int fsizes[8];
    int nf=assemble_frames(disc,tsec,slba,frames,fsizes,8);
    printf("Assembled %d frames\n",nf);

    // Process frames 0 and 1
    for (int fi = 0; fi < 2 && fi < nf; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        uint8_t qtab[16]; memcpy(qtab,f+4,16);
        int qscale = f[3];
        printf("\n=== FRAME %d: %d bytes, qscale=%d ===\n", fi, fsize, qscale);

        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;
        int W = 128, H = 144;
        int bw = W/4, bh = H/4;
        int total_blocks = bw * bh; // 1152

        // Configuration variants
        typedef struct {
            const char *name;
            int use_qtab;     // 0=none, 1=qtab only, 2=qtab*qscale
            int use_zigzag;   // 0=row scan, 1=zigzag
            int use_dct;      // 0=WHT, 1=DCT, 2=none (raw)
            int dc_bias;
        } Config;

        Config configs[] = {
            {"eg_dct_qf",  2, 1, 1, 128},
            {"eg_dct_qt",  1, 1, 1, 128},
            {"eg_dct_nq",  0, 1, 1, 128},
            {"eg_wht_qf",  2, 1, 0, 128},
            {"eg_wht_nq",  0, 1, 0, 128},
            {"eg_dct_nq_r", 0, 0, 1, 128},  // row scan
            {"eg_none_nq", 0, 1, 2, 128},   // no transform
        };
        int ncfg = sizeof(configs)/sizeof(configs[0]);

        for (int ci = 0; ci < ncfg; ci++) {
            Config *cfg = &configs[ci];
            BR br; br_init(&br, bs, bslen);
            uint8_t *img = calloc(W*H, 1);
            int blocks = 0;
            const int *scan = cfg->use_zigzag ? zigzag4 : rowscan;

            for (int by = 0; by < bh && !br_eof(&br); by++) {
                for (int bx = 0; bx < bw && !br_eof(&br); bx++) {
                    double coeff[16] = {0};

                    for (int k = 0; k < 16 && !br_eof(&br); k++) {
                        int val = eg_decode(&br);
                        int pos = scan[k];
                        switch (cfg->use_qtab) {
                            case 0: coeff[pos] = val; break;
                            case 1: coeff[pos] = val * qtab[k]; break;
                            case 2: coeff[pos] = val * qtab[k] * qscale; break;
                        }
                    }

                    int pixels[16];
                    switch (cfg->use_dct) {
                        case 0: iwht4x4(coeff, pixels); break;
                        case 1: idct4x4(coeff, pixels); break;
                        case 2: for(int i=0;i<16;i++) pixels[i]=(int)round(coeff[i]); break;
                    }

                    for (int dy = 0; dy < 4; dy++)
                        for (int dx = 0; dx < 4; dx++) {
                            int px = bx*4+dx, py = by*4+dy;
                            if (px<W && py<H) {
                                int v = pixels[dy*4+dx] + cfg->dc_bias;
                                if(v<0)v=0;if(v>255)v=255;
                                img[py*W+px] = v;
                            }
                        }
                    blocks++;
                }
            }

            printf("  %s: %d blocks, %d bits (%.1f bits/block)\n",
                   cfg->name, blocks, br.total, (double)br.total/blocks);

            char path[256];
            snprintf(path,sizeof(path),"/home/wizzard/share/GitHub/pd_%s_f%d.pgm",cfg->name,fi);
            write_pgm(path, img, W, H);
            free(img);
        }

        // === Try with run-length coding for AC ===
        // DC: Exp-Golomb, then AC: (run, level) pairs where run and level are Exp-Golomb
        // EOB could be signaled by run >= 16 or some special value
        printf("\n  --- EG DC + EG run-level AC ---\n");
        {
            BR br; br_init(&br, bs, bslen);
            uint8_t *img = calloc(W*H, 1);
            int blocks = 0;

            for (int by = 0; by < bh && !br_eof(&br); by++) {
                for (int bx = 0; bx < bw && !br_eof(&br); bx++) {
                    double coeff[16] = {0};

                    // DC
                    int dc = eg_decode(&br);
                    coeff[0] = dc;

                    // AC with run-level
                    int k = 1;
                    while (k < 16 && !br_eof(&br)) {
                        int run = eg_decode_u(&br); // unsigned run
                        if (run >= 16) break; // EOB signal
                        k += run;
                        if (k >= 16) break;
                        int level = eg_decode(&br); // signed level
                        coeff[zigzag4[k]] = level;
                        k++;
                    }

                    int pixels[16];
                    idct4x4(coeff, pixels);

                    for (int dy = 0; dy < 4; dy++)
                        for (int dx = 0; dx < 4; dx++) {
                            int px = bx*4+dx, py = by*4+dy;
                            if (px<W && py<H) {
                                int v = pixels[dy*4+dx] + 128;
                                if(v<0)v=0;if(v>255)v=255;
                                img[py*W+px] = v;
                            }
                        }
                    blocks++;
                }
            }

            printf("  eg_rl: %d blocks, %d bits (%.1f bits/block)\n",
                   blocks, br.total, (double)br.total/blocks);
            char path[256];
            snprintf(path,sizeof(path),"/home/wizzard/share/GitHub/pd_eg_rl_f%d.pgm",fi);
            write_pgm(path, img, W, H);
            free(img);
        }

        // DC-only: extract first of every 16 EG values
        printf("\n  --- EG DC-only image ---\n");
        {
            BR br; br_init(&br, bs, bslen);
            uint8_t dcimg[32*36];
            memset(dcimg, 128, sizeof(dcimg));
            int nblocks = 0;

            for (int i = 0; i < bw*bh && !br_eof(&br); i++) {
                int dc = eg_decode(&br);
                int v = dc + 128;
                if (v<0)v=0; if(v>255)v=255;
                dcimg[i] = v;

                // Skip 15 AC values
                for (int k = 1; k < 16 && !br_eof(&br); k++)
                    eg_decode(&br);
                nblocks++;
            }

            printf("  DC-only: %d blocks, %d bits\n", nblocks, br.total);

            // Scale
            uint8_t scaled[256*288];
            for (int y=0;y<288;y++)
                for(int x=0;x<256;x++)
                    scaled[y*256+x] = dcimg[(y/8)*bw + (x/8)];
            char path[256];
            snprintf(path,sizeof(path),"/home/wizzard/share/GitHub/pd_eg_dc_f%d.pgm",fi);
            write_pgm(path, scaled, 256, 288);
        }
    }

    free(disc); zip_close(z);
    return 0;
}
