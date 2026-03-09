/*
 * Playdia video - DPCM (Differential Pulse Code Modulation) at pixel level
 * Each pixel is coded as a signed Exp-Golomb difference from prediction
 * Predictions: left pixel, above pixel, or (left+above)/2
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <zip.h>

#define SECTOR_RAW 2352
#define MAX_FRAME  65536

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

static int eg_signed(BR *b) {
    int lz=0;
    while(!br_eof(b)){int bit=br_get1(b);if(bit<0)return 0;if(bit==1)break;lz++;if(lz>24)return 0;}
    int suf=lz>0?br_get(b,lz):0;
    int cn=(1<<lz)-1+suf;
    return (cn&1)?-((cn+1)/2):(cn/2);
}

// Rice code with parameter k
static int rice_signed(BR *b, int k) {
    int q = 0;
    while (!br_eof(b)) {
        int bit = br_get1(b);
        if (bit < 0) return 0;
        if (bit == 1) break;
        q++;
        if (q > 24) return 0;
    }
    int r = k > 0 ? br_get(b, k) : 0;
    int mag = q * (1 << k) + r;
    // Fold signed: even→positive, odd→negative
    return (mag & 1) ? -((mag + 1) / 2) : (mag / 2);
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

    int err;zip_t *z=zip_open(argv[1],ZIP_RDONLY,&err);
    if(!z)return 1;
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

    int fi = nf > 1 ? 1 : 0;
    uint8_t *f = frames[fi];
    int fsize = fsizes[fi];
    printf("Frame %d: %d bytes, header=%02X %02X %02X %02X\n", fi, fsize, f[0],f[1],f[2],f[3]);

    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;

    // Try different widths and DPCM modes
    int widths[] = {128, 144, 96, 192, 64, 256};
    int nw = 6;

    for (int wi = 0; wi < nw; wi++) {
        int W = widths[wi];
        int H_max = (bslen * 8) / 2; // rough max height estimate
        if (H_max > 384) H_max = 384;
        // For pixel DPCM, we need W*H Exp-Golomb values
        // Estimated: 3.5 bits per value, so H ≈ bslen*8 / (3.5*W)
        int H = (int)((bslen * 8.0) / (3.8 * W));
        H = (H / 4) * 4; // round to multiple of 4
        if (H > 384) H = 384;
        if (H < 16) continue;

        // === DPCM: predict from left ===
        {
            BR br; br_init(&br, bs, bslen);
            uint8_t *img = calloc(W*H, 1);
            for (int y = 0; y < H && !br_eof(&br); y++) {
                int prev = 128;
                for (int x = 0; x < W && !br_eof(&br); x++) {
                    int diff = eg_signed(&br);
                    int val = prev + diff;
                    if (val < 0) val = 0; if (val > 255) val = 255;
                    img[y*W+x] = val;
                    prev = val;
                }
            }
            char path[256];
            snprintf(path,sizeof(path),"/home/wizzard/share/GitHub/pd_dpcm_l_%dx%d.pgm",W,H);
            printf("DPCM-left %dx%d: %d bits\n", W, H, br.total);
            write_pgm(path, img, W, H);
            free(img);
        }

        // === DPCM: predict from above ===
        {
            BR br; br_init(&br, bs, bslen);
            uint8_t *img = calloc(W*H, 1);
            for (int y = 0; y < H && !br_eof(&br); y++) {
                for (int x = 0; x < W && !br_eof(&br); x++) {
                    int pred = y > 0 ? img[(y-1)*W+x] : 128;
                    int diff = eg_signed(&br);
                    int val = pred + diff;
                    if (val < 0) val = 0; if (val > 255) val = 255;
                    img[y*W+x] = val;
                }
            }
            char path[256];
            snprintf(path,sizeof(path),"/home/wizzard/share/GitHub/pd_dpcm_a_%dx%d.pgm",W,H);
            printf("DPCM-above %dx%d: %d bits\n", W, H, br.total);
            write_pgm(path, img, W, H);
            free(img);
        }

        if (W == 128) {
            // === DPCM: predict from left+above average ===
            int H2 = 144;
            BR br; br_init(&br, bs, bslen);
            uint8_t *img = calloc(W*H2, 1);
            for (int y = 0; y < H2 && !br_eof(&br); y++) {
                for (int x = 0; x < W && !br_eof(&br); x++) {
                    int pred;
                    if (x == 0 && y == 0) pred = 128;
                    else if (x == 0) pred = img[(y-1)*W];
                    else if (y == 0) pred = img[y*W+x-1];
                    else pred = (img[y*W+x-1] + img[(y-1)*W+x]) / 2;
                    int diff = eg_signed(&br);
                    int val = pred + diff;
                    if (val < 0) val = 0; if (val > 255) val = 255;
                    img[y*W+x] = val;
                }
            }
            printf("DPCM-avg 128x144: %d bits\n", br.total);
            write_pgm("/home/wizzard/share/GitHub/pd_dpcm_avg_128x144.pgm", img, W, H2);
            free(img);

            // === Also try Rice codes with various k ===
            for (int k = 0; k <= 3; k++) {
                BR br2; br_init(&br2, bs, bslen);
                uint8_t *img2 = calloc(128*144, 1);
                for (int y = 0; y < 144 && !br_eof(&br2); y++) {
                    int prev = 128;
                    for (int x = 0; x < 128 && !br_eof(&br2); x++) {
                        int diff = rice_signed(&br2, k);
                        int val = prev + diff;
                        if (val < 0) val = 0; if (val > 255) val = 255;
                        img2[y*128+x] = val;
                        prev = val;
                    }
                }
                char path[256];
                snprintf(path,sizeof(path),"/home/wizzard/share/GitHub/pd_rice%d_128x144.pgm",k);
                printf("Rice(k=%d) DPCM-left 128x144: %d bits\n", k, br2.total);
                write_pgm(path, img2, 128, 144);
                free(img2);
            }
        }
    }

    free(disc); zip_close(z);
    return 0;
}
