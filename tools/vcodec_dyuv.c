/*
 * Playdia DYUV Video Decode Test
 *
 * Test hypothesis: Playdia uses DYUV-like encoding (similar to CD-i)
 * The Philips TDA8772AH Triple DAC on the board suggests CD-i compatibility.
 *
 * CD-i DYUV format:
 *   Each pair of bytes encodes Y/U/V deltas:
 *     Byte 0: [U_delta_nibble:4][Y0_delta_nibble:4]
 *     Byte 1: [V_delta_nibble:4][Y1_delta_nibble:4]
 *   Delta table: {0,1,4,9,16,27,44,79,128,177,212,229,240,247,252,255}
 *   (128 = no change, <128 = decrease, >128 = increase)
 *
 * We test multiple variations:
 *   - Standard CD-i DYUV delta table
 *   - Playdia custom QTable as delta table
 *   - Various width/height combos
 *   - Different nibble orderings
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

// CD-i standard DYUV delta table
static const uint8_t cdi_dyuv_delta[16] = {
    0, 1, 4, 9, 16, 27, 44, 79, 128, 177, 212, 229, 240, 247, 252, 255
};

static void write_ppm(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, w * h * 3, f);
    fclose(f);
    printf("  -> %s (%dx%d)\n", path, w, h);
}

static void write_pgm(const char *path, const uint8_t *gray, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(gray, 1, w * h, f);
    fclose(f);
    printf("  -> %s (%dx%d)\n", path, w, h);
}

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// ─── CD-i DYUV decode (standard) ──────────────────────────────
// 2 bytes → 2 pixel columns (Y0,UV → Y1,UV), with UV subsampled
// Actually in CD-i: 2 bytes → 4 pixels (with midpoint interpolation)
// Simplified: 2 bytes → 2 pixels (no interpolation)
static void dyuv_decode(const uint8_t *data, int len, int w, int h,
                        const uint8_t *delta_tab, const char *path) {
    uint8_t *rgb = calloc(w * h * 3, 1);
    int y_acc = 128, u_acc = 128, v_acc = 128;
    int px = 0;

    for (int i = 0; i + 1 < len && px + 1 < w * h; i += 2) {
        uint8_t b0 = data[i];
        uint8_t b1 = data[i + 1];

        // Byte 0: hi=U_delta, lo=Y0_delta
        int du = (int)delta_tab[b0 >> 4] - 128;
        int dy0 = (int)delta_tab[b0 & 0xF] - 128;

        // Byte 1: hi=V_delta, lo=Y1_delta
        int dv = (int)delta_tab[b1 >> 4] - 128;
        int dy1 = (int)delta_tab[b1 & 0xF] - 128;

        y_acc = clamp255(y_acc + dy0);
        u_acc = clamp255(u_acc + du);
        v_acc = clamp255(v_acc + dv);

        // YUV→RGB (BT.601)
        int y_val = y_acc - 16;
        int u_val = u_acc - 128;
        int v_val = v_acc - 128;
        int r = clamp255((298 * y_val + 409 * v_val + 128) >> 8);
        int g = clamp255((298 * y_val - 100 * u_val - 208 * v_val + 128) >> 8);
        int b = clamp255((298 * y_val + 516 * u_val + 128) >> 8);
        rgb[px * 3 + 0] = r;
        rgb[px * 3 + 1] = g;
        rgb[px * 3 + 2] = b;
        px++;

        // Second pixel with Y1
        y_acc = clamp255(y_acc + dy1);
        y_val = y_acc - 16;
        r = clamp255((298 * y_val + 409 * v_val + 128) >> 8);
        g = clamp255((298 * y_val - 100 * u_val - 208 * v_val + 128) >> 8);
        b = clamp255((298 * y_val + 516 * u_val + 128) >> 8);
        rgb[px * 3 + 0] = r;
        rgb[px * 3 + 1] = g;
        rgb[px * 3 + 2] = b;
        px++;
    }
    write_ppm(path, rgb, w, h);
    free(rgb);
}

// ─── Variant: swapped nibble order ────────────────────────────
static void dyuv_decode_swap(const uint8_t *data, int len, int w, int h,
                              const uint8_t *delta_tab, const char *path) {
    uint8_t *rgb = calloc(w * h * 3, 1);
    int y_acc = 128, u_acc = 128, v_acc = 128;
    int px = 0;

    for (int i = 0; i + 1 < len && px + 1 < w * h; i += 2) {
        uint8_t b0 = data[i];
        uint8_t b1 = data[i + 1];

        // Byte 0: lo=U_delta, hi=Y0_delta
        int du = (int)delta_tab[b0 & 0xF] - 128;
        int dy0 = (int)delta_tab[b0 >> 4] - 128;

        // Byte 1: lo=V_delta, hi=Y1_delta
        int dv = (int)delta_tab[b1 & 0xF] - 128;
        int dy1 = (int)delta_tab[b1 >> 4] - 128;

        y_acc = clamp255(y_acc + dy0);
        u_acc = clamp255(u_acc + du);
        v_acc = clamp255(v_acc + dv);

        int y_val = y_acc - 16, u_val = u_acc - 128, v_val = v_acc - 128;
        int r = clamp255((298 * y_val + 409 * v_val + 128) >> 8);
        int g = clamp255((298 * y_val - 100 * u_val - 208 * v_val + 128) >> 8);
        int b = clamp255((298 * y_val + 516 * u_val + 128) >> 8);
        rgb[px * 3 + 0] = r; rgb[px * 3 + 1] = g; rgb[px * 3 + 2] = b;
        px++;

        y_acc = clamp255(y_acc + dy1);
        y_val = y_acc - 16;
        r = clamp255((298 * y_val + 409 * v_val + 128) >> 8);
        g = clamp255((298 * y_val - 100 * u_val - 208 * v_val + 128) >> 8);
        b = clamp255((298 * y_val + 516 * u_val + 128) >> 8);
        rgb[px * 3 + 0] = r; rgb[px * 3 + 1] = g; rgb[px * 3 + 2] = b;
        px++;
    }
    write_ppm(path, rgb, w, h);
    free(rgb);
}

// ─── DYUV with row reset ─────────────────────────────────────
static void dyuv_decode_rowreset(const uint8_t *data, int len, int w, int h,
                                  const uint8_t *delta_tab, const char *path) {
    uint8_t *rgb = calloc(w * h * 3, 1);
    int di = 0;

    for (int row = 0; row < h && di + 1 < len; row++) {
        int y_acc = 128, u_acc = 128, v_acc = 128;

        for (int col = 0; col < w && di + 1 < len; col += 2) {
            uint8_t b0 = data[di++];
            uint8_t b1 = data[di++];

            int du = (int)delta_tab[b0 >> 4] - 128;
            int dy0 = (int)delta_tab[b0 & 0xF] - 128;
            int dv = (int)delta_tab[b1 >> 4] - 128;
            int dy1 = (int)delta_tab[b1 & 0xF] - 128;

            y_acc = clamp255(y_acc + dy0);
            u_acc = clamp255(u_acc + du);
            v_acc = clamp255(v_acc + dv);

            int px = row * w + col;
            int y_val = y_acc - 16, u_val = u_acc - 128, v_val = v_acc - 128;
            rgb[px * 3 + 0] = clamp255((298 * y_val + 409 * v_val + 128) >> 8);
            rgb[px * 3 + 1] = clamp255((298 * y_val - 100 * u_val - 208 * v_val + 128) >> 8);
            rgb[px * 3 + 2] = clamp255((298 * y_val + 516 * u_val + 128) >> 8);

            y_acc = clamp255(y_acc + dy1);
            px++;
            y_val = y_acc - 16;
            rgb[px * 3 + 0] = clamp255((298 * y_val + 409 * v_val + 128) >> 8);
            rgb[px * 3 + 1] = clamp255((298 * y_val - 100 * u_val - 208 * v_val + 128) >> 8);
            rgb[px * 3 + 2] = clamp255((298 * y_val + 516 * u_val + 128) >> 8);
        }
    }
    write_ppm(path, rgb, w, h);
    free(rgb);
}

// ─── Simple 1 byte = 1 pixel DYUV (Y only, nibble delta) ─────
// Each byte: hi_nibble → Y delta via table, lo_nibble → Y delta via table
// 2 pixels per byte = simple grayscale DPCM using the delta table
static void dyuv_y_only(const uint8_t *data, int len, int w, int h,
                         const uint8_t *delta_tab, const char *path,
                         bool reset_rows) {
    uint8_t *gray = calloc(w * h, 1);
    int di = 0;

    for (int row = 0; row < h; row++) {
        int y_acc = 128;
        if (!reset_rows && row > 0) y_acc = gray[(row - 1) * w]; // carry from above

        for (int col = 0; col < w && di < len; col += 2) {
            int dy0 = (int)delta_tab[data[di] >> 4] - 128;
            int dy1 = (int)delta_tab[data[di] & 0xF] - 128;
            di++;

            y_acc = clamp255(y_acc + dy0);
            gray[row * w + col] = y_acc;

            y_acc = clamp255(y_acc + dy1);
            if (col + 1 < w)
                gray[row * w + col + 1] = y_acc;
        }
    }
    write_pgm(path, gray, w, h);
    free(gray);
}

// ─── Build Playdia QTable-based delta table ───────────────────
// Scale the 16-entry QTable to 0-255 range, centered at 128
static void build_pd_delta_table(const uint8_t *qtab, uint8_t *out) {
    // Find min/max
    int mn = 255, mx = 0;
    for (int i = 0; i < 16; i++) {
        if (qtab[i] < mn) mn = qtab[i];
        if (qtab[i] > mx) mx = qtab[i];
    }
    // Scale to 0-255
    for (int i = 0; i < 16; i++) {
        if (mx == mn)
            out[i] = 128;
        else
            out[i] = (uint8_t)((qtab[i] - mn) * 255 / (mx - mn));
    }
    printf("  PD delta table: ");
    for (int i = 0; i < 16; i++) printf("%d ", out[i]);
    printf("\n");
}

// ─── Process frame ────────────────────────────────────────────
static void process_frame(int fnum) {
    if (frame_pos < 40) return;
    if (frame_buf[0] != 0x00 || frame_buf[1] != 0x80) return;

    uint8_t qscale = frame_buf[3];
    uint8_t qtab[16];
    memcpy(qtab, frame_buf + 4, 16);

    const uint8_t *bs = frame_buf + 36; // bitstream (includes sub-header)
    int bs_len = frame_pos - 36;

    printf("\n=== FRAME %d: %d bytes, qscale=%d ===\n", fnum, frame_pos, qscale);
    printf("  Sub-header: %02X %02X %02X %02X\n", bs[0], bs[1], bs[2], bs[3]);

    // Data starts after sub-header
    const uint8_t *data = bs + 4;
    int dlen = bs_len - 4;

    // Also try without sub-header skip
    const uint8_t *data0 = bs;
    int dlen0 = bs_len;

    // Build Playdia-specific delta table from QTable
    uint8_t pd_delta[16];
    build_pd_delta_table(qtab, pd_delta);

    // Test matrix: multiple dimensions × multiple decode methods
    struct { int w, h; } dims[] = {
        {128, 96}, {128, 191}, {256, 96}, {256, 48},
        {256, 191}, {160, 76}, {176, 69}, {192, 64},
        {144, 85}, {320, 38}
    };
    int ndims = sizeof(dims)/sizeof(dims[0]);

    for (int d = 0; d < ndims; d++) {
        int w = dims[d].w, h = dims[d].h;
        // Check if dimensions are plausible
        if (w * h / 2 > dlen + 100) continue;  // need ~w*h/2 bytes for DYUV
        if (w * h / 2 < dlen / 3) continue;    // too few pixels

        char path[256];
        printf("\n  --- %dx%d ---\n", w, h);

        // Standard CD-i DYUV
        snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_dy_f%d_cdi_%dx%d.ppm", fnum, w, h);
        dyuv_decode(data, dlen, w, h, cdi_dyuv_delta, path);

        // CD-i DYUV with swapped nibbles
        snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_dy_f%d_cdi_swap_%dx%d.ppm", fnum, w, h);
        dyuv_decode_swap(data, dlen, w, h, cdi_dyuv_delta, path);

        // CD-i DYUV with row reset
        snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_dy_f%d_cdi_rr_%dx%d.ppm", fnum, w, h);
        dyuv_decode_rowreset(data, dlen, w, h, cdi_dyuv_delta, path);

        // Playdia QTable as delta table
        snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_dy_f%d_pd_%dx%d.ppm", fnum, w, h);
        dyuv_decode(data, dlen, w, h, pd_delta, path);

        // Y-only with CD-i delta table (grayscale DPCM)
        snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_dy_f%d_yonly_%dx%d.pgm", fnum, w, h);
        dyuv_y_only(data, dlen, w, h, cdi_dyuv_delta, path, true);

        // Y-only with CD-i, no sub-header skip
        snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_dy_f%d_yonly0_%dx%d.pgm", fnum, w, h);
        dyuv_y_only(data0, dlen0, w, h, cdi_dyuv_delta, path, true);

        // Y-only with Playdia delta table
        snprintf(path, sizeof path, "/home/wizzard/share/GitHub/pd_dy_f%d_pdy_%dx%d.pgm", fnum, w, h);
        dyuv_y_only(data, dlen, w, h, pd_delta, path, true);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <game.zip> [start_lba]\n", argv[0]);
        return 1;
    }
    int start_lba = argc > 2 ? atoi(argv[2]) : 500;

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
    printf("Reading %s...\n", st.name);

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
    int frames_found = 0;
    bool in_frame = false;

    for (int lba = start_lba; lba < total_sectors && frames_found < 2; lba++) {
        uint8_t *sector = disc + (long)lba * SECTOR_RAW;
        if (sector[0] != 0x00 || sector[1] != 0xFF) continue;
        if (sector[15] != 2) continue;
        uint8_t submode = sector[18];
        uint8_t marker = sector[24];
        if (submode & 0x04) continue;

        if (marker == 0xF3) { frame_pos = 0; in_frame = false; continue; }
        if (marker == 0xF1) {
            if (!in_frame) { in_frame = true; frame_pos = 0; }
            int dl = (submode & 0x20) ? 2335 : 2047;
            if (frame_pos + dl < MAX_FRAME) {
                memcpy(frame_buf + frame_pos, sector + 25, dl);
                frame_pos += dl;
            }
            continue;
        }
        if (marker == 0xF2 && in_frame) {
            process_frame(frames_found);
            frames_found++;
            in_frame = false;
            frame_pos = 0;
        }
    }

    free(disc);
    zip_close(z);
    printf("\nDone.\n");
    return 0;
}
