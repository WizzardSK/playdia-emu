/*
 * pframe_sequence.c — Analyze frame type sequences and test pixel-domain
 * P-frame prediction on Playdia video.
 *
 * Part 1: Print first 200 packets' type bytes as a sequence (20 per line),
 *         with [F3] markers for scene transitions.
 * Part 2: For ~50 consecutive packets around a scene transition, decode
 *         each as I-frame and also test pixel-domain and coefficient-domain
 *         P-frame addition.
 *
 * Usage: ./pframe_sequence <file.cue or Track2.bin>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <strings.h>

#define SECTOR_RAW  2352
#define MAX_FRAME   65536
#define MAX_PACKETS 2048
#define PD_W        256
#define PD_H        144
#define PD_MW       (PD_W / 16)
#define PD_MH       (PD_H / 16)
#define PD_NBLOCKS  (PD_MW * PD_MH * 6)  /* 16 * 9 * 6 = 864 */
#define PI          3.14159265358979323846
#define OUT_DIR     "/home/wizzard/share/GitHub/playdia-emu/tools/test_output/"

/* ---------- CUE parser ---------- */
static char *resolve_cue(const char *cuepath) {
    FILE *f = fopen(cuepath, "r");
    if (!f) return NULL;
    char line[1024], current_file[1024] = {0};
    char dir[1024] = {0};
    const char *slash = strrchr(cuepath, '/');
    if (slash) { size_t d = slash - cuepath; memcpy(dir, cuepath, d); dir[d] = '/'; dir[d+1] = 0; }
    char *result = NULL;
    int track_num = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "FILE ", 5) == 0) {
            char *q1 = strchr(p, '"');
            if (q1) { char *q2 = strchr(q1+1, '"');
                if (q2) { size_t len = q2-q1-1; memcpy(current_file, q1+1, len); current_file[len] = 0; } }
        } else if (strncmp(p, "TRACK ", 6) == 0) {
            track_num = atoi(p + 6);
            if (track_num == 2 && current_file[0]) {
                size_t total = strlen(dir) + strlen(current_file) + 1;
                result = malloc(total);
                snprintf(result, total, "%s%s", dir, current_file);
                break;
            }
        }
    }
    fclose(f);
    return result;
}

/* ---------- PPM writer ---------- */
static void write_ppm(const char *p, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(p, "wb"); if (!f) { perror(p); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, w * h * 3, f); fclose(f);
    printf("  -> %s\n", p);
}

/* ---------- Bitstream reader ---------- */
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

/* ---------- DC VLC table ---------- */
static const struct { int len; uint32_t code; } vlc_t[17] = {
    {3,0x4},{2,0x0},{2,0x1},{3,0x5},{3,0x6},{4,0xE},{5,0x1E},{6,0x3E},{7,0x7E},
    {8,0xFE},{9,0x1FE},{10,0x3FE},{11,0x7FE},{12,0xFFE},{13,0x1FFE},{14,0x3FFE},{15,0x7FFE}
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
            if (val < (1 << (i - 1))) val -= (1 << i) - 1;
            return val;
        }
    }
    return -9999;
}

/* ---------- Zigzag scan order ---------- */
static const int zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

/* ---------- Frame decoder (DC DPCM + AC run-level) ---------- */
static int decode_frame(BS *b, int coeff[PD_NBLOCKS][64]) {
    int dc_count = 0;
    memset(coeff, 0, sizeof(int) * PD_NBLOCKS * 64);
    int dc_pred[3] = {0, 0, 0};

    /* Phase 1: DC coefficients */
    for (int mb = 0; mb < PD_MW * PD_MH && b->pos < b->total_bits; mb++)
        for (int bl = 0; bl < 6 && b->pos < b->total_bits; bl++) {
            int comp = (bl < 4) ? 0 : (bl == 4) ? 1 : 2;
            int diff = read_vlc(b);
            if (diff == -9999) goto done;
            dc_pred[comp] += diff;
            coeff[dc_count][0] = dc_pred[comp];
            dc_count++;
        }
done:
    if (dc_count < 6) return 0;

    /* Phase 2: AC coefficients */
    for (int bi = 0; bi < dc_count && b->pos < b->total_bits; bi++) {
        int k = 1;
        while (k < 64 && b->pos < b->total_bits) {
            int peek = bs_peek(b, 6);
            if (peek < 0) break;
            if (peek == 0) { b->pos += 6; break; }
            int run = 0, ok = 1;
            while (run < 5 && b->pos < b->total_bits) {
                int bit = bs_bit(b);
                if (bit < 0) { ok = 0; break; }
                if (bit == 1) break;
                run++;
            }
            if (!ok) break;
            int p3 = bs_peek(b, 3);
            if (p3 == 4) { b->pos += 3; break; }
            int level = read_vlc(b);
            if (level == -9999) break;
            k += run;
            if (k < 64) coeff[bi][k] = level;
            k++;
        }
    }
    return dc_count;
}

/* ---------- Clamp ---------- */
static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

/* ---------- IDCT of one 8x8 block: coefficients -> pixel values (signed, centered at 0) ---------- */
static void idct_block(const int coeff64[64], double pixels[8][8]) {
    /* De-zigzag */
    double matrix[8][8];
    memset(matrix, 0, sizeof(matrix));
    for (int k = 0; k < 64; k++) {
        int row = zigzag[k] / 8, col = zigzag[k] % 8;
        matrix[row][col] = coeff64[k];
    }
    /* Row pass */
    double temp[8][8];
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            double sum = 0;
            for (int k = 0; k < 8; k++) {
                double ck = (k == 0) ? (1.0 / sqrt(2.0)) : 1.0;
                sum += ck * matrix[r][k] * cos((2*c+1) * k * PI / 16.0);
            }
            temp[r][c] = sum / 2.0;
        }
    /* Column pass */
    for (int c = 0; c < 8; c++)
        for (int r = 0; r < 8; r++) {
            double sum = 0;
            for (int k = 0; k < 8; k++) {
                double ck = (k == 0) ? (1.0 / sqrt(2.0)) : 1.0;
                sum += ck * temp[k][c] * cos((2*r+1) * k * PI / 16.0);
            }
            pixels[r][c] = sum / 2.0;
        }
}

/* ---------- Render coefficients to YCbCr pixel planes ---------- */
/* Stores signed pixel values (not offset by 128) for P-frame math */
static void coeff_to_planes_signed(int coeff[PD_NBLOCKS][64], int dc_count,
                                    int16_t Y[PD_H][PD_W],
                                    int16_t Cb[PD_H/2][PD_W/2],
                                    int16_t Cr[PD_H/2][PD_W/2]) {
    memset(Y, 0, sizeof(int16_t) * PD_H * PD_W);
    memset(Cb, 0, sizeof(int16_t) * (PD_H/2) * (PD_W/2));
    memset(Cr, 0, sizeof(int16_t) * (PD_H/2) * (PD_W/2));

    for (int i = 0; i < dc_count; i++) {
        int mb = i / 6, bl = i % 6;
        int mx = mb % PD_MW, my = mb / PD_MW;

        double pixels[8][8];
        idct_block(coeff[i], pixels);

        if (bl < 4) {
            int bx = mx * 16 + (bl & 1) * 8;
            int by = my * 16 + (bl >> 1) * 8;
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++)
                    if (by+r < PD_H && bx+c < PD_W)
                        Y[by+r][bx+c] = clamp16((int)round(pixels[r][c]));
        } else if (bl == 4) {
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++)
                    if (my*8+r < PD_H/2 && mx*8+c < PD_W/2)
                        Cb[my*8+r][mx*8+c] = clamp16((int)round(pixels[r][c]));
        } else {
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++)
                    if (my*8+r < PD_H/2 && mx*8+c < PD_W/2)
                        Cr[my*8+r][mx*8+c] = clamp16((int)round(pixels[r][c]));
        }
    }
}

/* ---------- Render coefficients to RGB (standard I-frame decode) ---------- */
static void render_iframe(int coeff[PD_NBLOCKS][64], int dc_count, uint8_t *rgb) {
    int16_t Y[PD_H][PD_W], Cb[PD_H/2][PD_W/2], Cr[PD_H/2][PD_W/2];
    coeff_to_planes_signed(coeff, dc_count, Y, Cb, Cr);

    for (int y = 0; y < PD_H; y++)
        for (int x = 0; x < PD_W; x++) {
            int yv = Y[y][x] + 128;
            int cb = Cb[y/2][x/2];  /* already signed (centered at 0) */
            int cr = Cr[y/2][x/2];
            int idx = (y * PD_W + x) * 3;
            rgb[idx + 0] = clamp8(yv + (int)(1.402 * cr));
            rgb[idx + 1] = clamp8(yv - (int)(0.344 * cb + 0.714 * cr));
            rgb[idx + 2] = clamp8(yv + (int)(1.772 * cb));
        }
}

/* ---------- YCbCr planes to RGB ---------- */
static void planes_to_rgb(const int16_t Y[PD_H][PD_W],
                           const int16_t Cb[PD_H/2][PD_W/2],
                           const int16_t Cr[PD_H/2][PD_W/2],
                           uint8_t *rgb) {
    for (int y = 0; y < PD_H; y++)
        for (int x = 0; x < PD_W; x++) {
            int yv = Y[y][x];
            int cb = Cb[y/2][x/2];
            int cr = Cr[y/2][x/2];
            int idx = (y * PD_W + x) * 3;
            rgb[idx + 0] = clamp8(yv + (int)(1.402 * cr));
            rgb[idx + 1] = clamp8(yv - (int)(0.344 * cb + 0.714 * cr));
            rgb[idx + 2] = clamp8(yv + (int)(1.772 * cb));
        }
}

/* ---------- Packet info (with F3 tracking) ---------- */
typedef struct {
    uint8_t *data;
    int      size;
    bool     f3_before;  /* was there an F3 scene marker before this packet? */
} Packet;

/* ---------- Trim trailing 0xFF padding ---------- */
static int trim_padding(const uint8_t *pkt, int size) {
    int end = size;
    while (end > 40 && pkt[end - 1] == 0xFF) end--;
    return end;
}

/* ---------- Assemble all packets from disc ---------- */
static int assemble_packets(const uint8_t *disc, int total_sectors,
                             Packet *pkts, int max_pkts) {
    int n = 0, pos = 0;
    bool in_frame = false;
    bool saw_f3 = false;

    for (int lba = 0; lba < total_sectors && n < max_pkts; lba++) {
        const uint8_t *sec = disc + (long)lba * SECTOR_RAW;
        if (sec[0] != 0x00 || sec[1] != 0xFF || sec[15] != 2) continue;
        if (sec[18] & 0x04) continue;
        if (!(sec[18] & 0x08)) continue;

        uint8_t marker = sec[24];
        if (marker == 0xF1) {
            if (!in_frame) { in_frame = true; pos = 0; }
            if (pos + 2047 < MAX_FRAME) {
                memcpy(pkts[n].data + pos, sec + 25, 2047);
                pos += 2047;
            }
        } else if (marker == 0xF2) {
            if (in_frame && pos > 0) {
                pkts[n].size = pos;
                pkts[n].f3_before = saw_f3;
                saw_f3 = false;
                n++;
                in_frame = false;
                pos = 0;
            }
        } else if (marker == 0xF3) {
            in_frame = false;
            pos = 0;
            saw_f3 = true;
        }
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.cue or Track2.bin>\n", argv[0]);
        return 1;
    }

    /* Resolve CUE if needed */
    const char *path = argv[1];
    char *resolved = NULL;
    size_t plen = strlen(path);
    if (plen > 4 && strcasecmp(path + plen - 4, ".cue") == 0) {
        resolved = resolve_cue(path);
        if (!resolved) { fprintf(stderr, "Could not find Track 2 in CUE: %s\n", path); return 1; }
        printf("Resolved CUE -> Track 2: %s\n", resolved);
        path = resolved;
    }

    /* Read disc image */
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); free(resolved); return 1; }
    fseek(f, 0, SEEK_END); long disc_size = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *disc = malloc(disc_size);
    if (!disc) { fprintf(stderr, "Out of memory\n"); fclose(f); free(resolved); return 1; }
    fread(disc, 1, disc_size, f);
    fclose(f);
    int total_sectors = (int)(disc_size / SECTOR_RAW);
    printf("Loaded %ld bytes (%d sectors)\n\n", disc_size, total_sectors);

    system("mkdir -p " OUT_DIR);

    /* Allocate packet storage */
    Packet *pkts = calloc(MAX_PACKETS, sizeof(Packet));
    if (!pkts) { fprintf(stderr, "Out of memory for packets\n"); free(disc); free(resolved); return 1; }
    for (int i = 0; i < MAX_PACKETS; i++) {
        pkts[i].data = malloc(MAX_FRAME);
        if (!pkts[i].data) { fprintf(stderr, "Out of memory for packet %d\n", i); return 1; }
    }

    int npkt = assemble_packets(disc, total_sectors, pkts, MAX_PACKETS);
    printf("Assembled %d video packets\n\n", npkt);
    free(disc);

    /* ================================================================
     * PART 1: Print first 200 packets' type bytes as a sequence
     * ================================================================ */
    printf("========================================\n");
    printf("PART 1: Frame type sequence (first 200 packets)\n");
    printf("========================================\n\n");

    int limit = npkt < 200 ? npkt : 200;
    int col = 0;
    for (int i = 0; i < limit; i++) {
        uint8_t *p = pkts[i].data;
        if (pkts[i].f3_before) {
            if (col > 0) { printf("\n"); col = 0; }
            printf("[F3]\n");
        }
        if (p[0] == 0x00 && p[1] == 0x80 && p[2] == 0x04) {
            printf("%02X ", p[39]);
        } else {
            printf("?? ");
        }
        col++;
        if (col >= 20) { printf("\n"); col = 0; }
    }
    if (col > 0) printf("\n");
    printf("\n");

    /* Print type distribution */
    int type_counts[256] = {0};
    for (int i = 0; i < npkt; i++) {
        uint8_t *p = pkts[i].data;
        if (p[0] == 0x00 && p[1] == 0x80 && p[2] == 0x04)
            type_counts[p[39]]++;
    }
    printf("Type distribution (all %d packets):\n", npkt);
    for (int t = 0; t < 256; t++)
        if (type_counts[t] > 0)
            printf("  Type 0x%02X: %d packets\n", t, type_counts[t]);
    printf("\n");

    /* ================================================================
     * PART 2: Pixel-domain P-frame test
     * ================================================================ */
    printf("========================================\n");
    printf("PART 2: Pixel-domain P-frame prediction test\n");
    printf("========================================\n\n");

    /* Find a good sequence: F3 -> type=0x00 -> type=0x06/0x07 */
    int seq_start = -1;
    for (int i = 0; i < npkt - 10; i++) {
        uint8_t *p = pkts[i].data;
        if (p[0] != 0x00 || p[1] != 0x80 || p[2] != 0x04) continue;
        /* Look for an F3-marked packet with type 0x00 followed by non-0x00 */
        if (pkts[i].f3_before && p[39] == 0x00) {
            /* Check next packet is a P-frame type */
            uint8_t *p2 = pkts[i+1].data;
            if (p2[0] == 0x00 && p2[1] == 0x80 && p2[2] == 0x04 &&
                (p2[39] == 0x06 || p2[39] == 0x07)) {
                seq_start = i;
                printf("Found scene transition at packet #%d (F3 -> type=0x00 -> type=0x%02X)\n",
                       i, p2[39]);
                break;
            }
        }
    }

    /* Fallback: find any type=0x00 followed by type=0x06 or 0x07 */
    if (seq_start < 0) {
        for (int i = 0; i < npkt - 10; i++) {
            uint8_t *p = pkts[i].data;
            if (p[0] != 0x00 || p[1] != 0x80 || p[2] != 0x04) continue;
            if (p[39] == 0x00) {
                uint8_t *p2 = pkts[i+1].data;
                if (p2[0] == 0x00 && p2[1] == 0x80 && p2[2] == 0x04 &&
                    (p2[39] == 0x06 || p2[39] == 0x07)) {
                    seq_start = i;
                    printf("Found type=0x00 -> type=0x%02X transition at packet #%d\n",
                           p2[39], i);
                    break;
                }
            }
        }
    }

    if (seq_start < 0) {
        printf("No suitable I->P frame sequence found!\n");
        goto cleanup;
    }

    /* Print the sequence around our chosen start */
    int seq_len = 50;
    if (seq_start + seq_len > npkt) seq_len = npkt - seq_start;
    if (seq_len < 10) seq_len = 10;
    if (seq_start + seq_len > npkt) seq_len = npkt - seq_start;

    printf("Sequence starting at packet #%d (length %d):\n  ", seq_start, seq_len);
    for (int i = seq_start; i < seq_start + seq_len; i++) {
        uint8_t *p = pkts[i].data;
        if (pkts[i].f3_before) printf("[F3] ");
        if (p[0] == 0x00 && p[1] == 0x80 && p[2] == 0x04)
            printf("%02X ", p[39]);
        else
            printf("?? ");
        if ((i - seq_start + 1) % 20 == 0) printf("\n  ");
    }
    printf("\n\n");

    /* Now process frames: decode each as I-frame, and for non-0x00 types
     * try pixel-domain and coefficient-domain P-frame addition */
    static int coeff_cur[PD_NBLOCKS][64];
    static int coeff_ref[PD_NBLOCKS][64];  /* reference (previous) frame coefficients */

    /* Previous frame pixel planes (for pixel-domain addition) */
    static int16_t ref_Y[PD_H][PD_W], ref_Cb[PD_H/2][PD_W/2], ref_Cr[PD_H/2][PD_W/2];
    static int16_t cur_Y[PD_H][PD_W], cur_Cb[PD_H/2][PD_W/2], cur_Cr[PD_H/2][PD_W/2];
    static uint8_t rgb[PD_W * PD_H * 3];

    bool have_ref = false;
    int output_count = 0;

    /* Process up to seq_len frames, output at least 10 around the transition */
    int render_start = seq_start;
    int render_end = seq_start + seq_len;
    /* Limit PPM output to ~20 frames to keep it manageable */
    int max_output = 20;

    for (int i = render_start; i < render_end && output_count < max_output; i++) {
        uint8_t *pkt = pkts[i].data;
        if (pkt[0] != 0x00 || pkt[1] != 0x80 || pkt[2] != 0x04) continue;

        int QS = pkt[3];
        int type = pkt[39];
        int data_end = trim_padding(pkt, pkts[i].size);
        BS bs = { pkt + 40, (data_end - 40) * 8, 0 };

        int dc = decode_frame(&bs, coeff_cur);
        printf("Packet #%03d: type=0x%02X QS=%d decoded=%d/%d bits=%d/%d%s\n",
               i, type, QS, dc, PD_NBLOCKS, bs.pos, bs.total_bits,
               pkts[i].f3_before ? " [F3]" : "");

        if (dc < PD_NBLOCKS) {
            printf("  WARNING: incomplete decode (%d/%d blocks), skipping\n", dc, PD_NBLOCKS);
            continue;
        }

        /* 1) Standard I-frame decode */
        render_iframe(coeff_cur, dc, rgb);
        char path_buf[512];
        snprintf(path_buf, sizeof(path_buf),
                 OUT_DIR "seq_%03d_type%02X_iframe.ppm", i, type);
        write_ppm(path_buf, rgb, PD_W, PD_H);

        /* Compute signed pixel planes for current frame */
        coeff_to_planes_signed(coeff_cur, dc, cur_Y, cur_Cb, cur_Cr);

        /* 2) If this is a P-frame type and we have a reference, try pixel-domain addition */
        if (type != 0x00 && have_ref) {
            /* pixel_P = pixel_ref + (decoded_value - 128)
             * where decoded_value = IDCT(coeff) + 128 (standard I-frame pixel)
             * so: pixel_P = pixel_ref + IDCT(coeff)
             * In our signed representation: IDCT gives signed values,
             * ref is signed (centered at 0).
             * Result in unsigned: (ref + 128) + signed_decoded = ref + 128 + signed_decoded
             */
            static int16_t pix_Y[PD_H][PD_W], pix_Cb[PD_H/2][PD_W/2], pix_Cr[PD_H/2][PD_W/2];

            /* Method: pixel_P = ref_pixel_unsigned + (cur_decoded_unsigned - 128)
             * = (ref_signed + 128) + cur_signed
             * In unsigned space: clamp(ref_unsigned + cur_signed)
             * For planes_to_rgb, we need unsigned values.
             */
            for (int y = 0; y < PD_H; y++)
                for (int x = 0; x < PD_W; x++)
                    pix_Y[y][x] = clamp8((ref_Y[y][x] + 128) + cur_Y[y][x]) ;
            for (int y = 0; y < PD_H/2; y++)
                for (int x = 0; x < PD_W/2; x++) {
                    pix_Cb[y][x] = (ref_Cb[y][x]) + cur_Cb[y][x];
                    pix_Cr[y][x] = (ref_Cr[y][x]) + cur_Cr[y][x];
                }

            /* Convert to RGB - but planes_to_rgb expects:
             * Y as unsigned (0-255 range), Cb/Cr as signed (-128..127)
             * Our pix_Y is already unsigned, pix_Cb/Cr are signed sums */
            for (int y = 0; y < PD_H; y++)
                for (int x = 0; x < PD_W; x++) {
                    int yv = pix_Y[y][x];
                    int cb = pix_Cb[y/2][x/2];
                    int cr = pix_Cr[y/2][x/2];
                    int idx = (y * PD_W + x) * 3;
                    rgb[idx + 0] = clamp8(yv + (int)(1.402 * cr));
                    rgb[idx + 1] = clamp8(yv - (int)(0.344 * cb + 0.714 * cr));
                    rgb[idx + 2] = clamp8(yv + (int)(1.772 * cb));
                }

            snprintf(path_buf, sizeof(path_buf),
                     OUT_DIR "seq_%03d_type%02X_pixadd.ppm", i, type);
            write_ppm(path_buf, rgb, PD_W, PD_H);
        }

        /* 3) Coefficient-domain P-frame: add DCT coefficients of current to reference */
        if (type != 0x00 && have_ref) {
            static int coeff_sum[PD_NBLOCKS][64];
            for (int b = 0; b < PD_NBLOCKS; b++)
                for (int k = 0; k < 64; k++)
                    coeff_sum[b][k] = coeff_ref[b][k] + coeff_cur[b][k];

            render_iframe(coeff_sum, PD_NBLOCKS, rgb);
            snprintf(path_buf, sizeof(path_buf),
                     OUT_DIR "seq_%03d_type%02X_dctadd.ppm", i, type);
            write_ppm(path_buf, rgb, PD_W, PD_H);
        }

        /* Save current frame as reference for next iteration */
        memcpy(coeff_ref, coeff_cur, sizeof(int) * PD_NBLOCKS * 64);
        memcpy(ref_Y, cur_Y, sizeof(ref_Y));
        memcpy(ref_Cb, cur_Cb, sizeof(ref_Cb));
        memcpy(ref_Cr, cur_Cr, sizeof(ref_Cr));
        have_ref = true;

        /* Reset reference on scene markers or I-frames */
        if (pkts[i].f3_before) {
            /* After F3, next type=0x00 is a fresh I-frame reference */
        }

        output_count++;
    }

    printf("\nGenerated %d frame sets.\n", output_count);

cleanup:
    for (int i = 0; i < MAX_PACKETS; i++) free(pkts[i].data);
    free(pkts);
    free(resolved);
    return 0;
}
