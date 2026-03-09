/*
 * DC-only decoder - just extract DC values to verify block ordering
 * and macroblock structure without IDCT/quantization complications.
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
#define OUT_DIR "/home/wizzard/share/GitHub/playdia-emu/tools/test_output/"

static void write_pgm(const char *p, const uint8_t *g, int w, int h) {
    FILE *f=fopen(p,"wb"); if(!f)return;
    fprintf(f,"P5\n%d %d\n255\n",w,h); fwrite(g,1,w*h,f); fclose(f);
    printf("  -> %s (%dx%d)\n",p,w,h);
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

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

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

    for (int fi = 0; fi < 4 && fi < nf; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        int qscale = f[3];
        uint8_t qt[16]; memcpy(qt, f+4, 16);
        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        printf("\n=== Frame %d: qscale=%d, type=%d, bslen=%d ===\n", fi, qscale, f[39], bslen);
        printf("Header: ");
        for (int i = 0; i < 40; i++) printf("%02X ", f[i]);
        printf("\n");

        BR br; br_init(&br, bs, bslen);
        int mbw = 8, mbh = 9;

        /* First pass: decode all DC values with continuous prediction */
        int dc_y = 0, dc_cb = 0, dc_cr = 0;
        int dcY[288], dcCb[72], dcCr[72];
        int bi_y = 0, bi_cb = 0, bi_cr = 0;

        for (int mby = 0; mby < mbh && !br_eof(&br); mby++) {
            for (int mbx = 0; mbx < mbw && !br_eof(&br); mbx++) {
                for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                    int raw0 = vlc_coeff(&br);
                    dc_y += raw0;
                    dcY[bi_y++] = dc_y;
                    for (int i = 1; i < 64 && !br_eof(&br); i++)
                        vlc_coeff(&br);
                }
                {
                    int raw0 = vlc_coeff(&br);
                    dc_cb += raw0;
                    dcCb[bi_cb++] = dc_cb;
                    for (int i = 1; i < 64 && !br_eof(&br); i++)
                        vlc_coeff(&br);
                }
                {
                    int raw0 = vlc_coeff(&br);
                    dc_cr += raw0;
                    dcCr[bi_cr++] = dc_cr;
                    for (int i = 1; i < 64 && !br_eof(&br); i++)
                        vlc_coeff(&br);
                }
            }
        }

        printf("Decoded %d Y blocks, %d Cb, %d Cr, bits=%d/%d\n",
               bi_y, bi_cb, bi_cr, br.total, bslen*8);

        printf("First 32 Y DC (diff): ");
        br_init(&br, bs, bslen);
        dc_y = 0;
        for (int i = 0; i < 32; i++) {
            int raw0 = vlc_coeff(&br);
            dc_y += raw0;
            printf("%+d(%d) ", raw0, dc_y);
            for (int j = 1; j < 64 && !br_eof(&br); j++) vlc_coeff(&br);
        }
        printf("\n");

        int ymin=99999,ymax=-99999;
        for (int i=0;i<bi_y;i++) { if(dcY[i]<ymin)ymin=dcY[i]; if(dcY[i]>ymax)ymax=dcY[i]; }
        printf("Y DC range: %d..%d\n", ymin, ymax);

        int yw = mbw * 2, yh = mbh * 2; /* 16 × 18 blocks */

        /* DC image with continuous prediction */
        uint8_t *dc_img = calloc(yw * yh, 1);
        int idx = 0;
        for (int mby = 0; mby < mbh; mby++) {
            for (int mbx = 0; mbx < mbw; mbx++) {
                int bx[4] = {mbx*2, mbx*2+1, mbx*2, mbx*2+1};
                int by[4] = {mby*2, mby*2, mby*2+1, mby*2+1};
                for (int yb = 0; yb < 4 && idx < bi_y; yb++) {
                    int v = dcY[idx++];
                    int scaled = (ymax!=ymin) ? (v-ymin)*255/(ymax-ymin) : 128;
                    dc_img[by[yb] * yw + bx[yb]] = clamp8(scaled);
                }
            }
        }
        char path[256];
        snprintf(path, sizeof(path), OUT_DIR "dconly_%s_f%d.pgm", game, fi);
        write_pgm(path, dc_img, yw, yh);

        /* Upscale */
        uint8_t *dc_up = calloc(128 * 144, 1);
        for (int y = 0; y < 144; y++)
            for (int x = 0; x < 128; x++)
                dc_up[y*128+x] = dc_img[(y/8)*yw + (x/8)];
        snprintf(path, sizeof(path), OUT_DIR "dconly_up_%s_f%d.pgm", game, fi);
        write_pgm(path, dc_up, 128, 144);

        /* Also try: what if DC is NOT differential? Absolute values */
        uint8_t *dc_abs = calloc(yw * yh, 1);
        br_init(&br, bs, bslen);
        idx = 0;
        int abs_min = 99999, abs_max = -99999;
        int absY[288];
        for (int mby = 0; mby < mbh && !br_eof(&br); mby++) {
            for (int mbx = 0; mbx < mbw && !br_eof(&br); mbx++) {
                for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                    int raw0 = vlc_coeff(&br);
                    absY[idx] = raw0;
                    if(raw0<abs_min)abs_min=raw0;
                    if(raw0>abs_max)abs_max=raw0;
                    idx++;
                    for (int i = 1; i < 64 && !br_eof(&br); i++) vlc_coeff(&br);
                }
                for (int ch = 0; ch < 2; ch++)
                    for (int i = 0; i < 64 && !br_eof(&br); i++) vlc_coeff(&br);
            }
        }
        printf("Y DC raw (non-diff) range: %d..%d\n", abs_min, abs_max);
        idx = 0;
        for (int mby = 0; mby < mbh; mby++) {
            for (int mbx = 0; mbx < mbw; mbx++) {
                int bx[4] = {mbx*2, mbx*2+1, mbx*2, mbx*2+1};
                int by[4] = {mby*2, mby*2, mby*2+1, mby*2+1};
                for (int yb = 0; yb < 4 && idx < 288; yb++) {
                    int v = absY[idx++];
                    int scaled = (abs_max!=abs_min) ? (v-abs_min)*255/(abs_max-abs_min) : 128;
                    dc_abs[by[yb] * yw + bx[yb]] = clamp8(scaled);
                }
            }
        }
        snprintf(path, sizeof(path), OUT_DIR "dcabs_%s_f%d.pgm", game, fi);
        write_pgm(path, dc_abs, yw, yh);

        uint8_t *dc_abs_up = calloc(128 * 144, 1);
        for (int y = 0; y < 144; y++)
            for (int x = 0; x < 128; x++)
                dc_abs_up[y*128+x] = dc_abs[(y/8)*yw + (x/8)];
        snprintf(path, sizeof(path), OUT_DIR "dcabs_up_%s_f%d.pgm", game, fi);
        write_pgm(path, dc_abs_up, 128, 144);

        /* Try different block orders within MB */
        /* What if it's TL,BL,TR,BR instead? Or row-by-row? */
        int orders[4][4] = {
            {0,1,2,3},  /* TL,TR,BL,BR (assumed) */
            {0,2,1,3},  /* TL,BL,TR,BR */
            {0,1,3,2},  /* TL,TR,BR,BL */
            {3,2,1,0}   /* BR,BL,TR,TL */
        };
        const char *onames[] = {"tltrblbr","tlbltrbi","tltrbrbr","brbltrtl"};
        for (int oi = 1; oi < 4; oi++) {
            uint8_t *dc_o = calloc(yw * yh, 1);
            idx = 0;
            for (int mby = 0; mby < mbh; mby++) {
                for (int mbx = 0; mbx < mbw; mbx++) {
                    int bx_base[4] = {mbx*2, mbx*2+1, mbx*2, mbx*2+1};
                    int by_base[4] = {mby*2, mby*2, mby*2+1, mby*2+1};
                    for (int yb = 0; yb < 4 && idx < bi_y; yb++) {
                        int v = dcY[idx++];
                        int dest = orders[oi][yb];
                        int scaled = (ymax!=ymin) ? (v-ymin)*255/(ymax-ymin) : 128;
                        dc_o[by_base[dest] * yw + bx_base[dest]] = clamp8(scaled);
                    }
                }
            }
            snprintf(path, sizeof(path), OUT_DIR "dconly_%s_%s_f%d.pgm", onames[oi], game, fi);
            write_pgm(path, dc_o, yw, yh);
            free(dc_o);
        }

        free(dc_img); free(dc_up); free(dc_abs); free(dc_abs_up);
    }

    free(disc); zip_close(z);
    return 0;
}
