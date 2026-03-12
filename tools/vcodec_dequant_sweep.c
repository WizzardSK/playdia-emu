/*
 * vcodec_dequant_sweep.c — Sweep formula: AC * QS * qt[mapping] / N
 *
 * Tests multiple:
 *  - Divisor N values (4, 8, 16, 32, 64)
 *  - QtTable-to-8×8 mappings (4×4 block expand, zigzag mod 16, band /4)
 *  - MPEG-1 variants (with/without factor of 2)
 *  - DC scale variants
 *
 * Also outputs the best PPM for each packet.
 *
 * Usage: ./vcodec_dequant_sweep <Track2.bin> [start_lba]
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
}

typedef struct { const uint8_t *data; int total_bits, pos; } BS;
static int bs_peek(BS *bs, int n) {
    if (n <= 0 || bs->pos + n > bs->total_bits) return -1;
    int v = 0;
    for (int i = 0; i < n; i++) { int bp = bs->pos + i; v = (v << 1) | ((bs->data[bp >> 3] >> (7 - (bp & 7))) & 1); }
    return v;
}
static int bs_read(BS *bs, int n) {
    if (n <= 0) return 0; if (bs->pos + n > bs->total_bits) return -1;
    int v = 0;
    for (int i = 0; i < n; i++) { int bp = bs->pos + i; v = (v << 1) | ((bs->data[bp >> 3] >> (7 - (bp & 7))) & 1); }
    bs->pos += n; return v;
}
static int bs_bit(BS *bs) {
    if (bs->pos >= bs->total_bits) return -1;
    int bp = bs->pos++; return (bs->data[bp >> 3] >> (7 - (bp & 7))) & 1;
}

static const struct { int len; uint32_t code; } vlc_t[17] = {
    {3,0x4},{2,0x0},{2,0x1},{3,0x5},{3,0x6},{4,0xE},{5,0x1E},{6,0x3E},{7,0x7E},
    {8,0xFE},{9,0x1FE},{10,0x3FE},{11,0x7FE},{12,0xFFE},{13,0x1FFE},{14,0x3FFE},{15,0x7FFE}
};
static int read_vlc(BS *bs) {
    for (int i = 0; i < 17; i++) {
        int bits = bs_peek(bs, vlc_t[i].len);
        if (bits < 0) continue;
        if (bits == (int)vlc_t[i].code) {
            bs->pos += vlc_t[i].len;
            if (i == 0) return 0;
            int val = bs_read(bs, i);
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

/* Inverse zigzag: given spatial position, what zigzag index? */
static int inv_zigzag[64];
static void init_inv_zigzag(void) {
    for (int i = 0; i < 64; i++) inv_zigzag[zigzag[i]] = i;
}

static int decode_frame(BS *bs, int coeff[PD_NBLOCKS][64]) {
    int dc_count = 0;
    memset(coeff, 0, sizeof(int) * PD_NBLOCKS * 64);
    int dc_pred[3] = {0, 0, 0};
    for (int mb = 0; mb < PD_MW * PD_MH && bs->pos < bs->total_bits; mb++)
        for (int bl = 0; bl < 6 && bs->pos < bs->total_bits; bl++) {
            int comp = (bl < 4) ? 0 : (bl == 4) ? 1 : 2;
            int diff = read_vlc(bs);
            if (diff == -9999) goto done;
            dc_pred[comp] += diff;
            coeff[dc_count][0] = dc_pred[comp];
            dc_count++;
        }
done:
    if (dc_count < 6) return 0;
    for (int b = 0; b < dc_count && bs->pos < bs->total_bits; b++) {
        int k = 1;
        while (k < 64 && bs->pos < bs->total_bits) {
            int peek = bs_peek(bs, 6);
            if (peek < 0) break;
            if (peek == 0) { bs->pos += 6; break; }
            int run = 0, ok = 1;
            while (run < 5 && bs->pos < bs->total_bits) {
                int bit = bs_bit(bs); if (bit < 0) { ok = 0; break; }
                if (bit == 1) break; run++;
            }
            if (!ok) break;
            int p3 = bs_peek(bs, 3);
            if (p3 == 4) { bs->pos += 3; break; }
            int level = read_vlc(bs);
            if (level == -9999) break;
            k += run;
            if (k < 64) coeff[b][k] = level;
            k++;
        }
    }
    return dc_count;
}

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* Build per-AC-position scale factor (indexed by zigzag position 0-63) */
typedef enum {
    MAP_4X4_EXPAND, /* 4×4 qtable, each entry covers 2×2 region */
    MAP_ZZ_MOD16,   /* qt[zigzag_index % 16] */
    MAP_ZZ_BAND,    /* qt[min(zigzag_index / 4, 15)] */
    MAP_ZZ_FIRST16, /* qt[zigzag_index] for 0-15, qt[15] for 16-63 */
    MAP_UNIFORM,    /* ignore qtable, use 1.0 everywhere */
    MAP_NUM
} QtMapping;

static const char *map_names[] = {"4x4exp", "zzmod16", "zzband", "zzfirst16", "uniform"};

static void build_scale(const uint8_t qt[16], QtMapping map, int QS,
                         double N, double scale[64]) {
    for (int zzi = 0; zzi < 64; zzi++) {
        int row = zigzag[zzi] / 8;
        int col = zigzag[zzi] % 8;
        double q;
        switch (map) {
        case MAP_4X4_EXPAND:
            q = qt[(row / 2) * 4 + (col / 2)];
            break;
        case MAP_ZZ_MOD16:
            q = qt[zzi % 16];
            break;
        case MAP_ZZ_BAND:
            q = qt[zzi / 4 < 16 ? zzi / 4 : 15];
            break;
        case MAP_ZZ_FIRST16:
            q = qt[zzi < 16 ? zzi : 15];
            break;
        case MAP_UNIFORM:
            q = 1.0;
            break;
        default: q = 1.0;
        }
        if (zzi == 0) {
            scale[zzi] = 1.0; /* DC always ×1 */
        } else {
            scale[zzi] = (q * QS) / N;
        }
    }
}

/* Render Y plane only (for blockiness analysis) */
static void render_Y(int coeff[PD_NBLOCKS][64], int dc_count,
                      const double scale[64], uint8_t Y[PD_H][PD_W]) {
    memset(Y, 128, PD_H * PD_W);
    for (int i = 0; i < dc_count; i++) {
        int mb = i / 6, bl = i % 6;
        if (bl >= 4) continue;
        int mx = mb % PD_MW, my = mb / PD_MW;

        /* Dequantize + IDCT */
        double matrix[8][8]; memset(matrix, 0, sizeof(matrix));
        for (int k = 0; k < 64; k++) {
            int row = zigzag[k] / 8, col = zigzag[k] % 8;
            matrix[row][col] = coeff[i][k] * scale[k];
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

        int bx = mx * 16 + (bl & 1) * 8;
        int by = my * 16 + (bl >> 1) * 8;
        for (int c = 0; c < 8; c++)
            for (int r = 0; r < 8; r++) {
                double sum = 0;
                for (int k = 0; k < 8; k++) {
                    double ck = (k == 0) ? (1.0 / sqrt(2.0)) : 1.0;
                    sum += ck * temp[k][c] * cos((2 * r + 1) * k * PI / 16.0);
                }
                int pix = (int)round(sum / 2.0) + 128;
                if (by + r < PD_H && bx + c < PD_W)
                    Y[by + r][bx + c] = clamp8(pix);
            }
    }
}

/* Full YCbCr render */
static void render_rgb(int coeff[PD_NBLOCKS][64], int dc_count,
                        const double scale[64], uint8_t *rgb) {
    uint8_t Y[PD_H][PD_W], Cb[PD_H/2][PD_W/2], Cr[PD_H/2][PD_W/2];
    memset(Y, 128, sizeof(Y));
    memset(Cb, 128, sizeof(Cb));
    memset(Cr, 128, sizeof(Cr));

    for (int i = 0; i < dc_count; i++) {
        int mb = i / 6, bl = i % 6;
        int mx = mb % PD_MW, my = mb / PD_MW;

        double matrix[8][8]; memset(matrix, 0, sizeof(matrix));
        for (int k = 0; k < 64; k++) {
            int row = zigzag[k] / 8, col = zigzag[k] % 8;
            matrix[row][col] = coeff[i][k] * scale[k];
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
            int bx = mx * 16 + (bl & 1) * 8;
            int by = my * 16 + (bl >> 1) * 8;
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

/* Blockiness metric */
static double blockiness(const uint8_t Y[PD_H][PD_W]) {
    double bnd = 0, inn = 0; int bc = 0, ic = 0;
    for (int y = 0; y < PD_H; y++)
        for (int x = 1; x < PD_W; x++) {
            double d = fabs((double)Y[y][x] - Y[y][x-1]);
            if (x % 8 == 0) { bnd += d; bc++; } else { inn += d; ic++; }
        }
    for (int y = 1; y < PD_H; y++)
        for (int x = 0; x < PD_W; x++) {
            double d = fabs((double)Y[y][x] - Y[y-1][x]);
            if (y % 8 == 0) { bnd += d; bc++; } else { inn += d; ic++; }
        }
    return (bc > 0 && ic > 0) ? (bnd / bc) / (inn / ic) : 999;
}

static double clip_ratio(const uint8_t Y[PD_H][PD_W]) {
    int c = 0;
    for (int y = 0; y < PD_H; y++)
        for (int x = 0; x < PD_W; x++)
            if (Y[y][x] == 0 || Y[y][x] == 255) c++;
    return (double)c / (PD_W * PD_H);
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
    if (argc < 2) { fprintf(stderr, "Usage: %s <Track2.bin> [start_lba]\n", argv[0]); return 1; }
    int start_lba = argc > 2 ? atoi(argv[2]) : 0;

    init_inv_zigzag();

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long disc_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *disc = malloc(disc_size);
    fread(disc, 1, disc_size, f); fclose(f);
    int total_sectors = (int)(disc_size / SECTOR_RAW);

    system("mkdir -p " OUT_DIR);

    static uint8_t packets[32][MAX_FRAME]; int sizes[32];
    int npkt = assemble_packets(disc, total_sectors, start_lba, packets, sizes, 32);
    printf("Assembled %d video packets\n\n", npkt);

    static int frame_coeff[PD_NBLOCKS][64];

    /* Divisor values to test */
    double divisors[] = {1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 128};
    int ndiv = sizeof(divisors) / sizeof(divisors[0]);

    printf("%-4s %-3s | ", "Pkt", "QS");
    for (int m = 0; m < MAP_NUM; m++)
        for (int d = 0; d < ndiv; d++)
            printf("%s/%-3.0f ", map_names[m], divisors[d]);
    printf("\n");

    /* Track overall best formula across all packets */
    double formula_score_sum[MAP_NUM * 64]; /* [map * ndiv + d] */
    int formula_count[MAP_NUM * 64];
    memset(formula_score_sum, 0, sizeof(formula_score_sum));
    memset(formula_count, 0, sizeof(formula_count));

    for (int pi = 0; pi < npkt && pi < 10; pi++) {
        uint8_t *pkt = packets[pi];
        if (pkt[0] != 0x00 || pkt[1] != 0x80 || pkt[2] != 0x04) continue;
        int QS = pkt[3]; uint8_t qt[16]; memcpy(qt, pkt + 4, 16);
        int data_end = sizes[pi];
        while (data_end > 40 && pkt[data_end - 1] == 0xFF) data_end--;
        BS bs = { pkt + 40, (data_end - 40) * 8, 0 };
        int dc_count = decode_frame(&bs, frame_coeff);
        if (dc_count < PD_NBLOCKS) continue;

        double best_score = 1e9;
        int best_map = 0, best_div = 0;
        double best_blk = 0, best_clip = 0;

        printf("\nPkt %-2d QS=%-2d: blockiness [clip%%] for each formula\n", pi, QS);

        for (int m = 0; m < MAP_NUM; m++) {
            printf("  %-10s: ", map_names[m]);
            for (int d = 0; d < ndiv; d++) {
                double scale[64];
                build_scale(qt, (QtMapping)m, QS, divisors[d], scale);

                uint8_t Y[PD_H][PD_W];
                render_Y(frame_coeff, dc_count, scale, Y);
                double blk = blockiness(Y);
                double clp = clip_ratio(Y);
                double score = blk + clp * 10.0; /* penalize clipping more */

                printf("%5.2f[%4.1f] ", blk, clp * 100);

                int fi = m * ndiv + d;
                formula_score_sum[fi] += score;
                formula_count[fi]++;

                if (score < best_score) {
                    best_score = score;
                    best_map = m;
                    best_div = d;
                    best_blk = blk;
                    best_clip = clp;
                }
            }
            printf("\n");
        }

        printf("  BEST: %s / %.0f (blk=%.2f, clip=%.1f%%, score=%.2f)\n",
               map_names[best_map], divisors[best_div], best_blk, best_clip * 100, best_score);

        /* Output best PPM */
        double scale[64];
        build_scale(qt, (QtMapping)best_map, QS, divisors[best_div], scale);
        uint8_t rgb[PD_W * PD_H * 3];
        render_rgb(frame_coeff, dc_count, scale, rgb);
        char path[512];
        snprintf(path, sizeof(path), OUT_DIR "sweep_best_pkt%02d_qs%d.ppm", pi, QS);
        write_ppm(path, rgb, PD_W, PD_H);
        printf("  -> %s\n", path);

        /* Also output with no dequant for comparison */
        double scale_none[64];
        for (int k = 0; k < 64; k++) scale_none[k] = 1.0;
        render_rgb(frame_coeff, dc_count, scale_none, rgb);
        snprintf(path, sizeof(path), OUT_DIR "sweep_none_pkt%02d_qs%d.ppm", pi, QS);
        write_ppm(path, rgb, PD_W, PD_H);
    }

    /* Overall rankings */
    printf("\n=== OVERALL FORMULA RANKINGS (lower = better) ===\n");
    typedef struct { double avg; int map; int div; } Rank;
    Rank ranks[MAP_NUM * 64];
    int nranks = 0;
    for (int m = 0; m < MAP_NUM; m++)
        for (int d = 0; d < ndiv; d++) {
            int fi = m * ndiv + d;
            if (formula_count[fi] > 0) {
                ranks[nranks].avg = formula_score_sum[fi] / formula_count[fi];
                ranks[nranks].map = m;
                ranks[nranks].div = d;
                nranks++;
            }
        }
    /* Sort by avg score */
    for (int i = 0; i < nranks - 1; i++)
        for (int j = i + 1; j < nranks; j++)
            if (ranks[j].avg < ranks[i].avg) {
                Rank tmp = ranks[i]; ranks[i] = ranks[j]; ranks[j] = tmp;
            }

    printf("%-4s %-10s %-6s %-8s\n", "Rank", "Mapping", "N", "AvgScore");
    for (int i = 0; i < nranks && i < 20; i++)
        printf("%-4d %-10s %-6.0f %8.3f\n",
               i + 1, map_names[ranks[i].map], divisors[ranks[i].div], ranks[i].avg);

    free(disc);
    return 0;
}
