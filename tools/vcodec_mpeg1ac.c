/*
 * vcodec_mpeg1ac.c - Test MPEG-1 Table B.14 run-level VLC for AC coefficients
 *
 * Hypothesis: DC uses our known VLC (MPEG-1 DC luminance size table),
 * but AC uses the standard MPEG-1 run-level VLC (Table B.14/B.15).
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
static int br_peek(BR *b, int n) {
    BR save = *b;
    int v = br_get(&save, n);
    return v;
}

/* DC VLC - our confirmed MPEG-1 DC luminance table */
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

/* MPEG-1 Table B.14 - AC coefficient run-level VLC
 * Returns: run in high 16 bits, level in low 16 bits
 * Returns -1 for EOB, -2 for escape, -3 for error */
#define EOB_CODE (-1)
#define ESC_CODE (-2)
#define ERR_CODE (-3)

typedef struct { int run; int level; } RL;

/* Simplified MPEG-1 Table B.14 decoder */
static RL decode_ac_b14(BR *b) {
    RL rl = {0, 0};

    /* Check for EOB: '10' */
    if (br_peek(b, 2) == 2) { /* 10 */
        br_get(b, 2);
        rl.run = -1; /* EOB marker */
        return rl;
    }

    /* Check for Escape: '000001' */
    if (br_peek(b, 6) == 1) { /* 000001 */
        br_get(b, 6);
        rl.run = br_get(b, 6);   /* 6-bit run */
        int level = br_get(b, 8); /* 8-bit level (signed) */
        if (level == 0) {
            level = br_get(b, 8);
        } else if (level == 128) {
            level = br_get(b, 8) - 256;
        } else if (level > 128) {
            level = level - 256;
        }
        rl.level = level;
        return rl;
    }

    /* VLC table B.14 - partial implementation of common codes */
    /* Format: code bits → (run, level), then sign bit */
    int code;

    /* 1s → (0,1) */
    code = br_peek(b, 1);
    if (code == 1) {
        br_get(b, 1);
        int s = br_get1(b);
        rl.run = 0; rl.level = s ? -1 : 1;
        return rl;
    }

    /* 2-bit prefix 01 */
    code = br_peek(b, 3);
    if ((code >> 0) == 3) { /* 011 → (1,1) */
        br_get(b, 3);
        int s = br_get1(b);
        rl.run = 1; rl.level = s ? -1 : 1;
        return rl;
    }

    /* 3-bit codes starting with 01 */
    code = br_peek(b, 4);
    if (code == 4) { /* 0100 → (0,2) */
        br_get(b, 4);
        int s = br_get1(b);
        rl.run = 0; rl.level = s ? -2 : 2;
        return rl;
    }
    if (code == 5) { /* 0101 → (2,1) */
        br_get(b, 4);
        int s = br_get1(b);
        rl.run = 2; rl.level = s ? -1 : 1;
        return rl;
    }

    /* 5-bit codes */
    code = br_peek(b, 5);
    if (code == 5) { /* 00101 → (0,3) */
        br_get(b, 5);
        int s = br_get1(b);
        rl.run = 0; rl.level = s ? -3 : 3;
        return rl;
    }
    if (code == 6) { /* 00110 → (4,1) */
        br_get(b, 5);
        int s = br_get1(b);
        rl.run = 4; rl.level = s ? -1 : 1;
        return rl;
    }
    if (code == 7) { /* 00111 → (3,1) */
        br_get(b, 5);
        int s = br_get1(b);
        rl.run = 3; rl.level = s ? -1 : 1;
        return rl;
    }

    /* 6-bit codes */
    code = br_peek(b, 6);
    if (code == 4) { /* 000100 → (7,1) */
        br_get(b, 6);
        int s = br_get1(b);
        rl.run = 7; rl.level = s ? -1 : 1;
        return rl;
    }
    if (code == 5) { /* 000101 → (6,1) */
        br_get(b, 6);
        int s = br_get1(b);
        rl.run = 6; rl.level = s ? -1 : 1;
        return rl;
    }
    if (code == 6) { /* 000110 → (1,2) */
        br_get(b, 6);
        int s = br_get1(b);
        rl.run = 1; rl.level = s ? -2 : 2;
        return rl;
    }
    if (code == 7) { /* 000111 → (5,1) */
        br_get(b, 6);
        int s = br_get1(b);
        rl.run = 5; rl.level = s ? -1 : 1;
        return rl;
    }

    /* 7-bit codes */
    code = br_peek(b, 7);
    if (code == 4) { /* 0000100 → (0,4) */
        br_get(b, 7);
        int s = br_get1(b);
        rl.run = 0; rl.level = s ? -4 : 4;
        return rl;
    }
    if (code == 5) { /* 0000101 → (2,2) */
        br_get(b, 7);
        int s = br_get1(b);
        rl.run = 2; rl.level = s ? -2 : 2;
        return rl;
    }
    if (code == 6) { /* 0000110 → (8,1) - first coeff different */
        br_get(b, 7);
        int s = br_get1(b);
        rl.run = 8; rl.level = s ? -1 : 1;
        return rl;
    }
    if (code == 7) { /* 0000111 → (9,1) */
        br_get(b, 7);
        int s = br_get1(b);
        rl.run = 9; rl.level = s ? -1 : 1;
        return rl;
    }

    /* 8-bit codes */
    code = br_peek(b, 8);
    if (code == 4) { /* 00000100 → (0,5) */
        br_get(b, 8);
        int s = br_get1(b);
        rl.run = 0; rl.level = s ? -5 : 5;
        return rl;
    }
    if (code == 5) { /* 00000101 → (0,6) */
        br_get(b, 8);
        int s = br_get1(b);
        rl.run = 0; rl.level = s ? -6 : 6;
        return rl;
    }
    if (code == 6) { /* 00000110 → (1,3) */
        br_get(b, 8);
        int s = br_get1(b);
        rl.run = 1; rl.level = s ? -3 : 3;
        return rl;
    }
    if (code == 7) { /* 00000111 → (3,2) */
        br_get(b, 8);
        int s = br_get1(b);
        rl.run = 3; rl.level = s ? -2 : 2;
        return rl;
    }

    /* 9-bit codes */
    code = br_peek(b, 9);
    if (code == 4) { /* 000000100 → (10,1) */
        br_get(b, 9);
        int s = br_get1(b);
        rl.run = 10; rl.level = s ? -1 : 1;
        return rl;
    }
    if (code == 5) { /* 000000101 → (11,1) */
        br_get(b, 9);
        int s = br_get1(b);
        rl.run = 11; rl.level = s ? -1 : 1;
        return rl;
    }
    if (code == 6) { /* 000000110 → (12,1) */
        br_get(b, 9);
        int s = br_get1(b);
        rl.run = 12; rl.level = s ? -1 : 1;
        return rl;
    }
    if (code == 7) { /* 000000111 → (13,1) */
        br_get(b, 9);
        int s = br_get1(b);
        rl.run = 13; rl.level = s ? -1 : 1;
        return rl;
    }

    /* 10-bit codes */
    code = br_peek(b, 10);
    if (code == 4) { /* 0000000100 → (0,7) */
        br_get(b, 10);
        int s = br_get1(b);
        rl.run = 0; rl.level = s ? -7 : 7;
        return rl;
    }
    if (code == 5) { /* 0000000101 → (1,4) */
        br_get(b, 10);
        int s = br_get1(b);
        rl.run = 1; rl.level = s ? -4 : 4;
        return rl;
    }
    if (code == 6) { /* 0000000110 → (2,3) */
        br_get(b, 10);
        int s = br_get1(b);
        rl.run = 2; rl.level = s ? -3 : 3;
        return rl;
    }
    if (code == 7) { /* 0000000111 → (4,2) */
        br_get(b, 10);
        int s = br_get1(b);
        rl.run = 4; rl.level = s ? -2 : 2;
        return rl;
    }

    /* 11-bit codes */
    code = br_peek(b, 11);
    if (code == 4) { /* 00000000100 → (5,2) */
        br_get(b, 11);
        int s = br_get1(b);
        rl.run = 5; rl.level = s ? -2 : 2;
        return rl;
    }
    if (code == 5) { /* 00000000101 → (14,1) */
        br_get(b, 11);
        int s = br_get1(b);
        rl.run = 14; rl.level = s ? -1 : 1;
        return rl;
    }
    if (code == 6) { /* 00000000110 → (15,1) */
        br_get(b, 11);
        int s = br_get1(b);
        rl.run = 15; rl.level = s ? -1 : 1;
        return rl;
    }
    if (code == 7) { /* 00000000111 → (16,1) */
        br_get(b, 11);
        int s = br_get1(b);
        rl.run = 16; rl.level = s ? -1 : 1;
        return rl;
    }

    /* Extended codes - 12+ bits */
    /* For now, use escape for anything longer */
    /* Actually let me add more from the full table */

    /* 12-bit codes: 0000 0000 01xx */
    code = br_peek(b, 12);
    if ((code & 0xFFC) == 0x010) { /* 00000000 01xx → various */
        int sub = code & 3;
        br_get(b, 12);
        int s = br_get1(b);
        switch(sub) {
            case 0: rl.run=0; rl.level=s?-8:8; break;
            case 1: rl.run=0; rl.level=s?-9:9; break;
            case 2: rl.run=0; rl.level=s?-10:10; break;
            case 3: rl.run=0; rl.level=s?-11:11; break;
        }
        return rl;
    }

    /* 13-bit codes */
    code = br_peek(b, 13);
    if ((code & 0x1FF8) == 0x010) {
        int sub = code & 7;
        br_get(b, 13);
        int s = br_get1(b);
        switch(sub) {
            case 0: rl.run=0; rl.level=s?-12:12; break;
            case 1: rl.run=0; rl.level=s?-13:13; break;
            case 2: rl.run=0; rl.level=s?-14:14; break;
            case 3: rl.run=0; rl.level=s?-15:15; break;
            case 4: rl.run=1; rl.level=s?-5:5; break;
            case 5: rl.run=1; rl.level=s?-6:6; break;
            case 6: rl.run=1; rl.level=s?-7:7; break;
            case 7: rl.run=2; rl.level=s?-4:4; break;
        }
        return rl;
    }

    /* If we get here, consume one bit and return error */
    br_get1(b);
    rl.run = -99;
    rl.level = 0;
    return rl;
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

static void place_block8(int *plane, int pw, int bx, int by, double spatial[64]) {
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            plane[(by*8+y)*pw + bx*8+x] = (int)round(spatial[y*8+x]);
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

    int mbw=8, mbh=9, imgW=128, imgH=144;

    for (int fi = 0; fi < 4 && fi < nf; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        int qscale = f[3];
        uint8_t qt[16]; memcpy(qt, f+4, 16);
        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        printf("\n=== Frame %d: qscale=%d, type=%d ===\n", fi, qscale, f[39]);

        int qm[64];
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 8; j++)
                qm[i*8+j] = qt[(i/2)*4 + (j/2)];

        /* Test: MPEG-1 DC VLC for DC, MPEG-1 B.14 for AC */
        BR br; br_init(&br, bs, bslen);
        int *planeY = calloc(imgW*imgH, sizeof(int));
        int *planeCb = calloc(imgW/2*imgH/2, sizeof(int));
        int *planeCr = calloc(imgW/2*imgH/2, sizeof(int));
        int dc_y=0, dc_cb=0, dc_cr=0;
        int total_eob=0, total_esc=0, total_err=0, total_rl=0;
        int total_blocks=0;

        for (int mby=0; mby<mbh && !br_eof(&br); mby++) {
            for (int mbx=0; mbx<mbw && !br_eof(&br); mbx++) {
                double yspat[4][64];
                for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                    int block[64]={0};
                    dc_y += vlc_dc(&br);
                    block[0] = dc_y * 8;

                    int ac_pos = 1;
                    while (ac_pos < 64 && !br_eof(&br)) {
                        RL rl = decode_ac_b14(&br);
                        if (rl.run == -1) { total_eob++; break; } /* EOB */
                        if (rl.run == -99) { total_err++; break; } /* Error */
                        ac_pos += rl.run;
                        if (ac_pos >= 64) break;
                        block[zigzag8[ac_pos]] = rl.level * qm[zigzag8[ac_pos]] * qscale / 16;
                        ac_pos++;
                        total_rl++;
                    }
                    idct8x8(block, yspat[yb]);
                    total_blocks++;
                }
                int bx0=mbx*2, by0=mby*2;
                place_block8(planeY, imgW, bx0,   by0,   yspat[0]);
                place_block8(planeY, imgW, bx0+1, by0,   yspat[1]);
                place_block8(planeY, imgW, bx0,   by0+1, yspat[2]);
                place_block8(planeY, imgW, bx0+1, by0+1, yspat[3]);

                double cspat[64];
                int cblock[64]={0};
                dc_cb += vlc_dc(&br); cblock[0]=dc_cb*8;
                {int ap=1; while(ap<64 && !br_eof(&br)){
                    RL rl=decode_ac_b14(&br);
                    if(rl.run==-1){total_eob++;break;}
                    if(rl.run==-99){total_err++;break;}
                    ap+=rl.run; if(ap>=64)break;
                    cblock[zigzag8[ap]]=rl.level*qm[zigzag8[ap]]*qscale/16;
                    ap++;total_rl++;
                }}
                idct8x8(cblock,cspat);
                place_block8(planeCb, imgW/2, mbx, mby, cspat);

                memset(cblock,0,sizeof(cblock));
                dc_cr += vlc_dc(&br); cblock[0]=dc_cr*8;
                {int ap=1; while(ap<64 && !br_eof(&br)){
                    RL rl=decode_ac_b14(&br);
                    if(rl.run==-1){total_eob++;break;}
                    if(rl.run==-99){total_err++;break;}
                    ap+=rl.run; if(ap>=64)break;
                    cblock[zigzag8[ap]]=rl.level*qm[zigzag8[ap]]*qscale/16;
                    ap++;total_rl++;
                }}
                idct8x8(cblock,cspat);
                place_block8(planeCr, imgW/2, mbx, mby, cspat);
            }
        }

        printf("  Bits: %d/%d (%.1f%%)\n", br.total, bslen*8, 100.0*br.total/(bslen*8));
        printf("  Blocks: %d, EOBs: %d, R/L pairs: %d, Escapes: %d, Errors: %d\n",
               total_blocks, total_eob, total_rl, total_esc, total_err);

        uint8_t *yimg=malloc(imgW*imgH);
        for(int i=0;i<imgW*imgH;i++) yimg[i]=clamp8(planeY[i]+128);
        char path[256];
        snprintf(path,sizeof(path),OUT_DIR "mpeg1ac_%s_f%d.pgm",game,fi);
        write_pgm(path, yimg, imgW, imgH);

        /* RGB */
        uint8_t *rgb=malloc(imgW*imgH*3);
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
        snprintf(path,sizeof(path),OUT_DIR "mpeg1ac_rgb_%s_f%d.ppm",game,fi);
        write_ppm(path, rgb, imgW, imgH);

        free(yimg); free(rgb); free(planeY); free(planeCb); free(planeCr);
    }

    free(disc); zip_close(z);
    return 0;
}
