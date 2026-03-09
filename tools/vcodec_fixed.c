/*
 * Playdia video - Fixed-width coding attempts
 * Try: 5-bit pixels, 4-bit pixels, various fixed-width coefficient coding
 * Also: different bitstream offsets, different data interpretation
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

// LSB-first bit reading
typedef struct { const uint8_t *data; int len,pos,bit,total; } BRL;
static void brl_init(BRL *b, const uint8_t *d, int l) { b->data=d;b->len=l;b->pos=0;b->bit=0;b->total=0; }
static int brl_get(BRL *b, int n) {
    int v=0;
    for(int i=0;i<n;i++){
        if(b->pos>=b->len) return v;
        v |= ((b->data[b->pos]>>b->bit)&1) << i;
        if(++b->bit>7){b->bit=0;b->pos++;}
        b->total++;
    }
    return v;
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
    int slba=argc>2?atoi(argv[2]):502;

    int err;zip_t *z=zip_open(argv[1],ZIP_RDONLY,&err); if(!z)return 1;
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

    int fi = nf > 1 ? 1 : 0;
    uint8_t *f = frames[fi];
    int fsize = fsizes[fi];
    uint8_t qtab[16]; memcpy(qtab,f+4,16);
    int qscale = f[3];
    printf("Frame %d: %d bytes, qscale=%d\n", fi, fsize, qscale);

    // Try bitstream from different offsets
    int offsets[] = {36, 40, 4, 20, 0};
    int noff = 5;

    int W = 128, H = 144;

    for (int oi = 0; oi < noff; oi++) {
        int off = offsets[oi];
        const uint8_t *bs = f + off;
        int bslen = fsize - off;

        printf("\n=== Offset %d ===\n", off);

        // 5-bit raw pixels, MSB first
        {
            BR br; br_init(&br, bs, bslen);
            uint8_t *img = calloc(W*H, 1);
            for (int i = 0; i < W*H; i++) {
                int v = br_get(&br, 5);
                img[i] = v * 255 / 31; // scale 0-31 to 0-255
            }
            char path[256];
            snprintf(path,sizeof(path),"/home/wizzard/share/GitHub/pd_5b_o%d.pgm",off);
            write_pgm(path, img, W, H);
            free(img);
        }

        // 5-bit raw pixels, LSB first
        {
            BRL br; brl_init(&br, bs, bslen);
            uint8_t *img = calloc(W*H, 1);
            for (int i = 0; i < W*H; i++) {
                int v = brl_get(&br, 5);
                img[i] = v * 255 / 31;
            }
            char path[256];
            snprintf(path,sizeof(path),"/home/wizzard/share/GitHub/pd_5bl_o%d.pgm",off);
            write_pgm(path, img, W, H);
            free(img);
        }

        // 4-bit raw pixels
        {
            uint8_t *img = calloc(W*H, 1);
            for (int i = 0; i < W*H && i/2 < bslen; i++) {
                int v = (i & 1) ? (bs[i/2] & 0x0F) : (bs[i/2] >> 4);
                img[i] = v * 17; // 0-15 → 0-255
            }
            char path[256];
            snprintf(path,sizeof(path),"/home/wizzard/share/GitHub/pd_4b_o%d.pgm",off);
            write_pgm(path, img, W, H);
            free(img);
        }

        // 6-bit raw pixels
        {
            BR br; br_init(&br, bs, bslen);
            uint8_t *img = calloc(W*H, 1);
            for (int i = 0; i < W*H; i++) {
                int v = br_get(&br, 6);
                img[i] = v * 255 / 63;
            }
            char path[256];
            snprintf(path,sizeof(path),"/home/wizzard/share/GitHub/pd_6b_o%d.pgm",off);
            write_pgm(path, img, W, H);
            free(img);
        }

        // Only do extended tests for offset 40
        if (off != 40) continue;

        // 3-bit signed fixed coefficients, 16 per 4×4 block, with QTable dequant, DCT
        {
            static const int zigzag4[16] = {0,1,4,8,5,2,3,6,9,12,13,10,7,11,14,15};
            BR br; br_init(&br, bs, bslen);
            uint8_t *img = calloc(W*H, 1);
            int blocks = 0;

            for (int by = 0; by < H/4; by++) {
                for (int bx = 0; bx < W/4; bx++) {
                    double coeff[16] = {0};
                    for (int k = 0; k < 16; k++) {
                        int raw = br_get(&br, 3);
                        // Sign-extend 3-bit
                        if (raw & 4) raw |= ~7;
                        int pos = zigzag4[k];
                        coeff[pos] = raw * qtab[k];
                    }

                    // 4×4 IDCT
                    int pixels[16];
                    for(int y=0;y<4;y++) for(int x=0;x<4;x++){
                        double s=0;
                        for(int v=0;v<4;v++){double cv=v==0?0.5:sqrt(0.5);
                        for(int u=0;u<4;u++){double cu=u==0?0.5:sqrt(0.5);
                        s+=cu*cv*coeff[v*4+u]*cos((2*x+1)*u*PI/8.0)*cos((2*y+1)*v*PI/8.0);}}
                        pixels[y*4+x]=(int)round(s);
                    }

                    for(int dy=0;dy<4;dy++)
                        for(int dx=0;dx<4;dx++){
                            int v=pixels[dy*4+dx]+128;
                            if(v<0)v=0;if(v>255)v=255;
                            img[(by*4+dy)*W+bx*4+dx]=v;
                        }
                    blocks++;
                }
            }
            printf("3bit-fixed DCT: %d blocks, %d bits\n", blocks, br.total);
            write_pgm("/home/wizzard/share/GitHub/pd_fix3_dct.pgm", img, W, H);
            free(img);
        }

        // 5-bit signed fixed coefficients, 16 per block, QTable dequant, DCT
        {
            static const int zigzag4[16] = {0,1,4,8,5,2,3,6,9,12,13,10,7,11,14,15};
            BR br; br_init(&br, bs, bslen);
            uint8_t *img = calloc(W*H, 1);

            for (int by = 0; by < H/4; by++) {
                for (int bx = 0; bx < W/4; bx++) {
                    double coeff[16] = {0};
                    for (int k = 0; k < 16; k++) {
                        int raw = br_get(&br, 5);
                        if (raw & 16) raw |= ~31; // sign-extend
                        coeff[zigzag4[k]] = raw * qtab[k];
                    }
                    int pixels[16];
                    for(int y=0;y<4;y++) for(int x=0;x<4;x++){
                        double s=0;
                        for(int v=0;v<4;v++){double cv=v==0?0.5:sqrt(0.5);
                        for(int u=0;u<4;u++){double cu=u==0?0.5:sqrt(0.5);
                        s+=cu*cv*coeff[v*4+u]*cos((2*x+1)*u*PI/8.0)*cos((2*y+1)*v*PI/8.0);}}
                        pixels[y*4+x]=(int)round(s);
                    }
                    for(int dy=0;dy<4;dy++)
                        for(int dx=0;dx<4;dx++){
                            int v=pixels[dy*4+dx]+128;
                            if(v<0)v=0;if(v>255)v=255;
                            img[(by*4+dy)*W+bx*4+dx]=v;
                        }
                }
            }
            write_pgm("/home/wizzard/share/GitHub/pd_fix5_dct.pgm", img, W, H);
            free(img);
        }

        // Try: raw 8-bit pixels at different resolutions
        // What if it's simply 8-bit raw with different width?
        printf("\n--- Raw 8-bit at various widths ---\n");
        int rw[] = {128, 160, 176, 192, 256, 320};
        for (int ri = 0; ri < 6; ri++) {
            int rW = rw[ri];
            int rH = bslen / rW;
            if (rH < 10) continue;
            uint8_t *img = calloc(rW*rH, 1);
            memcpy(img, bs, rW*rH);
            char path[256];
            snprintf(path,sizeof(path),"/home/wizzard/share/GitHub/pd_raw8_%d.pgm",rW);
            write_pgm(path, img, rW, rH);
            free(img);
        }
    }

    free(disc); zip_close(z);
    return 0;
}
