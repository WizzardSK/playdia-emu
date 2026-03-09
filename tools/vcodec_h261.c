/*
 * vcodec_h261.c - Test H.261-like and alternative block structures
 *
 * Tests:
 * 1. No zigzag (row-major scan) with per-AC bit flags
 * 2. Column-major scan with per-AC bit flags
 * 3. EOB where value=0 means end-of-block (for AC only)
 * 4. Run-length before each non-zero AC: VLC for run, then VLC for level
 * 5. Fixed-length runs (e.g. 4-bit or 6-bit run count)
 * 6. JPEG-style: VLC gives (run,size) combined, then value bits
 * 7. Test with 4x4 blocks instead of 8x8
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

/* Place decoded block into plane */
static void place_block8(int *plane, int pw, int bx, int by, double spatial[64]) {
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            plane[(by*8+y)*pw + bx*8+x] = (int)round(spatial[y*8+x]);
}

/* YCbCr -> RGB conversion */
static void yuv_to_rgb(int *Y, int *Cb, int *Cr, uint8_t *rgb, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int yv = Y[y*w+x];
            int cb = Cb[(y/2)*(w/2)+(x/2)] - 128;
            int cr = Cr[(y/2)*(w/2)+(x/2)] - 128;
            int r = yv + 1.402*cr;
            int g = yv - 0.344*cb - 0.714*cr;
            int b = yv + 1.772*cb;
            rgb[(y*w+x)*3+0] = clamp8(r);
            rgb[(y*w+x)*3+1] = clamp8(g);
            rgb[(y*w+x)*3+2] = clamp8(b);
        }
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    int slba = argc > 2 ? atoi(argv[2]) : 502;
    const char *game = argc > 3 ? argv[3] : "mari";

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

    static uint8_t frames[16][MAX_FRAME]; int fsizes[16];
    int nf = assemble_frames(disc,tsec,slba,frames,fsizes,16);

    int mbw = 8, mbh = 9, imgW = 128, imgH = 144;

    for (int fi = 0; fi < 2 && fi < nf; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        int qscale = f[3];
        uint8_t qt[16]; memcpy(qt, f+4, 16);
        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        printf("\n=== Frame %d: qscale=%d, type=%d, bslen=%d ===\n", fi, qscale, f[39], bslen);

        int qm[64];
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 8; j++)
                qm[i*8+j] = qt[(i/2)*4 + (j/2)];

        /* ===== TEST 1: Per-AC bit flag, NO zigzag (row-major) ===== */
        {
            printf("\n--- Test 1: Per-AC bit flag, ROW-MAJOR (no zigzag) ---\n");
            BR br; br_init(&br, bs, bslen);
            int *planeY = calloc(imgW*imgH, sizeof(int));
            int *planeCb = calloc(imgW/2*imgH/2, sizeof(int));
            int *planeCr = calloc(imgW/2*imgH/2, sizeof(int));
            int dc_y=0, dc_cb=0, dc_cr=0;

            for (int mby=0; mby<mbh && !br_eof(&br); mby++) {
                for (int mbx=0; mbx<mbw && !br_eof(&br); mbx++) {
                    double yspat[4][64];
                    for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                        int block[64]={0};
                        dc_y += vlc_coeff(&br);
                        block[0] = dc_y * 8;
                        for (int i=1; i<64 && !br_eof(&br); i++) {
                            if (br_get1(&br))
                                block[i] = vlc_coeff(&br) * qm[i]; /* NO zigzag */
                        }
                        idct8x8(block, yspat[yb]);
                    }
                    int bx0=mbx*2, by0=mby*2;
                    place_block8(planeY, imgW, bx0,   by0,   yspat[0]);
                    place_block8(planeY, imgW, bx0+1, by0,   yspat[1]);
                    place_block8(planeY, imgW, bx0,   by0+1, yspat[2]);
                    place_block8(planeY, imgW, bx0+1, by0+1, yspat[3]);

                    double cbspat[64], crspat[64];
                    int cblock[64]={0};
                    dc_cb += vlc_coeff(&br); cblock[0] = dc_cb*8;
                    for (int i=1; i<64 && !br_eof(&br); i++)
                        if (br_get1(&br)) cblock[i] = vlc_coeff(&br)*qm[i];
                    idct8x8(cblock, cbspat);
                    place_block8(planeCb, imgW/2, mbx, mby, cbspat);

                    memset(cblock,0,sizeof(cblock));
                    dc_cr += vlc_coeff(&br); cblock[0] = dc_cr*8;
                    for (int i=1; i<64 && !br_eof(&br); i++)
                        if (br_get1(&br)) cblock[i] = vlc_coeff(&br)*qm[i];
                    idct8x8(cblock, crspat);
                    place_block8(planeCr, imgW/2, mbx, mby, crspat);
                }
            }
            printf("  Bits used: %d/%d (%.1f%%)\n", br.total, bslen*8, 100.0*br.total/(bslen*8));

            uint8_t *yimg = malloc(imgW*imgH);
            for(int i=0;i<imgW*imgH;i++) yimg[i]=clamp8(planeY[i]+128);
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "h261_rowmaj_%s_f%d.pgm",game,fi);
            write_pgm(path, yimg, imgW, imgH);
            free(yimg); free(planeY); free(planeCb); free(planeCr);
        }

        /* ===== TEST 2: DC always coded + EOB (0=end of block) in AC ===== */
        /* Re-test EOB but with a twist: 0 only means EOB if it's the FIRST code in a run */
        /* Actually test: each block = DC VLC + AC VLCs until EOB (value 0) */
        {
            printf("\n--- Test 2: DC + AC VLCs until EOB(0), with dequant ---\n");
            BR br; br_init(&br, bs, bslen);
            int *planeY = calloc(imgW*imgH, sizeof(int));
            int *planeCb = calloc(imgW/2*imgH/2, sizeof(int));
            int *planeCr = calloc(imgW/2*imgH/2, sizeof(int));
            int dc_y=0, dc_cb=0, dc_cr=0;
            int total_ac = 0;

            for (int mby=0; mby<mbh && !br_eof(&br); mby++) {
                for (int mbx=0; mbx<mbw && !br_eof(&br); mbx++) {
                    double yspat[4][64];
                    for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                        int block[64]={0};
                        dc_y += vlc_coeff(&br);
                        block[0] = dc_y * 8;
                        int ac_pos = 1;
                        while (ac_pos < 64 && !br_eof(&br)) {
                            int v = vlc_coeff(&br);
                            if (v == 0) break; /* EOB */
                            block[zigzag8[ac_pos]] = v * qm[zigzag8[ac_pos]];
                            ac_pos++;
                            total_ac++;
                        }
                        idct8x8(block, yspat[yb]);
                    }
                    int bx0=mbx*2, by0=mby*2;
                    place_block8(planeY, imgW, bx0,   by0,   yspat[0]);
                    place_block8(planeY, imgW, bx0+1, by0,   yspat[1]);
                    place_block8(planeY, imgW, bx0,   by0+1, yspat[2]);
                    place_block8(planeY, imgW, bx0+1, by0+1, yspat[3]);

                    double cbspat[64], crspat[64];
                    int cblock[64]={0};
                    dc_cb += vlc_coeff(&br); cblock[0] = dc_cb*8;
                    int ap=1;
                    while(ap<64 && !br_eof(&br)){int v=vlc_coeff(&br);if(v==0)break;cblock[zigzag8[ap]]=v*qm[zigzag8[ap]];ap++;total_ac++;}
                    idct8x8(cblock, cbspat);
                    place_block8(planeCb, imgW/2, mbx, mby, cbspat);

                    memset(cblock,0,sizeof(cblock));
                    dc_cr += vlc_coeff(&br); cblock[0] = dc_cr*8;
                    ap=1;
                    while(ap<64 && !br_eof(&br)){int v=vlc_coeff(&br);if(v==0)break;cblock[zigzag8[ap]]=v*qm[zigzag8[ap]];ap++;total_ac++;}
                    idct8x8(cblock, crspat);
                    place_block8(planeCr, imgW/2, mbx, mby, crspat);
                }
            }
            printf("  Bits used: %d/%d (%.1f%%), total AC coded: %d\n",
                   br.total, bslen*8, 100.0*br.total/(bslen*8), total_ac);

            uint8_t *yimg=malloc(imgW*imgH), *rgb=malloc(imgW*imgH*3);
            for(int i=0;i<imgW*imgH;i++) yimg[i]=clamp8(planeY[i]+128);
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "h261_eob_%s_f%d.pgm",game,fi);
            write_pgm(path, yimg, imgW, imgH);
            for(int i=0;i<imgW/2*imgH/2;i++){planeCb[i]+=128;planeCr[i]+=128;}
            yuv_to_rgb(planeY, planeCb, planeCr, rgb, imgW, imgH);
            for(int i=0;i<imgW*imgH;i++){rgb[i*3]+=128;rgb[i*3+1]+=128;rgb[i*3+2]+=128;}
            snprintf(path,sizeof(path),OUT_DIR "h261_eob_rgb_%s_f%d.ppm",game,fi);
            write_ppm(path, rgb, imgW, imgH);
            free(yimg); free(rgb); free(planeY); free(planeCb); free(planeCr);
        }

        /* ===== TEST 3: VLC gives run (skip count), then VLC gives level ===== */
        /* Like H.261 TCOEFF but using our VLC for both run and level */
        {
            printf("\n--- Test 3: VLC(run) + VLC(level) pairs until run=0,level=0 ---\n");
            BR br; br_init(&br, bs, bslen);
            int *planeY = calloc(imgW*imgH, sizeof(int));
            int dc_y=0;
            int blocks_done = 0;

            for (int mby=0; mby<mbh && !br_eof(&br); mby++) {
                for (int mbx=0; mbx<mbw && !br_eof(&br); mbx++) {
                    double yspat[4][64];
                    for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                        int block[64]={0};
                        dc_y += vlc_coeff(&br);
                        block[0] = dc_y * 8;
                        int ac_pos = 1;
                        while (ac_pos < 64 && !br_eof(&br)) {
                            int run = vlc_coeff(&br);
                            if (run < 0) run = -run; /* run is unsigned */
                            int level = vlc_coeff(&br);
                            if (run == 0 && level == 0) break; /* EOB */
                            ac_pos += run;
                            if (ac_pos < 64)
                                block[zigzag8[ac_pos]] = level * qm[zigzag8[ac_pos]];
                            ac_pos++;
                        }
                        idct8x8(block, yspat[yb]);
                        blocks_done++;
                    }
                    int bx0=mbx*2, by0=mby*2;
                    place_block8(planeY, imgW, bx0,   by0,   yspat[0]);
                    place_block8(planeY, imgW, bx0+1, by0,   yspat[1]);
                    place_block8(planeY, imgW, bx0,   by0+1, yspat[2]);
                    place_block8(planeY, imgW, bx0+1, by0+1, yspat[3]);
                    /* Skip Cb/Cr for speed */
                    for (int c=0; c<2 && !br_eof(&br); c++) {
                        vlc_coeff(&br); /* DC */
                        int ap=1;
                        while(ap<64 && !br_eof(&br)){
                            int r=vlc_coeff(&br);if(r<0)r=-r;
                            int l=vlc_coeff(&br);
                            if(r==0&&l==0)break;
                            ap+=r; ap++;
                        }
                    }
                }
            }
            printf("  Bits used: %d/%d (%.1f%%), Y blocks: %d\n",
                   br.total, bslen*8, 100.0*br.total/(bslen*8), blocks_done);

            uint8_t *yimg=malloc(imgW*imgH);
            for(int i=0;i<imgW*imgH;i++) yimg[i]=clamp8(planeY[i]+128);
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "h261_rl_%s_f%d.pgm",game,fi);
            write_pgm(path, yimg, imgW, imgH);
            free(yimg); free(planeY);
        }

        /* ===== TEST 4: Raw VLC values as DPCM stream (no DCT) ===== */
        {
            printf("\n--- Test 4: Raw DPCM at various widths (no DCT, no blocks) ---\n");
            BR br; br_init(&br, bs, bslen);
            int values[25000];
            int nv = 0;
            while (nv < 25000 && !br_eof(&br)) {
                int old = br.total;
                int v = vlc_coeff(&br);
                if (br.total == old) break;
                values[nv++] = v;
            }
            printf("  Total VLC values: %d\n", nv);

            int widths[] = {128, 144, 160, 176, 256, 320, 352};
            for (int wi = 0; wi < 7; wi++) {
                int w = widths[wi];
                int h = nv / w;
                if (h < 20) continue;

                uint8_t *img = malloc(w*h);
                /* DPCM with row-reset, scaled by qscale */
                int acc;
                for (int y = 0; y < h; y++) {
                    acc = 128;
                    for (int x = 0; x < w; x++) {
                        acc += values[y*w+x] * qscale;
                        img[y*w+x] = clamp8(acc);
                    }
                }
                char path[256];
                snprintf(path,sizeof(path),OUT_DIR "dpcm_qs_%s_f%d_w%d.pgm",game,fi,w);
                write_pgm(path, img, w, h);

                /* DPCM continuous, scaled */
                acc = 128;
                for (int i = 0; i < w*h; i++) {
                    acc += values[i] * qscale;
                    img[i] = clamp8(acc);
                }
                snprintf(path,sizeof(path),OUT_DIR "dpcmc_qs_%s_f%d_w%d.pgm",game,fi,w);
                write_pgm(path, img, w, h);

                free(img);
            }
        }

        /* ===== TEST 5: 4x4 blocks with per-AC bit flag ===== */
        {
            printf("\n--- Test 5: 4x4 blocks (16 coeff), per-AC bit flag ---\n");
            BR br; br_init(&br, bs, bslen);
            /* 128x144 with 4x4 blocks = 32x36 = 1152 Y blocks */
            int bw4 = 32, bh4 = 36;
            int *planeY = calloc(imgW*imgH, sizeof(int));
            int dc = 0;
            int blocks = 0;

            for (int by = 0; by < bh4 && !br_eof(&br); by++) {
                for (int bx = 0; bx < bw4 && !br_eof(&br); bx++) {
                    int block[16] = {0};
                    dc += vlc_coeff(&br);
                    block[0] = dc * 4;
                    for (int i = 1; i < 16 && !br_eof(&br); i++) {
                        if (br_get1(&br))
                            block[i] = vlc_coeff(&br) * 16;
                    }
                    /* Simple 4x4 "IDCT" - just use values directly for now */
                    /* 4x4 zigzag (row-major for simplicity) */
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++)
                            planeY[(by*4+y)*imgW + bx*4+x] = block[y*4+x];
                    blocks++;
                }
            }
            printf("  Bits used: %d/%d (%.1f%%), blocks: %d\n",
                   br.total, bslen*8, 100.0*br.total/(bslen*8), blocks);

            uint8_t *yimg = malloc(imgW*imgH);
            for(int i=0;i<imgW*imgH;i++) yimg[i]=clamp8(planeY[i]+128);
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "h261_4x4_%s_f%d.pgm",game,fi);
            write_pgm(path, yimg, imgW, imgH);
            free(yimg); free(planeY);
        }

        /* ===== TEST 6: Per-AC bit flag with DIFFERENT dequant: coeff * qt_entry * qscale ===== */
        /* But also try: NO IDCT — just display the frequency-domain block */
        {
            printf("\n--- Test 6: Per-AC bit flag, display blocks WITHOUT IDCT ---\n");
            BR br; br_init(&br, bs, bslen);
            int *planeY = calloc(imgW*imgH, sizeof(int));
            int dc_y = 0;

            for (int mby=0; mby<mbh && !br_eof(&br); mby++) {
                for (int mbx=0; mbx<mbw && !br_eof(&br); mbx++) {
                    for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                        int block[64]={0};
                        dc_y += vlc_coeff(&br);
                        block[0] = dc_y;
                        for (int i=1; i<64 && !br_eof(&br); i++) {
                            if (br_get1(&br))
                                block[zigzag8[i]] = vlc_coeff(&br);
                        }
                        /* Display raw coefficients in spatial positions (NO IDCT) */
                        int bx = (mbx*2 + (yb&1));
                        int by = (mby*2 + (yb>>1));
                        for (int y=0;y<8;y++)
                            for(int x=0;x<8;x++)
                                planeY[(by*8+y)*imgW+bx*8+x] = block[y*8+x];
                    }
                    /* Skip chroma */
                    for (int c=0;c<2 && !br_eof(&br);c++) {
                        vlc_coeff(&br);
                        for(int i=1;i<64 && !br_eof(&br);i++)
                            if(br_get1(&br)) vlc_coeff(&br);
                    }
                }
            }
            printf("  Bits used: %d/%d (%.1f%%)\n", br.total, bslen*8, 100.0*br.total/(bslen*8));

            /* DC component should be visible - scale to show */
            uint8_t *yimg = malloc(imgW*imgH);
            /* Show DC (top-left of each block) and AC pattern */
            for(int i=0;i<imgW*imgH;i++) yimg[i]=clamp8(planeY[i]*2+128);
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "h261_noidct_%s_f%d.pgm",game,fi);
            write_pgm(path, yimg, imgW, imgH);
            free(yimg); free(planeY);
        }

        /* ===== TEST 7: Plane-sequential (all Y, then all Cb, then all Cr) ===== */
        /* with per-AC bit flag */
        {
            printf("\n--- Test 7: Plane-sequential, per-AC bit flag ---\n");
            BR br; br_init(&br, bs, bslen);
            int *planeY = calloc(imgW*imgH, sizeof(int));
            int *planeCb = calloc(imgW/2*imgH/2, sizeof(int));
            int *planeCr = calloc(imgW/2*imgH/2, sizeof(int));
            int dc = 0;

            /* All Y blocks first: 16x18 = 288 blocks in raster order */
            int ybw = 16, ybh = 18;
            for (int by=0; by<ybh && !br_eof(&br); by++) {
                for (int bx=0; bx<ybw && !br_eof(&br); bx++) {
                    int block[64]={0};
                    dc += vlc_coeff(&br);
                    block[0] = dc * 8;
                    for (int i=1; i<64 && !br_eof(&br); i++)
                        if (br_get1(&br)) block[zigzag8[i]] = vlc_coeff(&br) * qm[zigzag8[i]];
                    double spat[64];
                    idct8x8(block, spat);
                    place_block8(planeY, imgW, bx, by, spat);
                }
            }
            int y_bits = br.total;
            printf("  Y: %d bits\n", y_bits);

            /* All Cb blocks: 8x9 = 72 blocks */
            dc = 0;
            for (int by=0; by<9 && !br_eof(&br); by++) {
                for (int bx=0; bx<8 && !br_eof(&br); bx++) {
                    int block[64]={0};
                    dc += vlc_coeff(&br);
                    block[0] = dc * 8;
                    for (int i=1; i<64 && !br_eof(&br); i++)
                        if (br_get1(&br)) block[zigzag8[i]] = vlc_coeff(&br) * qm[zigzag8[i]];
                    double spat[64];
                    idct8x8(block, spat);
                    place_block8(planeCb, imgW/2, bx, by, spat);
                }
            }
            int cb_bits = br.total - y_bits;
            printf("  Cb: %d bits (total %d)\n", cb_bits, br.total);

            /* All Cr blocks: 8x9 = 72 */
            dc = 0;
            for (int by=0; by<9 && !br_eof(&br); by++) {
                for (int bx=0; bx<8 && !br_eof(&br); bx++) {
                    int block[64]={0};
                    dc += vlc_coeff(&br);
                    block[0] = dc * 8;
                    for (int i=1; i<64 && !br_eof(&br); i++)
                        if (br_get1(&br)) block[zigzag8[i]] = vlc_coeff(&br) * qm[zigzag8[i]];
                    double spat[64];
                    idct8x8(block, spat);
                    place_block8(planeCr, imgW/2, bx, by, spat);
                }
            }
            printf("  Cr: %d bits (total %d/%d = %.1f%%)\n",
                   br.total-y_bits-cb_bits, br.total, bslen*8, 100.0*br.total/(bslen*8));

            uint8_t *yimg=malloc(imgW*imgH), *rgb=malloc(imgW*imgH*3);
            for(int i=0;i<imgW*imgH;i++) yimg[i]=clamp8(planeY[i]+128);
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "h261_plseq_%s_f%d.pgm",game,fi);
            write_pgm(path, yimg, imgW, imgH);

            for(int i=0;i<imgW/2*imgH/2;i++){planeCb[i]+=128;planeCr[i]+=128;}
            yuv_to_rgb(planeY, planeCb, planeCr, rgb, imgW, imgH);
            for(int i=0;i<imgW*imgH;i++){rgb[i*3]+=128;rgb[i*3+1]+=128;rgb[i*3+2]+=128;}
            snprintf(path,sizeof(path),OUT_DIR "h261_plseq_rgb_%s_f%d.ppm",game,fi);
            write_ppm(path, rgb, imgW, imgH);
            free(yimg); free(rgb); free(planeY); free(planeCb); free(planeCr);
        }
    }

    free(disc); zip_close(z);
    return 0;
}
