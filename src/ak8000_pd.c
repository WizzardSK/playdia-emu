// ─────────────────────────────────────────────────────────────
//  AK8000 picture decoder — recovered 4×4 DCT syntax.
//
//  Ported from PlaydiaEmu (https://github.com/AloysHF/PlaydiaEmu),
//  crates/playdiaemu-core/src/video/{ak8000,structure}.rs.
//  Copyright (c) PlaydiaEmu contributors.
//  Licensed under the BSD 3-Clause License; see LICENSE-PlaydiaEmu.
// ─────────────────────────────────────────────────────────────

#include "ak8000_pd.h"

#include <string.h>

// ── Bit reader ───────────────────────────────────────────────

typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;   // bit offset
    int            row;
    int            block;
    pd_status      status;
} PdReader;

// Big-endian bit pull, MSB first, as the hardware bitstream is laid out.
static int pd_read_bits(const uint8_t *data, size_t len, size_t offset,
                        unsigned count, uint32_t *out)
{
    if (count > 32 || offset + count > len * 8) return 0;
    uint32_t value = 0;
    for (size_t pos = offset; pos < offset + count; pos++)
        value = (value << 1) | ((data[pos / 8] >> (7 - pos % 8)) & 1);
    *out = value;
    return 1;
}

static pd_result pd_fail(const PdReader *r, pd_status s)
{
    pd_result res = { s, (int)r->pos, r->row, r->block };
    return res;
}

// Returns 0 and sets r->status on failure.
static int pd_read(PdReader *r, unsigned n, uint32_t *out)
{
    if (!pd_read_bits(r->data, r->len, r->pos, n, out)) {
        r->status = PD_ERR_TRUNCATED;
        return 0;
    }
    r->pos += n;
    return 1;
}

// ── Coefficient VLC ──────────────────────────────────────────
//
//  Codewords inferred from disc row and coefficient boundaries.
//  Two codes are handled outside the table: 0b01 (2 bits) ends the
//  block, and 0b00001000 (6 bits) is the escape that carries a 4-bit
//  run and a 10-bit signed level.

typedef struct {
    uint8_t bits;   // 0 = no code
    uint8_t run;
    int16_t level;
} PdEntry;

static const struct { uint16_t code; uint8_t bits; uint8_t run; int16_t level; }
PD_COEFFICIENT_CODES[] = {
    { 0x0010, 13, 0, 25 }, { 0x0011, 13, 5,  4 }, { 0x0012, 13, 0, 24 },
    { 0x0013, 13, 0, 23 }, { 0x0014, 13, 3,  8 }, { 0x0015, 13, 3,  7 },
    { 0x0016, 13, 3,  6 }, { 0x0017, 13, 2,  8 }, { 0x0018, 13, 2,  7 },
    { 0x0019, 13, 2,  6 }, { 0x001A, 13, 1,  9 }, { 0x001B, 13, 1,  8 },
    { 0x001C, 13, 1,  7 }, { 0x001D, 13, 0, 22 }, { 0x001E, 13, 0, 21 },
    { 0x001F, 13, 0, 20 },
    { 0x0010, 12, 9,  1 }, { 0x0011, 12, 8,  1 }, { 0x0012, 12, 7,  1 },
    { 0x0013, 12, 5,  3 }, { 0x0014, 12, 5,  2 }, { 0x0015, 12, 4,  5 },
    { 0x0016, 12, 4,  4 }, { 0x0017, 12, 4,  3 }, { 0x0018, 12, 3,  5 },
    { 0x0019, 12, 3,  4 }, { 0x001A, 12, 2,  5 }, { 0x001B, 12, 1,  6 },
    { 0x001C, 12, 0, 19 }, { 0x001D, 12, 0, 18 }, { 0x001E, 12, 0, 17 },
    { 0x001F, 12, 0, 16 },
    { 0x0008, 10, 3,  3 }, { 0x0009, 10, 2,  4 }, { 0x000A, 10, 2,  3 },
    { 0x000B, 10, 1,  5 }, { 0x000C, 10, 0, 15 }, { 0x000D, 10, 0, 14 },
    { 0x000E, 10, 0, 13 }, { 0x000F, 10, 0, 12 },
    { 0x0004,  8, 6,  1 }, { 0x0005,  8, 4,  2 }, { 0x0006,  8, 3,  2 },
    { 0x0007,  8, 1,  4 },
    { 0x0004,  7, 5,  1 }, { 0x0005,  7, 2,  2 }, { 0x0006,  7, 0,  8 },
    { 0x0007,  7, 0,  7 },
    { 0x0004,  6, 4,  1 }, { 0x0005,  6, 1,  2 }, { 0x0006,  6, 0,  6 },
    { 0x0007,  6, 0,  5 },
    { 0x0024,  8, 1,  3 }, { 0x0025,  8, 0, 11 }, { 0x0026,  8, 0, 10 },
    { 0x0027,  8, 0,  9 },
    { 0x0005,  5, 3,  1 }, { 0x0006,  5, 2,  1 }, { 0x0007,  5, 0,  4 },
    { 0x0008,  4, 1,  1 }, { 0x0009,  4, 0,  3 },
    { 0x0005,  3, 0,  2 },
    { 0x0003,  2, 0,  1 },
};

#define PD_CODE_COUNT ((int)(sizeof PD_COEFFICIENT_CODES / sizeof PD_COEFFICIENT_CODES[0]))

static PdEntry pd_lookup[8192];
static int     pd_lookup_ready;

static void pd_lookup_init(void)
{
    if (pd_lookup_ready) return;
    memset(pd_lookup, 0, sizeof pd_lookup);
    for (int i = 0; i < 8192; i++) {
        if ((i >> 11) == 1) pd_lookup[i].bits = 2;   // 0b01 — end of block
        if ((i >> 7) == 8)  pd_lookup[i].bits = 6;   // 0b001000 — escape
        for (int c = 0; c < PD_CODE_COUNT; c++) {
            uint8_t bits = PD_COEFFICIENT_CODES[c].bits;
            if ((unsigned)(i >> (13 - bits)) == PD_COEFFICIENT_CODES[c].code) {
                pd_lookup[i].bits  = bits;
                pd_lookup[i].run   = PD_COEFFICIENT_CODES[c].run;
                pd_lookup[i].level = PD_COEFFICIENT_CODES[c].level;
            }
        }
    }
    pd_lookup_ready = 1;
}

// Reads one run/level pair. Returns 1 on a pair, 0 on end of block,
// -1 on error (r->status set).
static int pd_symbol(PdReader *r, int *run_out, int32_t *level_out)
{
    size_t available = r->len * 8 > r->pos ? r->len * 8 - r->pos : 0;
    if (available > 13) available = 13;
    uint32_t prefix = 0;
    if (available) pd_read_bits(r->data, r->len, r->pos, (unsigned)available, &prefix);
    prefix <<= (13 - available);

    PdEntry entry = pd_lookup[prefix];
    if (entry.bits == 0) {
        r->status = available < 13 ? PD_ERR_TRUNCATED : PD_ERR_INVALID_CODE;
        return -1;
    }
    uint32_t tmp;
    if (entry.bits == 2 && entry.level == 0) {
        if (!pd_read(r, 2, &tmp)) return -1;
        return 0;
    }
    if (entry.bits == 6 && entry.level == 0) {
        uint32_t run, raw;
        if (!pd_read(r, 6, &tmp)) return -1;
        if (!pd_read(r, 4, &run)) return -1;
        if (!pd_read(r, 10, &raw)) return -1;
        *run_out   = (int)run;
        *level_out = (int32_t)(raw << 22) >> 22;   // sign-extend 10 bits
        return 1;
    }
    uint32_t sign;
    if (!pd_read(r, entry.bits, &tmp)) return -1;
    if (!pd_read(r, 1, &sign)) return -1;
    *run_out   = entry.run;
    *level_out = sign ? -(int32_t)entry.level : entry.level;
    return 1;
}

static const int PD_SCAN[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };

static int pd_coefficients(PdReader *r, int32_t coefficients[16])
{
    memset(coefficients, 0, 16 * sizeof *coefficients);
    int position = 0;
    while (position < 16) {
        int     run;
        int32_t level;
        int got = pd_symbol(r, &run, &level);
        if (got < 0) return 0;
        if (got == 0) break;
        position += run;
        if (position >= 16) {
            r->status = PD_ERR_COEFF_OVERFLOW;
            return 0;
        }
        coefficients[PD_SCAN[position]] = level;
        position++;
    }
    return 1;
}

// ── 4×4 inverse transform ────────────────────────────────────

static const int64_t PD_BASIS[4][4] = {
    {  8192,   8192,   8192,   8192 },
    { 10703,   4433,  -4433, -10703 },
    {  8192,  -8192,  -8192,   8192 },
    {  4433, -10703,  10703,  -4433 },
};

static void pd_inverse(const int32_t coefficients[16], const uint8_t quant[16],
                       uint8_t factor, int32_t pixels[16])
{
    int64_t intermediate[4][4];
    for (int v = 0; v < 4; v++) {
        for (int x = 0; x < 4; x++) {
            int64_t value = 0;
            for (int u = 0; u < 4; u++) {
                int i = v * 4 + u;
                value += (int64_t)coefficients[i] * quant[i] * factor * PD_BASIS[u][x];
            }
            intermediate[v][x] = value;
        }
    }
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int64_t sum = 0;
            for (int v = 0; v < 4; v++) sum += intermediate[v][x] * PD_BASIS[v][y];
            pixels[y * 4 + x] = (int32_t)((sum + ((int64_t)1 << 33)) >> 34);
        }
    }
}

static int pd_clamp255(int64_t v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (int)v;
}

// ── Picture ──────────────────────────────────────────────────

typedef struct {
    uint8_t picture_type;
    uint8_t quantizer_shift;
    uint8_t factor;
    uint8_t quant_luma[16];
    uint8_t quant_chroma[16];
} PdPictureHeader;

static int pd_parse_header(const uint8_t *data, size_t len, PdPictureHeader *h)
{
    uint32_t marker;
    if (len < PD_HEADER_BYTES) return 0;
    if (!pd_read_bits(data, len, 0, 19, &marker) || marker != 0x400) return 0;
    h->picture_type    = (data[2] >> 2) & 7;
    h->quantizer_shift = data[2] & 3;
    h->factor          = data[3];
    memcpy(h->quant_luma,   data + 4,  16);
    memcpy(h->quant_chroma, data + 20, 16);
    return 1;
}

static pd_result pd_decode_inner(const uint8_t *data, size_t len, uint8_t *rgb, int render)
{
    pd_lookup_init();

    PdReader r = { data, len, 0, 0, 0, PD_OK };

    PdPictureHeader header;
    if (!pd_parse_header(data, len, &header)) return pd_fail(&r, PD_ERR_HEADER);
    if (header.picture_type != 1 || header.quantizer_shift != 0 || header.factor == 0)
        return pd_fail(&r, PD_ERR_UNSUPPORTED);

    r.pos = (size_t)PD_HEADER_BYTES * 8;

    static int32_t luma[PD_PIC_W * PD_PIC_H];
    static int32_t chroma[2][PD_PIC_W * PD_PIC_H / 4];
    if (render) {
        memset(luma, 0, sizeof luma);
        memset(chroma, 0, sizeof chroma);
    }

    for (int row = 0; row < PD_PIC_ROWS; row++) {
        r.row   = row + 1;
        r.block = 0;
        uint32_t marker;
        if (!pd_read(&r, 19, &marker)) return pd_fail(&r, r.status);
        if (marker != (uint32_t)((0x20 << 5) | (row + 1)))
            return pd_fail(&r, PD_ERR_ROW_MARKER);

        int32_t predictors[3] = { 0, 0, 0 };
        for (int mb = 0; mb < PD_PIC_MBS; mb++) {
            for (int block = 0; block < 6; block++) {
                r.block = mb * 6 + block;
                int component = block < 4 ? 0 : block - 3;

                int32_t coefficients[16];
                if (!pd_coefficients(&r, coefficients)) return pd_fail(&r, r.status);
                coefficients[0] += predictors[component];
                // Y1 predicts Y2/Y3/Y4 and the next macroblock's Y1.
                if (block == 0 || component != 0) predictors[component] = coefficients[0];
                if (!render) continue;

                const uint8_t *quant = component == 0 ? header.quant_luma
                                                      : header.quant_chroma;
                int32_t pixels[16];
                pd_inverse(coefficients, quant, header.factor, pixels);

                int32_t *plane  = component == 0 ? luma : chroma[component - 1];
                int      stride = component == 0 ? PD_PIC_W : PD_PIC_W / 2;
                int      x      = component == 0 ? mb * 8 + (block % 2) * 4 : mb * 4;
                int      y      = component == 0 ? row * 8 + (block / 2) * 4 : row * 4;
                for (int py = 0; py < 4; py++)
                    memcpy(plane + (y + py) * stride + x, pixels + py * 4,
                           4 * sizeof *plane);
            }
        }
    }

    uint32_t terminator;
    if (!pd_read(&r, 14, &terminator)) return pd_fail(&r, r.status);
    if (terminator != 0x21) return pd_fail(&r, PD_ERR_TERMINATOR);

    // The observed trailer has at most 15 zero bits before byte-aligned FF fill.
    size_t tail = len;
    while (tail > 0 && data[tail - 1] == 0xFF) tail--;
    size_t used = tail * 8;
    if (used < r.pos || used - r.pos > 15) return pd_fail(&r, PD_ERR_PADDING);
    uint32_t padding = 0;
    if (!pd_read(&r, (unsigned)(used - r.pos), &padding)) return pd_fail(&r, r.status);
    if (padding != 0) return pd_fail(&r, PD_ERR_PADDING);

    if (render) {
        for (int y = 0; y < PD_PIC_H; y++) {
            for (int x = 0; x < PD_PIC_W; x++) {
                int64_t l  = luma[y * PD_PIC_W + x] + 128;
                int64_t cb = chroma[0][(y / 2) * (PD_PIC_W / 2) + x / 2];
                int64_t cr = chroma[1][(y / 2) * (PD_PIC_W / 2) + x / 2];
                uint8_t *px = rgb + (y * PD_PIC_W + x) * 3;
                px[0] = (uint8_t)pd_clamp255(l + ((91881 * cr) >> 16));
                px[1] = (uint8_t)pd_clamp255(l - ((22554 * cb + 46802 * cr) >> 16));
                px[2] = (uint8_t)pd_clamp255(l + ((116130 * cb) >> 16));
            }
        }
    }

    pd_result ok = { PD_OK, (int)r.pos, r.row, r.block };
    return ok;
}

pd_result pd_picture_decode(const uint8_t *data, size_t len, uint8_t *rgb)
{
    return pd_decode_inner(data, len, rgb, 1);
}

pd_result pd_picture_validate(const uint8_t *data, size_t len)
{
    return pd_decode_inner(data, len, NULL, 0);
}

const char *pd_status_name(pd_status s)
{
    switch (s) {
    case PD_OK:                 return "ok";
    case PD_ERR_HEADER:         return "header";
    case PD_ERR_UNSUPPORTED:    return "unsupported-header";
    case PD_ERR_TRUNCATED:      return "truncated";
    case PD_ERR_INVALID_CODE:   return "invalid-code";
    case PD_ERR_COEFF_OVERFLOW: return "coefficient-overflow";
    case PD_ERR_ROW_MARKER:     return "row-marker";
    case PD_ERR_TERMINATOR:     return "terminator";
    case PD_ERR_PADDING:        return "padding";
    }
    return "?";
}
