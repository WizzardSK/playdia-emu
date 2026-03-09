/*
 * Playdia video codec decoder - refined JPEG-style 4×4 attempt
 * Focus on 128×144 resolution, trying multiple variations:
 * 1. Different QTable indexing (zigzag vs spatial)
 * 2. No quantization (raw coefficients)
 * 3. Walsh-Hadamard Transform instead of DCT
 * 4. YUV420 decode (Y + Cb/Cr planes)
 * 5. Different bit reading orders (MSB vs LSB first)
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

static void write_ppm(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, w * h * 3, f);
    fclose(f);
    printf("  -> %s (%dx%d RGB)\n", path, w, h);
}

// ─── Bitstream reader (MSB first) ───
typedef struct {
    const uint8_t *data;
    int len;
    int pos;
    int bit;
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

// LSB-first bitstream reader
typedef struct {
    const uint8_t *data;
    int len;
    int pos;
    int bit;
    int total_bits_read;
} BitReaderLSB;

static void br_lsb_init(BitReaderLSB *br, const uint8_t *data, int len) {
    br->data = data; br->len = len;
    br->pos = 0; br->bit = 0;
    br->total_bits_read = 0;
}

static int br_lsb_get1(BitReaderLSB *br) {
    if (br->pos >= br->len) return 0;
    int b = (br->data[br->pos] >> br->bit) & 1;
    if (++br->bit > 7) { br->bit = 0; br->pos++; }
    br->total_bits_read++;
    return b;
}

static int br_lsb_get(BitReaderLSB *br, int n) {
    int val = 0;
    for (int i = 0; i < n; i++)
        val |= (br_lsb_get1(br) << i);
    return val;
}

static bool br_lsb_eof(BitReaderLSB *br) {
    return br->pos >= br->len;
}

// ─── Huffman tables ───
typedef struct {
    int min_code[17];
    int max_code[17];
    int val_ptr[17];
    int values[256];
    int count;
} HuffTable;

static void build_huff(HuffTable *ht, const int *bits, const int *vals, int nvals) {
    int code = 0, idx = 0;
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
    return -1;
}

static int jpeg_extend(int val, int nbits) {
    if (nbits == 0) return 0;
    if (val < (1 << (nbits - 1)))
        val -= (1 << nbits) - 1;
    return val;
}

// Standard JPEG luminance tables
static HuffTable dc_lum, ac_lum;
// Also chrominance tables
static HuffTable dc_chr, ac_chr;

static void init_jpeg_tables(void) {
    static const int dc_bits[17] = {0, 0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
    static const int dc_vals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};
    build_huff(&dc_lum, dc_bits, dc_vals, 12);

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

    // DC Chrominance
    static const int dc_chr_bits[17] = {0, 0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
    static const int dc_chr_vals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};
    build_huff(&dc_chr, dc_chr_bits, dc_chr_vals, 12);

    // AC Chrominance
    static const int ac_chr_bits[17] = {0, 0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
    static const int ac_chr_vals[] = {
        0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
        0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
        0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
        0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
        0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
        0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
        0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
        0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
        0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
        0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
        0xf9,0xfa
    };
    build_huff(&ac_chr, ac_chr_bits, ac_chr_vals, 162);
}

// 4×4 zigzag orders
static const int zigzag4[16] = {
    0, 1, 4, 8,  5, 2, 3, 6,
    9, 12, 13, 10, 7, 11, 14, 15
};

// Alternative: row-major scan (no zigzag)
static const int rowscan4[16] = {
    0,1,2,3, 4,5,6,7, 8,9,10,11, 12,13,14,15
};

// ─── 4×4 IDCT ───
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

// ─── 4×4 Inverse Walsh-Hadamard Transform ───
static void iwht4x4(const double coeff[16], int out[16]) {
    // 4-point WHT butterfly
    double tmp[16];
    // rows
    for (int r = 0; r < 4; r++) {
        double a = coeff[r*4+0], b = coeff[r*4+1], c = coeff[r*4+2], d = coeff[r*4+3];
        double s0 = a + b + c + d;
        double s1 = a - b + c - d;
        double s2 = a + b - c - d;
        double s3 = a - b - c + d;
        tmp[r*4+0] = s0; tmp[r*4+1] = s1; tmp[r*4+2] = s2; tmp[r*4+3] = s3;
    }
    // columns
    for (int c = 0; c < 4; c++) {
        double a = tmp[0+c], b = tmp[4+c], cc = tmp[8+c], d = tmp[12+c];
        double s0 = a + b + cc + d;
        double s1 = a - b + cc - d;
        double s2 = a + b - cc - d;
        double s3 = a - b - cc + d;
        out[0+c]  = (int)round(s0 / 4.0);
        out[4+c]  = (int)round(s1 / 4.0);
        out[8+c]  = (int)round(s2 / 4.0);
        out[12+c] = (int)round(s3 / 4.0);
    }
}

// ─── Decode one plane worth of blocks ───
typedef enum { XFORM_DCT, XFORM_WHT, XFORM_NONE } XformType;
typedef enum { QUANT_FULL, QUANT_QTAB_ONLY, QUANT_NONE } QuantMode;

static int decode_plane(BitReader *br, uint8_t *img, int img_w, int img_h,
                        const uint8_t *qtab, int qscale,
                        const int *scan_order, XformType xform,
                        QuantMode qmode, int dc_bias,
                        HuffTable *ht_dc, HuffTable *ht_ac) {
    int bw = img_w / 4;
    int bh = img_h / 4;
    int prev_dc = 0;
    int blocks = 0;

    for (int by = 0; by < bh && !br_eof(br); by++) {
        for (int bx = 0; bx < bw && !br_eof(br); bx++) {
            double coeff[16] = {0};

            // DC
            int cat = huff_decode(br, ht_dc);
            if (cat < 0) return blocks;
            int dc_diff = 0;
            if (cat > 0) {
                dc_diff = br_get(br, cat);
                dc_diff = jpeg_extend(dc_diff, cat);
            }
            prev_dc += dc_diff;

            switch (qmode) {
                case QUANT_FULL: coeff[0] = prev_dc * qtab[0] * qscale; break;
                case QUANT_QTAB_ONLY: coeff[0] = prev_dc * qtab[0]; break;
                case QUANT_NONE: coeff[0] = prev_dc; break;
            }

            // AC
            for (int k = 1; k < 16; ) {
                int sym = huff_decode(br, ht_ac);
                if (sym < 0) return blocks;
                if (sym == 0x00) break;
                int run = (sym >> 4) & 0xF;
                int size = sym & 0xF;
                k += run;
                if (k >= 16) break;
                int val = 0;
                if (size > 0) {
                    val = br_get(br, size);
                    val = jpeg_extend(val, size);
                }
                // Use scan_order for position, qtab indexed by k (zigzag order)
                int pos = scan_order[k];
                switch (qmode) {
                    case QUANT_FULL: coeff[pos] = val * qtab[k] * qscale; break;
                    case QUANT_QTAB_ONLY: coeff[pos] = val * qtab[k]; break;
                    case QUANT_NONE: coeff[pos] = val; break;
                }
                k++;
            }

            int pixels[16];
            switch (xform) {
                case XFORM_DCT: idct4x4(coeff, pixels); break;
                case XFORM_WHT: iwht4x4(coeff, pixels); break;
                case XFORM_NONE:
                    for (int i = 0; i < 16; i++) pixels[i] = (int)round(coeff[i]);
                    break;
            }

            for (int dy = 0; dy < 4; dy++) {
                for (int dx = 0; dx < 4; dx++) {
                    int px = bx * 4 + dx;
                    int py = by * 4 + dy;
                    if (px < img_w && py < img_h) {
                        int v = pixels[dy * 4 + dx] + dc_bias;
                        if (v < 0) v = 0; if (v > 255) v = 255;
                        img[py * img_w + px] = v;
                    }
                }
            }
            blocks++;
        }
    }
    return blocks;
}

// ─── Assemble frames from zip ───
static int assemble_frames(const uint8_t *disc, int total_sectors,
                            int start_lba, uint8_t frames[][MAX_FRAME],
                            int frame_sizes[], int max_frames) {
    int nframes = 0;
    int cur_pos = 0;
    bool in_frame = false;

    for (int lba = start_lba; lba < total_sectors && nframes < max_frames; lba++) {
        const uint8_t *s = disc + (long)lba * SECTOR_RAW;
        if (s[0] != 0x00 || s[1] != 0xFF) continue;
        if (s[15] != 2) continue;
        if (s[18] & 0x04) continue; // skip audio

        uint8_t marker = s[24];
        if (marker == 0xF1) {
            if (!in_frame) { in_frame = true; cur_pos = 0; }
            int payload_len = 2324 - 4 - 4; // Form 1: 2048, but we have subheader
            // Actually, CD-ROM XA Mode 2 Form 1 payload = 2048 bytes at offset 24
            // But marker byte is at offset 24, so real data starts at 25
            // Let's take bytes 25..2071 (2047 bytes) to skip the F1 marker
            if (cur_pos + 2047 < MAX_FRAME) {
                memcpy(frames[nframes] + cur_pos, s + 25, 2047);
                cur_pos += 2047;
            }
        } else if (marker == 0xF2) {
            if (in_frame && cur_pos > 0) {
                frame_sizes[nframes] = cur_pos;
                nframes++;
                in_frame = false;
                cur_pos = 0;
            }
        }
    }
    return nframes;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <game.zip> [start_lba]\n", argv[0]);
        return 1;
    }
    int start_lba = argc > 2 ? atoi(argv[2]) : 502;

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
        if (r <= 0) break;
        rd += r;
    }
    zip_fclose(zf);
    int total_sectors = (int)(st.size / SECTOR_RAW);

    init_jpeg_tables();

    // Assemble frames
    static uint8_t frames[4][MAX_FRAME];
    int frame_sizes[4];
    int nframes = assemble_frames(disc, total_sectors, start_lba, frames, frame_sizes, 4);
    printf("Assembled %d frames from LBA %d\n\n", nframes, start_lba);

    for (int fi = 0; fi < nframes && fi < 2; fi++) {
        int fsize = frame_sizes[fi];
        uint8_t *f = frames[fi];

        printf("=== FRAME %d: %d bytes ===\n", fi, fsize);
        printf("  Header: %02X %02X %02X %02X\n", f[0], f[1], f[2], f[3]);
        printf("  QTable: ");
        for (int i = 0; i < 16; i++) printf("%d ", f[4 + i]);
        printf("\n");

        uint8_t qtab[16];
        memcpy(qtab, f + 4, 16);
        int qscale = f[3];
        int block_type = f[2];

        printf("  Sub-header @36: %02X %02X %02X %02X\n", f[36], f[37], f[38], f[39]);

        // Try starting at byte 36 (skip frame header) and byte 40 (skip sub-header too)
        int offsets[] = {36, 40};
        const char *off_names[] = {"h36", "h40"};

        for (int oi = 0; oi < 2; oi++) {
            int off = offsets[oi];
            const uint8_t *bs = f + off;
            int bs_len = fsize - off;

            printf("\n  --- Offset %d (%s) ---\n", off, off_names[oi]);

            // Dump first 32 bits for analysis
            printf("  First 8 bytes: ");
            for (int i = 0; i < 8 && i < bs_len; i++) printf("%02X ", bs[i]);
            printf("\n  First 32 bits: ");
            for (int i = 0; i < 32 && i < bs_len * 8; i++) {
                printf("%d", (bs[i/8] >> (7 - i%8)) & 1);
                if (i % 8 == 7) printf(" ");
            }
            printf("\n");

            char path[256];
            int W = 128, H = 144;
            int expected_blocks = (W/4) * (H/4); // 1152

            // Test 1: DCT + full quant + zigzag QTab indexed by k
            {
                uint8_t *img = calloc(W * H, 1);
                BitReader br;
                br_init(&br, bs, bs_len);
                int n = decode_plane(&br, img, W, H, qtab, qscale,
                                      zigzag4, XFORM_DCT, QUANT_FULL, 128,
                                      &dc_lum, &ac_lum);
                printf("  DCT+QFull: %d blocks, %d bits\n", n, br.total_bits_read);
                snprintf(path, sizeof(path), "/home/wizzard/share/GitHub/pd_b_%s_dct_qf_f%d.pgm", off_names[oi], fi);
                write_pgm(path, img, W, H);
                free(img);
            }

            // Test 2: DCT + qtab only (no qscale)
            {
                uint8_t *img = calloc(W * H, 1);
                BitReader br;
                br_init(&br, bs, bs_len);
                int n = decode_plane(&br, img, W, H, qtab, qscale,
                                      zigzag4, XFORM_DCT, QUANT_QTAB_ONLY, 128,
                                      &dc_lum, &ac_lum);
                printf("  DCT+QTab: %d blocks, %d bits\n", n, br.total_bits_read);
                snprintf(path, sizeof(path), "/home/wizzard/share/GitHub/pd_b_%s_dct_qt_f%d.pgm", off_names[oi], fi);
                write_pgm(path, img, W, H);
                free(img);
            }

            // Test 3: DCT + NO quant
            {
                uint8_t *img = calloc(W * H, 1);
                BitReader br;
                br_init(&br, bs, bs_len);
                int n = decode_plane(&br, img, W, H, qtab, qscale,
                                      zigzag4, XFORM_DCT, QUANT_NONE, 128,
                                      &dc_lum, &ac_lum);
                printf("  DCT+NoQ: %d blocks, %d bits\n", n, br.total_bits_read);
                snprintf(path, sizeof(path), "/home/wizzard/share/GitHub/pd_b_%s_dct_nq_f%d.pgm", off_names[oi], fi);
                write_pgm(path, img, W, H);
                free(img);
            }

            // Test 4: WHT + full quant
            {
                uint8_t *img = calloc(W * H, 1);
                BitReader br;
                br_init(&br, bs, bs_len);
                int n = decode_plane(&br, img, W, H, qtab, qscale,
                                      zigzag4, XFORM_WHT, QUANT_FULL, 128,
                                      &dc_lum, &ac_lum);
                printf("  WHT+QFull: %d blocks, %d bits\n", n, br.total_bits_read);
                snprintf(path, sizeof(path), "/home/wizzard/share/GitHub/pd_b_%s_wht_qf_f%d.pgm", off_names[oi], fi);
                write_pgm(path, img, W, H);
                free(img);
            }

            // Test 5: WHT + no quant
            {
                uint8_t *img = calloc(W * H, 1);
                BitReader br;
                br_init(&br, bs, bs_len);
                int n = decode_plane(&br, img, W, H, qtab, qscale,
                                      zigzag4, XFORM_WHT, QUANT_NONE, 128,
                                      &dc_lum, &ac_lum);
                printf("  WHT+NoQ: %d blocks, %d bits\n", n, br.total_bits_read);
                snprintf(path, sizeof(path), "/home/wizzard/share/GitHub/pd_b_%s_wht_nq_f%d.pgm", off_names[oi], fi);
                write_pgm(path, img, W, H);
                free(img);
            }

            // Test 6: DCT + no quant + row scan (no zigzag)
            {
                uint8_t *img = calloc(W * H, 1);
                BitReader br;
                br_init(&br, bs, bs_len);
                int n = decode_plane(&br, img, W, H, qtab, qscale,
                                      rowscan4, XFORM_DCT, QUANT_NONE, 128,
                                      &dc_lum, &ac_lum);
                printf("  DCT+NoQ+RowScan: %d blocks, %d bits\n", n, br.total_bits_read);
                snprintf(path, sizeof(path), "/home/wizzard/share/GitHub/pd_b_%s_dct_nq_rs_f%d.pgm", off_names[oi], fi);
                write_pgm(path, img, W, H);
                free(img);
            }

            // Test 7: YUV420 decode (Y + Cb + Cr)
            if (oi == 0) { // only do this for offset 36
                uint8_t *y_plane = calloc(W * H, 1);
                uint8_t *cb_plane = calloc((W/2) * (H/2), 1);
                uint8_t *cr_plane = calloc((W/2) * (H/2), 1);
                BitReader br;
                br_init(&br, bs, bs_len);

                int ny = decode_plane(&br, y_plane, W, H, qtab, qscale,
                                       zigzag4, XFORM_DCT, QUANT_FULL, 128,
                                       &dc_lum, &ac_lum);
                int ncb = decode_plane(&br, cb_plane, W/2, H/2, qtab, qscale,
                                        zigzag4, XFORM_DCT, QUANT_FULL, 128,
                                        &dc_chr, &ac_chr);
                int ncr = decode_plane(&br, cr_plane, W/2, H/2, qtab, qscale,
                                        zigzag4, XFORM_DCT, QUANT_FULL, 128,
                                        &dc_chr, &ac_chr);

                printf("  YUV420: Y=%d Cb=%d Cr=%d blocks, %d bits total\n",
                       ny, ncb, ncr, br.total_bits_read);

                // Convert to RGB
                uint8_t *rgb = calloc(W * H * 3, 1);
                for (int py = 0; py < H; py++) {
                    for (int px = 0; px < W; px++) {
                        int Y = y_plane[py * W + px];
                        int Cb = cb_plane[(py/2) * (W/2) + (px/2)] - 128;
                        int Cr = cr_plane[(py/2) * (W/2) + (px/2)] - 128;
                        int R = Y + (int)(1.402 * Cr);
                        int G = Y - (int)(0.344 * Cb + 0.714 * Cr);
                        int B = Y + (int)(1.772 * Cb);
                        if (R < 0) R = 0; if (R > 255) R = 255;
                        if (G < 0) G = 0; if (G > 255) G = 255;
                        if (B < 0) B = 0; if (B > 255) B = 255;
                        rgb[(py * W + px) * 3 + 0] = R;
                        rgb[(py * W + px) * 3 + 1] = G;
                        rgb[(py * W + px) * 3 + 2] = B;
                    }
                }
                snprintf(path, sizeof(path), "/home/wizzard/share/GitHub/pd_b_yuv420_f%d.ppm", fi);
                write_ppm(path, rgb, W, H);

                free(y_plane); free(cb_plane); free(cr_plane); free(rgb);
            }

            // Test 8: Dump first 10 decoded blocks' coefficients for analysis
            if (oi == 0 && fi == 1) {
                printf("\n  First 10 blocks decoded coefficients (raw, no quant):\n");
                BitReader br;
                br_init(&br, bs, bs_len);
                int prev_dc = 0;
                for (int b = 0; b < 10 && !br_eof(&br); b++) {
                    int cat = huff_decode(&br, &dc_lum);
                    if (cat < 0) { printf("  Block %d: DC decode failed\n", b); break; }
                    int dc_diff = 0;
                    if (cat > 0) {
                        dc_diff = br_get(&br, cat);
                        dc_diff = jpeg_extend(dc_diff, cat);
                    }
                    prev_dc += dc_diff;
                    printf("  Block %2d: DC=%4d (cat=%d diff=%d) AC=", b, prev_dc, cat, dc_diff);

                    int ac_count = 0;
                    for (int k = 1; k < 16; ) {
                        int sym = huff_decode(&br, &ac_lum);
                        if (sym < 0) { printf("FAIL "); break; }
                        if (sym == 0x00) { printf("EOB "); break; }
                        int run = (sym >> 4) & 0xF;
                        int size = sym & 0xF;
                        k += run;
                        if (k >= 16) break;
                        int val = 0;
                        if (size > 0) {
                            val = br_get(&br, size);
                            val = jpeg_extend(val, size);
                        }
                        printf("[%d:%d] ", k, val);
                        k++;
                        ac_count++;
                    }
                    printf("(%d ACs, %d bits so far)\n", ac_count, br.total_bits_read);
                }
            }
        }
    }

    free(disc);
    zip_close(z);
    return 0;
}
