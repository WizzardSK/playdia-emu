/*
 * vcodec_4x4test.c - Test 4x4 DCT block hypothesis
 *
 * The 16-entry qtable maps 1:1 to 16 coefficients in a 4x4 block.
 * Resolution 128x144 → 32x36 Y blocks (4x4 each) = 1152 Y blocks
 * 4:2:0 → 16x18 Cb + 16x18 Cr = 288+288 = 576 chroma blocks
 * Total: 1728 blocks
 *
 * Test variants:
 * 1. Per-AC bit flag (15 flags per block)
 * 2. EOB (value=0 terminates block)
 * 3. Flat 16 values per block (no flags, no EOB)
 * 4. Different macroblock organizations
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

/* 4x4 zigzag scan order */
static const int zigzag4[16] = {
     0, 1, 4, 8,
     5, 2, 3, 6,
     9,12,13,10,
     7,11,14,15
};

/* Alternative: row-major (no zigzag) */
static const int rowmaj4[16] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
};

/* 4x4 IDCT */
static void idct4x4(int block[16], double out[16]) {
    double tmp[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double sum = 0;
            for (int k = 0; k < 4; k++) {
                double ck = (k == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += ck * block[i*4+k] * cos((2*j+1)*k*PI/8.0);
            }
            tmp[i*4+j] = sum * 0.5;
        }
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            double sum = 0;
            for (int k = 0; k < 4; k++) {
                double ck = (k == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += ck * tmp[k*4+j] * cos((2*i+1)*k*PI/8.0);
            }
            out[i*4+j] = sum * 0.5;
        }
}

/* H.264-style 4x4 integer transform (butterfly) */
static void hadamard4x4(int block[16], double out[16]) {
    int tmp[16];
    /* Row transform */
    for (int i = 0; i < 4; i++) {
        int *b = &block[i*4];
        int s03 = b[0]+b[3], d03 = b[0]-b[3];
        int s12 = b[1]+b[2], d12 = b[1]-b[2];
        tmp[i*4+0] = s03+s12;
        tmp[i*4+1] = 2*d03+d12;
        tmp[i*4+2] = s03-s12;
        tmp[i*4+3] = d03-2*d12;
    }
    /* Column transform */
    for (int j = 0; j < 4; j++) {
        int s03 = tmp[0*4+j]+tmp[3*4+j], d03 = tmp[0*4+j]-tmp[3*4+j];
        int s12 = tmp[1*4+j]+tmp[2*4+j], d12 = tmp[1*4+j]-tmp[2*4+j];
        out[0*4+j] = (s03+s12)/4.0;
        out[1*4+j] = (2*d03+d12)/4.0;
        out[2*4+j] = (s03-s12)/4.0;
        out[3*4+j] = (d03-2*d12)/4.0;
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

static void place_block4(int *plane, int pw, int bx, int by, double spatial[16]) {
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            plane[(by*4+y)*pw + bx*4+x] = (int)round(spatial[y*4+x]);
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    int slba = argc > 2 ? atoi(argv[2]) : 502;
    const char *tag = argc > 3 ? argv[3] : "m";

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
    if (nf < 1) return 1;

    int imgW = 128, imgH = 144;
    int ybw = 32, ybh = 36;  /* Y block grid for 4x4 */
    int cbw = 16, cbh = 18;  /* Chroma block grid */

    for (int fi = 0; fi < 2 && fi < nf; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        int qscale = f[3];
        uint8_t qt[16]; memcpy(qt, f+4, 16);
        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        printf("\n=== Frame %d: qscale=%d, type=%d ===\n", fi, qscale, f[39]);

        /* ===== TEST 1: 4x4 blocks, per-AC bit flag (15 flags), MB-interleaved =====
         * Macroblock = 8x8 pixels = 4 Y blocks (2x2 of 4x4) + 1 Cb + 1 Cr
         * 16x18 = 288 MBs */
        {
            printf("\n--- Test 1: 4x4 per-AC flag, MB-interleaved, zigzag ---\n");
            BR br; br_init(&br, bs, bslen);
            int *planeY = calloc(imgW*imgH, sizeof(int));
            int *planeCb = calloc(imgW/2*imgH/2, sizeof(int));
            int *planeCr = calloc(imgW/2*imgH/2, sizeof(int));
            int dc_y=0, dc_cb=0, dc_cr=0;

            for (int mby=0; mby<18 && !br_eof(&br); mby++) {
                for (int mbx=0; mbx<16 && !br_eof(&br); mbx++) {
                    double yspat[4][16];
                    for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                        int block[16]={0};
                        dc_y += vlc_coeff(&br);
                        block[0] = dc_y * qscale;
                        for (int i=1; i<16 && !br_eof(&br); i++) {
                            if (br_get1(&br))
                                block[zigzag4[i]] = vlc_coeff(&br) * qt[zigzag4[i]];
                        }
                        idct4x4(block, yspat[yb]);
                    }
                    int bx0 = mbx*2, by0 = mby*2;
                    place_block4(planeY, imgW, bx0,   by0,   yspat[0]);
                    place_block4(planeY, imgW, bx0+1, by0,   yspat[1]);
                    place_block4(planeY, imgW, bx0,   by0+1, yspat[2]);
                    place_block4(planeY, imgW, bx0+1, by0+1, yspat[3]);

                    /* Cb */
                    double cspat[16];
                    int cblock[16]={0};
                    dc_cb += vlc_coeff(&br); cblock[0]=dc_cb*qscale;
                    for(int i=1;i<16&&!br_eof(&br);i++)
                        if(br_get1(&br)) cblock[zigzag4[i]]=vlc_coeff(&br)*qt[zigzag4[i]];
                    idct4x4(cblock, cspat);
                    place_block4(planeCb, imgW/2, mbx, mby, cspat);

                    /* Cr */
                    memset(cblock,0,sizeof(cblock));
                    dc_cr += vlc_coeff(&br); cblock[0]=dc_cr*qscale;
                    for(int i=1;i<16&&!br_eof(&br);i++)
                        if(br_get1(&br)) cblock[zigzag4[i]]=vlc_coeff(&br)*qt[zigzag4[i]];
                    idct4x4(cblock, cspat);
                    place_block4(planeCr, imgW/2, mbx, mby, cspat);
                }
            }
            int used = br.total;
            printf("  Bits: %d/%d (%.1f%%)\n", used, bslen*8, 100.0*used/(bslen*8));

            uint8_t *yimg = malloc(imgW*imgH);
            for(int i=0;i<imgW*imgH;i++) yimg[i]=clamp8(planeY[i]+128);
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "4x4_t1_%s_f%d.pgm",tag,fi);
            write_pgm(path, yimg, imgW, imgH);

            /* RGB output */
            uint8_t *rgb = malloc(imgW*imgH*3);
            for(int i=0;i<imgW/2*imgH/2;i++){planeCb[i]+=128;planeCr[i]+=128;}
            for(int y=0;y<imgH;y++)
                for(int x=0;x<imgW;x++){
                    int yv=planeY[y*imgW+x]+128;
                    int cb=planeCb[(y/2)*(imgW/2)+(x/2)]-128;
                    int cr=planeCr[(y/2)*(imgW/2)+(x/2)]-128;
                    rgb[(y*imgW+x)*3+0]=clamp8(yv+1.402*cr);
                    rgb[(y*imgW+x)*3+1]=clamp8(yv-0.344*cb-0.714*cr);
                    rgb[(y*imgW+x)*3+2]=clamp8(yv+1.772*cb);
                }
            snprintf(path,sizeof(path),OUT_DIR "4x4_t1_rgb_%s_f%d.ppm",tag,fi);
            write_ppm(path, rgb, imgW, imgH);
            free(yimg); free(rgb); free(planeY); free(planeCb); free(planeCr);
        }

        /* ===== TEST 2: 4x4 blocks, per-AC flag, plane-sequential ===== */
        {
            printf("\n--- Test 2: 4x4 per-AC flag, plane-sequential ---\n");
            BR br; br_init(&br, bs, bslen);
            int *planeY = calloc(imgW*imgH, sizeof(int));
            int dc=0;

            for (int by=0; by<ybh && !br_eof(&br); by++) {
                for (int bx=0; bx<ybw && !br_eof(&br); bx++) {
                    int block[16]={0};
                    dc += vlc_coeff(&br);
                    block[0] = dc * qscale;
                    for(int i=1;i<16&&!br_eof(&br);i++)
                        if(br_get1(&br)) block[zigzag4[i]]=vlc_coeff(&br)*qt[zigzag4[i]];
                    double spat[16];
                    idct4x4(block, spat);
                    place_block4(planeY, imgW, bx, by, spat);
                }
            }
            int ybits = br.total;
            printf("  Y: %d bits (%.1f%%)\n", ybits, 100.0*ybits/(bslen*8));

            uint8_t *yimg = malloc(imgW*imgH);
            for(int i=0;i<imgW*imgH;i++) yimg[i]=clamp8(planeY[i]+128);
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "4x4_t2_%s_f%d.pgm",tag,fi);
            write_pgm(path, yimg, imgW, imgH);
            free(yimg); free(planeY);
        }

        /* ===== TEST 3: 4x4 blocks, EOB, MB-interleaved ===== */
        {
            printf("\n--- Test 3: 4x4 EOB (val=0 ends block), MB-interleaved ---\n");
            BR br; br_init(&br, bs, bslen);
            int *planeY = calloc(imgW*imgH, sizeof(int));
            int dc_y=0;
            int total_ac=0, total_blocks=0;

            for (int mby=0; mby<18 && !br_eof(&br); mby++) {
                for (int mbx=0; mbx<16 && !br_eof(&br); mbx++) {
                    double yspat[4][16];
                    for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                        int block[16]={0};
                        dc_y += vlc_coeff(&br);
                        block[0] = dc_y * qscale;
                        int pos=1;
                        while(pos<16 && !br_eof(&br)){
                            int v=vlc_coeff(&br);
                            if(v==0) break;
                            block[zigzag4[pos]]=v*qt[zigzag4[pos]];
                            pos++; total_ac++;
                        }
                        idct4x4(block, yspat[yb]);
                        total_blocks++;
                    }
                    int bx0=mbx*2, by0=mby*2;
                    place_block4(planeY, imgW, bx0,   by0,   yspat[0]);
                    place_block4(planeY, imgW, bx0+1, by0,   yspat[1]);
                    place_block4(planeY, imgW, bx0,   by0+1, yspat[2]);
                    place_block4(planeY, imgW, bx0+1, by0+1, yspat[3]);
                    /* Skip chroma */
                    for(int c=0;c<2 && !br_eof(&br);c++){
                        vlc_coeff(&br);
                        int pos=1;
                        while(pos<16 && !br_eof(&br)){
                            int v=vlc_coeff(&br); if(v==0)break;
                            pos++; total_ac++;
                        }
                        total_blocks++;
                    }
                }
            }
            printf("  Bits: %d/%d (%.1f%%), blocks=%d, avg_ac=%.1f\n",
                   br.total, bslen*8, 100.0*br.total/(bslen*8),
                   total_blocks, (double)total_ac/total_blocks);

            uint8_t *yimg = malloc(imgW*imgH);
            for(int i=0;i<imgW*imgH;i++) yimg[i]=clamp8(planeY[i]+128);
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "4x4_t3_%s_f%d.pgm",tag,fi);
            write_pgm(path, yimg, imgW, imgH);
            free(yimg); free(planeY);
        }

        /* ===== TEST 4: 4x4, flat 16 values per block, different DC scalings ===== */
        {
            printf("\n--- Test 4: 4x4 flat (16 VLC per block), various dequant ---\n");
            BR br; br_init(&br, bs, bslen);
            int *planeY = calloc(imgW*imgH, sizeof(int));
            int dc_y=0;
            int blocks=0;

            for (int mby=0; mby<18 && !br_eof(&br); mby++) {
                for (int mbx=0; mbx<16 && !br_eof(&br); mbx++) {
                    double yspat[4][16];
                    for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                        int block[16]={0};
                        dc_y += vlc_coeff(&br);
                        block[0] = dc_y * qt[0];
                        for(int i=1;i<16 && !br_eof(&br);i++){
                            int v=vlc_coeff(&br);
                            block[zigzag4[i]] = v * qt[zigzag4[i]];
                        }
                        idct4x4(block, yspat[yb]);
                        blocks++;
                    }
                    int bx0=mbx*2, by0=mby*2;
                    place_block4(planeY, imgW, bx0,   by0,   yspat[0]);
                    place_block4(planeY, imgW, bx0+1, by0,   yspat[1]);
                    place_block4(planeY, imgW, bx0,   by0+1, yspat[2]);
                    place_block4(planeY, imgW, bx0+1, by0+1, yspat[3]);
                    /* Skip chroma */
                    for(int c=0;c<2 && !br_eof(&br);c++){
                        vlc_coeff(&br);
                        for(int i=1;i<16 && !br_eof(&br);i++) vlc_coeff(&br);
                    }
                }
            }
            printf("  Bits: %d/%d (%.1f%%), Y blocks=%d\n",
                   br.total, bslen*8, 100.0*br.total/(bslen*8), blocks);

            uint8_t *yimg = malloc(imgW*imgH);
            for(int i=0;i<imgW*imgH;i++) yimg[i]=clamp8(planeY[i]+128);
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "4x4_t4_%s_f%d.pgm",tag,fi);
            write_pgm(path, yimg, imgW, imgH);
            free(yimg); free(planeY);
        }

        /* ===== TEST 5: 4x4, per-AC flag, NO zigzag (row-major) ===== */
        {
            printf("\n--- Test 5: 4x4 per-AC flag, row-major, MB-interleaved ---\n");
            BR br; br_init(&br, bs, bslen);
            int *planeY = calloc(imgW*imgH, sizeof(int));
            int dc_y=0;

            for (int mby=0; mby<18 && !br_eof(&br); mby++) {
                for (int mbx=0; mbx<16 && !br_eof(&br); mbx++) {
                    double yspat[4][16];
                    for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                        int block[16]={0};
                        dc_y += vlc_coeff(&br);
                        block[0] = dc_y * qscale;
                        for(int i=1;i<16 && !br_eof(&br);i++)
                            if(br_get1(&br)) block[i]=vlc_coeff(&br)*qt[i]; /* row-major */
                        idct4x4(block, yspat[yb]);
                    }
                    int bx0=mbx*2, by0=mby*2;
                    place_block4(planeY, imgW, bx0,   by0,   yspat[0]);
                    place_block4(planeY, imgW, bx0+1, by0,   yspat[1]);
                    place_block4(planeY, imgW, bx0,   by0+1, yspat[2]);
                    place_block4(planeY, imgW, bx0+1, by0+1, yspat[3]);
                    for(int c=0;c<2 && !br_eof(&br);c++){
                        vlc_coeff(&br);
                        for(int i=1;i<16&&!br_eof(&br);i++)
                            if(br_get1(&br)) vlc_coeff(&br);
                    }
                }
            }
            printf("  Bits: %d/%d (%.1f%%)\n", br.total, bslen*8, 100.0*br.total/(bslen*8));

            uint8_t *yimg = malloc(imgW*imgH);
            for(int i=0;i<imgW*imgH;i++) yimg[i]=clamp8(planeY[i]+128);
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "4x4_t5_%s_f%d.pgm",tag,fi);
            write_pgm(path, yimg, imgW, imgH);
            free(yimg); free(planeY);
        }

        /* ===== TEST 6: 4x4, per-AC flag, DC scaling variants ===== */
        printf("\n--- Test 6: DC scaling variants ---\n");
        for (int dcs = 0; dcs < 4; dcs++) {
            BR br; br_init(&br, bs, bslen);
            int *planeY = calloc(imgW*imgH, sizeof(int));
            int dc_y=0;
            const char *dcs_name[] = {"dc*qs", "dc*qt0", "dc*8", "dc*qs*qt0/16"};

            for (int mby=0; mby<18 && !br_eof(&br); mby++) {
                for (int mbx=0; mbx<16 && !br_eof(&br); mbx++) {
                    double yspat[4][16];
                    for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                        int block[16]={0};
                        dc_y += vlc_coeff(&br);
                        switch(dcs){
                            case 0: block[0]=dc_y*qscale; break;
                            case 1: block[0]=dc_y*qt[0]; break;
                            case 2: block[0]=dc_y*8; break;
                            case 3: block[0]=dc_y*qscale*qt[0]/16; break;
                        }
                        for(int i=1;i<16&&!br_eof(&br);i++)
                            if(br_get1(&br)){
                                int v=vlc_coeff(&br);
                                block[zigzag4[i]]=v*qt[zigzag4[i]]*qscale/16;
                            }
                        idct4x4(block, yspat[yb]);
                    }
                    int bx0=mbx*2, by0=mby*2;
                    place_block4(planeY, imgW, bx0,   by0,   yspat[0]);
                    place_block4(planeY, imgW, bx0+1, by0,   yspat[1]);
                    place_block4(planeY, imgW, bx0,   by0+1, yspat[2]);
                    place_block4(planeY, imgW, bx0+1, by0+1, yspat[3]);
                    for(int c=0;c<2&&!br_eof(&br);c++){
                        vlc_coeff(&br);
                        for(int i=1;i<16&&!br_eof(&br);i++)
                            if(br_get1(&br)) vlc_coeff(&br);
                    }
                }
            }
            printf("  %s: bits %d/%d (%.1f%%)\n", dcs_name[dcs],
                   br.total, bslen*8, 100.0*br.total/(bslen*8));

            uint8_t *yimg = malloc(imgW*imgH);
            for(int i=0;i<imgW*imgH;i++) yimg[i]=clamp8(planeY[i]+128);
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "4x4_t6d%d_%s_f%d.pgm",dcs,tag,fi);
            write_pgm(path, yimg, imgW, imgH);
            free(yimg); free(planeY);
        }

        /* ===== TEST 7: 8x8 blocks for comparison with multiple dequant ===== */
        printf("\n--- Test 7: 8x8 per-AC flag comparison (DC*8, AC*qt*qs/8) ---\n");
        {
            BR br; br_init(&br, bs, bslen);
            int *planeY = calloc(imgW*imgH, sizeof(int));
            int dc_y=0;
            int qm[64];
            for(int i=0;i<8;i++) for(int j=0;j<8;j++) qm[i*8+j]=qt[(i/2)*4+(j/2)];

            static const int zz8[64]={
                0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
                12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
                35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
                58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

            for(int mby=0;mby<9&&!br_eof(&br);mby++){
                for(int mbx=0;mbx<8&&!br_eof(&br);mbx++){
                    double yspat[4][64];
                    for(int yb=0;yb<4&&!br_eof(&br);yb++){
                        int block[64]={0};
                        dc_y+=vlc_coeff(&br);
                        block[0]=dc_y*8;
                        for(int i=1;i<64&&!br_eof(&br);i++)
                            if(br_get1(&br))
                                block[zz8[i]]=vlc_coeff(&br)*qm[zz8[i]]*qscale/8;
                        /* 8x8 IDCT */
                        double tmp[64];
                        for(int r=0;r<8;r++)for(int c=0;c<8;c++){
                            double s=0;for(int k=0;k<8;k++){
                                double ck=(k==0)?1.0/sqrt(2.0):1.0;
                                s+=ck*block[r*8+k]*cos((2*c+1)*k*PI/16.0);}
                            tmp[r*8+c]=s*0.5;}
                        for(int c=0;c<8;c++)for(int r=0;r<8;r++){
                            double s=0;for(int k=0;k<8;k++){
                                double ck=(k==0)?1.0/sqrt(2.0):1.0;
                                s+=ck*tmp[k*8+c]*cos((2*r+1)*k*PI/16.0);}
                            yspat[yb][r*8+c]=s*0.5;}
                    }
                    int bx0=mbx*2,by0=mby*2;
                    for(int yb=0;yb<4;yb++){
                        int bx=bx0+(yb&1),by=by0+(yb>>1);
                        for(int y=0;y<8;y++)for(int x=0;x<8;x++)
                            planeY[(by*8+y)*imgW+bx*8+x]=(int)round(yspat[yb][y*8+x]);}
                    for(int c=0;c<2&&!br_eof(&br);c++){
                        vlc_coeff(&br);
                        for(int i=1;i<64&&!br_eof(&br);i++)
                            if(br_get1(&br)) vlc_coeff(&br);
                    }
                }
            }
            printf("  Bits: %d/%d (%.1f%%)\n", br.total, bslen*8, 100.0*br.total/(bslen*8));

            uint8_t *yimg = malloc(imgW*imgH);
            for(int i=0;i<imgW*imgH;i++) yimg[i]=clamp8(planeY[i]+128);
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "4x4_t7_8x8_%s_f%d.pgm",tag,fi);
            write_pgm(path, yimg, imgW, imgH);
            free(yimg); free(planeY);
        }
    }

    free(disc); zip_close(z);
    return 0;
}
