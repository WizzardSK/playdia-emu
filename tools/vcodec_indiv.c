/*
 * Playdia video - Individual coefficient VLC hypothesis
 * Each DCT coefficient coded independently with magnitude/sign VLC
 * No MPEG-1 run/level, no EOB. Just 64 VLC values per 8x8 block.
 * Also try: coded block pattern (CBP) + only coded blocks get data
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

/* Magnitude/sign VLC (same as MPEG-1 DC lum) */
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

/* Alternative VLC: simpler unary + sign */
static int vlc_unary(BR *b) {
    /* 0 → 0
     * 10 + sign → ±1
     * 110 + 2bits → ±(2..3)
     * 1110 + 3bits → ±(4..7)
     * etc.
     */
    int n = 0;
    while (!br_eof(b) && br_get1(b) == 1 && n < 10) n++;
    if (n == 0) return 0;
    int val = (1 << (n-1)) + (n > 1 ? br_get(b, n-1) : 0);
    int sign = br_get1(b);
    return sign ? -val : val;
}

/* Alternative VLC: Exp-Golomb (order 0, signed) */
static int vlc_egk0(BR *b) {
    int lz = 0;
    while (!br_eof(b) && br_get1(b) == 0 && lz < 24) lz++;
    int suf = lz > 0 ? br_get(b, lz) : 0;
    int cn = (1 << lz) - 1 + suf;
    return (cn & 1) ? -((cn+1)/2) : (cn/2);
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

/*
 * Method 1: Each coefficient is individually VLC-coded (magnitude/sign)
 * DC is differential (like MPEG-1), AC is absolute
 */
static void try_indiv_vlc(const uint8_t *bs, int bslen, int qscale,
                           const uint8_t qt[16], const char *tag,
                           int imgW, int imgH, int vlc_type) {
    int bw = imgW / 8, bh = imgH / 8;
    uint8_t *img = calloc(imgW * imgH, 1);
    BR br; br_init(&br, bs, bslen);
    int dc_pred = 0;

    int qm[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            qm[i*8+j] = qt[(i/2)*4 + (j/2)];

    for (int by = 0; by < bh && !br_eof(&br); by++) {
        for (int bx = 0; bx < bw && !br_eof(&br); bx++) {
            int block[64] = {0}, spatial[64];

            /* DC: differential magnitude/sign */
            int dc_diff;
            if (vlc_type == 0) dc_diff = vlc_magsign(&br);
            else if (vlc_type == 1) dc_diff = vlc_unary(&br);
            else dc_diff = vlc_egk0(&br);

            dc_pred += dc_diff;
            block[0] = dc_pred * qm[0] * qscale;

            /* AC: each coefficient individually */
            for (int i = 1; i < 64 && !br_eof(&br); i++) {
                int val;
                if (vlc_type == 0) val = vlc_magsign(&br);
                else if (vlc_type == 1) val = vlc_unary(&br);
                else val = vlc_egk0(&br);

                int pos = zigzag8[i];
                block[pos] = val * qm[pos] * qscale;
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

    const char *vlc_names[] = {"magsign", "unary", "egk0"};
    printf("Indiv-%s %s: %d/%d bits used\n", vlc_names[vlc_type], tag, br.total, bslen*8);

    char path[256];
    snprintf(path, sizeof(path), OUT_DIR "ind_%s_%s.pgm", vlc_names[vlc_type], tag);
    write_pgm(path, img, imgW, imgH);
    free(img);
}

/*
 * Method 2: Bitmap + fixed-width coefficients
 * For each 8×8 block: 63-bit bitmap (which ACs are present),
 * then fixed-width values for DC and present ACs
 */
static void try_bitmap_ac(const uint8_t *bs, int bslen, int qscale,
                           const uint8_t qt[16], const char *tag,
                           int imgW, int imgH) {
    int bw = imgW / 8, bh = imgH / 8;
    uint8_t *img = calloc(imgW * imgH, 1);
    BR br; br_init(&br, bs, bslen);
    int dc_pred = 0;

    int qm[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            qm[i*8+j] = qt[(i/2)*4 + (j/2)];

    int nblocks = 0;
    for (int by = 0; by < bh && !br_eof(&br); by++) {
        for (int bx = 0; bx < bw && !br_eof(&br); bx++) {
            int block[64] = {0}, spatial[64];

            /* DC with VLC */
            int dc_diff = vlc_magsign(&br);
            dc_pred += dc_diff;
            block[0] = dc_pred * qm[0] * qscale;

            /* Read 63-bit bitmap */
            uint64_t bitmap = 0;
            for (int i = 0; i < 63 && !br_eof(&br); i++)
                bitmap = (bitmap << 1) | br_get1(&br);

            /* Read values for set bits */
            for (int i = 0; i < 63 && !br_eof(&br); i++) {
                if ((bitmap >> (62 - i)) & 1) {
                    int val = vlc_magsign(&br);
                    int pos = zigzag8[i + 1];
                    block[pos] = val * qm[pos] * qscale;
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
            nblocks++;
        }
    }

    printf("Bitmap %s: %d blocks, %d/%d bits used\n", tag, nblocks, br.total, bslen*8);
    char path[256];
    snprintf(path, sizeof(path), OUT_DIR "bitmap_%s.pgm", tag);
    write_pgm(path, img, imgW, imgH);
    free(img);
}

/*
 * Method 3: Zero-runlength before each non-zero coefficient
 * Like MPEG-1 but using magnitude/sign VLC for both run and level
 * Read run (magsign VLC), read level (magsign VLC), repeat
 * Run=0 means next coefficient position. Negative run is invalid (error).
 * A very large run (>=remaining) means end of block.
 */
static void try_run_level_vlc(const uint8_t *bs, int bslen, int qscale,
                               const uint8_t qt[16], const char *tag,
                               int imgW, int imgH) {
    int bw = imgW / 8, bh = imgH / 8;
    uint8_t *img = calloc(imgW * imgH, 1);
    BR br; br_init(&br, bs, bslen);
    int dc_pred = 0;

    int qm[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            qm[i*8+j] = qt[(i/2)*4 + (j/2)];

    int ok = 0, fail = 0;
    for (int by = 0; by < bh && !br_eof(&br); by++) {
        for (int bx = 0; bx < bw && !br_eof(&br); bx++) {
            int block[64] = {0}, spatial[64];

            /* DC */
            int dc_diff = vlc_magsign(&br);
            dc_pred += dc_diff;
            block[0] = dc_pred * qm[0] * qscale;

            /* AC: run/level pairs using magsign VLC */
            int idx = 1;
            bool block_ok = true;
            while (idx < 64 && !br_eof(&br)) {
                int run = vlc_magsign(&br);
                if (run < 0) { block_ok = false; break; } /* invalid */
                idx += run;
                if (idx >= 64) break; /* end of block */
                int level = vlc_magsign(&br);
                if (abs(level) > 127) { block_ok = false; break; }
                int pos = zigzag8[idx];
                block[pos] = level * qm[pos] * qscale;
                idx++;
            }
            if (block_ok) ok++; else fail++;

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

    printf("RunLvl %s: ok=%d fail=%d, %d/%d bits\n", tag, ok, fail, br.total, bslen*8);
    char path[256];
    snprintf(path, sizeof(path), OUT_DIR "runlvl_%s.pgm", tag);
    write_pgm(path, img, imgW, imgH);
    free(img);
}

/*
 * Method 4: Just render the raw bytes as an image
 * Sometimes the simplest approach reveals structure
 */
static void raw_byte_render(const uint8_t *bs, int bslen, const char *tag) {
    int widths[] = {128, 192, 256, 384};
    for (int wi = 0; wi < 4; wi++) {
        int W = widths[wi];
        int H = bslen / W;
        if (H < 2) continue;
        uint8_t *img = calloc(W * H, 1);
        memcpy(img, bs, W * H);
        char path[256];
        snprintf(path, sizeof(path), OUT_DIR "rawbytes_%s_w%d.pgm", tag, W);
        write_pgm(path, img, W, H);
        free(img);
    }
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

    int fi = 0;
    uint8_t *f = frames[fi];
    int fsize = fsizes[fi];
    int qscale = f[3];
    uint8_t qt[16];
    memcpy(qt, f+4, 16);
    printf("Frame %d: %d bytes, qscale=%d, type=%d\n", fi, fsize, qscale, f[39]);

    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;

    char tag[64];
    snprintf(tag, sizeof(tag), "%s_f%d", game, fi);

    /* Raw byte visualization first */
    raw_byte_render(bs, bslen, tag);

    /* Individual coefficient VLC: 3 VLC types × 128x144 */
    for (int vt = 0; vt < 3; vt++)
        try_indiv_vlc(bs, bslen, qscale, qt, tag, 128, 144, vt);

    /* Bitmap + VLC */
    try_bitmap_ac(bs, bslen, qscale, qt, tag, 128, 144);

    /* Run/Level with separate VLCs */
    try_run_level_vlc(bs, bslen, qscale, qt, tag, 128, 144);

    free(disc); zip_close(z);
    return 0;
}
