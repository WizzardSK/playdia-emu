/*
 * Playdia video - Final decoder with confirmed structure:
 * - 128×144 YCbCr 4:2:0
 * - 8×8 DCT blocks, zigzag scan
 * - ALL coefficients (DC and AC) use MPEG-1 DC luminance VLC (magnitude/sign)
 * - DC is differential (accumulated from previous block)
 * - Macroblock order: 4Y (TL,TR,BL,BR) + 1Cb + 1Cr
 *
 * TODO: figure out correct dequantization and DC prediction reset
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
#define OUT_DIR "/home/wizzard/share/GitHub/playdia-emu/tools/test_output/"

static void write_pgm(const char *p, const uint8_t *g, int w, int h) {
    FILE *f=fopen(p,"wb"); if(!f)return;
    fprintf(f,"P5\n%d %d\n255\n",w,h); fwrite(g,1,w*h,f); fclose(f);
    printf("  -> %s (%dx%d)\n",p,w,h);
}

static void write_ppm(const char *p, const uint8_t *rgb, int w, int h) {
    FILE *f=fopen(p,"wb"); if(!f)return;
    fprintf(f,"P6\n%d %d\n255\n",w,h); fwrite(rgb,1,w*h*3,f); fclose(f);
    printf("  -> %s (%dx%d RGB)\n",p,w,h);
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

/* MPEG-1 DC luminance VLC - confirmed as the VLC for all coefficients */
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

static const int zigzag8[64] = {
     0, 1, 8,16, 9, 2, 3,10,
    17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,
    27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,
    29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};

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

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/*
 * Decode a frame with specific dequantization mode
 * dq_mode:
 *   0: raw (no dequant)
 *   1: val * qt[pos]
 *   2: val * qt[pos] * qscale / 8
 *   3: val * qt[pos] * qscale / 16
 *   4: DC = val * 8, AC = val * qt[pos] * qscale / 8
 *   5: DC = val, AC = val * qt[pos]
 *   6: DC = val * qt[0], AC = val * qt[pos]
 *   7: all * qscale
 *   8: DC differential, AC absolute, all * qt[pos]
 *
 * dc_reset: 0=never, 1=per MB row, 2=per MB
 */
static void decode_frame(const uint8_t *bs, int bslen, int qscale,
                          const uint8_t qt_raw[16], const char *tag,
                          int imgW, int imgH, int dq_mode, int dc_reset) {
    int mbw = imgW / 16, mbh = imgH / 16;
    int *planeY = calloc(imgW * imgH, sizeof(int));
    int *planeCb = calloc(imgW/2 * imgH/2, sizeof(int));
    int *planeCr = calloc(imgW/2 * imgH/2, sizeof(int));
    BR br; br_init(&br, bs, bslen);

    /* Build 8×8 quant matrix from 4×4 qtable */
    int qm[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            qm[i*8+j] = qt_raw[(i/2)*4 + (j/2)];

    int dc_y = 0, dc_cb = 0, dc_cr = 0;

    for (int mby = 0; mby < mbh && !br_eof(&br); mby++) {
        if (dc_reset == 1) { dc_y = 0; dc_cb = 0; dc_cr = 0; }

        for (int mbx = 0; mbx < mbw && !br_eof(&br); mbx++) {
            if (dc_reset == 2) { dc_y = 0; dc_cb = 0; dc_cr = 0; }

            /* 4 Y blocks */
            double yspat[4][64];
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                int block[64] = {0};

                /* Read all 64 coefficients */
                int raw[64];
                for (int i = 0; i < 64 && !br_eof(&br); i++)
                    raw[i] = vlc_coeff(&br);

                /* DC is differential */
                dc_y += raw[0];

                /* Dequantize based on mode */
                switch (dq_mode) {
                case 0: /* raw */
                    block[0] = dc_y;
                    for (int i=1;i<64;i++) block[zigzag8[i]] = raw[i];
                    break;
                case 1: /* val * qt */
                    block[0] = dc_y * qm[0];
                    for (int i=1;i<64;i++) block[zigzag8[i]] = raw[i] * qm[zigzag8[i]];
                    break;
                case 2: /* val * qt * qs / 8 */
                    block[0] = dc_y * qm[0] * qscale / 8;
                    for (int i=1;i<64;i++) block[zigzag8[i]] = raw[i] * qm[zigzag8[i]] * qscale / 8;
                    break;
                case 3: /* val * qt * qs / 16 */
                    block[0] = dc_y * qm[0] * qscale / 16;
                    for (int i=1;i<64;i++) block[zigzag8[i]] = raw[i] * qm[zigzag8[i]] * qscale / 16;
                    break;
                case 4: /* MPEG-1 style: DC*8, AC*qt*qs/8 */
                    block[0] = dc_y * 8;
                    for (int i=1;i<64;i++) block[zigzag8[i]] = raw[i] * qm[zigzag8[i]] * qscale / 8;
                    break;
                case 5: /* DC raw, AC * qt */
                    block[0] = dc_y;
                    for (int i=1;i<64;i++) block[zigzag8[i]] = raw[i] * qm[zigzag8[i]];
                    break;
                case 6: /* all * qt */
                    block[0] = dc_y * qm[0];
                    for (int i=1;i<64;i++) block[zigzag8[i]] = raw[i] * qm[zigzag8[i]];
                    break;
                case 7: /* all * qs */
                    block[0] = dc_y * qscale;
                    for (int i=1;i<64;i++) block[zigzag8[i]] = raw[i] * qscale;
                    break;
                }

                idct8x8(block, yspat[yb]);
            }

            /* Place Y */
            int offsets[4][2] = {{0,0},{8,0},{0,8},{8,8}};
            for (int yb = 0; yb < 4; yb++)
                for (int dy = 0; dy < 8; dy++)
                    for (int dx = 0; dx < 8; dx++) {
                        int px=mbx*16+offsets[yb][0]+dx;
                        int py=mby*16+offsets[yb][1]+dy;
                        if(px<imgW&&py<imgH)
                            planeY[py*imgW+px] = (int)round(yspat[yb][dy*8+dx]);
                    }

            /* Cb */
            {
                int block[64]={0}; double spatial[64];
                int raw[64];
                for(int i=0;i<64&&!br_eof(&br);i++) raw[i]=vlc_coeff(&br);
                dc_cb += raw[0];
                switch(dq_mode) {
                case 0: block[0]=dc_cb; for(int i=1;i<64;i++)block[zigzag8[i]]=raw[i]; break;
                case 1: case 5: case 6:
                    block[0]=dc_cb*qm[0]; for(int i=1;i<64;i++)block[zigzag8[i]]=raw[i]*qm[zigzag8[i]]; break;
                case 2: block[0]=dc_cb*qm[0]*qscale/8; for(int i=1;i<64;i++)block[zigzag8[i]]=raw[i]*qm[zigzag8[i]]*qscale/8; break;
                case 3: block[0]=dc_cb*qm[0]*qscale/16; for(int i=1;i<64;i++)block[zigzag8[i]]=raw[i]*qm[zigzag8[i]]*qscale/16; break;
                case 4: block[0]=dc_cb*8; for(int i=1;i<64;i++)block[zigzag8[i]]=raw[i]*qm[zigzag8[i]]*qscale/8; break;
                case 7: block[0]=dc_cb*qscale; for(int i=1;i<64;i++)block[zigzag8[i]]=raw[i]*qscale; break;
                }
                idct8x8(block, spatial);
                for(int dy=0;dy<8;dy++) for(int dx=0;dx<8;dx++){
                    int px=mbx*8+dx,py=mby*8+dy;
                    if(px<imgW/2&&py<imgH/2)
                        planeCb[py*(imgW/2)+px]=(int)round(spatial[dy*8+dx]);
                }
            }

            /* Cr */
            {
                int block[64]={0}; double spatial[64];
                int raw[64];
                for(int i=0;i<64&&!br_eof(&br);i++) raw[i]=vlc_coeff(&br);
                dc_cr += raw[0];
                switch(dq_mode) {
                case 0: block[0]=dc_cr; for(int i=1;i<64;i++)block[zigzag8[i]]=raw[i]; break;
                case 1: case 5: case 6:
                    block[0]=dc_cr*qm[0]; for(int i=1;i<64;i++)block[zigzag8[i]]=raw[i]*qm[zigzag8[i]]; break;
                case 2: block[0]=dc_cr*qm[0]*qscale/8; for(int i=1;i<64;i++)block[zigzag8[i]]=raw[i]*qm[zigzag8[i]]*qscale/8; break;
                case 3: block[0]=dc_cr*qm[0]*qscale/16; for(int i=1;i<64;i++)block[zigzag8[i]]=raw[i]*qm[zigzag8[i]]*qscale/16; break;
                case 4: block[0]=dc_cr*8; for(int i=1;i<64;i++)block[zigzag8[i]]=raw[i]*qm[zigzag8[i]]*qscale/8; break;
                case 7: block[0]=dc_cr*qscale; for(int i=1;i<64;i++)block[zigzag8[i]]=raw[i]*qscale; break;
                }
                idct8x8(block, spatial);
                for(int dy=0;dy<8;dy++) for(int dx=0;dx<8;dx++){
                    int px=mbx*8+dx,py=mby*8+dy;
                    if(px<imgW/2&&py<imgH/2)
                        planeCr[py*(imgW/2)+px]=(int)round(spatial[dy*8+dx]);
                }
            }
        }
    }

    /* Output Y plane */
    uint8_t *imgout = calloc(imgW * imgH, 1);
    for (int i = 0; i < imgW * imgH; i++)
        imgout[i] = clamp8(planeY[i] + 128);

    char path[256];
    snprintf(path, sizeof(path), OUT_DIR "fin_y_dq%d_dr%d_%s.pgm", dq_mode, dc_reset, tag);
    write_pgm(path, imgout, imgW, imgH);

    /* Also output YCbCr → RGB composite */
    uint8_t *rgb = calloc(imgW * imgH * 3, 1);
    for (int y = 0; y < imgH; y++) {
        for (int x = 0; x < imgW; x++) {
            int yv = planeY[y*imgW+x] + 128;
            int cb = planeCb[(y/2)*(imgW/2)+(x/2)];
            int cr = planeCr[(y/2)*(imgW/2)+(x/2)];
            int r = clamp8(yv + (int)(1.402 * cr));
            int g = clamp8(yv - (int)(0.344 * cb) - (int)(0.714 * cr));
            int b = clamp8(yv + (int)(1.772 * cb));
            rgb[(y*imgW+x)*3+0] = r;
            rgb[(y*imgW+x)*3+1] = g;
            rgb[(y*imgW+x)*3+2] = b;
        }
    }
    snprintf(path, sizeof(path), OUT_DIR "fin_rgb_dq%d_dr%d_%s.ppm", dq_mode, dc_reset, tag);
    write_ppm(path, rgb, imgW, imgH);

    printf("dq%d dr%d %s: %d/%d bits\n", dq_mode, dc_reset, tag, br.total, bslen*8);

    free(planeY); free(planeCb); free(planeCr); free(imgout); free(rgb);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <zip> [lba] [game]\n", argv[0]); return 1; }
    int slba = argc > 2 ? atoi(argv[2]) : 502;
    const char *game = argc > 3 ? argv[3] : "mari";

    int err; zip_t *z = zip_open(argv[1], ZIP_RDONLY, &err); if (!z) return 1;
    int bi=-1; zip_uint64_t bs2=0;
    for (int i=0; i<(int)zip_get_num_entries(z,0); i++) {
        zip_stat_t st; if(zip_stat_index(z,i,0,&st)==0 && st.size>bs2){bs2=st.size;bi=i;}}
    zip_stat_t st; zip_stat_index(z,bi,0,&st);
    zip_file_t *zf = zip_fopen_index(z,bi,0);
    uint8_t *disc = malloc(st.size);
    zip_int64_t rd=0;
    while(rd<(zip_int64_t)st.size){zip_int64_t r=zip_fread(zf,disc+rd,st.size-rd);if(r<=0)break;rd+=r;}
    zip_fclose(zf);
    int tsec = (int)(st.size/SECTOR_RAW);

    static uint8_t frames[16][MAX_FRAME]; int fsizes[16];
    int nf = assemble_frames(disc,tsec,slba,frames,fsizes,16);
    printf("Assembled %d frames\n", nf);

    /* Try multiple frames */
    for (int fi = 0; fi < 4 && fi < nf; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        int qscale = f[3];
        uint8_t qt[16]; memcpy(qt, f+4, 16);
        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        printf("\n=== Frame %d: qscale=%d, type=%d ===\n", fi, qscale, f[39]);
        char tag[64];
        snprintf(tag, sizeof(tag), "%s_f%d", game, fi);

        /* Try all dequant modes with DC prediction continuous */
        for (int dq = 0; dq <= 7; dq++)
            decode_frame(bs, bslen, qscale, qt, tag, 128, 144, dq, 0);

        /* Try best modes with row-reset DC */
        decode_frame(bs, bslen, qscale, qt, tag, 128, 144, 0, 1);
        decode_frame(bs, bslen, qscale, qt, tag, 128, 144, 1, 1);
        decode_frame(bs, bslen, qscale, qt, tag, 128, 144, 2, 1);
        decode_frame(bs, bslen, qscale, qt, tag, 128, 144, 6, 1);

        /* Per-MB DC reset */
        decode_frame(bs, bslen, qscale, qt, tag, 128, 144, 0, 2);
        decode_frame(bs, bslen, qscale, qt, tag, 128, 144, 6, 2);
    }

    free(disc); zip_close(z);
    return 0;
}
