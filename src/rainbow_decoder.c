/* ============================================================
 *  rainbow_decoder.c — PC-FX RAINBOW algorithm experiment
 *
 *  This module is a C port of the MJPEG-like decoder from
 *  Mednafen's NEC PC-FX implementation (`src/pcfx/rainbow.cpp`),
 *  Copyright (C) 2006-2019 Mednafen Team, GPL-2.0+.  The original
 *  decoder algorithm and Huffman tables were provided by David
 *  Michel of MagicEngine and MagicEngine-FX.
 *
 *  Linking this file into the playdia-emu build effectively makes
 *  the resulting binary GPL-2.0+ (copyleft).  See `src/rainbow_*.inc`
 *  for the lookup tables.
 *
 *  Purpose: empirical test of whether the Playdia AK8000 bitstream
 *  decodes correctly with PC-FX RAINBOW's tables and decode loop.
 *  Both consoles are 1994 Japanese FMV systems; the AK8000 is by
 *  Asahi Kasei, RAINBOW (KING) is by NEC — different vendors but
 *  similar era and target.  If the same tables work, the AK8000
 *  likely licenses the same IP or follows the same David-Michel
 *  spec.  If not, we've ruled out one more candidate.
 *
 *  Output: writes 192×144 YCbCr to a host-provided RGB888 buffer.
 * ============================================================ */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "rainbow_decoder.h"

static inline double cos_table(double x) { return cos(x); }

/* ── Huffman quick-LUT pair (matches MAME/Mednafen layout) ── */
typedef struct { uint8_t val; uint8_t bitc; } HuffPair;

/* The four pre-computed tables imported verbatim from Mednafen. */
static const HuffPair ac_y_qlut [1 << 12] = {
#include "rainbow_acy.inc"
};
static const HuffPair ac_uv_qlut[1 << 12] = {
#include "rainbow_acuv.inc"
};
static const HuffPair dc_y_qlut [1 << 9 ] = {
#include "rainbow_dcy.inc"
};
static const HuffPair dc_uv_qlut[1 << 8 ] = {
#include "rainbow_dcuv.inc"
};

/* MJPEG-style zigzag scan order (PC-FX uses 63 entries — AC only;
 * DC is at position 0).                                            */
static const uint8_t rbw_zigzag[63] = {
    0x01, 0x08, 0x10, 0x09, 0x02, 0x03, 0x0A, 0x11,
    0x18, 0x20, 0x19, 0x12, 0x0B, 0x04, 0x05, 0x0C,
    0x13, 0x1A, 0x21, 0x28, 0x30, 0x29, 0x22, 0x1B,
    0x14, 0x0D, 0x06, 0x07, 0x0E, 0x15, 0x1C, 0x23,
    0x2A, 0x31, 0x38, 0x39, 0x32, 0x2B, 0x24, 0x1D,
    0x16, 0x0F, 0x17, 0x1E, 0x25, 0x2C, 0x33, 0x3A,
    0x3B, 0x34, 0x2D, 0x26, 0x1F, 0x27, 0x2E, 0x35,
    0x3C, 0x3D, 0x36, 0x2F, 0x37, 0x3E, 0x3F
};

/* ── bitstream reader ──────────────────────────────────────
 *  Adapted from Mednafen's InitBits/GetBits/SkipBits, sans the
 *  PC-FX byte-stuffing (0xFF) handling — we feed raw Playdia
 *  bytes since their bitstream is not byte-stuffed (verified by
 *  the existing ak8000.c decode).                                */
typedef struct {
    const uint8_t *buf;
    int            bytes_left;
    uint32_t       bits_buffer;
    int            bits_buffered;
} rbw_bits;

static void rbw_init_bits(rbw_bits *b, const uint8_t *buf, int bytes) {
    b->buf = buf;
    b->bytes_left = bytes;
    b->bits_buffer = 0;
    b->bits_buffered = 0;
}

static uint8_t rbw_fetch_byte(rbw_bits *b) {
    if (b->bytes_left <= 0) return 0;
    uint8_t v = *b->buf++;
    b->bytes_left--;
    return v;
}

static uint32_t rbw_get_bits(rbw_bits *b, int count, bool peek, bool funny) {
    while (b->bits_buffered < count) {
        b->bits_buffer <<= 8;
        b->bits_buffer |= rbw_fetch_byte(b);
        b->bits_buffered += 8;
    }
    uint32_t r = (b->bits_buffer >> (b->bits_buffered - count)) &
                 ((1u << count) - 1u);
    if (!peek) b->bits_buffered -= count;
    if (funny && count > 0) {
        if (r < (1u << (count - 1)))
            r += 1u - (1u << count);   /* sign-extend negative   */
    }
    return r;
}
static void rbw_skip_bits(rbw_bits *b, int count) { b->bits_buffered -= count; }

/* ── AC coefficient decode ─────────────────────────────────
 *  Returns: magnitude (signed via FUNNYSIGN) and *zeroes count.   */
static int32_t rbw_get_ac(rbw_bits *b, const HuffPair *table, int32_t *zeroes) {
    uint32_t rawbits = rbw_get_bits(b, 12, true, false);
    uint8_t  code    = table[rawbits].val;
    rbw_skip_bits(b, table[rawbits].bitc);
    int numbits = code & 0xF;
    *zeroes = code >> 4;
    return (int32_t)rbw_get_bits(b, numbits, false, true);
}

/* ── DC luma coefficient decode (9-bit lookup + nested AC) ── */
static int32_t rbw_get_dc_y(rbw_bits *b, int32_t *zeroes,
                            uint32_t qbase[2][64], uint32_t q[2][64]);

/* DC chroma — 8-bit lookup, no nesting */
static int32_t rbw_get_dc_uv(rbw_bits *b) {
    uint32_t rawbits = rbw_get_bits(b, 8, true, false);
    uint8_t  code    = dc_uv_qlut[rawbits].val;
    rbw_skip_bits(b, dc_uv_qlut[rawbits].bitc);
    return (int32_t)rbw_get_bits(b, code, false, true);
}

static int32_t rbw_get_dc_y(rbw_bits *b, int32_t *zeroes,
                            uint32_t qbase[2][64], uint32_t q[2][64]) {
    for (;;) {
        uint32_t rawbits = rbw_get_bits(b, 9, true, false);
        uint8_t  code    = dc_y_qlut[rawbits].val;
        rbw_skip_bits(b, dc_y_qlut[rawbits].bitc);

        if (code < 0xF) {
            *zeroes = 0;
            return (int32_t)rbw_get_bits(b, code, false, true);
        } else if (code == 0xF) {
            /* RLE for blank columns: read AC code, increment zeroes. */
            rbw_get_ac(b, ac_y_qlut, zeroes);
            (*zeroes)++;
            return 0;
        } else if (code >= 0x10) {
            /* Quantization-table update marker.  code is a multiplier;
             * recompute the active QuantTables from QuantTablesBase. */
            int mul = code - 0x10;
            for (int i = 0; i < 64; i++) {
                uint32_t coeff = (qbase[0][i] * (uint32_t)mul) >> 2;
                if (coeff < 1) coeff = 1; else if (coeff > 0xFE) coeff = 0xFE;
                q[0][i] = coeff;
                if (i) coeff = (qbase[1][i] * (uint32_t)mul) >> 2;
                else   coeff = (qbase[1][i]) >> 2;
                if (coeff < 1) coeff = 1; else if (coeff > 0xFE) coeff = 0xFE;
                q[1][i] = coeff;
            }
            /* loop: next iteration reads another DC code */
        }
    }
}

/* ── 8×8 inverse DCT (floating point) ──────────────────────
 *  Cheap reference IDCT — sufficient for an experiment.  Replace
 *  with the integer IDCT from ak8000.c later if results warrant.   */
static void rbw_idct8(int32_t coeff[64]) {
    static double c[8][8];
    static int init = 0;
    if (!init) {
        for (int k = 0; k < 8; k++)
            for (int n = 0; n < 8; n++)
                c[k][n] = (k == 0 ? 0.5 / 1.4142135624 : 0.5) *
                          cos_table(((2 * n + 1) * k * 3.141592653589793) / 16.0);
        init = 1;
    }
    double tmp[64];
    /* row pass */
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            double s = 0;
            for (int k = 0; k < 8; k++)
                s += coeff[i * 8 + k] * c[k][j];
            tmp[i * 8 + j] = s;
        }
    }
    /* column pass */
    for (int j = 0; j < 8; j++) {
        for (int i = 0; i < 8; i++) {
            double s = 0;
            for (int k = 0; k < 8; k++)
                s += tmp[k * 8 + j] * c[k][i];
            coeff[i * 8 + j] = (int32_t)s;
        }
    }
}

/* ── single-block decode helper ──────────────────────────── */
static void rbw_decode_block(rbw_bits *b, int32_t dct[64], const uint32_t qt[64],
                             int32_t dc, const HuffPair *ac_table) {
    memset(dct, 0, sizeof(int32_t) * 64);
    dct[0] = (int32_t)((int16_t)(qt[0] * (uint32_t)dc));

    int count = 0;
    while (count < 63) {
        int32_t zeroes;
        int32_t coeff = rbw_get_ac(b, ac_table, &zeroes);
        if (!coeff) {
            if (!zeroes) {
                /* genuine EOB */
                break;
            } else if (zeroes == 1) {
                zeroes = 0xF;          /* RLE: 15 zeros */
            }
        }
        while (zeroes-- && count < 63) {
            dct[rbw_zigzag[count++]] = 0;
        }
        if (count < 63) {
            int idx = rbw_zigzag[count++];
            dct[idx] = (int32_t)((int16_t)(qt[idx] * (uint32_t)coeff));
        }
    }
    rbw_idct8(dct);
}

/* ── YCbCr → RGB888 (BT.601, with +128 level shift) ────────── */
static inline int rbw_clip(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static void rbw_yuv2rgb(int y, int cb, int cr, uint8_t out[3]) {
    int C  = y;                          /* already includes +128 shift */
    int D  = cb - 128;
    int E  = cr - 128;
    out[0] = rbw_clip((298 * C           + 409 * E + 128) >> 8);  /* R */
    out[1] = rbw_clip((298 * C - 100 * D - 208 * E + 128) >> 8);  /* G */
    out[2] = rbw_clip((298 * C + 516 * D           + 128) >> 8);  /* B */
}

/* ── public entry: decode a Playdia packet bitstream with RAINBOW
 *  semantics.  Returns true on success.                          */
bool rainbow_decode_frame(const uint8_t *bitstream, int bytes,
                          const uint8_t qtable[16], int qscale,
                          uint8_t *rgb_out, int out_stride,
                          int width, int height) {
    rbw_bits b;
    rbw_init_bits(&b, bitstream, bytes);

    /* Seed both quantization tables.  PC-FX RAINBOW carries a 128-
     * byte qtable in the F8/FF frame headers; AK8000 only gives us
     * 16 bytes.  Replicate the AK8000 4×4 qtable as the base for
     * the 8×8 grid (`qt[i*8+j] = qtable[(i/2)*4 + (j/2)]`) the same
     * way the existing decoder does.                              */
    uint32_t qbase[2][64], qt[2][64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) {
            uint32_t v = qtable[(i / 2) * 4 + (j / 2)];
            qbase[0][i * 8 + j] = qt[0][i * 8 + j] = v;
            qbase[1][i * 8 + j] = qt[1][i * 8 + j] = v;
        }
    /* Apply qscale once up-front (RAINBOW uses a per-frame multiplier
     * via the code 0x10+ marker; AK8000 supplies it in byte [3]).   */
    (void)qscale;   /* TODO: factor in if results look promising */

    int mb_cols = width  / 16;
    int mb_rows = height / 16;

    int32_t dc_y = 0, dc_u = 0, dc_v = 0;
    int32_t dct_y[4][64];
    int32_t dct_u[64], dct_v[64];

    for (int mb_y = 0; mb_y < mb_rows; mb_y++) {
        for (int mb_x = 0; mb_x < mb_cols; mb_x++) {
            int32_t zeroes = 0;

            /* Read first Y DC, may carry quant-table update */
            dc_y += rbw_get_dc_y(&b, &zeroes, qbase, qt);
            if (zeroes) {
                /* fill `zeroes` MBs with the "null run" color */
                int skipped = 0;
                while (zeroes-- && mb_x + skipped < mb_cols) {
                    for (int y = 0; y < 16; y++) {
                        uint8_t *row = rgb_out +
                                       (mb_y * 16 + y) * out_stride +
                                       (mb_x + skipped) * 16 * 3;
                        for (int x = 0; x < 16; x++) {
                            row[x * 3 + 0] = 0;
                            row[x * 3 + 1] = 0;
                            row[x * 3 + 2] = 0;
                        }
                    }
                    skipped++;
                }
                mb_x += skipped - 1;
                dc_y = dc_u = dc_v = 0;
                continue;
            }

            /* Y[0] = TL (A) */
            rbw_decode_block(&b, dct_y[0], qt[0], dc_y, ac_y_qlut);
            /* Y[1] = BL (B) — RAINBOW order, NOT TL/TR/BL/BR */
            dc_y += rbw_get_dc_y(&b, &zeroes, qbase, qt);
            rbw_decode_block(&b, dct_y[1], qt[0], dc_y, ac_y_qlut);
            /* Y[2] = TR (C) */
            dc_y += rbw_get_dc_y(&b, &zeroes, qbase, qt);
            rbw_decode_block(&b, dct_y[2], qt[0], dc_y, ac_y_qlut);
            /* Y[3] = BR (D) */
            dc_y += rbw_get_dc_y(&b, &zeroes, qbase, qt);
            rbw_decode_block(&b, dct_y[3], qt[0], dc_y, ac_y_qlut);

            /* U/V 8×8 each, DPCM */
            dc_u += rbw_get_dc_uv(&b);
            rbw_decode_block(&b, dct_u, qt[1], dc_u, ac_uv_qlut);
            dc_v += rbw_get_dc_uv(&b);
            rbw_decode_block(&b, dct_v, qt[1], dc_v, ac_uv_qlut);

            /* Compose 16×16 MB into the output frame buffer.
             * Y layout: TL → (0,0), BL → (0,8), TR → (8,0), BR → (8,8) */
            for (int y = 0; y < 16; y++) {
                uint8_t *row = rgb_out + (mb_y * 16 + y) * out_stride +
                               mb_x * 16 * 3;
                for (int x = 0; x < 16; x++) {
                    int sub_block;     /* 0=A=TL, 1=B=BL, 2=C=TR, 3=D=BR */
                    int yy = y & 7;
                    int xx = x & 7;
                    if (x < 8)  sub_block = (y < 8) ? 0 : 1;
                    else        sub_block = (y < 8) ? 2 : 3;
                    int yval = dct_y[sub_block][yy * 8 + xx] + 0x80;
                    /* chroma 4:2:0 lookup */
                    int cv = (y >> 1) * 8 + (x >> 1);
                    int cb = dct_u[cv] + 0x80;
                    int cr = dct_v[cv] + 0x80;
                    rbw_yuv2rgb(rbw_clip(yval), rbw_clip(cb),
                                rbw_clip(cr), &row[x * 3]);
                }
            }
        }
    }

    return true;
}
