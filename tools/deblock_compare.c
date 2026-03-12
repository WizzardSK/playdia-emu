/*
 * deblock_compare.c — Render frames with and without deblocking filter
 *
 * Parses the CUE file to find Track 2 bin, assembles first 16 packets,
 * then for packets 0, 4, 7 renders three variants:
 *   - No deblocking
 *   - Mild deblock (threshold 4, adj = d/4)
 *   - Strong deblock (threshold 2, adj = d/3)
 *
 * Outputs individual PPMs and side-by-side strip for each packet.
 *
 * Usage: ./deblock_compare <cue_file>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#define SECTOR_RAW  2352
#define MAX_FRAME   (512 * 1024)
#define PD_W        256
#define PD_H        144
#define PD_MW       (PD_W / 16)   /* 16 */
#define PD_MH       (PD_H / 16)   /*  9 */
#define PD_NBLOCKS  (PD_MW * PD_MH * 6)  /* 864 */
#define PI          3.14159265358979323846
#define OUT_DIR     "/home/wizzard/share/GitHub/playdia-emu/tools/test_output/"

/* ── PPM output ──────────────────────────────────────────────── */
static void write_ppm(const char *p, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(p, "wb");
    if (!f) { fprintf(stderr, "Cannot write %s\n", p); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, w * h * 3, f);
    fclose(f);
    printf("  -> %s\n", p);
}

/* ── Bitstream reader ────────────────────────────────────────── */
typedef struct { const uint8_t *data; int total_bits, pos; } BS;

static int bs_peek(BS *b, int n) {
    if (n <= 0 || b->pos + n > b->total_bits) return -1;
    int v = 0;
    for (int i = 0; i < n; i++) {
        int bp = b->pos + i;
        v = (v << 1) | ((b->data[bp >> 3] >> (7 - (bp & 7))) & 1);
    }
    return v;
}
static int bs_read(BS *b, int n) {
    if (n <= 0) return 0;
    if (b->pos + n > b->total_bits) return -1;
    int v = 0;
    for (int i = 0; i < n; i++) {
        int bp = b->pos + i;
        v = (v << 1) | ((b->data[bp >> 3] >> (7 - (bp & 7))) & 1);
    }
    b->pos += n;
    return v;
}
static int bs_bit(BS *b) {
    if (b->pos >= b->total_bits) return -1;
    int bp = b->pos++;
    return (b->data[bp >> 3] >> (7 - (bp & 7))) & 1;
}

/* ── VLC table (extended MPEG-1 DC, sizes 0-16) ─────────────── */
static const struct { int len; uint32_t code; } vlc_t[17] = {
    {3,0x4}, {2,0x0}, {2,0x1}, {3,0x5}, {3,0x6},
    {4,0xE}, {5,0x1E}, {6,0x3E}, {7,0x7E},
    {8,0xFE}, {9,0x1FE}, {10,0x3FE},
    {11,0x7FE}, {12,0xFFE}, {13,0x1FFE}, {14,0x3FFE}, {15,0x7FFE}
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
            if (val < (1 << (i - 1)))
                val -= (1 << i) - 1;
            return val;
        }
    }
    return -9999;
}

/* ── Zigzag table ────────────────────────────────────────────── */
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

/* ── Decode one frame (DC DPCM + unary-run AC + dual EOB) ──── */
static int decode_frame(BS *bs, int coeff[PD_NBLOCKS][64]) {
    int dc_count = 0;
    memset(coeff, 0, sizeof(int) * PD_NBLOCKS * 64);

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

    for (int bi = 0; bi < dc_count && bs->pos < bs->total_bits; bi++) {
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
            if (k < 64) coeff[bi][k] = level;
            k++;
        }
    }
    return dc_count;
}

/* ── Helpers ─────────────────────────────────────────────────── */
static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* ── Render frame into Y/Cb/Cr planes (with clamp_qs dequant) ─ */
static void render_planes(int coeff_in[PD_NBLOCKS][64], int dc_count, int QS,
                           uint8_t Y[PD_H][PD_W],
                           uint8_t Cb[PD_H / 2][PD_W / 2],
                           uint8_t Cr[PD_H / 2][PD_W / 2]) {
    /* Work on copy to not modify original */
    static int coeff[PD_NBLOCKS][64];
    memcpy(coeff, coeff_in, sizeof(int) * PD_NBLOCKS * 64);

    /* Apply clamp_qs: clamp AC to +/- 2048/QS, min 32 */
    int limit = QS > 0 ? 2048 / QS : 255;
    if (limit < 32) limit = 32;
    for (int b = 0; b < dc_count; b++)
        for (int i = 1; i < 64; i++) {
            if (coeff[b][i] > limit) coeff[b][i] = limit;
            else if (coeff[b][i] < -limit) coeff[b][i] = -limit;
        }

    memset(Y, 128, PD_H * PD_W);
    memset(Cb, 128, (PD_H / 2) * (PD_W / 2));
    memset(Cr, 128, (PD_H / 2) * (PD_W / 2));

    for (int i = 0; i < dc_count; i++) {
        int mb = i / 6, bl = i % 6;
        int mx = mb % PD_MW, my = mb / PD_MW;

        /* De-zigzag into 8x8 matrix */
        double matrix[8][8];
        memset(matrix, 0, sizeof(matrix));
        for (int k = 0; k < 64; k++) {
            int row = zigzag[k] / 8, col = zigzag[k] % 8;
            matrix[row][col] = coeff[i][k];
        }

        /* 2D IDCT: horizontal pass */
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

        /* Vertical pass */
        uint8_t block[8][8];
        for (int c = 0; c < 8; c++)
            for (int r = 0; r < 8; r++) {
                double sum = 0;
                for (int k = 0; k < 8; k++) {
                    double ck = (k == 0) ? (1.0 / sqrt(2.0)) : 1.0;
                    sum += ck * temp[k][c] * cos((2 * r + 1) * k * PI / 16.0);
                }
                block[r][c] = (uint8_t)clamp8((int)round(sum / 2.0) + 128);
            }

        /* Place block into plane */
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
}

/* ── Deblocking filter on a plane ────────────────────────────── */
/*
 * 3-tap boundary filter applied at each 8x8 block boundary.
 * For each boundary pair (pixel_before, pixel_after):
 *   d = after - before
 *   if |d| > threshold:
 *       adj = d / divisor
 *       before += adj
 *       after  -= adj
 */
static void deblock_plane(uint8_t *plane, int w, int h, int threshold, int divisor) {
    /* Horizontal block boundaries (vertical edges): x = 8, 16, 24, ... */
    for (int y = 0; y < h; y++) {
        for (int bx = 8; bx < w; bx += 8) {
            int before_idx = y * w + (bx - 1);
            int after_idx  = y * w + bx;
            int pb = plane[before_idx];
            int pa = plane[after_idx];
            int d = pa - pb;
            if (d > threshold || d < -threshold) {
                int adj = d / divisor;
                plane[before_idx] = (uint8_t)clamp8(pb + adj);
                plane[after_idx]  = (uint8_t)clamp8(pa - adj);
            }
        }
    }

    /* Vertical block boundaries (horizontal edges): y = 8, 16, 24, ... */
    for (int by = 8; by < h; by += 8) {
        for (int x = 0; x < w; x++) {
            int before_idx = (by - 1) * w + x;
            int after_idx  = by * w + x;
            int pb = plane[before_idx];
            int pa = plane[after_idx];
            int d = pa - pb;
            if (d > threshold || d < -threshold) {
                int adj = d / divisor;
                plane[before_idx] = (uint8_t)clamp8(pb + adj);
                plane[after_idx]  = (uint8_t)clamp8(pa - adj);
            }
        }
    }
}

/* ── YCbCr to RGB conversion ─────────────────────────────────── */
static void ycbcr_to_rgb(const uint8_t Y[PD_H][PD_W],
                          const uint8_t Cb[PD_H / 2][PD_W / 2],
                          const uint8_t Cr[PD_H / 2][PD_W / 2],
                          uint8_t *rgb) {
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
}

/* ── CUE parser: extract Track 2 bin filename ────────────────── */
static int parse_cue_track2(const char *cue_path, char *bin_path, int bin_path_size) {
    FILE *f = fopen(cue_path, "r");
    if (!f) { perror(cue_path); return -1; }

    /* Extract directory from CUE path */
    char dir[1024] = "";
    const char *slash = strrchr(cue_path, '/');
    if (slash) {
        int dlen = (int)(slash - cue_path);
        if (dlen >= (int)sizeof(dir)) dlen = (int)sizeof(dir) - 1;
        memcpy(dir, cue_path, dlen);
        dir[dlen] = '\0';
    } else {
        strcpy(dir, ".");
    }

    char line[1024];
    char current_file[1024] = "";
    int current_track = 0;
    bool found = false;

    while (fgets(line, sizeof(line), f)) {
        /* Parse FILE "filename" BINARY */
        char *fp = strstr(line, "FILE ");
        if (fp) {
            char *q1 = strchr(fp, '"');
            if (q1) {
                char *q2 = strchr(q1 + 1, '"');
                if (q2) {
                    int len = (int)(q2 - q1 - 1);
                    if (len >= (int)sizeof(current_file)) len = (int)sizeof(current_file) - 1;
                    memcpy(current_file, q1 + 1, len);
                    current_file[len] = '\0';
                }
            }
        }
        /* Parse TRACK NN */
        char *tp = strstr(line, "TRACK ");
        if (tp) {
            current_track = atoi(tp + 6);
            if (current_track == 2 && current_file[0]) {
                snprintf(bin_path, bin_path_size, "%s/%s", dir, current_file);
                found = true;
                break;
            }
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

/* ── Assemble video packets from raw disc sectors ────────────── */
static int assemble_packets(const uint8_t *disc, int total_sectors, int start_lba,
                             uint8_t **packets, int sizes[], int max_packets) {
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
            if (pos + 2047 < MAX_FRAME) {
                memcpy(packets[n] + pos, sec + 25, 2047);
                pos += 2047;
            }
        } else if (marker == 0xF2) {
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

/* ── Main ────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    const char *cue_path = "/home/wizzard/share/GitHub/playdia-roms/"
        "Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku - Uchuu-hen (Japan).cue";

    if (argc > 1) cue_path = argv[1];

    /* Parse CUE to find Track 2 bin */
    char bin_path[2048];
    if (parse_cue_track2(cue_path, bin_path, sizeof(bin_path)) < 0) {
        fprintf(stderr, "Failed to parse CUE or find Track 2\n");
        return 1;
    }
    printf("Track 2 bin: %s\n", bin_path);

    /* Load disc image */
    FILE *f = fopen(bin_path, "rb");
    if (!f) { perror(bin_path); return 1; }
    fseek(f, 0, SEEK_END);
    long disc_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *disc = malloc(disc_size);
    if (!disc) { fprintf(stderr, "OOM\n"); return 1; }
    fread(disc, 1, disc_size, f);
    fclose(f);

    int total_sectors = (int)(disc_size / SECTOR_RAW);
    printf("Disc: %ld bytes, %d sectors\n", disc_size, total_sectors);

    system("mkdir -p " OUT_DIR);

    /* Allocate packet buffers */
    uint8_t *packet_bufs[16];
    int sizes[16];
    for (int i = 0; i < 16; i++) {
        packet_bufs[i] = malloc(MAX_FRAME);
        if (!packet_bufs[i]) { fprintf(stderr, "OOM\n"); return 1; }
    }

    int npkt = assemble_packets(disc, total_sectors, 0, packet_bufs, sizes, 16);
    printf("Assembled %d video packets\n\n", npkt);
    free(disc);

    /* Decode coefficients for all 16 packets */
    static int all_coeff[16][PD_NBLOCKS][64];
    int all_dc_count[16];
    int all_qs[16];
    int all_type[16];

    for (int pi = 0; pi < npkt; pi++) {
        uint8_t *pkt = packet_bufs[pi];
        if (pkt[0] != 0x00 || pkt[1] != 0x80 || pkt[2] != 0x04) {
            all_dc_count[pi] = 0;
            continue;
        }
        all_qs[pi] = pkt[3];
        all_type[pi] = pkt[39];

        int data_end = sizes[pi];
        while (data_end > 40 && pkt[data_end - 1] == 0xFF) data_end--;

        BS bs = { pkt + 40, (data_end - 40) * 8, 0 };
        all_dc_count[pi] = decode_frame(&bs, all_coeff[pi]);

        printf("Packet %02d: QS=%2d, type=%d, size=%d, dc_count=%d, bits=%d/%d\n",
               pi, all_qs[pi], all_type[pi], sizes[pi],
               all_dc_count[pi], bs.pos, bs.total_bits);
    }
    printf("\n");

    /* Process target packets: 0, 4, 7 */
    int target_pkts[] = {0, 4, 7};
    int ntargets = 3;

    for (int ti = 0; ti < ntargets; ti++) {
        int pi = target_pkts[ti];
        if (pi >= npkt) {
            printf("Packet %d not available (only %d packets)\n", pi, npkt);
            continue;
        }
        if (all_dc_count[pi] < PD_NBLOCKS) {
            printf("Packet %d: only %d blocks decoded (need %d), skipping\n",
                   pi, all_dc_count[pi], PD_NBLOCKS);
            continue;
        }

        int QS = all_qs[pi];
        printf("=== Packet %02d: QS=%d, type=%d ===\n", pi, QS, all_type[pi]);

        /* Render three variants */
        static uint8_t Y[PD_H][PD_W], Cb[PD_H / 2][PD_W / 2], Cr[PD_H / 2][PD_W / 2];
        static uint8_t rgb_none[PD_W * PD_H * 3];
        static uint8_t rgb_mild[PD_W * PD_H * 3];
        static uint8_t rgb_strong[PD_W * PD_H * 3];

        /* (a) No deblocking */
        render_planes(all_coeff[pi], all_dc_count[pi], QS,
                      Y, Cb, Cr);
        ycbcr_to_rgb(Y, Cb, Cr, rgb_none);

        /* (b) Mild deblock: threshold=4, divisor=4 */
        render_planes(all_coeff[pi], all_dc_count[pi], QS,
                      Y, Cb, Cr);
        deblock_plane(&Y[0][0], PD_W, PD_H, 4, 4);
        deblock_plane(&Cb[0][0], PD_W / 2, PD_H / 2, 4, 4);
        deblock_plane(&Cr[0][0], PD_W / 2, PD_H / 2, 4, 4);
        ycbcr_to_rgb(Y, Cb, Cr, rgb_mild);

        /* (c) Strong deblock: threshold=2, divisor=3 */
        render_planes(all_coeff[pi], all_dc_count[pi], QS,
                      Y, Cb, Cr);
        deblock_plane(&Y[0][0], PD_W, PD_H, 2, 3);
        deblock_plane(&Cb[0][0], PD_W / 2, PD_H / 2, 2, 3);
        deblock_plane(&Cr[0][0], PD_W / 2, PD_H / 2, 2, 3);
        ycbcr_to_rgb(Y, Cb, Cr, rgb_strong);

        /* Write individual PPMs */
        char path[512];
        snprintf(path, sizeof(path), OUT_DIR "deblock_pkt%02d_none.ppm", pi);
        write_ppm(path, rgb_none, PD_W, PD_H);

        snprintf(path, sizeof(path), OUT_DIR "deblock_pkt%02d_mild.ppm", pi);
        write_ppm(path, rgb_mild, PD_W, PD_H);

        snprintf(path, sizeof(path), OUT_DIR "deblock_pkt%02d_strong.ppm", pi);
        write_ppm(path, rgb_strong, PD_W, PD_H);

        /* Create side-by-side strip (3x wide) */
        int strip_w = PD_W * 3;
        static uint8_t rgb_strip[PD_W * 3 * PD_H * 3];

        for (int y = 0; y < PD_H; y++) {
            memcpy(rgb_strip + (y * strip_w + 0 * PD_W) * 3,
                   rgb_none + y * PD_W * 3, PD_W * 3);
            memcpy(rgb_strip + (y * strip_w + 1 * PD_W) * 3,
                   rgb_mild + y * PD_W * 3, PD_W * 3);
            memcpy(rgb_strip + (y * strip_w + 2 * PD_W) * 3,
                   rgb_strong + y * PD_W * 3, PD_W * 3);
        }

        snprintf(path, sizeof(path), OUT_DIR "deblock_pkt%02d_strip.ppm", pi);
        write_ppm(path, rgb_strip, strip_w, PD_H);

        printf("\n");
    }

    /* Cleanup */
    for (int i = 0; i < 16; i++) free(packet_bufs[i]);

    printf("Done.\n");
    return 0;
}
