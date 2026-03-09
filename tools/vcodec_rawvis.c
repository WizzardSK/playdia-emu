/*
 * vcodec_rawvis.c - Raw VLC value visualization
 *
 * Strategy: Decode ALL VLC values as a flat stream, then render as:
 * 1. DPCM (accumulate) at various widths
 * 2. Direct values (centered at 128) at various widths
 * 3. Look for repeating patterns in the value stream
 *
 * Also test different frame sizes to see if 128x144 is correct.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct { const uint8_t *data; int len,pos,bit,total; } BR;
static void br_init(BR *b, const uint8_t *d, int l) { b->data=d;b->len=l;b->pos=0;b->bit=7;b->total=0; }
static int br_eof(BR *b) { return b->pos>=b->len; }
static int br_get1(BR *b) {
    if(b->pos>=b->len) return 0;
    int v=(b->data[b->pos]>>b->bit)&1;
    if(--b->bit<0){b->bit=7;b->pos++;}
    b->total++; return v;
}
static int br_get(BR *b, int n) {
    int v=0; for(int i=0;i<n;i++) v=(v<<1)|br_get1(b);
    return v;
}

static int vlc_coeff(BR *b) {
    int old_total = b->total;
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
    if (b->total == old_total + (b->pos >= b->len ? 0 : -1)) return 0x7FFF; // EOF sentinel
    if (size == 0) return 0;
    int val = br_get(b, size);
    if (val < (1 << (size-1))) val = val - (1 << size) + 1;
    return val;
}

/* Decode all VLC values from bitstream, stopping at true EOF */
static int decode_all_vlc(const uint8_t *data, int len, int *values, int maxvals) {
    BR br;
    br_init(&br, data, len);
    int count = 0;
    while (count < maxvals && !br_eof(&br)) {
        int old_total = br.total;
        int v = vlc_coeff(&br);
        if (br.total == old_total) break; // True EOF
        values[count++] = v;
    }
    return count;
}

static void save_pgm(const char *fn, uint8_t *img, int w, int h) {
    FILE *f = fopen(fn, "wb");
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(img, 1, w*h, f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <raw_frame_file> <tag>\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    uint8_t frame[12282];
    fread(frame, 1, 12282, f);
    fclose(f);

    const char *tag = argv[2];
    char path[256];

    uint8_t *bs = frame + 40;
    int bslen = 12242;

    /* Decode all VLC values */
    int *values = malloc(100000 * sizeof(int));
    int nvals = decode_all_vlc(bs, bslen, values, 100000);
    printf("Total VLC values: %d\n", nvals);

    /* Print first 200 values */
    printf("First 200 values:\n");
    for (int i = 0; i < 200 && i < nvals; i++) {
        printf("%4d", values[i]);
        if ((i+1) % 20 == 0) printf("\n");
    }
    printf("\n");

    /* Look for repeating patterns - check if there's periodicity */
    printf("\nPeriodicity analysis (autocorrelation of absolute values):\n");
    for (int period = 1; period <= 128; period++) {
        double corr = 0;
        int cnt = 0;
        for (int i = 0; i < nvals - period; i++) {
            corr += abs(values[i]) * abs(values[i + period]);
            cnt++;
        }
        corr /= cnt;
        if (period <= 20 || period == 32 || period == 48 || period == 64 ||
            period == 80 || period == 96 || period == 128)
            printf("  period %3d: %.2f\n", period, corr);
    }

    /* Check: every Nth value might be larger (DC?) */
    printf("\nAverage |value| at position mod N:\n");
    for (int N = 16; N <= 128; N *= 2) {
        printf("N=%d: ", N);
        for (int pos = 0; pos < N && pos < 8; pos++) {
            double sum = 0; int cnt = 0;
            for (int i = pos; i < nvals; i += N) {
                sum += abs(values[i]);
                cnt++;
            }
            printf("[%d]=%.1f ", pos, sum/cnt);
        }
        printf("...\n");
    }

    /* Test various widths for DPCM rendering */
    int widths[] = {128, 144, 160, 176, 192, 256, 320, 352, 64, 96, 80};
    int nwidths = sizeof(widths)/sizeof(widths[0]);

    for (int wi = 0; wi < nwidths; wi++) {
        int w = widths[wi];
        int h = nvals / w;
        if (h < 10) continue;

        uint8_t *img = calloc(w * h, 1);

        /* Direct values (centered at 128) */
        for (int i = 0; i < w * h; i++) {
            int v = values[i] + 128;
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            img[i] = v;
        }
        snprintf(path, sizeof(path), "test_output/raw_direct_%s_w%d.pgm", tag, w);
        save_pgm(path, img, w, h);

        /* DPCM with row reset */
        int acc = 128;
        for (int y = 0; y < h; y++) {
            acc = 128;
            for (int x = 0; x < w; x++) {
                acc += values[y * w + x];
                int v = acc;
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                img[y * w + x] = v;
            }
        }
        snprintf(path, sizeof(path), "test_output/raw_dpcm_%s_w%d.pgm", tag, w);
        save_pgm(path, img, w, h);

        /* DPCM continuous (no reset) */
        acc = 128;
        for (int i = 0; i < w * h; i++) {
            acc += values[i];
            int v = acc;
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            img[i] = v;
        }
        snprintf(path, sizeof(path), "test_output/raw_dpcmc_%s_w%d.pgm", tag, w);
        save_pgm(path, img, w, h);

        free(img);
    }

    /* Also: look at what happens every 64 values (8x8 block boundary) */
    printf("\nValue magnitude at block boundaries (every 64):\n");
    printf("Block#  val[0]  avg|ac|\n");
    for (int b = 0; b < 20 && b * 64 < nvals; b++) {
        double acsum = 0;
        for (int i = 1; i < 64 && b*64+i < nvals; i++)
            acsum += abs(values[b*64+i]);
        printf("  %3d   %5d   %.1f\n", b, values[b*64], acsum/63.0);
    }

    /* Look at value[0] of every potential block */
    printf("\nPotential DC values (first value every N values):\n");
    for (int N = 16; N <= 64; N += 16) {
        printf("Every %d: ", N);
        for (int i = 0; i < 20 && i*N < nvals; i++)
            printf("%d ", values[i*N]);
        printf("...\n");
    }

    free(values);
    return 0;
}
