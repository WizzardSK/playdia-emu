/*
 * Playdia JPEG-style 4×4 DCT decoder attempt
 *
 * Hypothesis: The bitstream uses JPEG-like Huffman coding with 4×4 blocks
 * and the QTable from the frame header for dequantization.
 *
 * We try standard JPEG luminance Huffman tables first.
 * Image dimensions: 128×144 (32×36 blocks of 4×4)
 * or 256×192 (64×48 blocks)
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

static uint8_t frame_buf[MAX_FRAME];
static int frame_pos = 0;

static void write_pgm(const char *path, const uint8_t *gray, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(gray, 1, w * h, f);
    fclose(f);
    printf("  -> %s (%dx%d)\n", path, w, h);
}

// ─── Bitstream reader ─────────────────────────────────────────
typedef struct {
    const uint8_t *data;
    int len;
    int pos;     // byte position
    int bit;     // bit position 7..0, MSB first
    int total_bits_read;
} BitReader;

static void br_init(BitReader *br, const uint8_t *data, int len) {
    br->data = data; br->len = len;
    br->pos = 0; br->bit = 7;
    br->total_bits_read = 0;
}

static int br_get1(BitReader *br) {
    if (br->pos >= br->len) return 0;
    int b = (br->data[br->pos] >> br->bit) & 1;
    if (--br->bit < 0) { br->bit = 7; br->pos++; }
    br->total_bits_read++;
    return b;
}

static int br_get(BitReader *br, int n) {
    int val = 0;
    for (int i = 0; i < n; i++)
        val = (val << 1) | br_get1(br);
    return val;
}

static bool br_eof(BitReader *br) {
    return br->pos >= br->len;
}

// ─── JPEG standard Huffman tables ─────────────────────────────
// DC luminance: generate from bit counts and values
typedef struct {
    int min_code[17]; // minimum code for each bit length
    int max_code[17]; // maximum code for each bit length
    int val_ptr[17];  // index into values array for each bit length
    int values[256];
    int count;        // total number of values
} HuffTable;

static void build_huff(HuffTable *ht, const int *bits, const int *vals, int nvals) {
    // bits[i] = number of codes of length i (1-based)
    int code = 0;
    int idx = 0;
    ht->count = nvals;
    memcpy(ht->values, vals, nvals * sizeof(int));

    for (int len = 1; len <= 16; len++) {
        ht->min_code[len] = code;
        ht->val_ptr[len] = idx;
        if (bits[len] > 0) {
            ht->max_code[len] = code + bits[len] - 1;
            idx += bits[len];
            code += bits[len];
        } else {
            ht->max_code[len] = -1;
        }
        code <<= 1;
    }
}

static int huff_decode(BitReader *br, HuffTable *ht) {
    int code = 0;
    for (int len = 1; len <= 16; len++) {
        code = (code << 1) | br_get1(br);
        if (ht->max_code[len] >= 0 && code <= ht->max_code[len]) {
            int idx = ht->val_ptr[len] + (code - ht->min_code[len]);
            if (idx < ht->count) return ht->values[idx];
            return -1;
        }
    }
    return -1; // no valid code found
}

// JPEG extend function: convert unsigned to signed
static int jpeg_extend(int val, int nbits) {
    if (val < (1 << (nbits - 1)))
        val -= (1 << nbits) - 1;
    return val;
}

// ─── Standard JPEG Luminance Huffman Tables ───────────────────
static HuffTable dc_lum, ac_lum;

static void init_jpeg_tables(void) {
    // DC Luminance
    static const int dc_bits[17] = {0, 0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
    static const int dc_vals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};
    build_huff(&dc_lum, dc_bits, dc_vals, 12);

    // AC Luminance
    static const int ac_bits[17] = {0, 0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
    static const int ac_vals[] = {
        0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
        0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
        0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
        0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
        0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
        0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
        0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
        0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
        0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
        0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
        0xf9,0xfa
    };
    build_huff(&ac_lum, ac_bits, ac_vals, 162);
}

// ─── 4×4 IDCT ────────────────────────────────────────────────
static void idct4x4(const double coeff[16], int out[16]) {
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            double sum = 0;
            for (int v = 0; v < 4; v++) {
                double cv = (v == 0) ? 0.5 : sqrt(0.5);
                for (int u = 0; u < 4; u++) {
                    double cu = (u == 0) ? 0.5 : sqrt(0.5);
                    sum += cu * cv * coeff[v * 4 + u] *
                           cos((2*x+1)*u*PI/8.0) * cos((2*y+1)*v*PI/8.0);
                }
            }
            out[y * 4 + x] = (int)round(sum);
        }
    }
}

// 4×4 zigzag order
static const int zigzag4[16] = {
    0, 1, 4, 8,  5, 2, 3, 6,
    9, 12, 13, 10, 7, 11, 14, 15
};

// ─── Decode one 4×4 block ─────────────────────────────────────
static int decode_block(BitReader *br, double coeff[16], int *prev_dc,
                        const uint8_t *qtab, int qscale) {
    memset(coeff, 0, 16 * sizeof(double));

    // DC coefficient
    int cat = huff_decode(br, &dc_lum);
    if (cat < 0) return -1;
    int dc_diff = 0;
    if (cat > 0) {
        dc_diff = br_get(br, cat);
        dc_diff = jpeg_extend(dc_diff, cat);
    }
    *prev_dc += dc_diff;
    coeff[0] = *prev_dc * qtab[0] * qscale;

    // AC coefficients (up to 15)
    for (int k = 1; k < 16; ) {
        int sym = huff_decode(br, &ac_lum);
        if (sym < 0) return -1;
        if (sym == 0x00) break; // EOB
        int run = (sym >> 4) & 0xF;
        int size = sym & 0xF;
        k += run;
        if (k >= 16) break;
        int val = 0;
        if (size > 0) {
            val = br_get(br, size);
            val = jpeg_extend(val, size);
        }
        // Dequantize: zigzag order
        int zz = zigzag4[k];
        coeff[zz] = val * qtab[zz < 16 ? zz : 0] * qscale;
        k++;
    }
    return 0;
}

// ─── Decode full image ────────────────────────────────────────
static void try_jpeg4_decode(const uint8_t *data, int dlen,
                              const uint8_t *qtab, int qscale,
                              int img_w, int img_h, const char *path) {
    int bw = img_w / 4;
    int bh = img_h / 4;
    uint8_t *img = calloc(img_w * img_h, 1);
    BitReader br;
    br_init(&br, data, dlen);

    int prev_dc = 0;
    int blocks_decoded = 0;
    bool failed = false;

    for (int by = 0; by < bh && !br_eof(&br) && !failed; by++) {
        for (int bx = 0; bx < bw && !br_eof(&br) && !failed; bx++) {
            double coeff[16];
            if (decode_block(&br, coeff, &prev_dc, qtab, qscale) < 0) {
                failed = true;
                break;
            }

            int pixels[16];
            idct4x4(coeff, pixels);

            for (int dy = 0; dy < 4; dy++) {
                for (int dx = 0; dx < 4; dx++) {
                    int px = bx * 4 + dx;
                    int py = by * 4 + dy;
                    if (px < img_w && py < img_h) {
                        int v = pixels[dy * 4 + dx] + 128;
                        if (v < 0) v = 0; if (v > 255) v = 255;
                        img[py * img_w + px] = v;
                    }
                }
            }
            blocks_decoded++;
        }
    }

    printf("  JPEG4: %d blocks decoded, %d bits used (of %d), failed=%d\n",
           blocks_decoded, br.total_bits_read, dlen * 8, failed);

    write_pgm(path, img, img_w, img_h);
    free(img);
}

// ─── Also try with qscale=1 (no extra scaling) ───────────────
static void try_jpeg4_noscale(const uint8_t *data, int dlen,
                               const uint8_t *qtab,
                               int img_w, int img_h, const char *path) {
    int bw = img_w / 4;
    int bh = img_h / 4;
    uint8_t *img = calloc(img_w * img_h, 1);
    BitReader br;
    br_init(&br, data, dlen);

    int prev_dc = 0;
    int blocks_decoded = 0;

    for (int by = 0; by < bh && !br_eof(&br); by++) {
        for (int bx = 0; bx < bw && !br_eof(&br); bx++) {
            double coeff[16] = {0};

            // DC
            int cat = huff_decode(&br, &dc_lum);
            if (cat < 0) goto done;
            int dc_diff = 0;
            if (cat > 0) {
                dc_diff = br_get(&br, cat);
                dc_diff = jpeg_extend(dc_diff, cat);
            }
            prev_dc += dc_diff;
            coeff[0] = prev_dc * qtab[0];

            // AC
            for (int k = 1; k < 16; ) {
                int sym = huff_decode(&br, &ac_lum);
                if (sym < 0) goto done;
                if (sym == 0x00) break;
                int run = (sym >> 4) & 0xF;
                int size = sym & 0xF;
                k += run;
                if (k >= 16) break;
                int val = 0;
                if (size > 0) {
                    val = br_get(&br, size);
                    val = jpeg_extend(val, size);
                }
                int zz = zigzag4[k];
                coeff[zz] = val * qtab[zz < 16 ? zz : 0];
                k++;
            }

            int pixels[16];
            idct4x4(coeff, pixels);

            for (int dy = 0; dy < 4; dy++) {
                for (int dx = 0; dx < 4; dx++) {
                    int px = bx * 4 + dx;
                    int py = by * 4 + dy;
                    if (px < img_w && py < img_h) {
                        int v = pixels[dy * 4 + dx] + 128;
                        if (v < 0) v = 0; if (v > 255) v = 255;
                        img[py * img_w + px] = v;
                    }
                }
            }
            blocks_decoded++;
        }
    }
done:
    printf("  JPEG4(noscale): %d blocks decoded, %d bits used\n",
           blocks_decoded, br.total_bits_read);
    write_pgm(path, img, img_w, img_h);
    free(img);
}

// ─── Try simple fixed-width codes (not Huffman) ───────────────
// Maybe each coefficient is encoded as a fixed number of bits?
static void try_fixed_width(const uint8_t *data, int dlen,
                             const uint8_t *qtab, int qscale,
                             int img_w, int img_h, int bits_per_coeff,
                             const char *path) {
    int bw = img_w / 4;
    int bh = img_h / 4;
    uint8_t *img = calloc(img_w * img_h, 1);
    BitReader br;
    br_init(&br, data, dlen);

    for (int by = 0; by < bh && !br_eof(&br); by++) {
        for (int bx = 0; bx < bw && !br_eof(&br); bx++) {
            double coeff[16] = {0};
            for (int k = 0; k < 16 && !br_eof(&br); k++) {
                int val = br_get(&br, bits_per_coeff);
                // Sign-extend
                if (val >= (1 << (bits_per_coeff - 1)))
                    val -= (1 << bits_per_coeff);
                int zz = zigzag4[k];
                coeff[zz] = val * qtab[zz < 16 ? zz : 0];
            }

            int pixels[16];
            idct4x4(coeff, pixels);

            for (int dy = 0; dy < 4; dy++) {
                for (int dx = 0; dx < 4; dx++) {
                    int px = bx * 4 + dx;
                    int py = by * 4 + dy;
                    if (px < img_w && py < img_h) {
                        int v = pixels[dy * 4 + dx] + 128;
                        if (v < 0) v = 0; if (v > 255) v = 255;
                        img[py * img_w + px] = v;
                    }
                }
            }
        }
    }
    write_pgm(path, img, img_w, img_h);
    free(img);
}

static void process_frame(int fnum) {
    if (frame_pos < 40) return;
    if (frame_buf[0] != 0x00 || frame_buf[1] != 0x80) return;

    uint8_t qscale = frame_buf[3];
    uint8_t qtab[16];
    memcpy(qtab, frame_buf + 4, 16);

    // Skip frame header (36 bytes) + sub-header (4 bytes)
    const uint8_t *data = frame_buf + 40;
    int dlen = frame_pos - 40;

    // Also try from byte 36 (including sub-header)
    const uint8_t *data36 = frame_buf + 36;
    int dlen36 = frame_pos - 36;

    printf("\n=== FRAME %d: %d bytes, qscale=%d ===\n", fnum, frame_pos, qscale);

    char path[256];

    // JPEG-style Huffman at various resolutions
    struct { int w, h; const char *tag; } dims[] = {
        {128, 144, "128x144"},
        {128, 96, "128x96"},
        {256, 192, "256x192"},
        {256, 96, "256x96"},
        {160, 120, "160x120"},
        {192, 128, "192x128"},
    };

    for (int d = 0; d < 6; d++) {
        int w = dims[d].w, h = dims[d].h;
        printf("\n  --- %s ---\n", dims[d].tag);

        // With sub-header skipped
        snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_j4_f%d_%s.pgm", fnum, dims[d].tag);
        try_jpeg4_decode(data, dlen, qtab, qscale, w, h, path);

        // Without qscale multiplication
        snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_j4ns_f%d_%s.pgm", fnum, dims[d].tag);
        try_jpeg4_noscale(data, dlen, qtab, w, h, path);

        // From byte 36 (include sub-header as data)
        snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_j4s_f%d_%s.pgm", fnum, dims[d].tag);
        try_jpeg4_decode(data36, dlen36, qtab, qscale, w, h, path);

        // Fixed-width 5 bits per coefficient
        snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_fw5_f%d_%s.pgm", fnum, dims[d].tag);
        try_fixed_width(data, dlen, qtab, 1, w, h, 5, path);

        // Fixed-width 6 bits per coefficient
        snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_fw6_f%d_%s.pgm", fnum, dims[d].tag);
        try_fixed_width(data, dlen, qtab, 1, w, h, 6, path);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <game.zip> [start_lba]\n", argv[0]);
        return 1;
    }
    int start_lba = argc > 2 ? atoi(argv[2]) : 500;

    init_jpeg_tables();

    int err;
    zip_t *z = zip_open(argv[1], ZIP_RDONLY, &err);
    if (!z) { fprintf(stderr, "Cannot open zip\n"); return 1; }

    int best_idx = -1;
    zip_uint64_t best_size = 0;
    for (int i = 0; i < (int)zip_get_num_entries(z, 0); i++) {
        zip_stat_t st;
        if (zip_stat_index(z, i, 0, &st) == 0 && st.size > best_size) {
            best_size = st.size; best_idx = i;
        }
    }

    zip_stat_t st;
    zip_stat_index(z, best_idx, 0, &st);

    zip_file_t *zf = zip_fopen_index(z, best_idx, 0);
    uint8_t *disc = malloc(st.size);
    zip_int64_t rd = 0;
    while (rd < (zip_int64_t)st.size) {
        zip_int64_t r = zip_fread(zf, disc + rd, st.size - rd);
        if (r <= 0) break; rd += r;
    }
    zip_fclose(zf);

    int total_sectors = (int)(st.size / SECTOR_RAW);
    int frames_found = 0;
    bool in_frame = false;

    for (int lba = start_lba; lba < total_sectors && frames_found < 2; lba++) {
        uint8_t *s = disc + (long)lba * SECTOR_RAW;
        if (s[0] != 0x00 || s[1] != 0xFF) continue;
        if (s[15] != 2) continue;
        if (s[18] & 0x04) continue;
        uint8_t marker = s[24];

        if (marker == 0xF3) { frame_pos = 0; in_frame = false; continue; }
        if (marker == 0xF1) {
            if (!in_frame) { in_frame = true; frame_pos = 0; }
            int dl = (s[18] & 0x20) ? 2335 : 2047;
            if (frame_pos + dl < MAX_FRAME) {
                memcpy(frame_buf + frame_pos, s + 25, dl);
                frame_pos += dl;
            }
            continue;
        }
        if (marker == 0xF2 && in_frame) {
            if (frame_buf[0] == 0x00 && frame_buf[1] == 0x80 && frame_pos > 40) {
                process_frame(frames_found);
                frames_found++;
            }
            in_frame = false;
            frame_pos = 0;
        }
    }

    free(disc);
    zip_close(z);
    printf("\nDone.\n");
    return 0;
}
