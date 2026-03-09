/*
 * Playdia video - Refined individual coefficient VLC
 * Building on the discovery that magnitude/sign VLC per coefficient
 * produces visible block structure. Now refine:
 * - Try different quantization scaling
 * - Try with/without qtable
 * - Try 4:2:0 macroblock structure
 * - Try different DC prediction modes
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

static int vlc_magsign(BR *b) {
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

/* Row-major scan (not zigzag) */
static const int rowscan8[64] = {
     0, 1, 2, 3, 4, 5, 6, 7,
     8, 9,10,11,12,13,14,15,
    16,17,18,19,20,21,22,23,
    24,25,26,27,28,29,30,31,
    32,33,34,35,36,37,38,39,
    40,41,42,43,44,45,46,47,
    48,49,50,51,52,53,54,55,
    56,57,58,59,60,61,62,63
};

static void idct8x8(int block[64], int out[64]) {
    double tmp[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) {
            double sum = 0;
            for (int k = 0; k < 8; k++) {
                double ck = (k == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += ck * block[i*8+k] * cos((2*j+1)*k*PI/16.0);
            }
            tmp[i*8+j] = sum / 2.0;
        }
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) {
            double sum = 0;
            for (int k = 0; k < 8; k++) {
                double ck = (k == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += ck * tmp[k*8+j] * cos((2*i+1)*k*PI/16.0);
            }
            out[i*8+j] = (int)round(sum / 2.0);
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

/*
 * Decode with various quantization strategies
 * mode 0: no quant (raw VLC values as DCT coefficients)
 * mode 1: multiply by qtable only
 * mode 2: multiply by qscale only
 * mode 3: multiply by qtable × qscale / 8
 * mode 4: multiply by qtable × qscale / 16
 * mode 5: DC absolute (not differential), AC as-is
 */
static void decode_variant(const uint8_t *bs, int bslen, int qscale,
                           const uint8_t qt[16], const char *tag, int mode,
                           int imgW, int imgH, const int *scan) {
    int bw = imgW / 8, bh = imgH / 8;
    uint8_t *img = calloc(imgW * imgH, 1);
    BR br; br_init(&br, bs, bslen);

    int qm[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            qm[i*8+j] = qt[(i/2)*4 + (j/2)];

    int dc_pred = 0;

    for (int by = 0; by < bh && !br_eof(&br); by++) {
        if (mode >= 10) dc_pred = 0; /* reset per row for modes 10+ */
        for (int bx = 0; bx < bw && !br_eof(&br); bx++) {
            int block[64] = {0}, spatial[64];

            /* Read DC */
            int dc_raw = vlc_magsign(&br);

            if (mode == 5 || mode == 15) {
                /* Absolute DC */
                block[0] = dc_raw * 8;
            } else {
                dc_pred += dc_raw;
                block[0] = dc_pred * 8;
            }

            /* Read 63 AC coefficients */
            for (int i = 1; i < 64 && !br_eof(&br); i++) {
                int val = vlc_magsign(&br);
                int pos = scan[i];
                switch (mode % 10) {
                    case 0: block[pos] = val; break;
                    case 1: block[pos] = val * qm[pos]; break;
                    case 2: block[pos] = val * qscale; break;
                    case 3: block[pos] = val * qm[pos] * qscale / 8; break;
                    case 4: block[pos] = val * qm[pos] * qscale / 16; break;
                    case 5: block[pos] = val; break;
                    case 6: block[pos] = val * 2; break;
                    case 7: block[pos] = val * 4; break;
                }
            }

            idct8x8(block, spatial);
            for (int dy = 0; dy < 8; dy++)
                for (int dx = 0; dx < 8; dx++) {
                    int v = spatial[dy*8+dx] + 128;
                    if (v < 0) v = 0; if (v > 255) v = 255;
                    if (by*8+dy < imgH && bx*8+dx < imgW)
                        img[(by*8+dy)*imgW + bx*8+dx] = v;
                }
        }
    }

    printf("Mode %d %s: %d/%d bits\n", mode, tag, br.total, bslen*8);
    char path[256];
    snprintf(path, sizeof(path), OUT_DIR "v%d_%s.pgm", mode, tag);
    write_pgm(path, img, imgW, imgH);
    free(img);
}

/*
 * Decode with 4:2:0 macroblock structure
 * Each MB: 4 Y blocks (8x8) + 1 Cb block + 1 Cr block
 * All coefficients individually VLC coded
 */
static void decode_420(const uint8_t *bs, int bslen, int qscale,
                       const uint8_t qt[16], const char *tag,
                       int imgW, int imgH) {
    int mbw = imgW / 16, mbh = imgH / 16;
    uint8_t *imgY = calloc(imgW * imgH, 1);
    uint8_t *imgCb = calloc(imgW/2 * imgH/2, 1);
    uint8_t *imgCr = calloc(imgW/2 * imgH/2, 1);
    BR br; br_init(&br, bs, bslen);

    int qm[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            qm[i*8+j] = qt[(i/2)*4 + (j/2)];

    int dc_y = 0, dc_cb = 0, dc_cr = 0;

    for (int mby = 0; mby < mbh && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < mbw && !br_eof(&br); mbx++) {
            /* 4 Y blocks: TL, TR, BL, BR */
            int yspat[4][64];
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                int block[64] = {0}, spatial[64];
                int dc_raw = vlc_magsign(&br);
                dc_y += dc_raw;
                block[0] = dc_y * 8;
                for (int i = 1; i < 64 && !br_eof(&br); i++)
                    block[zigzag8[i]] = vlc_magsign(&br);
                idct8x8(block, spatial);
                memcpy(yspat[yb], spatial, sizeof(spatial));
            }

            /* Place Y */
            int offsets[4][2] = {{0,0},{8,0},{0,8},{8,8}};
            for (int yb = 0; yb < 4; yb++)
                for (int dy = 0; dy < 8; dy++)
                    for (int dx = 0; dx < 8; dx++) {
                        int v = yspat[yb][dy*8+dx] + 128;
                        if (v<0)v=0;if(v>255)v=255;
                        int px=mbx*16+offsets[yb][0]+dx;
                        int py=mby*16+offsets[yb][1]+dy;
                        if(px<imgW&&py<imgH) imgY[py*imgW+px]=v;
                    }

            /* Cb */
            {
                int block[64]={0}, spatial[64];
                dc_cb += vlc_magsign(&br);
                block[0] = dc_cb * 8;
                for(int i=1;i<64&&!br_eof(&br);i++)
                    block[zigzag8[i]] = vlc_magsign(&br);
                idct8x8(block, spatial);
                for(int dy=0;dy<8;dy++)
                    for(int dx=0;dx<8;dx++){
                        int v=spatial[dy*8+dx]+128;
                        if(v<0)v=0;if(v>255)v=255;
                        int px=mbx*8+dx,py=mby*8+dy;
                        if(px<imgW/2&&py<imgH/2) imgCb[py*(imgW/2)+px]=v;
                    }
            }

            /* Cr */
            {
                int block[64]={0}, spatial[64];
                dc_cr += vlc_magsign(&br);
                block[0] = dc_cr * 8;
                for(int i=1;i<64&&!br_eof(&br);i++)
                    block[zigzag8[i]] = vlc_magsign(&br);
                idct8x8(block, spatial);
                for(int dy=0;dy<8;dy++)
                    for(int dx=0;dx<8;dx++){
                        int v=spatial[dy*8+dx]+128;
                        if(v<0)v=0;if(v>255)v=255;
                        int px=mbx*8+dx,py=mby*8+dy;
                        if(px<imgW/2&&py<imgH/2) imgCr[py*(imgW/2)+px]=v;
                    }
            }
        }
    }

    printf("420 %s: %d/%d bits (%.1f%%)\n", tag, br.total, bslen*8,
           100.0*br.total/(bslen*8));
    char path[256];
    snprintf(path, sizeof(path), OUT_DIR "v420y_%s.pgm", tag);
    write_pgm(path, imgY, imgW, imgH);
    snprintf(path, sizeof(path), OUT_DIR "v420cb_%s.pgm", tag);
    write_pgm(path, imgCb, imgW/2, imgH/2);
    snprintf(path, sizeof(path), OUT_DIR "v420cr_%s.pgm", tag);
    write_pgm(path, imgCr, imgW/2, imgH/2);
    free(imgY); free(imgCb); free(imgCr);
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

    /* Try first 2 frames */
    for (int fi = 0; fi < 2 && fi < nf; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        int qscale = f[3];
        uint8_t qt[16];
        memcpy(qt, f+4, 16);
        printf("\n=== Frame %d: qscale=%d, type=%d ===\n", fi, qscale, f[39]);

        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;
        char tag[64];
        snprintf(tag, sizeof(tag), "%s_f%d", game, fi);

        /* Various quantization modes with zigzag scan */
        for (int mode = 0; mode <= 7; mode++)
            decode_variant(bs, bslen, qscale, qt, tag, mode, 128, 144, zigzag8);

        /* Mode 0 (no quant) with row-major scan */
        decode_variant(bs, bslen, qscale, qt, tag, 0, 128, 144, rowscan8);

        /* Row-reset DC prediction (mode 10 = no quant + row reset) */
        decode_variant(bs, bslen, qscale, qt, tag, 10, 128, 144, zigzag8);

        /* Absolute DC (mode 5) */
        decode_variant(bs, bslen, qscale, qt, tag, 5, 128, 144, zigzag8);

        /* 4:2:0 macroblock */
        decode_420(bs, bslen, qscale, qt, tag, 128, 144);

        /* Try 128x128 */
        decode_420(bs, bslen, qscale, qt, tag, 128, 128);
    }

    free(disc); zip_close(z);
    return 0;
}
