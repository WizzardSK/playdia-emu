/*
 * vcodec_dequant_test.c — Test AC dequantization formulas
 *
 * Uses the CONFIRMED bitstream decoder (DC DPCM + unary-run AC + dual EOB)
 * and applies various dequantization formulas to find the correct one.
 *
 * Reads raw BIN disc images (Mode 2/2352).
 * Outputs PPM files for visual comparison.
 *
 * Usage: ./vcodec_dequant_test <Track2.bin> [start_lba]
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
#define PD_NBLOCKS  (PD_MW * PD_MH * 6)  /* 864 */
#define PI          3.14159265358979323846
#define OUT_DIR     "/home/wizzard/share/GitHub/playdia-emu/tools/test_output/"

/* ── PPM/PGM output ─────────────────────────────────────────── */
static void write_ppm(const char *p, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(p, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, w * h * 3, f); fclose(f);
    printf("  -> %s\n", p);
}

/* ── Bitstream reader ───────────────────────────────────────── */
typedef struct {
    const uint8_t *data;
    int total_bits;
    int pos;
} BS;

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
    bs->pos += n;
    return v;
}
static int bs_bit(BS *bs) {
    if (bs->pos >= bs->total_bits) return -1;
    int bp = bs->pos++;
    return (bs->data[bp >> 3] >> (7 - (bp & 7))) & 1;
}

/* ── VLC table (extended MPEG-1 DC, sizes 0-16) ────────────── */
static const struct { int len; uint32_t code; } vlc[17] = {
    {3,0x4}, {2,0x0}, {2,0x1}, {3,0x5}, {3,0x6},
    {4,0xE}, {5,0x1E}, {6,0x3E}, {7,0x7E},
    {8,0xFE}, {9,0x1FE}, {10,0x3FE},
    {11,0x7FE}, {12,0xFFE}, {13,0x1FFE}, {14,0x3FFE}, {15,0x7FFE}
};

static int read_vlc(BS *bs) {
    for (int i = 0; i < 17; i++) {
        int bits = bs_peek(bs, vlc[i].len);
        if (bits < 0) continue;
        if (bits == (int)vlc[i].code) {
            bs->pos += vlc[i].len;
            if (i == 0) return 0;
            int val = bs_read(bs, i);
            if (val < 0) return -9999;
            if (val < (1 << (i - 1)))
                val -= (1 << i) - 1;
            return val;
        }
    }
    return -9999;
}

/* ── Zigzag table ───────────────────────────────────────────── */
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

/* ── Decode one frame from bitstream (DC + AC) ──────────────── */
/* Returns: DC block count (864 on success) */
static int decode_frame(BS *bs, int coeff[PD_NBLOCKS][64]) {
    int dc_count = 0;
    memset(coeff, 0, sizeof(int) * PD_NBLOCKS * 64);

    /* DC section */
    int dc_pred[3] = {0, 0, 0};
    for (int mb = 0; mb < PD_MW * PD_MH && bs->pos < bs->total_bits; mb++) {
        for (int bl = 0; bl < 6 && bs->pos < bs->total_bits; bl++) {
            int comp = (bl < 4) ? 0 : (bl == 4) ? 1 : 2;
            int diff = read_vlc(bs);
            if (diff == -9999) goto dc_done;
            dc_pred[comp] += diff;
            coeff[dc_count][0] = dc_pred[comp];
            dc_count++;
        }
    }
dc_done:
    if (dc_count < 6) return 0;

    /* AC section */
    for (int b = 0; b < dc_count && bs->pos < bs->total_bits; b++) {
        int k = 1;
        while (k < 64 && bs->pos < bs->total_bits) {
            int peek = bs_peek(bs, 6);
            if (peek < 0) break;
            if (peek == 0) { bs->pos += 6; break; }

            int run = 0;
            int ok = 1;
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

/* ── IDCT ───────────────────────────────────────────────────── */
static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void idct_block(const double dequant[64], uint8_t out[8][8]) {
    double matrix[8][8];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            matrix[i][j] = dequant[i * 8 + j];

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

/* ── Assemble video packets from raw disc sectors ───────────── */
static int assemble_packets(const uint8_t *disc, int total_sectors, int start_lba,
                             uint8_t packets[][MAX_FRAME], int sizes[], int max_packets) {
    int n = 0, pos = 0;
    bool in_frame = false;

    for (int lba = start_lba; lba < total_sectors && n < max_packets; lba++) {
        const uint8_t *sec = disc + (long)lba * SECTOR_RAW;
        /* Validate sync pattern */
        if (sec[0] != 0x00 || sec[1] != 0xFF) continue;
        /* Mode 2 only */
        if (sec[15] != 2) continue;
        /* Skip audio sectors (submode bit 2) */
        if (sec[18] & 0x04) continue;
        /* Must be data sector (submode bit 3) */
        if (!(sec[18] & 0x08)) continue;

        uint8_t marker = sec[24];

        if (marker == 0xF1) {
            if (!in_frame) { in_frame = true; pos = 0; }
            int copy = 2047;
            if (pos + copy < MAX_FRAME) {
                memcpy(packets[n] + pos, sec + 25, copy);
                pos += copy;
            }
        } else if (marker == 0xF2) {
            /* Only regular frame-end markers (not interactive commands) */
            if (in_frame && pos > 0) {
                sizes[n] = pos;
                n++;
                in_frame = false;
                pos = 0;
            }
        } else if (marker == 0xF3) {
            in_frame = false;
            pos = 0;
        }
    }
    return n;
}

/* ── Build 8×8 quant matrix from 16-entry qtable ───────────── */
/* Method 1: 4×4 → 8×8 bilinear (each entry covers 2×2 region) */
static void qtable_to_8x8_block(const uint8_t qt[16], double qm[64]) {
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            qm[i * 8 + j] = qt[(i / 2) * 4 + (j / 2)];
}

/* ── Dequantization modes ───────────────────────────────────── */
typedef enum {
    DQ_NONE,            /* No dequant: coeff as-is */
    DQ_AC_DIV_QS,       /* AC / QS */
    DQ_AC_DIV_8,        /* AC / 8 */
    DQ_MPEG1_MUL,       /* AC * QS * qm[pos] / 16 (MPEG-1 intra) */
    DQ_MPEG1_MUL2,      /* AC * (2*QS+1) * qm[pos] / 32 (MPEG-1 non-intra) */
    DQ_AC_MUL_QT_DIV_QS,/* AC * qm[pos] / QS */
    DQ_AC_DIV_QT,       /* AC * QS / qm[pos] */
    DQ_AC_MUL_QS_DIV_QT,/* AC * QS / qm[pos] */
    DQ_AC_DIV_QS_QT,    /* AC / (QS * qm[pos] / 8) */
    DQ_AC_MOD16,        /* AC * qt[zigzag_idx % 16] */
    DQ_AC_BAND,         /* AC * qt[zigzag_idx / 4] */
    DQ_AC_MUL_QS,       /* AC * QS */
    DQ_AC_MUL_QS_DIV_8, /* AC * QS / 8 */
    DQ_JPEG_STYLE,      /* AC * qt[zigzag_idx % 16] (JPEG = multiply by qtable) */
    DQ_AC_DIV_QT_MUL_QS,/* AC / qm[pos] * QS */
    DQ_NUM_MODES
} DequantMode;

static const char *dq_names[] = {
    "none",           "ac_div_qs",      "ac_div_8",
    "mpeg1_mul",      "mpeg1_mul2",     "ac_mul_qt_div_qs",
    "ac_div_qt",      "ac_mul_qs_div_qt","ac_div_qs_qt",
    "ac_mod16",       "ac_band",        "ac_mul_qs",
    "ac_mul_qs_d8",   "jpeg_style",     "ac_div_qt_mul_qs"
};

static void apply_dequant(int raw_coeff[64], double dequant[64],
                           DequantMode mode, int QS, const uint8_t qt[16],
                           const double qm[64]) {
    /* DC is always scale ×1 */
    dequant[zigzag[0]] = raw_coeff[0];

    for (int i = 1; i < 64; i++) {
        int zpos = zigzag[i]; /* spatial position */
        double v = raw_coeff[i];
        double q = qm[zpos]; /* expanded qtable value at this spatial position */
        int qt_idx = i % 16; /* zigzag index mod 16 */
        int qt_band = i / 4; /* band index */
        if (qt_band > 15) qt_band = 15;

        switch (mode) {
        case DQ_NONE:
            break;
        case DQ_AC_DIV_QS:
            v = v / QS;
            break;
        case DQ_AC_DIV_8:
            v = v / 8.0;
            break;
        case DQ_MPEG1_MUL:
            v = v * QS * q / 16.0;
            break;
        case DQ_MPEG1_MUL2:
            v = v * (2.0 * QS + 1) * q / 32.0;
            break;
        case DQ_AC_MUL_QT_DIV_QS:
            v = v * q / QS;
            break;
        case DQ_AC_DIV_QT:
            v = (q > 0) ? v / q : v;
            break;
        case DQ_AC_MUL_QS_DIV_QT:
            v = (q > 0) ? v * QS / q : v;
            break;
        case DQ_AC_DIV_QS_QT:
            v = (q > 0) ? v * 8.0 / (QS * q) : v;
            break;
        case DQ_AC_MOD16:
            v = v * qt[qt_idx];
            break;
        case DQ_AC_BAND:
            v = v * qt[qt_band];
            break;
        case DQ_AC_MUL_QS:
            v = v * QS;
            break;
        case DQ_AC_MUL_QS_DIV_8:
            v = v * QS / 8.0;
            break;
        case DQ_JPEG_STYLE:
            v = v * qt[qt_idx];
            break;
        case DQ_AC_DIV_QT_MUL_QS:
            v = (q > 0) ? v * QS / q : v;
            break;
        default: break;
        }
        dequant[zpos] = v;
    }
}

/* ── Render frame with given dequantization ─────────────────── */
static void render_frame(int coeff[PD_NBLOCKS][64], int dc_count,
                          DequantMode mode, int QS, const uint8_t qt[16],
                          const char *out_path) {
    double qm[64];
    qtable_to_8x8_block(qt, qm);

    uint8_t Y[PD_H][PD_W];
    uint8_t Cb[PD_H / 2][PD_W / 2];
    uint8_t Cr[PD_H / 2][PD_W / 2];
    memset(Y, 128, sizeof(Y));
    memset(Cb, 128, sizeof(Cb));
    memset(Cr, 128, sizeof(Cr));

    /* Stats */
    double ac_sum = 0, ac_abs_sum = 0;
    int ac_count = 0, ac_max = 0;

    for (int i = 0; i < dc_count; i++) {
        int mb = i / 6, bl = i % 6;
        int mx = mb % PD_MW, my = mb / PD_MW;

        /* Collect AC stats (before dequant) */
        for (int j = 1; j < 64; j++) {
            if (coeff[i][j] != 0) {
                ac_sum += coeff[i][j];
                ac_abs_sum += abs(coeff[i][j]);
                ac_count++;
                if (abs(coeff[i][j]) > ac_max) ac_max = abs(coeff[i][j]);
            }
        }

        double dequant[64];
        apply_dequant(coeff[i], dequant, mode, QS, qt, qm);

        uint8_t block[8][8];
        idct_block(dequant, block);

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
                    if (my * 8 + r < PD_H / 2 && mx * 8 + c < PD_W / 2)
                        Cb[my * 8 + r][mx * 8 + c] = block[r][c];
        } else {
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++)
                    if (my * 8 + r < PD_H / 2 && mx * 8 + c < PD_W / 2)
                        Cr[my * 8 + r][mx * 8 + c] = block[r][c];
        }
    }

    /* YCbCr → RGB */
    uint8_t rgb[PD_H * PD_W * 3];
    for (int y = 0; y < PD_H; y++)
        for (int x = 0; x < PD_W; x++) {
            int yv = Y[y][x];
            int cb = Cb[y / 2][x / 2] - 128;
            int cr = Cr[y / 2][x / 2] - 128;
            int idx = (y * PD_W + x) * 3;
            rgb[idx + 0] = (uint8_t)clamp8(yv + (int)(1.402 * cr));
            rgb[idx + 1] = (uint8_t)clamp8(yv - (int)(0.344 * cb + 0.714 * cr));
            rgb[idx + 2] = (uint8_t)clamp8(yv + (int)(1.772 * cb));
        }

    write_ppm(out_path, rgb, PD_W, PD_H);

    if (ac_count > 0)
        printf("    AC stats: %d nonzero, avg=%.1f, avg_abs=%.1f, max=%d\n",
               ac_count, ac_sum / ac_count, ac_abs_sum / ac_count, ac_max);
}

/* ── Analyze coefficient statistics per zigzag position ─────── */
static void analyze_coefficients(int coeff[PD_NBLOCKS][64], int dc_count,
                                  int QS, const uint8_t qt[16]) {
    printf("\n  === Coefficient analysis (QS=%d) ===\n", QS);

    /* Per-zigzag-position stats */
    printf("  Zigzag pos | count | avg_abs |  max  | qt[%%16] | qt[/4]\n");
    printf("  -----------|-------|---------|-------|---------|-------\n");
    for (int zz = 0; zz < 16; zz++) {
        double sum_abs = 0;
        int count = 0, maxv = 0;
        for (int b = 0; b < dc_count; b++) {
            int v = coeff[b][zz];
            if (v != 0 || zz == 0) {
                sum_abs += abs(v);
                count++;
                if (abs(v) > maxv) maxv = abs(v);
            }
        }
        int qt_mod = qt[zz % 16];
        int qt_band = qt[zz / 4 < 16 ? zz / 4 : 15];
        printf("  %5d      | %5d | %7.1f | %5d | %4d    | %4d\n",
               zz, count, count > 0 ? sum_abs / count : 0.0, maxv,
               qt_mod, qt_band);
    }

    /* Check if dividing by QS normalizes across different QS values */
    double total_energy = 0;
    int total_ac = 0;
    for (int b = 0; b < dc_count; b++)
        for (int j = 1; j < 64; j++)
            if (coeff[b][j] != 0) {
                total_energy += (double)coeff[b][j] * coeff[b][j];
                total_ac++;
            }
    printf("\n  Total AC energy: %.0f, RMS: %.1f\n",
           total_energy, total_ac > 0 ? sqrt(total_energy / total_ac) : 0);
    printf("  AC_RMS / QS = %.1f\n",
           total_ac > 0 ? sqrt(total_energy / total_ac) / QS : 0);

    /* Test: what does AC_RMS / QS look like for different QS? */
    /* If it's roughly constant, then dividing by QS is correct */
}

/* ── Main ───────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <Track2.bin> [start_lba]\n", argv[0]);
        return 1;
    }

    int start_lba = argc > 2 ? atoi(argv[2]) : 0;

    /* Load disc image */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long disc_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *disc = malloc(disc_size);
    if (!disc) { fprintf(stderr, "OOM\n"); return 1; }
    fread(disc, 1, disc_size, f);
    fclose(f);

    int total_sectors = (int)(disc_size / SECTOR_RAW);
    printf("Disc: %ld bytes, %d sectors\n", disc_size, total_sectors);

    /* Ensure output directory exists */
    system("mkdir -p " OUT_DIR);

    /* Assemble video packets */
    static uint8_t packets[32][MAX_FRAME];
    int sizes[32];
    int npkt = assemble_packets(disc, total_sectors, start_lba, packets, sizes, 32);
    printf("Assembled %d video packets from LBA %d\n\n", npkt, start_lba);

    /* Process first few packets with different QS values */
    static int frame_coeff[PD_NBLOCKS][64];

    for (int pi = 0; pi < npkt && pi < 12; pi++) {
        uint8_t *pkt = packets[pi];
        int pkt_size = sizes[pi];

        if (pkt[0] != 0x00 || pkt[1] != 0x80 || pkt[2] != 0x04) {
            printf("Packet %d: invalid header, skipping\n", pi);
            continue;
        }

        int QS = pkt[3];
        uint8_t qt[16];
        memcpy(qt, pkt + 4, 16);
        int frame_type = pkt[39];

        printf("=== Packet %d: QS=%d, type=%d, size=%d ===\n",
               pi, QS, frame_type, pkt_size);
        printf("  QtTable: ");
        for (int i = 0; i < 16; i++) printf("%02X ", qt[i]);
        printf("\n");

        /* Strip trailing 0xFF padding */
        int data_end = pkt_size;
        while (data_end > 40 && pkt[data_end - 1] == 0xFF) data_end--;

        /* Decode first frame in this packet */
        BS bs = { pkt + 40, (data_end - 40) * 8, 0 };
        int dc_count = decode_frame(&bs, frame_coeff);
        printf("  Decoded: %d DC blocks, %d bits consumed (%.1f%%)\n",
               dc_count, bs.pos, 100.0 * bs.pos / bs.total_bits);

        if (dc_count < 6) {
            printf("  FAILED — skipping\n\n");
            continue;
        }

        /* Coefficient analysis */
        analyze_coefficients(frame_coeff, dc_count, QS, qt);

        /* Render with each dequantization mode */
        char path[512];
        for (int m = 0; m < DQ_NUM_MODES; m++) {
            snprintf(path, sizeof(path), OUT_DIR "pkt%02d_qs%d_%s.ppm",
                     pi, QS, dq_names[m]);
            printf("  Mode: %s\n", dq_names[m]);
            render_frame(frame_coeff, dc_count, (DequantMode)m, QS, qt, path);
        }
        printf("\n");
    }

    free(disc);
    return 0;
}
