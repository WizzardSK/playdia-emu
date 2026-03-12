/*
 * vcodec_compare2.c — Visual comparison of division-based dequant formulas
 * Generates comparison images: none vs /QS vs /8 vs clamp
 *
 * Usage: ./vcodec_compare2 <Track2.bin> [start_lba]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#define SECTOR_RAW  2352
#define MAX_FRAME   65536
#define PD_W        256
#define PD_H        144
#define PD_MW       (PD_W / 16)
#define PD_MH       (PD_H / 16)
#define PD_NBLOCKS  (PD_MW * PD_MH * 6)
#define PI          3.14159265358979323846
#define OUT_DIR     "/home/wizzard/share/GitHub/playdia-emu/tools/test_output/"

static void write_ppm(const char *p, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(p, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, w * h * 3, f); fclose(f);
    printf("-> %s\n", p);
}

typedef struct { const uint8_t *data; int total_bits, pos; } BS;
static int bs_peek(BS *b, int n) {
    if (n <= 0 || b->pos + n > b->total_bits) return -1;
    int v = 0;
    for (int i = 0; i < n; i++) { int bp = b->pos + i; v = (v << 1) | ((b->data[bp >> 3] >> (7 - (bp & 7))) & 1); }
    return v;
}
static int bs_read(BS *b, int n) {
    if (n <= 0) return 0; if (b->pos + n > b->total_bits) return -1;
    int v = 0;
    for (int i = 0; i < n; i++) { int bp = b->pos + i; v = (v << 1) | ((b->data[bp >> 3] >> (7 - (bp & 7))) & 1); }
    b->pos += n; return v;
}
static int bs_bit(BS *b) {
    if (b->pos >= b->total_bits) return -1;
    int bp = b->pos++; return (b->data[bp >> 3] >> (7 - (bp & 7))) & 1;
}
static const struct { int len; uint32_t code; } vlc_t[17] = {
    {3,0x4},{2,0x0},{2,0x1},{3,0x5},{3,0x6},{4,0xE},{5,0x1E},{6,0x3E},{7,0x7E},
    {8,0xFE},{9,0x1FE},{10,0x3FE},{11,0x7FE},{12,0xFFE},{13,0x1FFE},{14,0x3FFE},{15,0x7FFE}
};
static int read_vlc(BS *b) {
    for (int i = 0; i < 17; i++) {
        int bits = bs_peek(b, vlc_t[i].len);
        if (bits < 0) continue;
        if (bits == (int)vlc_t[i].code) {
            b->pos += vlc_t[i].len;
            if (i == 0) return 0;
            int val = bs_read(b, i);
            if (val < 0) return -9999;
            if (val < (1 << (i - 1))) val -= (1 << i) - 1;
            return val;
        }
    }
    return -9999;
}
static const int zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static int decode_frame(BS *b, int coeff[PD_NBLOCKS][64]) {
    int dc_count = 0;
    memset(coeff, 0, sizeof(int) * PD_NBLOCKS * 64);
    int dc_pred[3] = {0, 0, 0};
    for (int mb = 0; mb < PD_MW * PD_MH && b->pos < b->total_bits; mb++)
        for (int bl = 0; bl < 6 && b->pos < b->total_bits; bl++) {
            int comp = (bl < 4) ? 0 : (bl == 4) ? 1 : 2;
            int diff = read_vlc(b);
            if (diff == -9999) goto done;
            dc_pred[comp] += diff;
            coeff[dc_count][0] = dc_pred[comp];
            dc_count++;
        }
done:
    if (dc_count < 6) return 0;
    for (int bi = 0; bi < dc_count && b->pos < b->total_bits; bi++) {
        int k = 1;
        while (k < 64 && b->pos < b->total_bits) {
            int peek = bs_peek(b, 6);
            if (peek < 0) break;
            if (peek == 0) { b->pos += 6; break; }
            int run = 0, ok = 1;
            while (run < 5 && b->pos < b->total_bits) {
                int bit = bs_bit(b); if (bit < 0) { ok = 0; break; }
                if (bit == 1) break; run++;
            }
            if (!ok) break;
            int p3 = bs_peek(b, 3);
            if (p3 == 4) { b->pos += 3; break; }
            int level = read_vlc(b);
            if (level == -9999) break;
            k += run;
            if (k < 64) coeff[bi][k] = level;
            k++;
        }
    }
    return dc_count;
}

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

typedef enum {
    MODE_NONE,         /* No dequant */
    MODE_CLAMP_QS,     /* Clamp to ±2048/QS */
    MODE_CLAMP_128,    /* Clamp to ±128 */
    MODE_CLAMP_64,     /* Clamp to ±64 */
    MODE_NUM
} Mode;

static const char *mode_names[] = {"none", "clamp_qs", "clamp128", "clamp64"};

static void render(int coeff_in[PD_NBLOCKS][64], int dc_count, int QS,
                    Mode mode, uint8_t *rgb) {
    /* Copy coefficients so we don't modify the original */
    static int coeff[PD_NBLOCKS][64];
    memcpy(coeff, coeff_in, sizeof(int) * PD_NBLOCKS * 64);

    /* Apply mode */
    for (int b = 0; b < dc_count; b++) {
        int limit;
        switch (mode) {
        case MODE_CLAMP_QS:
            limit = QS > 0 ? 2048 / QS : 255;
            if (limit < 32) limit = 32;
            for (int i = 1; i < 64; i++) {
                if (coeff[b][i] > limit) coeff[b][i] = limit;
                else if (coeff[b][i] < -limit) coeff[b][i] = -limit;
            }
            break;
        case MODE_CLAMP_128:
            for (int i = 1; i < 64; i++) {
                if (coeff[b][i] > 128) coeff[b][i] = 128;
                else if (coeff[b][i] < -128) coeff[b][i] = -128;
            }
            break;
        case MODE_CLAMP_64:
            for (int i = 1; i < 64; i++) {
                if (coeff[b][i] > 64) coeff[b][i] = 64;
                else if (coeff[b][i] < -64) coeff[b][i] = -64;
            }
            break;
        default: break;
        }
    }

    uint8_t Y[PD_H][PD_W], Cb[PD_H/2][PD_W/2], Cr[PD_H/2][PD_W/2];
    memset(Y, 128, sizeof(Y)); memset(Cb, 128, sizeof(Cb)); memset(Cr, 128, sizeof(Cr));

    for (int i = 0; i < dc_count; i++) {
        int mb = i / 6, bl = i % 6;
        int mx = mb % PD_MW, my = mb / PD_MW;

        double matrix[8][8]; memset(matrix, 0, sizeof(matrix));
        for (int k = 0; k < 64; k++) {
            int row = zigzag[k] / 8, col = zigzag[k] % 8;
            matrix[row][col] = coeff[i][k];
        }

        double temp[8][8];
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++) {
                double sum = 0;
                for (int k = 0; k < 8; k++) {
                    double ck = (k == 0) ? (1.0 / sqrt(2.0)) : 1.0;
                    sum += ck * matrix[r][k] * cos((2 * c + 1) * k * PI / 16.0);
                }
                temp[r][c] = sum / 2.0;
            }
        uint8_t block[8][8];
        for (int c = 0; c < 8; c++)
            for (int r = 0; r < 8; r++) {
                double sum = 0;
                for (int k = 0; k < 8; k++) {
                    double ck = (k == 0) ? (1.0 / sqrt(2.0)) : 1.0;
                    sum += ck * temp[k][c] * cos((2 * r + 1) * k * PI / 16.0);
                }
                block[r][c] = clamp8((int)round(sum / 2.0) + 128);
            }

        if (bl < 4) {
            int bx = mx * 16 + (bl & 1) * 8, by = my * 16 + (bl >> 1) * 8;
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++)
                    if (by + r < PD_H && bx + c < PD_W)
                        Y[by + r][bx + c] = block[r][c];
        } else if (bl == 4) {
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++)
                    if (my*8+r < PD_H/2 && mx*8+c < PD_W/2)
                        Cb[my*8+r][mx*8+c] = block[r][c];
        } else {
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++)
                    if (my*8+r < PD_H/2 && mx*8+c < PD_W/2)
                        Cr[my*8+r][mx*8+c] = block[r][c];
        }
    }

    for (int y = 0; y < PD_H; y++)
        for (int x = 0; x < PD_W; x++) {
            int yv = Y[y][x], cb = Cb[y/2][x/2] - 128, cr = Cr[y/2][x/2] - 128;
            int idx = (y * PD_W + x) * 3;
            rgb[idx + 0] = clamp8(yv + (int)(1.402 * cr));
            rgb[idx + 1] = clamp8(yv - (int)(0.344 * cb + 0.714 * cr));
            rgb[idx + 2] = clamp8(yv + (int)(1.772 * cb));
        }
}

static int assemble_packets(const uint8_t *disc, int total_sectors, int start_lba,
                             uint8_t packets[][MAX_FRAME], int sizes[], int max_packets) {
    int n = 0, pos = 0; bool in_frame = false;
    for (int lba = start_lba; lba < total_sectors && n < max_packets; lba++) {
        const uint8_t *sec = disc + (long)lba * SECTOR_RAW;
        if (sec[0] != 0x00 || sec[1] != 0xFF || sec[15] != 2) continue;
        if (sec[18] & 0x04) continue;
        if (!(sec[18] & 0x08)) continue;
        uint8_t marker = sec[24];
        if (marker == 0xF1) {
            if (!in_frame) { in_frame = true; pos = 0; }
            if (pos + 2047 < MAX_FRAME) { memcpy(packets[n] + pos, sec + 25, 2047); pos += 2047; }
        } else if (marker == 0xF2) {
            if (in_frame && pos > 0) { sizes[n] = pos; n++; in_frame = false; pos = 0; }
        } else if (marker == 0xF3) { in_frame = false; pos = 0; }
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    int start_lba = argc > 2 ? atoi(argv[2]) : 0;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long disc_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *disc = malloc(disc_size);
    fread(disc, 1, disc_size, f); fclose(f);
    int total_sectors = (int)(disc_size / SECTOR_RAW);

    system("mkdir -p " OUT_DIR);

    static uint8_t packets[16][MAX_FRAME]; int sizes[16];
    int npkt = assemble_packets(disc, total_sectors, start_lba, packets, sizes, 16);

    static int frame_coeff[PD_NBLOCKS][64];

    /* Test a few representative packets */
    int test_pkts[] = {0, 4, 5, 7};
    int ntests = 4;

    for (int ti = 0; ti < ntests && test_pkts[ti] < npkt; ti++) {
        int pi = test_pkts[ti];
        uint8_t *pkt = packets[pi];
        if (pkt[0] != 0x00 || pkt[1] != 0x80 || pkt[2] != 0x04) continue;
        int QS = pkt[3]; uint8_t qt[16]; memcpy(qt, pkt + 4, 16);
        int data_end = sizes[pi];
        while (data_end > 40 && pkt[data_end - 1] == 0xFF) data_end--;
        BS bs = { pkt + 40, (data_end - 40) * 8, 0 };
        int dc_count = decode_frame(&bs, frame_coeff);
        if (dc_count < PD_NBLOCKS) continue;

        printf("Packet %d: QS=%d\n", pi, QS);

        /* Generate 4 horizontal comparison strips */
        static uint8_t rgb_strip[PD_W * MODE_NUM * PD_H * 3];
        memset(rgb_strip, 0, sizeof(rgb_strip));

        for (int m = 0; m < MODE_NUM; m++) {
            static uint8_t rgb[PD_W * PD_H * 3];
            render(frame_coeff, dc_count, QS, (Mode)m, rgb);

            /* Copy into strip */
            for (int y = 0; y < PD_H; y++)
                memcpy(rgb_strip + (y * PD_W * MODE_NUM + m * PD_W) * 3,
                       rgb + y * PD_W * 3, PD_W * 3);

            /* Also save individual */
            char path[512];
            snprintf(path, sizeof(path), OUT_DIR "v2_%s_pkt%02d_qs%d.ppm",
                     mode_names[m], pi, QS);
            write_ppm(path, rgb, PD_W, PD_H);
        }

        char path[512];
        snprintf(path, sizeof(path), OUT_DIR "v2_strip_pkt%02d_qs%d.ppm", pi, QS);
        write_ppm(path, rgb_strip, PD_W * MODE_NUM, PD_H);
        printf("\n");
    }

    free(disc);
    return 0;
}
