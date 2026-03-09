/*
 * Playdia video - Zero-flag VLC hypothesis
 * Key insight: with 4:2:0 at 128x144, we need ~97936 bits for all coefficients.
 * The MPEG-1 DC VLC uses 3 bits minimum (even for zero), which is too many.
 * Try: 0 = zero (1 bit), 1 + magnitude/sign = non-zero
 * This allows ~70% zero coefficients at 1 bit each, fitting the bit budget.
 *
 * Also try the inverse: 1 = zero, 0 + mag/sign = non-zero
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

/* VLC type A: 0=zero, 1+size_code+value = non-zero
 * Size coding: unary 0s terminated by 1
 * 0 → 0
 * 1 0 s → ±1 (3 bits)
 * 1 10 ss → ±2,3 (5 bits)
 * 1 110 sss → ±4..7 (7 bits)
 * 1 1110 ssss → ±8..15 (9 bits)
 * etc.
 */
static int vlc_zfA(BR *b) {
    if (br_get1(b) == 0) return 0;
    /* Non-zero: read magnitude size (unary) */
    int size = 1;
    while (!br_eof(b) && br_get1(b) == 1 && size < 12) size++;
    int val = br_get(b, size);
    if (val < (1 << (size-1))) val = val - (1 << size) + 1;
    return val;
}

/* VLC type B: 0=zero, 1+MPEG1_DC_style = non-zero */
static int vlc_zfB(BR *b) {
    if (br_get1(b) == 0) return 0;
    /* Size from MPEG-1 DC luminance table */
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
    if (size == 0) return 0; /* shouldn't happen after flag=1, but safety */
    int val = br_get(b, size);
    if (val < (1 << (size-1))) val = val - (1 << size) + 1;
    return val;
}

/* VLC type C: Inverse: 1=zero, 0+size+value = non-zero */
static int vlc_zfC(BR *b) {
    if (br_get1(b) == 1) return 0;
    int size = 1;
    while (!br_eof(b) && br_get1(b) == 1 && size < 12) size++;
    int val = br_get(b, size);
    if (val < (1 << (size-1))) val = val - (1 << size) + 1;
    return val;
}

/* VLC type D: Simple sign+magnitude
 * 0 → 0
 * 1 s → ±1 (2 bits)
 * If |value| > 1: escape to larger coding?
 * Actually: this is too simple. Let me use:
 * 0 → 0
 * 10 v → ±1 where v is sign (3 bits)
 * 11 vv → ±(2-3) where vv is 2 bits (4 bits total)
 * Hmm, too ad-hoc. Skip this.
 */

/* VLC type E: Pure MPEG-1 DC lum (original, for reference) */
static int vlc_dc_lum(BR *b) {
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

/* VLC type F: 0=zero, 1+sign+unary_magnitude
 * 0 → 0
 * 1 s 0 → ±1 (3 bits)
 * 1 s 10 → ±2 (4 bits)
 * 1 s 110 → ±3 (5 bits)
 * 1 s 1..10 → ±(n+1)
 */
static int vlc_zfF(BR *b) {
    if (br_get1(b) == 0) return 0;
    int sign = br_get1(b);
    int mag = 1;
    while (!br_eof(b) && br_get1(b) == 1 && mag < 127) mag++;
    return sign ? -mag : mag;
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

typedef int (*vlc_func)(BR *);

static void decode_420_generic(const uint8_t *bs, int bslen, int qscale,
                                const uint8_t qt[16], const char *tag,
                                vlc_func dc_vlc, vlc_func ac_vlc,
                                int imgW, int imgH, int dc_scale) {
    int mbw = imgW / 16, mbh = imgH / 16;
    uint8_t *imgY = calloc(imgW * imgH, 1);
    BR br; br_init(&br, bs, bslen);

    int qm[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            qm[i*8+j] = qt[(i/2)*4 + (j/2)];

    int dc_y = 0, dc_cb = 0, dc_cr = 0;

    for (int mby = 0; mby < mbh && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < mbw && !br_eof(&br); mbx++) {
            /* 4 Y blocks */
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                int block[64] = {0}, spatial[64];

                int dc_raw = dc_vlc(&br);
                dc_y += dc_raw;
                block[0] = dc_y * dc_scale;

                for (int i = 1; i < 64 && !br_eof(&br); i++) {
                    int val = ac_vlc(&br);
                    block[zigzag8[i]] = val;
                }

                idct8x8(block, spatial);

                int offx = (yb & 1) * 8, offy = (yb >> 1) * 8;
                for (int dy = 0; dy < 8; dy++)
                    for (int dx = 0; dx < 8; dx++) {
                        int v = spatial[dy*8+dx] + 128;
                        if (v<0) v=0; if (v>255) v=255;
                        int px=mbx*16+offx+dx, py=mby*16+offy+dy;
                        if(px<imgW&&py<imgH) imgY[py*imgW+px]=v;
                    }
            }

            /* Cb - 1 block */
            {
                int block[64]={0}, spatial[64];
                dc_cb += dc_vlc(&br);
                block[0] = dc_cb * dc_scale;
                for(int i=1;i<64&&!br_eof(&br);i++)
                    block[zigzag8[i]] = ac_vlc(&br);
                idct8x8(block, spatial);
                /* just skip - we only render Y for now */
            }

            /* Cr - 1 block */
            {
                int block[64]={0}, spatial[64];
                dc_cr += dc_vlc(&br);
                block[0] = dc_cr * dc_scale;
                for(int i=1;i<64&&!br_eof(&br);i++)
                    block[zigzag8[i]] = ac_vlc(&br);
                idct8x8(block, spatial);
            }
        }
    }

    printf("%s: %d/%d bits (%.1f%%)\n", tag, br.total, bslen*8,
           100.0*br.total/(bslen*8));
    char path[256];
    snprintf(path, sizeof(path), OUT_DIR "%s.pgm", tag);
    write_pgm(path, imgY, imgW, imgH);
    free(imgY);
}

/* Count bits used for Y and chroma separately */
static void count_bits_420(const uint8_t *bs, int bslen,
                           vlc_func dc_vlc, vlc_func ac_vlc,
                           const char *label, int imgW, int imgH) {
    int mbw = imgW / 16, mbh = imgH / 16;
    BR br; br_init(&br, bs, bslen);
    int y_bits = 0, cb_bits = 0, cr_bits = 0;

    for (int mby = 0; mby < mbh && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < mbw && !br_eof(&br); mbx++) {
            int before = br.total;
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                dc_vlc(&br);
                for (int i = 1; i < 64 && !br_eof(&br); i++) ac_vlc(&br);
            }
            y_bits += br.total - before;

            before = br.total;
            dc_vlc(&br);
            for (int i = 1; i < 64 && !br_eof(&br); i++) ac_vlc(&br);
            cb_bits += br.total - before;

            before = br.total;
            dc_vlc(&br);
            for (int i = 1; i < 64 && !br_eof(&br); i++) ac_vlc(&br);
            cr_bits += br.total - before;
        }
    }

    int total = y_bits + cb_bits + cr_bits;
    printf("%s: Y=%d Cb=%d Cr=%d total=%d/%d (%.1f%%)\n",
           label, y_bits, cb_bits, cr_bits, total, bslen*8,
           100.0*total/(bslen*8));
    printf("  bits/coeff: Y=%.2f Cb=%.2f Cr=%.2f overall=%.2f\n",
           (double)y_bits/(288*64), (double)cb_bits/(72*64),
           (double)cr_bits/(72*64), (double)total/(432*64));
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

    uint8_t *f = frames[0];
    int fsize = fsizes[0];
    int qscale = f[3];
    uint8_t qt[16]; memcpy(qt, f+4, 16);
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    printf("Frame 0: %d bytes, qscale=%d, type=%d\n", fsize, qscale, f[39]);

    /* Count bits for each VLC type */
    printf("\n=== Bit counting per VLC type ===\n");
    count_bits_420(bs, bslen, vlc_dc_lum, vlc_dc_lum, "MPEG1-DC", 128, 144);
    count_bits_420(bs, bslen, vlc_dc_lum, vlc_zfA, "DC+zfA", 128, 144);
    count_bits_420(bs, bslen, vlc_dc_lum, vlc_zfB, "DC+zfB", 128, 144);
    count_bits_420(bs, bslen, vlc_dc_lum, vlc_zfC, "DC+zfC", 128, 144);
    count_bits_420(bs, bslen, vlc_dc_lum, vlc_zfF, "DC+zfF", 128, 144);
    count_bits_420(bs, bslen, vlc_zfA, vlc_zfA, "zfA-all", 128, 144);
    count_bits_420(bs, bslen, vlc_zfB, vlc_zfB, "zfB-all", 128, 144);

    /* Now decode with best matches */
    printf("\n=== Decode attempts ===\n");

    /* Type A: 0=zero, 1+unary_size+value */
    char tag[128];
    snprintf(tag, sizeof(tag), "zfA_%s_f0", game);
    decode_420_generic(bs, bslen, qscale, qt, tag, vlc_dc_lum, vlc_zfA, 128, 144, 8);

    snprintf(tag, sizeof(tag), "zfA4_%s_f0", game);
    decode_420_generic(bs, bslen, qscale, qt, tag, vlc_dc_lum, vlc_zfA, 128, 144, 4);

    snprintf(tag, sizeof(tag), "zfB_%s_f0", game);
    decode_420_generic(bs, bslen, qscale, qt, tag, vlc_dc_lum, vlc_zfB, 128, 144, 8);

    snprintf(tag, sizeof(tag), "zfC_%s_f0", game);
    decode_420_generic(bs, bslen, qscale, qt, tag, vlc_dc_lum, vlc_zfC, 128, 144, 8);

    snprintf(tag, sizeof(tag), "zfF_%s_f0", game);
    decode_420_generic(bs, bslen, qscale, qt, tag, vlc_dc_lum, vlc_zfF, 128, 144, 8);

    /* All coefficients (including DC) with zfA */
    snprintf(tag, sizeof(tag), "zfAall_%s_f0", game);
    decode_420_generic(bs, bslen, qscale, qt, tag, vlc_zfA, vlc_zfA, 128, 144, 8);

    /* Try frame 1 with best match */
    f = frames[1];
    bs = f + 40;
    bslen = fsizes[1] - 40;
    printf("\nFrame 1: qscale=%d, type=%d\n", f[3], f[39]);

    count_bits_420(bs, bslen, vlc_dc_lum, vlc_dc_lum, "F1-MPEG1-DC", 128, 144);
    count_bits_420(bs, bslen, vlc_dc_lum, vlc_zfA, "F1-DC+zfA", 128, 144);
    count_bits_420(bs, bslen, vlc_dc_lum, vlc_zfB, "F1-DC+zfB", 128, 144);

    snprintf(tag, sizeof(tag), "zfA_%s_f1", game);
    decode_420_generic(bs, bslen, f[3], qt, tag, vlc_dc_lum, vlc_zfA, 128, 144, 8);

    free(disc); zip_close(z);
    return 0;
}
