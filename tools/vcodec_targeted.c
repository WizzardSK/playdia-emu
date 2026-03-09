/*
 * Targeted Playdia Video Decode Tests
 *
 * Based on analysis:
 *   Frame header: 00 80 04 QS [16-byte qtable] [16-byte qtable copy]
 *   Bitstream sub-header: 00 80 24 XX (where XX = 00 or 01 alternating)
 *   Key hypothesis: width=128 (0x80), 4bpp format
 *   256×192 at 2bpp also close match (12288 vs 12242 bytes)
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

static uint8_t frame_buf[MAX_FRAME];
static int frame_pos = 0;

static void write_pgm(const char *path, const uint8_t *gray, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(gray, 1, w * h, f);
    fclose(f);
    printf("  -> %s\n", path);
}

static void write_ppm(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, w * h * 3, f);
    fclose(f);
    printf("  -> %s\n", path);
}

// ─── Test: 4bpp with row-major, hi nibble first ──────────────
static void test_4bpp_variants(const uint8_t *data, int len, int w, int h,
                                const char *tag) {
    uint8_t *gray = calloc(w * h, 1);
    int px = 0;

    // Variant A: hi nibble = left pixel, lo nibble = right pixel
    for (int i = 0; i < len && px + 1 < w * h; i++) {
        gray[px++] = ((data[i] >> 4) & 0xF) * 17;
        gray[px++] = (data[i] & 0xF) * 17;
    }
    char path[256];
    snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_%s_4bpp_hilo_%dx%d.pgm",
             tag, w, h);
    write_pgm(path, gray, w, h);

    // Variant B: lo nibble first
    px = 0;
    for (int i = 0; i < len && px + 1 < w * h; i++) {
        gray[px++] = (data[i] & 0xF) * 17;
        gray[px++] = ((data[i] >> 4) & 0xF) * 17;
    }
    snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_%s_4bpp_lohi_%dx%d.pgm",
             tag, w, h);
    write_pgm(path, gray, w, h);

    // Variant C: bit-planar (1st half = plane 0-1, 2nd half = plane 2-3)
    // Each half contains w*h/4 bytes
    memset(gray, 0, w * h);
    int half = len / 2;
    for (int i = 0; i < half && i * 4 + 3 < w * h; i++) {
        uint8_t b0 = data[i];
        uint8_t b1 = (i + half < len) ? data[i + half] : 0;
        for (int bit = 7; bit >= 0 && (i * 8 + (7-bit)) * 1 < w * h; bit--) {
            int px_idx = i * 8 + (7 - bit);
            if (px_idx >= w * h) break;
            int val = ((b0 >> bit) & 1) | (((b1 >> bit) & 1) << 1);
            gray[px_idx] = val * 85; // 0, 85, 170, 255
        }
    }
    snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_%s_2bpp_planar_%dx%d.pgm",
             tag, w, h);
    write_pgm(path, gray, w, h);

    free(gray);
}

// ─── Test: 8bpp raw at various widths ─────────────────────────
static void test_raw_widths(const uint8_t *data, int len, const char *tag) {
    int widths[] = {128, 144, 160, 176, 192, 96, 112, 136, 152};
    for (int wi = 0; wi < 9; wi++) {
        int w = widths[wi];
        int h = len / w;
        if (h < 10 || h > 300) continue;
        uint8_t *gray = calloc(w * h, 1);
        memcpy(gray, data, w * h);
        char path[256];
        snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_%s_8bpp_%dx%d.pgm",
                 tag, w, h);
        write_pgm(path, gray, w, h);
        free(gray);
    }
}

// ─── Test: YCbCr 4:2:0 (Y plane + subsampled Cb/Cr) ──────────
// Y: w×h bytes, Cb: w/2×h/2, Cr: w/2×h/2
// Total = w*h * 1.5
static void test_yuv420(const uint8_t *data, int len, int w, int h, const char *tag) {
    int y_size = w * h;
    int uv_size = (w/2) * (h/2);
    if (y_size + 2 * uv_size > len) return;

    uint8_t *rgb = calloc(w * h * 3, 1);
    const uint8_t *Y = data;
    const uint8_t *Cb = data + y_size;
    const uint8_t *Cr = data + y_size + uv_size;

    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            int y_val = Y[py * w + px];
            int cb = Cb[(py/2) * (w/2) + (px/2)] - 128;
            int cr = Cr[(py/2) * (w/2) + (px/2)] - 128;
            int r = y_val + (int)(1.402 * cr);
            int g = y_val - (int)(0.344 * cb) - (int)(0.714 * cr);
            int b = y_val + (int)(1.772 * cb);
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            rgb[(py * w + px) * 3 + 0] = r;
            rgb[(py * w + px) * 3 + 1] = g;
            rgb[(py * w + px) * 3 + 2] = b;
        }
    }
    char path[256];
    snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_%s_yuv420_%dx%d.ppm",
             tag, w, h);
    write_ppm(path, rgb, w, h);
    free(rgb);
}

// ─── Test: Interleaved YCbCr 4:2:2 (YUYV / UYVY) ─────────────
static void test_yuyv(const uint8_t *data, int len, int w, int h, const char *tag) {
    if (w * h > len) return; // need w*h bytes minimum for YUYV (2 bytes per pixel pair)
    uint8_t *rgb = calloc(w * h * 3, 1);

    // YUYV: Y0 U Y1 V
    for (int i = 0; i + 3 < len && i/2 + 1 < w * h; i += 4) {
        int y0 = data[i], u = data[i+1] - 128, y1 = data[i+2], v = data[i+3] - 128;
        for (int j = 0; j < 2; j++) {
            int y = (j == 0) ? y0 : y1;
            int px = i/2 + j;
            if (px >= w * h) break;
            int r = y + (int)(1.402 * v);
            int g = y - (int)(0.344 * u) - (int)(0.714 * v);
            int b = y + (int)(1.772 * u);
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            rgb[px * 3 + 0] = r;
            rgb[px * 3 + 1] = g;
            rgb[px * 3 + 2] = b;
        }
    }
    char path[256];
    snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_%s_yuyv_%dx%d.ppm",
             tag, w, h);
    write_ppm(path, rgb, w, h);
    free(rgb);
}

// ─── Test: Bit-level analysis of first 128 bytes ──────────────
static void analyze_bits(const uint8_t *data, int len) {
    printf("  Bit-level first 16 bytes:\n");
    for (int i = 0; i < 16 && i < len; i++) {
        printf("  %3d [%02X]: ", i, data[i]);
        for (int b = 7; b >= 0; b--)
            printf("%d", (data[i] >> b) & 1);
        printf("\n");
    }

    // Look for recurring bit patterns
    printf("  Auto-correlation (bit-level):\n");
    for (int stride = 1; stride <= 256; stride++) {
        int matches = 0, total = 0;
        for (int i = stride; i < len && i < 2048; i++) {
            for (int b = 0; b < 8; b++) {
                int bit1 = (data[i] >> b) & 1;
                int bit2 = (data[i - stride] >> b) & 1;
                if (bit1 == bit2) matches++;
                total++;
            }
        }
        double corr = (double)matches / total;
        // Only print high correlations
        if (corr > 0.6 || stride <= 8 || stride == 64 || stride == 128 || stride == 192 || stride == 256)
            printf("    stride=%3d: %.3f%s\n", stride, corr,
                   corr > 0.65 ? " ***" : "");
    }
}

// ─── Test: Treat as signed 8-bit deltas from 128 ──────────────
static void test_signed_delta(const uint8_t *data, int len, int w, int h, const char *tag) {
    uint8_t *gray = calloc(w * h, 1);
    int val = 128;
    for (int i = 0; i < len && i < w * h; i++) {
        int delta = (int8_t)data[i];
        val += delta;
        if (val < 0) val = 0;
        if (val > 255) val = 255;
        gray[i] = val;
    }
    char path[256];
    snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_%s_sdelta_%dx%d.pgm",
             tag, w, h);
    write_pgm(path, gray, w, h);
    free(gray);
}

// ─── Test: Skip sub-header, try various interpretations ─────────
static void test_skip_subheader(const uint8_t *bs, int bs_len, const char *tag) {
    // Sub-header is 00 80 24 XX (4 bytes)
    // Data starts at bs+4
    if (bs_len < 8) return;

    printf("\n  Sub-header: %02X %02X %02X %02X\n", bs[0], bs[1], bs[2], bs[3]);

    const uint8_t *data = bs + 4;
    int dlen = bs_len - 4;

    char stag[64];

    // Test at width=128, 4bpp → height = dlen*2/128 = dlen/64
    int h128_4 = dlen * 2 / 128;
    if (h128_4 > 0) {
        snprintf(stag, sizeof stag, "%s_skip4", tag);
        test_4bpp_variants(data, dlen, 128, h128_4 > 192 ? 192 : h128_4, stag);
    }

    // 8bpp raw at width=128
    int h128_8 = dlen / 128;
    snprintf(stag, sizeof stag, "%s_skip4", tag);
    test_raw_widths(data, dlen, stag);

    // Signed delta
    snprintf(stag, sizeof stag, "%s_skip4", tag);
    test_signed_delta(data, dlen, 128, h128_8 > 96 ? 96 : h128_8, stag);

    // YUV420 at 128×80 (128*80*1.5 = 15360 → close to 12242)
    // Actually 128×h where h = dlen/(128*1.5) = dlen/192
    int h_yuv = dlen * 2 / (128 * 3); // dlen / (w * 1.5) * 2/2
    h_yuv = (h_yuv / 2) * 2; // must be even
    if (h_yuv > 4) {
        snprintf(stag, sizeof stag, "%s_skip4", tag);
        test_yuv420(data, dlen, 128, h_yuv, stag);
    }

    // YUYV at 128×(dlen/128) — need dlen/2 pixels
    int h_yuyv = dlen / 128;  // 2 bytes per pixel pair, but packed differently
    if (h_yuyv > 4) {
        snprintf(stag, sizeof stag, "%s_skip4", tag);
        test_yuyv(data, dlen, 128, h_yuyv / 2, stag);
    }
}

// ─── Test: What if the 00 80 at bs[0:1] is a repeat of the ──────
//     frame marker, and there's ANOTHER qtable or param block?
static void test_double_header(const uint8_t *bs, int bs_len, const char *tag) {
    // Check if bs starts with 00 80
    if (bs_len < 40 || bs[0] != 0x00 || bs[1] != 0x80) return;

    printf("\n  Double-header analysis:\n");
    printf("  Bytes 0-7:  %02X %02X %02X %02X %02X %02X %02X %02X\n",
           bs[0], bs[1], bs[2], bs[3], bs[4], bs[5], bs[6], bs[7]);

    // Maybe: 00 80 = marker, 24 = width/4 (=36→144 pixels?) or another meaning
    // 00 = something, then 7F 06 19 60...

    // Try offset 2 (skip just "00 80")
    const uint8_t *d2 = bs + 2;
    int l2 = bs_len - 2;
    char stag[64];
    snprintf(stag, sizeof stag, "%s_off2", tag);
    test_raw_widths(d2, l2, stag);

    // Try offset 4 (skip "00 80 24 XX")
    // Already done in test_skip_subheader

    // Try offset 8
    if (bs_len > 8) {
        const uint8_t *d8 = bs + 8;
        int l8 = bs_len - 8;
        snprintf(stag, sizeof stag, "%s_off8", tag);
        test_raw_widths(d8, l8, stag);
    }

    // Hypothesis: 0x24 = 36 pixel rows are encoded as a "stripe"
    // Each stripe has some fixed structure
    // 12242 bytes / 36 stripes = 340 bytes per stripe?
    // 340 bytes at 128 wide = 2.65 bytes per pixel... doesn't fit nicely

    // Or: 0x24 could be unrelated. Let me check if the pattern holds
    // across all frames (we know it does from the analyzer output)
}

// ─── Compare two frames to find which bytes change ─────────────
static uint8_t frame_a[MAX_FRAME], frame_b[MAX_FRAME];
static int frame_a_len = 0, frame_b_len = 0;

static void compare_frames(void) {
    if (frame_a_len == 0 || frame_b_len == 0) return;
    if (frame_a_len != frame_b_len) {
        printf("\n  Frames differ in length: %d vs %d\n", frame_a_len, frame_b_len);
    }
    int min_len = frame_a_len < frame_b_len ? frame_a_len : frame_b_len;
    int diff_count = 0;
    int first_diff = -1, last_diff = -1;
    for (int i = 0; i < min_len; i++) {
        if (frame_a[i] != frame_b[i]) {
            diff_count++;
            if (first_diff < 0) first_diff = i;
            last_diff = i;
        }
    }
    printf("\n  Frame comparison: %d bytes differ (first=%d last=%d of %d)\n",
           diff_count, first_diff, last_diff, min_len);

    // Show diff distribution in 256-byte blocks
    printf("  Diff density per 256-byte block:\n  ");
    for (int block = 0; block < min_len; block += 256) {
        int d = 0;
        int end = block + 256;
        if (end > min_len) end = min_len;
        for (int i = block; i < end; i++)
            if (frame_a[i] != frame_b[i]) d++;
        if (d > 0) printf("[%d:%d] ", block/256, d);
    }
    printf("\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <game.zip> [start_lba]\n", argv[0]);
        return 1;
    }

    int start_lba = argc > 2 ? atoi(argv[2]) : 500;

    // Open zip
    int err;
    zip_t *z = zip_open(argv[1], ZIP_RDONLY, &err);
    if (!z) { fprintf(stderr, "Cannot open zip\n"); return 1; }

    // Find largest .bin
    int n_entries = (int)zip_get_num_entries(z, 0);
    int best_idx = -1;
    zip_uint64_t best_size = 0;
    for (int i = 0; i < n_entries; i++) {
        zip_stat_t st;
        if (zip_stat_index(z, i, 0, &st) == 0 && st.size > best_size) {
            best_size = st.size;
            best_idx = i;
        }
    }

    zip_stat_t st;
    zip_stat_index(z, best_idx, 0, &st);
    printf("Using: %s (%llu bytes)\n", st.name, (unsigned long long)st.size);

    zip_file_t *zf = zip_fopen_index(z, best_idx, 0);
    uint8_t *disc = malloc(st.size);
    zip_int64_t total_read = 0;
    while (total_read < (zip_int64_t)st.size) {
        zip_int64_t r = zip_fread(zf, disc + total_read, st.size - total_read);
        if (r <= 0) break;
        total_read += r;
    }
    zip_fclose(zf);

    int total_sectors = (int)(st.size / SECTOR_RAW);
    int frames_found = 0;
    bool in_frame = false;

    for (int lba = start_lba; lba < total_sectors && frames_found < 6; lba++) {
        uint8_t *sector = disc + (long)lba * SECTOR_RAW;
        if (sector[0] != 0x00 || sector[1] != 0xFF) continue;
        uint8_t mode = sector[15];
        if (mode != 2) continue;
        uint8_t submode = sector[18];
        uint8_t marker  = sector[24];

        if (submode & 0x04) continue; // skip audio

        if (marker == 0xF3) {
            frame_pos = 0; in_frame = false; continue;
        }

        if (marker == 0xF1) {
            if (!in_frame) {
                in_frame = true;
                frame_pos = 0;
            }
            int data_len = (submode & 0x20) ? 2335 : 2047;
            if (frame_pos + data_len < MAX_FRAME) {
                memcpy(frame_buf + frame_pos, sector + 25, data_len);
                frame_pos += data_len;
            }
            continue;
        }

        if (marker == 0xF2 && in_frame) {
            // Only process standard frames (00 80 header)
            if (frame_pos < 36 || frame_buf[0] != 0x00 || frame_buf[1] != 0x80) {
                printf("LBA %d: F2 but non-standard header: %02X %02X %02X %02X (size=%d), skipping\n",
                       lba, frame_buf[0], frame_buf[1], frame_buf[2], frame_buf[3], frame_pos);
                in_frame = false; frame_pos = 0;
                continue;
            }

            uint8_t qscale = frame_buf[3];
            printf("\n╔══ FRAME %d (LBA %d) ══════════════════════════════\n",
                   frames_found, lba);
            printf("║ Size: %d bytes, QScale: %d\n", frame_pos, qscale);
            printf("║ Header: %02X %02X %02X %02X\n",
                   frame_buf[0], frame_buf[1], frame_buf[2], frame_buf[3]);

            const uint8_t *bs = frame_buf + 36;
            int bs_len = frame_pos - 36;

            char tag[32];
            snprintf(tag, sizeof tag, "t%d", frames_found);

            // Bit-level analysis
            analyze_bits(bs, bs_len);

            // Main tests
            test_skip_subheader(bs, bs_len, tag);
            test_double_header(bs, bs_len, tag);

            // Also try treating entire bitstream (including sub-header) at various widths
            test_4bpp_variants(bs, bs_len, 128, 191, tag);
            test_4bpp_variants(bs, bs_len, 256, 95, tag);

            // Try 2bpp
            {
                uint8_t *gray = calloc(256 * 192, 1);
                const uint8_t *d = bs + 4;  // skip sub-header
                int dl = bs_len - 4;
                int px = 0;
                for (int i = 0; i < dl && px + 3 < 256*192; i++) {
                    // 4 pixels per byte, 2bpp
                    gray[px++] = ((d[i] >> 6) & 3) * 85;
                    gray[px++] = ((d[i] >> 4) & 3) * 85;
                    gray[px++] = ((d[i] >> 2) & 3) * 85;
                    gray[px++] = (d[i] & 3) * 85;
                }
                char path[256];
                snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_%s_2bpp_256x192.pgm", tag);
                write_pgm(path, gray, 256, 192);
                free(gray);
            }

            // Save frame for comparison
            if (frames_found == 0) {
                memcpy(frame_a, frame_buf, frame_pos);
                frame_a_len = frame_pos;
            } else if (frames_found == 1) {
                memcpy(frame_b, frame_buf, frame_pos);
                frame_b_len = frame_pos;
                compare_frames();
            }

            printf("╚════════════════════════════════════════════════\n");
            frames_found++;
            in_frame = false;
            frame_pos = 0;
        }
    }

    free(disc);
    zip_close(z);
    printf("\nDone. %d frames processed.\n", frames_found);
    return 0;
}
