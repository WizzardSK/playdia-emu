/*
 * Playdia video - 4×4 DCT hypothesis
 * The 16-entry qtable perfectly matches 4×4 block size (16 coefficients)
 * Try: MPEG-1 DC VLC + simple AC VLC for 4×4 blocks
 * Also try: treating the data as raw quantized coefficients
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
static int br_pos_bits(BR *b) { return b->pos*8 + (7-b->bit); }

/* MPEG-1 DC luminance VLC decode (Table B.12) */
static int mpeg1_dc_lum(BR *b) {
    int bit = br_get1(b);
    int size;
    if (bit == 0) {
        size = br_get1(b) ? 2 : 1;
    } else {
        bit = br_get1(b);
        if (bit == 0) { size = br_get1(b) ? 3 : 0; }
        else {
            bit = br_get1(b);
            if (bit == 0) size = 4;
            else { bit = br_get1(b);
                if (bit == 0) size = 5;
                else { bit = br_get1(b);
                    if (bit == 0) size = 6;
                    else { bit = br_get1(b);
                        if (bit == 0) size = 7;
                        else size = 8;
                    }
                }
            }
        }
    }
    if (size == 0) return 0;
    int val = br_get(b, size);
    if (val < (1 << (size-1))) val = val - (1 << size) + 1;
    return val;
}

/* MPEG-1 DC chrominance VLC (Table B.13) */
static int mpeg1_dc_chr(BR *b) {
    int bit = br_get1(b);
    int size;
    if (bit == 0) { size = br_get1(b) ? 1 : 0; }
    else {
        bit = br_get1(b);
        if (bit == 0) size = 2;
        else { bit = br_get1(b);
            if (bit == 0) size = 3;
            else { bit = br_get1(b);
                if (bit == 0) size = 4;
                else { bit = br_get1(b);
                    if (bit == 0) size = 5;
                    else { bit = br_get1(b);
                        if (bit == 0) size = 6;
                        else { bit = br_get1(b);
                            size = bit ? 8 : 7;
                        }
                    }
                }
            }
        }
    }
    if (size == 0) return 0;
    int val = br_get(b, size);
    if (val < (1 << (size-1))) val = val - (1 << size) + 1;
    return val;
}

/* Full MPEG-1 AC VLC Table B.14 - run/level pairs */
/* Returns: 1 on success (run/level set), 0 on EOB, -1 on error */
static int mpeg1_ac_vlc(BR *b, int *run, int *level) {
    /* Check for EOB: 10 */
    int bits = br_get1(b);
    if (bits == 1) {
        bits = br_get1(b);
        if (bits == 0) { /* 10 = EOB */ return 0; }
        /* 11s = run=0, level=1 */
        int s = br_get1(b);
        *run = 0; *level = s ? -1 : 1;
        return 1;
    }
    /* bits == 0 */
    bits = br_get1(b);
    if (bits == 1) {
        /* 01s = run=1, level=1 */
        int s = br_get1(b);
        *run = 1; *level = s ? -1 : 1;
        return 1;
    }
    /* 00... */
    bits = br_get1(b);
    if (bits == 1) {
        bits = br_get1(b);
        if (bits == 0) {
            /* 0010s = run=0, level=2 */
            int s = br_get1(b);
            *run = 0; *level = s ? -2 : 2;
            return 1;
        }
        /* 0011s = run=2, level=1 */
        int s = br_get1(b);
        *run = 2; *level = s ? -1 : 1;
        return 1;
    }
    /* 000... */
    bits = br_get1(b);
    if (bits == 1) {
        bits = br_get1(b);
        if (bits == 0) {
            bits = br_get1(b);
            if (bits == 0) {
                /* 000100s = run=0, level=3 */
                int s = br_get1(b);
                *run = 0; *level = s ? -3 : 3;
                return 1;
            }
            /* 000101s = run=4, level=1 */
            int s = br_get1(b);
            *run = 4; *level = s ? -1 : 1;
            return 1;
        }
        bits = br_get1(b);
        if (bits == 0) {
            /* 000110s = run=3, level=1 */
            int s = br_get1(b);
            *run = 3; *level = s ? -1 : 1;
            return 1;
        }
        /* 000111s = run=0, level=4 (escape check first) */
        /* Actually 0001 11s doesn't exist. Let me re-check the table. */
        /* Sorry, let me use a proper lookup. */
    }

    /* This is getting complex. Use escape code approach:
       0000 01 = escape, followed by 6-bit run + 8-bit level (or 16-bit if level==0 or -128)
    */

    /* For simplicity and correctness, let me use a table-driven approach */
    /* Reset position and use the full table */
    return -1; /* error for now */
}

/* 4x4 zigzag scan order */
static const int zigzag4[16] = {
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};

/* 4x4 IDCT */
static void idct4x4(int block[16], int out[16]) {
    double tmp[16];
    /* Row transform */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            double sum = 0;
            for (int k = 0; k < 4; k++) {
                double ck = (k == 0) ? 0.5 : sqrt(0.5) * cos((2*j+1)*k*PI/8.0);
                sum += block[i*4+k] * ck;
            }
            tmp[i*4+j] = sum;
        }
    }
    /* Column transform */
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            double sum = 0;
            for (int k = 0; k < 4; k++) {
                double ck = (k == 0) ? 0.5 : sqrt(0.5) * cos((2*i+1)*k*PI/8.0);
                sum += tmp[k*4+j] * ck;
            }
            out[i*4+j] = (int)round(sum);
        }
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
 * Method 1: Try treating bitstream as raw quantized 4x4 coefficients
 * Each block: read DC (e.g. 8 bits signed), then 15 AC as quantized values
 */
static void try_raw_4x4(const uint8_t *bs, int bslen, const uint8_t qt[16],
                         int qscale, const char *tag, int bw) {
    int blocks_total = bslen / 16;  /* 1 byte per coeff, 16 coeffs per block */
    int bh = blocks_total / bw;
    if (bh < 2) return;

    int W = bw * 4, H = bh * 4;
    uint8_t *img = calloc(W * H, 1);
    if (!img) return;

    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            int bi = by * bw + bx;
            const uint8_t *src = bs + bi * 16;

            /* Dequantize */
            int block[16], spatial[16];
            for (int i = 0; i < 16; i++) {
                int val = (int8_t)src[zigzag4[i]];
                block[i] = val * qt[i] * qscale / 8;
            }

            idct4x4(block, spatial);

            for (int dy = 0; dy < 4; dy++) {
                for (int dx = 0; dx < 4; dx++) {
                    int v = spatial[dy*4+dx] + 128;
                    if (v < 0) v = 0; if (v > 255) v = 255;
                    img[(by*4+dy)*W + bx*4+dx] = v;
                }
            }
        }
    }
    char path[256];
    snprintf(path, sizeof(path), OUT_DIR "raw4x4_%s_w%d.pgm", tag, bw);
    write_pgm(path, img, W, H);
    free(img);
}

/*
 * Method 2: Nibble-coded 4x4 blocks
 * Each coefficient encoded as a 4-bit nibble (signed: -7 to +7 or -8 to +7)
 * 16 nibbles per block = 8 bytes per block
 */
static void try_nibble_4x4(const uint8_t *bs, int bslen, const uint8_t qt[16],
                            int qscale, const char *tag, int bw) {
    int blocks_total = bslen / 8;  /* 8 bytes = 16 nibbles per block */
    int bh = blocks_total / bw;
    if (bh < 2) return;

    int W = bw * 4, H = bh * 4;
    uint8_t *img = calloc(W * H, 1);
    if (!img) return;

    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            int bi = by * bw + bx;
            const uint8_t *src = bs + bi * 8;

            int block[16], spatial[16];
            for (int i = 0; i < 16; i++) {
                int nib;
                if (i & 1) nib = src[i/2] & 0x0F;
                else nib = (src[i/2] >> 4) & 0x0F;
                /* sign extend 4-bit */
                if (nib >= 8) nib -= 16;
                block[zigzag4[i]] = nib * qt[i] * qscale / 8;
            }

            idct4x4(block, spatial);

            for (int dy = 0; dy < 4; dy++) {
                for (int dx = 0; dx < 4; dx++) {
                    int v = spatial[dy*4+dx] + 128;
                    if (v < 0) v = 0; if (v > 255) v = 255;
                    img[(by*4+dy)*W + bx*4+dx] = v;
                }
            }
        }
    }
    char path[256];
    snprintf(path, sizeof(path), OUT_DIR "nib4x4_%s_w%d.pgm", tag, bw);
    write_pgm(path, img, W, H);
    free(img);
}

/*
 * Method 3: VLC-coded 4x4 blocks with MPEG-1 DC + simple AC
 * DC: MPEG-1 DC luminance VLC
 * AC: try run-length with sign, or simple exp-golomb
 *
 * With row-based DC prediction reset
 */
static void try_vlc_4x4(const uint8_t *bs, int bslen, const uint8_t qt[16],
                         int qscale, const char *tag, int bw, int bh) {
    int total = bw * bh;
    int W = bw * 4, H = bh * 4;
    uint8_t *img = calloc(W * H, 1);
    if (!img) return;

    BR br;
    br_init(&br, bs, bslen);

    int dc_pred = 0;
    int ok = 0, fail = 0;

    for (int by = 0; by < bh && !br_eof(&br); by++) {
        dc_pred = 0;  /* reset per row */
        for (int bx = 0; bx < bw && !br_eof(&br); bx++) {
            int block[16] = {0};
            int spatial[16];

            /* DC */
            int dc_diff = mpeg1_dc_lum(&br);
            dc_pred += dc_diff;
            block[0] = dc_pred * qt[0] * qscale / 8;

            /* AC: try MPEG-1 style run/level */
            int idx = 1;
            bool block_ok = true;
            while (idx < 16 && !br_eof(&br)) {
                /* Check EOB: '10' */
                int save_pos = br.pos;
                int save_bit = br.bit;
                int save_total = br.total;

                int b1 = br_get1(&br);
                if (b1 == 1) {
                    int b2 = br_get1(&br);
                    if (b2 == 0) break; /* EOB */
                    /* 11s = run=0, level=1 */
                    int s = br_get1(&br);
                    if (idx < 16) {
                        int coeff = s ? -1 : 1;
                        block[zigzag4[idx]] = coeff * qt[idx] * qscale / 8;
                        idx++;
                    }
                    continue;
                }

                /* 0... */
                int b2 = br_get1(&br);
                if (b2 == 1) {
                    /* 01s = run=1, level=1 */
                    int s = br_get1(&br);
                    idx++; /* skip one */
                    if (idx < 16) {
                        int coeff = s ? -1 : 1;
                        block[zigzag4[idx]] = coeff * qt[idx] * qscale / 8;
                        idx++;
                    }
                    continue;
                }

                /* 00... */
                int b3 = br_get1(&br);
                if (b3 == 1) {
                    int b4 = br_get1(&br);
                    int s = br_get1(&br);
                    if (b4 == 0) {
                        /* 0010s = run=0, level=2 */
                        if (idx < 16) {
                            block[zigzag4[idx]] = (s ? -2 : 2) * qt[idx] * qscale / 8;
                            idx++;
                        }
                    } else {
                        /* 0011s = run=2, level=1 */
                        idx += 2;
                        if (idx < 16) {
                            block[zigzag4[idx]] = (s ? -1 : 1) * qt[idx] * qscale / 8;
                            idx++;
                        }
                    }
                    continue;
                }

                /* 000... */
                int b4 = br_get1(&br);
                if (b4 == 0) {
                    int b5 = br_get1(&br);
                    if (b5 == 1) {
                        /* 00001 = escape: 6-bit run + 8-bit signed level */
                        int run = br_get(&br, 6);
                        int lev = br_get(&br, 8);
                        if (lev == 0) lev = br_get(&br, 8);
                        else if (lev == 128) { lev = br_get(&br, 8) - 256; }
                        else if (lev > 128) lev -= 256;
                        idx += run;
                        if (idx < 16) {
                            block[zigzag4[idx]] = lev * qt[idx] * qscale / 8;
                            idx++;
                        }
                        continue;
                    }
                    /* 000 00... more VLC codes */
                    block_ok = false;
                    break;
                }

                /* 0001... */
                int b5 = br_get1(&br);
                int s = br_get1(&br);
                if (b5 == 0) {
                    int b6_saved = b5; /* 00010Xs */
                    int b6 = br_get1(&br);  /* extra bit to distinguish */
                    /* Approximate: skip this block */
                    block_ok = false;
                    break;
                } else {
                    /* 00011s = various */
                    block_ok = false;
                    break;
                }
            }

            if (block_ok) ok++; else fail++;

            idct4x4(block, spatial);
            for (int dy = 0; dy < 4; dy++) {
                for (int dx = 0; dx < 4; dx++) {
                    int v = spatial[dy*4+dx] + 128;
                    if (v < 0) v = 0; if (v > 255) v = 255;
                    img[(by*4+dy)*W + bx*4+dx] = v;
                }
            }
        }
    }
    printf("VLC 4x4 %s: ok=%d fail=%d, %d bits used\n", tag, ok, fail, br.total);
    char path[256];
    snprintf(path, sizeof(path), OUT_DIR "vlc4x4_%s_w%d.pgm", tag, bw);
    write_pgm(path, img, W, H);
    free(img);
}

/*
 * Method 4: Bit-level analysis of the first few hundred bits
 * Try to decode manually and identify the pattern
 */
static void bitstream_analysis(const uint8_t *bs, int bslen, const uint8_t qt[16]) {
    printf("\n=== BITSTREAM ANALYSIS ===\n");

    /* First 256 bits as binary */
    printf("First 256 bits:\n");
    for (int i = 0; i < 256 && i/8 < bslen; i++) {
        int bit = (bs[i/8] >> (7 - (i%8))) & 1;
        printf("%d", bit);
        if ((i & 63) == 63) printf("\n");
        else if ((i & 7) == 7) printf(" ");
    }
    printf("\n");

    /* Try to decode several MPEG-1 DC values and see how many bits each takes */
    printf("\nMPEG-1 DC lum decode attempt (first 20 values):\n");
    BR br;
    br_init(&br, bs, bslen);
    int prev = 0;
    for (int i = 0; i < 20 && !br_eof(&br); i++) {
        int before = br.total;
        int diff = mpeg1_dc_lum(&br);
        prev += diff;
        printf("  DC[%2d]: diff=%4d val=%4d (bits %d-%d, %d bits)\n",
               i, diff, prev, before, br.total-1, br.total - before);
    }

    /* Reset and try interpreting as: DC + 15 AC coefficients for first block */
    printf("\nTrying first 4x4 block (DC + 15 AC):\n");
    br_init(&br, bs, bslen);
    int dc_diff = mpeg1_dc_lum(&br);
    printf("DC: diff=%d (bits consumed: %d)\n", dc_diff, br.total);

    /* Now look at next bits - are they consistent with MPEG-1 AC VLC? */
    printf("Next 64 bits after DC:\n");
    int pos_after_dc = br_pos_bits(&br);
    for (int i = 0; i < 64; i++) {
        int bit = (bs[(pos_after_dc+i)/8] >> (7 - ((pos_after_dc+i)%8))) & 1;
        printf("%d", bit);
        if ((i & 7) == 7) printf(" ");
    }
    printf("\n");

    /* Try H.261 style: after DC, AC coefficients use TCOEFF VLC
     * In H.261, EOB is just '10', same as MPEG-1
     * The first coefficient after DC in intra block: '1s' = level±1 (no run)
     */

    /* Also try: what if after DC there's a fixed number of bits per AC coeff?
     * With qtable values 10-37, at qscale 4-10, the quantized levels
     * would be small. Maybe 3-5 bits per AC?
     */
    printf("\nTrying fixed-width AC (3-bit signed, 15 ACs per block):\n");
    br_init(&br, bs, bslen);
    for (int blk = 0; blk < 5; blk++) {
        /* DC: MPEG-1 VLC */
        int dc_bits_before = br.total;
        int diff = mpeg1_dc_lum(&br);
        printf("Block %d: DC diff=%d (%d bits), ACs: ", blk, diff, br.total - dc_bits_before);

        for (int i = 0; i < 15; i++) {
            int val = br_get(&br, 3);
            if (val >= 4) val -= 8;  /* sign extend 3-bit */
            printf("%d ", val);
        }
        printf("(%d total bits)\n", br.total);
    }

    printf("\nTrying fixed-width AC (4-bit signed, 15 ACs per block):\n");
    br_init(&br, bs, bslen);
    for (int blk = 0; blk < 5; blk++) {
        int dc_bits_before = br.total;
        int diff = mpeg1_dc_lum(&br);
        printf("Block %d: DC diff=%d (%d bits), ACs: ", blk, diff, br.total - dc_bits_before);

        for (int i = 0; i < 15; i++) {
            int val = br_get(&br, 4);
            if (val >= 8) val -= 16;
            printf("%d ", val);
        }
        printf("(%d total bits)\n", br.total);
    }

    /* Try: what if there's no VLC at all, and DC is also fixed width? */
    printf("\nTrying all-fixed 5-bit signed coefficients (16 per block):\n");
    br_init(&br, bs, bslen);
    for (int blk = 0; blk < 5; blk++) {
        printf("Block %d: ", blk);
        for (int i = 0; i < 16; i++) {
            int val = br_get(&br, 5);
            if (val >= 16) val -= 32;
            printf("%d ", val);
        }
        printf("\n");
    }

    /* Interesting test: what if the whole frame is just raw 4-bit pixels? */
    /* 12242 bytes × 2 nibbles = 24484 pixels. At 128 wide = 191 rows. */
    /* Or at 4 bits/pixel: 97936 / 4 = 24484 pixels = 128 × 191 */
    printf("\nRaw 4-bit pixel count: %d pixels at 128w = %d rows\n",
           bslen * 2, bslen * 2 / 128);

    /* What about 6-bit pixels? 97936/6 = 16322 pixels, 128 wide = 127 rows */
    printf("Raw 6-bit pixel count: %d pixels at 128w = %d rows\n",
           bslen * 8 / 6, bslen * 8 / 6 / 128);

    /* 8-bit raw: 12242 pixels at 128w = 95 rows */
    printf("Raw 8-bit pixel count: %d pixels at 128w = %d rows\n",
           bslen, bslen / 128);
}

/*
 * Method 5: Raw pixel dump at various bit depths
 */
static void try_raw_pixels(const uint8_t *bs, int bslen, const char *tag) {
    /* 4-bit pixels (16 levels) */
    {
        int npix = bslen * 2;
        int W = 128, H = npix / W;
        if (H > 1) {
            uint8_t *img = calloc(W * H, 1);
            BR br;
            br_init(&br, bs, bslen);
            for (int i = 0; i < W * H; i++) {
                int v = br_get(&br, 4);
                img[i] = v * 17;  /* scale 0-15 to 0-255 */
            }
            char path[256];
            snprintf(path, sizeof(path), OUT_DIR "raw4b_%s.pgm", tag);
            write_pgm(path, img, W, H);
            free(img);
        }
    }

    /* 4-bit pixels at 160 wide */
    {
        int npix = bslen * 2;
        int W = 160, H = npix / W;
        if (H > 1) {
            uint8_t *img = calloc(W * H, 1);
            BR br;
            br_init(&br, bs, bslen);
            for (int i = 0; i < W * H; i++) {
                int v = br_get(&br, 4);
                img[i] = v * 17;
            }
            char path[256];
            snprintf(path, sizeof(path), OUT_DIR "raw4b160_%s.pgm", tag);
            write_pgm(path, img, W, H);
            free(img);
        }
    }

    /* 6-bit pixels */
    {
        int npix = bslen * 8 / 6;
        int W = 128, H = npix / W;
        if (H > 1) {
            uint8_t *img = calloc(W * H, 1);
            BR br;
            br_init(&br, bs, bslen);
            for (int i = 0; i < W * H; i++) {
                int v = br_get(&br, 6);
                img[i] = v * 4;  /* scale 0-63 to 0-252 */
            }
            char path[256];
            snprintf(path, sizeof(path), OUT_DIR "raw6b_%s.pgm", tag);
            write_pgm(path, img, W, H);
            free(img);
        }
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
    printf("Assembled %d frames from %s\n", nf, argv[1]);

    for (int fi = 0; fi < nf && fi < 2; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        int qscale = f[3];
        uint8_t qt[16];
        memcpy(qt, f+4, 16);

        printf("\n=== Frame %d: %d bytes, qscale=%d, type=%d ===\n",
               fi, fsize, qscale, f[39]);

        printf("QTable: ");
        for (int i = 0; i < 16; i++) printf("%d ", qt[i]);
        printf("\n");

        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        /* Detailed bitstream analysis (first frame only) */
        if (fi == 0) bitstream_analysis(bs, bslen, qt);

        /* Try raw pixel dumps */
        char tag[64];
        snprintf(tag, sizeof(tag), "%s_f%d", game, fi);
        try_raw_pixels(bs, bslen, tag);

        /* Try raw 4x4 blocks at various widths */
        int widths[] = {32, 36, 40};
        for (int wi = 0; wi < 3; wi++)
            try_raw_4x4(bs, bslen, qt, qscale, tag, widths[wi]);

        /* Try nibble-coded 4x4 blocks */
        for (int wi = 0; wi < 3; wi++)
            try_nibble_4x4(bs, bslen, qt, qscale, tag, widths[wi]);

        /* Try VLC 4x4 */
        try_vlc_4x4(bs, bslen, qt, qscale, tag, 32, 36);
    }

    free(disc); zip_close(z);
    return 0;
}
