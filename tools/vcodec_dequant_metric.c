/*
 * vcodec_dequant_metric.c — Quantitative AC dequantization search
 *
 * Computes a blockiness metric for each dequantization formula:
 * measures discontinuity at 8×8 block boundaries vs smooth interior.
 * Lower blockiness = better formula.
 *
 * Also outputs PPM for the best-scoring formula.
 *
 * Usage: ./vcodec_dequant_metric <Track2.bin> [start_lba]
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

/* ── Bitstream ──────────────────────────────────────────────── */
typedef struct { const uint8_t *data; int total_bits, pos; } BS;

static int bs_peek(BS *bs, int n) {
    if (n <= 0 || bs->pos + n > bs->total_bits) return -1;
    int v = 0;
    for (int i = 0; i < n; i++) {
        int bp = bs->pos + i;
        v = (v << 1) | ((bs->data[bp >> 3] >> (7 - (bp & 7))) & 1);
    }
    return v;
}
static int bs_read(BS *bs, int n) {
    if (n <= 0) return 0;
    if (bs->pos + n > bs->total_bits) return -1;
    int v = 0;
    for (int i = 0; i < n; i++) {
        int bp = bs->pos + i;
        v = (v << 1) | ((bs->data[bp >> 3] >> (7 - (bp & 7))) & 1);
    }
    bs->pos += n; return v;
}
static int bs_bit(BS *bs) {
    if (bs->pos >= bs->total_bits) return -1;
    int bp = bs->pos++;
    return (bs->data[bp >> 3] >> (7 - (bp & 7))) & 1;
}

static const struct { int len; uint32_t code; } vlc_t[17] = {
    {3,0x4},{2,0x0},{2,0x1},{3,0x5},{3,0x6},
    {4,0xE},{5,0x1E},{6,0x3E},{7,0x7E},
    {8,0xFE},{9,0x1FE},{10,0x3FE},
    {11,0x7FE},{12,0xFFE},{13,0x1FFE},{14,0x3FFE},{15,0x7FFE}
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
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* ── Decode one frame ───────────────────────────────────────── */
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
                int bit = bs_bit(bs);
                if (bit < 0) { ok = 0; break; }
                if (bit == 1) break;
                run++;
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

/* ── IDCT with dequantization ───────────────────────────────── */
static void idct_dequant(const int raw[64], uint8_t out[8][8],
                          double ac_scale, const double *per_pos_scale) {
    /* Build dequantized matrix in spatial order */
    double matrix[8][8];
    memset(matrix, 0, sizeof(matrix));
    /* DC */
    matrix[0][0] = raw[0]; /* always scale ×1 */
    /* AC */
    for (int i = 1; i < 64; i++) {
        int row = zigzag[i] / 8;
        int col = zigzag[i] % 8;
        double v = raw[i];
        if (per_pos_scale)
            v *= per_pos_scale[zigzag[i]];
        else
            v *= ac_scale;
        matrix[row][col] = v;
    }

    double temp[8][8];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) {
            double sum = 0;
            for (int k = 0; k < 8; k++) {
                double c = (k == 0) ? (1.0 / sqrt(2.0)) : 1.0;
                sum += c * matrix[i][k] * cos((2 * j + 1) * k * PI / 16.0);
            }
            temp[i][j] = sum / 2.0;
        }
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) {
            double sum = 0;
            for (int k = 0; k < 8; k++) {
                double c = (k == 0) ? (1.0 / sqrt(2.0)) : 1.0;
                sum += c * temp[k][j] * cos((2 * i + 1) * k * PI / 16.0);
            }
            out[i][j] = (uint8_t)clamp8((int)round(sum / 2.0) + 128);
        }
}

/* ── Render to Y plane (luma only for blockiness analysis) ─── */
static void render_Y(int coeff[PD_NBLOCKS][64], int dc_count,
                      double ac_scale, const double *per_pos_scale,
                      uint8_t Y[PD_H][PD_W]) {
    memset(Y, 128, PD_H * PD_W);
    for (int i = 0; i < dc_count; i++) {
        int mb = i / 6, bl = i % 6;
        if (bl >= 4) continue; /* skip chroma for blockiness */
        int mx = mb % PD_MW, my = mb / PD_MW;
        uint8_t block[8][8];
        idct_dequant(coeff[i], block, ac_scale, per_pos_scale);
        int bx = mx * 16 + (bl & 1) * 8;
        int by = my * 16 + (bl >> 1) * 8;
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                if (by + r < PD_H && bx + c < PD_W)
                    Y[by + r][bx + c] = block[r][c];
    }
}

/* ── Render full YCbCr → RGB frame ──────────────────────────── */
static void render_rgb(int coeff[PD_NBLOCKS][64], int dc_count,
                        double ac_scale, const double *per_pos_scale,
                        uint8_t *rgb) {
    uint8_t Y[PD_H][PD_W];
    uint8_t Cb[PD_H/2][PD_W/2];
    uint8_t Cr[PD_H/2][PD_W/2];
    memset(Y, 128, sizeof(Y));
    memset(Cb, 128, sizeof(Cb));
    memset(Cr, 128, sizeof(Cr));

    for (int i = 0; i < dc_count; i++) {
        int mb = i / 6, bl = i % 6;
        int mx = mb % PD_MW, my = mb / PD_MW;
        uint8_t block[8][8];
        idct_dequant(coeff[i], block, ac_scale, per_pos_scale);

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
            int yv = Y[y][x];
            int cb = Cb[y/2][x/2] - 128;
            int cr = Cr[y/2][x/2] - 128;
            int idx = (y * PD_W + x) * 3;
            rgb[idx + 0] = (uint8_t)clamp8(yv + (int)(1.402 * cr));
            rgb[idx + 1] = (uint8_t)clamp8(yv - (int)(0.344 * cb + 0.714 * cr));
            rgb[idx + 2] = (uint8_t)clamp8(yv + (int)(1.772 * cb));
        }
}

/* ── Blockiness metric ──────────────────────────────────────── */
/* Measures average absolute difference at 8-pixel block boundaries
   vs average difference within blocks. Ratio > 1 = visible blocking.
   Lower boundary_diff = better. */
static double blockiness(const uint8_t Y[PD_H][PD_W]) {
    double boundary_sum = 0;
    int boundary_count = 0;
    double interior_sum = 0;
    int interior_count = 0;

    /* Horizontal boundaries (columns that are multiples of 8) */
    for (int y = 0; y < PD_H; y++)
        for (int x = 1; x < PD_W; x++) {
            double d = fabs((double)Y[y][x] - Y[y][x-1]);
            if (x % 8 == 0) { boundary_sum += d; boundary_count++; }
            else { interior_sum += d; interior_count++; }
        }
    /* Vertical boundaries */
    for (int y = 1; y < PD_H; y++)
        for (int x = 0; x < PD_W; x++) {
            double d = fabs((double)Y[y][x] - Y[y-1][x]);
            if (y % 8 == 0) { boundary_sum += d; boundary_count++; }
            else { interior_sum += d; interior_count++; }
        }

    double boundary_avg = boundary_count > 0 ? boundary_sum / boundary_count : 0;
    double interior_avg = interior_count > 0 ? interior_sum / interior_count : 1;
    return boundary_avg / interior_avg; /* ratio: 1.0 = no blocking */
}

/* Also compute PSNR-like metric (smoothness measure) */
/* Count clipped pixels (0 or 255) as a quality indicator */
static double clip_ratio(const uint8_t Y[PD_H][PD_W]) {
    int clipped = 0;
    for (int y = 0; y < PD_H; y++)
        for (int x = 0; x < PD_W; x++)
            if (Y[y][x] == 0 || Y[y][x] == 255) clipped++;
    return (double)clipped / (PD_W * PD_H);
}

/* ── Assemble video packets ─────────────────────────────────── */
static int assemble_packets(const uint8_t *disc, int total_sectors, int start_lba,
                             uint8_t packets[][MAX_FRAME], int sizes[], int max_packets) {
    int n = 0, pos = 0;
    bool in_frame = false;
    for (int lba = start_lba; lba < total_sectors && n < max_packets; lba++) {
        const uint8_t *sec = disc + (long)lba * SECTOR_RAW;
        if (sec[0] != 0x00 || sec[1] != 0xFF) continue;
        if (sec[15] != 2) continue;
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

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long disc_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *disc = malloc(disc_size);
    fread(disc, 1, disc_size, f);
    fclose(f);
    int total_sectors = (int)(disc_size / SECTOR_RAW);

    system("mkdir -p " OUT_DIR);

    static uint8_t packets[32][MAX_FRAME];
    int sizes[32];
    int npkt = assemble_packets(disc, total_sectors, start_lba, packets, sizes, 32);
    printf("Assembled %d video packets\n\n", npkt);

    static int frame_coeff[PD_NBLOCKS][64];

    /* Test formulas across multiple packets with different QS values */
    printf("%-6s %-3s %-5s | %-20s %-8s %-8s %-8s\n",
           "Pkt", "QS", "Type", "Formula", "Block", "Clip%%", "Score");
    printf("------|-----|------|----------------------|---------|---------|--------\n");

    for (int pi = 0; pi < npkt && pi < 12; pi++) {
        uint8_t *pkt = packets[pi];
        if (pkt[0] != 0x00 || pkt[1] != 0x80 || pkt[2] != 0x04) continue;

        int QS = pkt[3];
        uint8_t qt[16]; memcpy(qt, pkt + 4, 16);
        int frame_type = pkt[39];

        int data_end = sizes[pi];
        while (data_end > 40 && pkt[data_end - 1] == 0xFF) data_end--;

        BS bs = { pkt + 40, (data_end - 40) * 8, 0 };
        int dc_count = decode_frame(&bs, frame_coeff);
        if (dc_count < PD_NBLOCKS) continue;

        /* Build per-position scale arrays for qtable-based formulas */
        /* Map: qtable as 4×4 → expand each entry to 2×2 region of 8×8 */
        double qm_spatial[64]; /* indexed by spatial (row*8+col) position */
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                qm_spatial[r * 8 + c] = qt[(r / 2) * 4 + (c / 2)];

        /* Define test formulas */
        struct { const char *name; double ac_scale; double *per_pos; } tests[] = {
            {"none (×1)",       1.0,  NULL},
            {"AC / QS",         1.0 / QS,  NULL},
            {"AC / (QS/2)",     2.0 / QS,  NULL},
            {"AC / 8",          1.0 / 8.0,  NULL},
            {"AC / 4",          1.0 / 4.0,  NULL},
            {"AC / 16",         1.0 / 16.0, NULL},
            {"AC * 2",          2.0, NULL},
            {"AC * 4",          4.0, NULL},
            {"AC * QS",         (double)QS, NULL},
            {"AC * QS/8",       (double)QS / 8.0, NULL},
        };
        int ntests = sizeof(tests) / sizeof(tests[0]);

        /* Per-position formulas: need dynamic arrays */
        /* Formula: AC / qm[pos] */
        double pp_div_qt[64];
        for (int i = 0; i < 64; i++)
            pp_div_qt[i] = qm_spatial[i] > 0 ? 1.0 / qm_spatial[i] : 1.0;

        /* Formula: AC * qm[pos] / QS */
        double pp_qt_div_qs[64];
        for (int i = 0; i < 64; i++)
            pp_qt_div_qs[i] = qm_spatial[i] / QS;

        /* Formula: AC * QS / qm[pos] */
        double pp_qs_div_qt[64];
        for (int i = 0; i < 64; i++)
            pp_qs_div_qt[i] = qm_spatial[i] > 0 ? (double)QS / qm_spatial[i] : 1.0;

        /* Formula: AC * qm[pos] * QS / 16 (MPEG-1 intra) */
        double pp_mpeg1[64];
        for (int i = 0; i < 64; i++)
            pp_mpeg1[i] = qm_spatial[i] * QS / 16.0;

        /* Formula: AC / (qm[pos] * QS / 8) */
        double pp_div_qt_qs[64];
        for (int i = 0; i < 64; i++)
            pp_div_qt_qs[i] = qm_spatial[i] > 0 ? 8.0 / (qm_spatial[i] * QS) : 1.0;

        /* Formula: AC * 8 / (qm[pos] * QS) — same as above */
        /* Formula: AC / sqrt(QS) */
        /* Formula: AC * qm[pos] */
        double pp_mul_qt[64];
        for (int i = 0; i < 64; i++)
            pp_mul_qt[i] = qm_spatial[i];

        struct { const char *name; double *per_pos; } pp_tests[] = {
            {"AC / qt[pos]",        pp_div_qt},
            {"AC*qt[pos]/QS",       pp_qt_div_qs},
            {"AC*QS/qt[pos]",       pp_qs_div_qt},
            {"AC*qt*QS/16 (MPEG1)", pp_mpeg1},
            {"AC*8/(qt*QS)",        pp_div_qt_qs},
            {"AC * qt[pos]",        pp_mul_qt},
        };
        int npp = sizeof(pp_tests) / sizeof(pp_tests[0]);

        double best_score = 1e9;
        const char *best_name = "";
        double best_ac_scale = 1.0;
        double *best_pp = NULL;

        uint8_t Y[PD_H][PD_W];

        /* Test uniform scale formulas */
        for (int t = 0; t < ntests; t++) {
            render_Y(frame_coeff, dc_count, tests[t].ac_scale, NULL, Y);
            double blk = blockiness(Y);
            double clp = clip_ratio(Y);
            /* Score: blockiness + clip penalty. Lower = better */
            double score = blk + clp * 5.0;
            printf("%-6d %-3d %-5d | %-20s %7.3f  %7.1f%% %7.3f%s\n",
                   pi, QS, frame_type, tests[t].name,
                   blk, clp * 100, score, score < best_score ? " *" : "");
            if (score < best_score) {
                best_score = score;
                best_name = tests[t].name;
                best_ac_scale = tests[t].ac_scale;
                best_pp = NULL;
            }
        }

        /* Test per-position formulas */
        for (int t = 0; t < npp; t++) {
            render_Y(frame_coeff, dc_count, 1.0, pp_tests[t].per_pos, Y);
            double blk = blockiness(Y);
            double clp = clip_ratio(Y);
            double score = blk + clp * 5.0;
            printf("%-6d %-3d %-5d | %-20s %7.3f  %7.1f%% %7.3f%s\n",
                   pi, QS, frame_type, pp_tests[t].name,
                   blk, clp * 100, score, score < best_score ? " *" : "");
            if (score < best_score) {
                best_score = score;
                best_name = pp_tests[t].name;
                best_ac_scale = 1.0;
                best_pp = pp_tests[t].per_pos;
            }
        }

        printf("  >> BEST: %s (score=%.3f)\n\n", best_name, best_score);

        /* Output best formula as PPM */
        char path[512];
        uint8_t rgb[PD_W * PD_H * 3];
        render_rgb(frame_coeff, dc_count, best_ac_scale, best_pp, rgb);
        snprintf(path, sizeof(path), OUT_DIR "best_pkt%02d_qs%d.ppm", pi, QS);
        write_ppm(path, rgb, PD_W, PD_H);
        printf("  -> %s\n\n", path);

        /* Also output "none" for comparison */
        render_rgb(frame_coeff, dc_count, 1.0, NULL, rgb);
        snprintf(path, sizeof(path), OUT_DIR "raw_pkt%02d_qs%d.ppm", pi, QS);
        write_ppm(path, rgb, PD_W, PD_H);
    }

    /* Sweep: try AC / X for X = 1..64 to find optimal divisor per QS */
    printf("\n=== AC DIVISOR SWEEP ===\n");
    printf("%-6s %-3s | ", "Pkt", "QS");
    for (int d = 1; d <= 32; d++) printf("/%2d    ", d);
    printf("\n");

    for (int pi = 0; pi < npkt && pi < 12; pi++) {
        uint8_t *pkt = packets[pi];
        if (pkt[0] != 0x00 || pkt[1] != 0x80 || pkt[2] != 0x04) continue;
        int QS = pkt[3];
        int data_end = sizes[pi];
        while (data_end > 40 && pkt[data_end - 1] == 0xFF) data_end--;
        BS bs = { pkt + 40, (data_end - 40) * 8, 0 };
        int dc_count = decode_frame(&bs, frame_coeff);
        if (dc_count < PD_NBLOCKS) continue;

        uint8_t Y[PD_H][PD_W];
        printf("%-6d %-3d | ", pi, QS);
        double best = 1e9;
        int best_d = 1;
        for (int d = 1; d <= 32; d++) {
            render_Y(frame_coeff, dc_count, 1.0 / d, NULL, Y);
            double blk = blockiness(Y);
            printf("%6.3f ", blk);
            if (blk < best) { best = blk; best_d = d; }
        }
        printf(" best=/%d (%.3f)\n", best_d, best);
    }

    free(disc);
    return 0;
}
