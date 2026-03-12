/*
 * pframe_delta.c — Find longest scenes and test proper P-frame delta application
 *
 * Part 1: Scan entire disc. Count consecutive packets between F3 markers.
 *         Report top 10 longest scenes.
 * Part 2: Pick the longest scene. Decode all packets. Test three delta models
 *         for P-frame (type 0x06) with I-frames (type 0x00 / 0x07) as reference.
 *
 * Usage: ./pframe_delta <file.cue>
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
#define MAX_PACKETS 4096
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

/* ---------- DC VLC table (MPEG-1 DC luminance style) ---------- */
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

/* ---------- Frame decoder (DC DPCM + AC run-level with dual EOB) ---------- */
/* Returns number of blocks decoded (should be PD_NBLOCKS=864 for a full frame).
 * DC predictors reset to 0 each packet (standard I-frame decode). */
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

    /* Phase 2: AC coefficients (run-level with dual EOB) */
    for (int bi = 0; bi < dc_count && b->pos < b->total_bits; bi++) {
        int k = 1;
        while (k < 64 && b->pos < b->total_bits) {
            /* Check for 6-bit zero EOB */
            int peek = bs_peek(b, 6);
            if (peek < 0) break;
            if (peek == 0) { b->pos += 6; break; }

            /* Unary-coded run (0=skip, terminated by 1) */
            int run = 0, ok = 1;
            while (run < 5 && b->pos < b->total_bits) {
                int bit = bs_bit(b);
                if (bit < 0) { ok = 0; break; }
                if (bit == 1) break;
                run++;
            }
            if (!ok) break;

            /* Check for 3-bit EOB (100) */
            int p3 = bs_peek(b, 3);
            if (p3 == 4) { b->pos += 3; break; }

            /* VLC level */
            int level = read_vlc(b);
            if (level == -9999) break;
            k += run;
            if (k < 64) coeff[bi][k] = level;
            k++;
        }
    }
    return dc_count;
}

/* ---------- Clamp helpers ---------- */
static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* ---------- IDCT of one 8x8 block ---------- */
static void idct_block(const int coeff64[64], double pixels[8][8]) {
    double matrix[8][8];
    memset(matrix, 0, sizeof(matrix));
    for (int k = 0; k < 64; k++) {
        int row = zigzag[k] / 8, col = zigzag[k] % 8;
        matrix[row][col] = coeff64[k];
    }
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

/* ---------- Render coefficients to signed YCbCr pixel planes ---------- */
/* IDCT output is signed (centered at 0). Y pixel = IDCT + 128 for display. */
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
                        Y[by+r][bx+c] = (int16_t)round(pixels[r][c]);
        } else if (bl == 4) {
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++)
                    if (my*8+r < PD_H/2 && mx*8+c < PD_W/2)
                        Cb[my*8+r][mx*8+c] = (int16_t)round(pixels[r][c]);
        } else {
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++)
                    if (my*8+r < PD_H/2 && mx*8+c < PD_W/2)
                        Cr[my*8+r][mx*8+c] = (int16_t)round(pixels[r][c]);
        }
    }
}

/* ---------- Render signed planes to RGB (I-frame: +128 level shift on Y) ---------- */
static void planes_signed_to_rgb(const int16_t Y[PD_H][PD_W],
                                  const int16_t Cb[PD_H/2][PD_W/2],
                                  const int16_t Cr[PD_H/2][PD_W/2],
                                  uint8_t *rgb) {
    for (int y = 0; y < PD_H; y++)
        for (int x = 0; x < PD_W; x++) {
            int yv = Y[y][x] + 128;
            int cb = Cb[y/2][x/2];
            int cr = Cr[y/2][x/2];
            int idx = (y * PD_W + x) * 3;
            rgb[idx + 0] = clamp8(yv + (int)(1.402 * cr));
            rgb[idx + 1] = clamp8(yv - (int)(0.344 * cb + 0.714 * cr));
            rgb[idx + 2] = clamp8(yv + (int)(1.772 * cb));
        }
}

/* ---------- Render unsigned Y/Cb/Cr planes to RGB ---------- */
/* Y in 0..255, Cb/Cr in signed range */
static void planes_unsigned_to_rgb(const int16_t Y[PD_H][PD_W],
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

/* ---------- Render I-frame from coefficients ---------- */
static void render_iframe(int coeff[PD_NBLOCKS][64], int dc_count, uint8_t *rgb) {
    int16_t Y[PD_H][PD_W], Cb[PD_H/2][PD_W/2], Cr[PD_H/2][PD_W/2];
    coeff_to_planes_signed(coeff, dc_count, Y, Cb, Cr);
    planes_signed_to_rgb(Y, Cb, Cr, rgb);
}

/* ---------- Packet info (with F3 tracking and scene index) ---------- */
typedef struct {
    uint8_t *data;
    int      size;
    bool     f3_before;   /* was there an F3 scene marker before this packet? */
    int      scene_idx;   /* which scene this packet belongs to */
} Packet;

/* ---------- Trim trailing 0xFF padding ---------- */
static int trim_padding(const uint8_t *pkt, int size) {
    int end = size;
    while (end > 40 && pkt[end - 1] == 0xFF) end--;
    return end;
}

/* ---------- Scene tracking ---------- */
typedef struct {
    int start_pkt;   /* first packet index in this scene */
    int length;      /* number of packets */
    int type_counts[256]; /* distribution of type bytes within */
} Scene;

#define MAX_SCENES 1024

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

/* ---------- Compute pixel value statistics ---------- */
static void compute_plane_stats(const int16_t *plane, int count,
                                 int *out_min, int *out_max, double *out_mean,
                                 double *out_stddev) {
    int mn = 32767, mx = -32768;
    double sum = 0;
    for (int i = 0; i < count; i++) {
        int v = plane[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum += v;
    }
    double mean = sum / count;
    double var = 0;
    for (int i = 0; i < count; i++) {
        double d = plane[i] - mean;
        var += d * d;
    }
    *out_min = mn; *out_max = mx;
    *out_mean = mean;
    *out_stddev = sqrt(var / count);
}

int main(int argc, char **argv) {
    const char *cuepath = "/home/wizzard/share/GitHub/playdia-roms/"
        "Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku - Uchuu-hen (Japan).cue";

    if (argc > 1) cuepath = argv[1];

    /* Resolve CUE to Track 2 bin */
    char *binpath = resolve_cue(cuepath);
    if (!binpath) {
        fprintf(stderr, "Could not find Track 2 in CUE: %s\n", cuepath);
        return 1;
    }
    printf("CUE: %s\nTrack 2: %s\n\n", cuepath, binpath);

    /* Read disc image */
    FILE *f = fopen(binpath, "rb");
    if (!f) { perror(binpath); free(binpath); return 1; }
    fseek(f, 0, SEEK_END); long disc_size = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *disc = malloc(disc_size);
    if (!disc) { fprintf(stderr, "Out of memory\n"); fclose(f); free(binpath); return 1; }
    fread(disc, 1, disc_size, f);
    fclose(f);
    int total_sectors = (int)(disc_size / SECTOR_RAW);
    printf("Loaded %ld bytes (%d sectors)\n\n", disc_size, total_sectors);

    system("mkdir -p " OUT_DIR);

    /* Allocate packet storage */
    Packet *pkts = calloc(MAX_PACKETS, sizeof(Packet));
    if (!pkts) { fprintf(stderr, "Out of memory\n"); free(disc); free(binpath); return 1; }
    for (int i = 0; i < MAX_PACKETS; i++) {
        pkts[i].data = malloc(MAX_FRAME);
        if (!pkts[i].data) { fprintf(stderr, "OOM at pkt %d\n", i); return 1; }
    }

    int npkt = assemble_packets(disc, total_sectors, pkts, MAX_PACKETS);
    printf("Assembled %d video packets total\n\n", npkt);
    free(disc);

    /* ================================================================
     * PART 1: Find scenes (groups of packets between F3 markers)
     * ================================================================ */
    printf("================================================================\n");
    printf("PART 1: Scene analysis — packets between F3 markers\n");
    printf("================================================================\n\n");

    Scene scenes[MAX_SCENES];
    int nscenes = 0;
    int cur_scene_start = 0;

    for (int i = 0; i < npkt; i++) {
        if (pkts[i].f3_before && i > cur_scene_start) {
            /* End previous scene */
            if (nscenes < MAX_SCENES) {
                scenes[nscenes].start_pkt = cur_scene_start;
                scenes[nscenes].length = i - cur_scene_start;
                memset(scenes[nscenes].type_counts, 0, sizeof(scenes[nscenes].type_counts));
                for (int j = cur_scene_start; j < i; j++) {
                    uint8_t *p = pkts[j].data;
                    if (p[0] == 0x00 && p[1] == 0x80 && p[2] == 0x04)
                        scenes[nscenes].type_counts[p[39]]++;
                }
                nscenes++;
            }
            cur_scene_start = i;
        }
        pkts[i].scene_idx = nscenes;
    }
    /* Last scene */
    if (cur_scene_start < npkt && nscenes < MAX_SCENES) {
        scenes[nscenes].start_pkt = cur_scene_start;
        scenes[nscenes].length = npkt - cur_scene_start;
        memset(scenes[nscenes].type_counts, 0, sizeof(scenes[nscenes].type_counts));
        for (int j = cur_scene_start; j < npkt; j++) {
            uint8_t *p = pkts[j].data;
            if (p[0] == 0x00 && p[1] == 0x80 && p[2] == 0x04)
                scenes[nscenes].type_counts[p[39]]++;
        }
        nscenes++;
    }

    printf("Found %d scenes total\n\n", nscenes);

    /* Sort scenes by length (descending) to find top 10 */
    int sorted[MAX_SCENES];
    for (int i = 0; i < nscenes; i++) sorted[i] = i;
    for (int i = 0; i < nscenes - 1; i++)
        for (int j = i + 1; j < nscenes; j++)
            if (scenes[sorted[j]].length > scenes[sorted[i]].length) {
                int t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t;
            }

    int top = nscenes < 10 ? nscenes : 10;
    printf("Top %d longest scenes:\n", top);
    printf("%-6s %-10s %-8s  %-40s\n", "Rank", "StartPkt", "Length", "Types within");
    for (int r = 0; r < top; r++) {
        int si = sorted[r];
        Scene *s = &scenes[si];
        printf("%-6d %-10d %-8d  ", r + 1, s->start_pkt, s->length);
        for (int t = 0; t < 256; t++)
            if (s->type_counts[t] > 0)
                printf("0x%02X:%d ", t, s->type_counts[t]);
        printf("\n");
    }
    printf("\n");

    /* Also print type sequence for top 3 scenes */
    for (int r = 0; r < 3 && r < top; r++) {
        int si = sorted[r];
        Scene *s = &scenes[si];
        printf("Scene rank %d (start=%d, len=%d) type sequence:\n  ",
               r+1, s->start_pkt, s->length);
        int limit = s->length < 80 ? s->length : 80;
        for (int j = 0; j < limit; j++) {
            int pi = s->start_pkt + j;
            uint8_t *p = pkts[pi].data;
            if (p[0] == 0x00 && p[1] == 0x80 && p[2] == 0x04)
                printf("%02X ", p[39]);
            else
                printf("?? ");
            if ((j + 1) % 30 == 0) printf("\n  ");
        }
        if (limit < s->length) printf("... (%d more)", s->length - limit);
        printf("\n\n");
    }

    /* ================================================================
     * PART 2: Delta testing on the longest scene
     * ================================================================ */
    printf("================================================================\n");
    printf("PART 2: P-frame delta testing on longest scene\n");
    printf("================================================================\n\n");

    int longest_scene = sorted[0];
    Scene *ls = &scenes[longest_scene];
    printf("Longest scene: start=%d, length=%d packets\n\n", ls->start_pkt, ls->length);

    /* Storage for coefficients and pixel planes */
    static int coeff_cur[PD_NBLOCKS][64];
    static int coeff_ref[PD_NBLOCKS][64]; /* reference I-frame coefficients */

    static int16_t ref_Y[PD_H][PD_W], ref_Cb[PD_H/2][PD_W/2], ref_Cr[PD_H/2][PD_W/2];
    static int16_t cur_Y[PD_H][PD_W], cur_Cb[PD_H/2][PD_W/2], cur_Cr[PD_H/2][PD_W/2];
    static int16_t out_Y[PD_H][PD_W], out_Cb[PD_H/2][PD_W/2], out_Cr[PD_H/2][PD_W/2];
    static uint8_t rgb[PD_W * PD_H * 3];

    bool have_ref = false;
    int output_count = 0;
    int max_output = 8;  /* output PPMs for first 8 frames */

    /* Process all packets in the longest scene */
    int scene_end = ls->start_pkt + ls->length;
    if (scene_end > npkt) scene_end = npkt;

    for (int i = ls->start_pkt; i < scene_end; i++) {
        uint8_t *pkt = pkts[i].data;
        if (pkt[0] != 0x00 || pkt[1] != 0x80 || pkt[2] != 0x04) continue;

        int QS = pkt[3];
        int type = pkt[39];
        int data_end = trim_padding(pkt, pkts[i].size);
        int pkt_idx = i - ls->start_pkt;

        /* Decode with DC DPCM predictors reset to 0 (standard I-frame decode) */
        BS bs = { pkt + 40, (data_end - 40) * 8, 0 };
        int dc = decode_frame(&bs, coeff_cur);

        printf("Scene pkt %03d (abs #%03d): type=0x%02X QS=%d decoded=%d/%d bits=%d/%d",
               pkt_idx, i, type, QS, dc, PD_NBLOCKS, bs.pos, bs.total_bits);

        if (dc < PD_NBLOCKS) {
            printf(" INCOMPLETE\n");
            continue;
        }

        /* Compute signed pixel planes for the current frame */
        coeff_to_planes_signed(coeff_cur, dc, cur_Y, cur_Cb, cur_Cr);

        /* Stats on the decoded IDCT values (signed, before +128) */
        int mn, mx; double mean, sd;
        compute_plane_stats((int16_t*)cur_Y, PD_H*PD_W, &mn, &mx, &mean, &sd);
        printf(" Y=[%d..%d] mean=%.1f sd=%.1f", mn, mx, mean, sd);

        bool is_iframe = (type == 0x00 || type == 0x07);
        bool is_pframe = (type == 0x06);

        printf(" %s\n", is_iframe ? "(I-frame)" : is_pframe ? "(P-frame candidate)" : "(unknown)");

        if (output_count < max_output) {
            char path_buf[512];

            /* === Output 1: Standard I-frame decode === */
            render_iframe(coeff_cur, dc, rgb);
            snprintf(path_buf, sizeof(path_buf),
                     OUT_DIR "delta_%03d_type%02X_iframe.ppm", pkt_idx, type);
            write_ppm(path_buf, rgb, PD_W, PD_H);

            if (is_pframe && have_ref) {
                /* === Output 2: Pixel delta (no level shift) ===
                 * output_Y[y][x] = clamp(ref_Y_unsigned + IDCT_result)
                 * where ref_Y_unsigned = ref_Y_signed + 128
                 * and IDCT_result = cur_Y_signed (the raw IDCT output, no +128)
                 * This treats the P-frame IDCT output as a signed delta. */
                for (int y = 0; y < PD_H; y++)
                    for (int x = 0; x < PD_W; x++)
                        out_Y[y][x] = clamp8((ref_Y[y][x] + 128) + cur_Y[y][x]);
                for (int y = 0; y < PD_H/2; y++)
                    for (int x = 0; x < PD_W/2; x++) {
                        out_Cb[y][x] = ref_Cb[y][x] + cur_Cb[y][x];
                        out_Cr[y][x] = ref_Cr[y][x] + cur_Cr[y][x];
                    }
                planes_unsigned_to_rgb(out_Y, out_Cb, out_Cr, rgb);
                snprintf(path_buf, sizeof(path_buf),
                         OUT_DIR "delta_%03d_type%02X_pixdelta.ppm", pkt_idx, type);
                write_ppm(path_buf, rgb, PD_W, PD_H);

                /* Report pixel-delta stats */
                int pd_mn, pd_mx; double pd_mean, pd_sd;
                compute_plane_stats((int16_t*)out_Y, PD_H*PD_W, &pd_mn, &pd_mx, &pd_mean, &pd_sd);
                printf("    pixdelta Y: [%d..%d] mean=%.1f sd=%.1f\n", pd_mn, pd_mx, pd_mean, pd_sd);

                /* === Output 3: DCT coefficient addition ===
                 * For each block: sum_coeff[k] = ref_coeff[k] + cur_coeff[k]
                 * Then: pixel = IDCT(sum_coeff) + 128 */
                {
                    static int coeff_sum[PD_NBLOCKS][64];
                    for (int b = 0; b < PD_NBLOCKS; b++)
                        for (int k = 0; k < 64; k++)
                            coeff_sum[b][k] = coeff_ref[b][k] + coeff_cur[b][k];

                    render_iframe(coeff_sum, PD_NBLOCKS, rgb);
                    snprintf(path_buf, sizeof(path_buf),
                             OUT_DIR "delta_%03d_type%02X_dctdelta.ppm", pkt_idx, type);
                    write_ppm(path_buf, rgb, PD_W, PD_H);

                    /* Stats on DCT-delta result */
                    int16_t dct_Y[PD_H][PD_W], dct_Cb[PD_H/2][PD_W/2], dct_Cr[PD_H/2][PD_W/2];
                    coeff_to_planes_signed(coeff_sum, PD_NBLOCKS, dct_Y, dct_Cb, dct_Cr);
                    int dct_mn, dct_mx; double dct_mean, dct_sd;
                    compute_plane_stats((int16_t*)dct_Y, PD_H*PD_W, &dct_mn, &dct_mx, &dct_mean, &dct_sd);
                    printf("    dctdelta Y: [%d..%d] mean=%.1f sd=%.1f\n",
                           dct_mn, dct_mx, dct_mean, dct_sd);
                }
            }

            output_count++;
        }

        /* Update reference:
         * If type is 0x00 or 0x07 (I-frame), update the reference.
         * Type 0x06 is a P-frame, do NOT update reference. */
        if (is_iframe) {
            memcpy(coeff_ref, coeff_cur, sizeof(int) * PD_NBLOCKS * 64);
            memcpy(ref_Y, cur_Y, sizeof(ref_Y));
            memcpy(ref_Cb, cur_Cb, sizeof(ref_Cb));
            memcpy(ref_Cr, cur_Cr, sizeof(ref_Cr));
            have_ref = true;
        }
    }

    printf("\n");

    /* ================================================================
     * PART 2b: Alternative model — what if every frame updates the reference?
     * ================================================================ */
    printf("================================================================\n");
    printf("PART 2b: Alternative — every decoded frame becomes new reference\n");
    printf("================================================================\n\n");

    have_ref = false;
    output_count = 0;

    for (int i = ls->start_pkt; i < scene_end; i++) {
        uint8_t *pkt = pkts[i].data;
        if (pkt[0] != 0x00 || pkt[1] != 0x80 || pkt[2] != 0x04) continue;

        int type = pkt[39];
        int data_end = trim_padding(pkt, pkts[i].size);
        int pkt_idx = i - ls->start_pkt;

        BS bs = { pkt + 40, (data_end - 40) * 8, 0 };
        int dc = decode_frame(&bs, coeff_cur);
        if (dc < PD_NBLOCKS) continue;

        coeff_to_planes_signed(coeff_cur, dc, cur_Y, cur_Cb, cur_Cr);

        bool is_pframe = (type == 0x06);

        if (output_count < max_output) {
            char path_buf[512];

            if (is_pframe && have_ref) {
                /* Pixel delta using chain reference */
                for (int y = 0; y < PD_H; y++)
                    for (int x = 0; x < PD_W; x++)
                        out_Y[y][x] = clamp8((ref_Y[y][x] + 128) + cur_Y[y][x]);
                for (int y = 0; y < PD_H/2; y++)
                    for (int x = 0; x < PD_W/2; x++) {
                        out_Cb[y][x] = ref_Cb[y][x] + cur_Cb[y][x];
                        out_Cr[y][x] = ref_Cr[y][x] + cur_Cr[y][x];
                    }
                planes_unsigned_to_rgb(out_Y, out_Cb, out_Cr, rgb);
                snprintf(path_buf, sizeof(path_buf),
                         OUT_DIR "delta_%03d_type%02X_chain_pixdelta.ppm", pkt_idx, type);
                write_ppm(path_buf, rgb, PD_W, PD_H);

                /* For the chained model, the result of delta becomes new ref */
                /* So the pixel-delta output is the "decoded" frame */
                /* Copy out_Y back as new ref (signed = out_Y - 128) */
                for (int y = 0; y < PD_H; y++)
                    for (int x = 0; x < PD_W; x++)
                        ref_Y[y][x] = out_Y[y][x] - 128;
                for (int y = 0; y < PD_H/2; y++)
                    for (int x = 0; x < PD_W/2; x++) {
                        ref_Cb[y][x] = out_Cb[y][x];
                        ref_Cr[y][x] = out_Cr[y][x];
                    }

                int pd_mn, pd_mx; double pd_mean, pd_sd;
                compute_plane_stats((int16_t*)out_Y, PD_H*PD_W, &pd_mn, &pd_mx, &pd_mean, &pd_sd);
                printf("  chain pkt %03d type=0x%02X pixdelta Y: [%d..%d] mean=%.1f sd=%.1f\n",
                       pkt_idx, type, pd_mn, pd_mx, pd_mean, pd_sd);
            } else {
                /* I-frame or first frame: just use standard decode as reference */
                memcpy(ref_Y, cur_Y, sizeof(ref_Y));
                memcpy(ref_Cb, cur_Cb, sizeof(ref_Cb));
                memcpy(ref_Cr, cur_Cr, sizeof(ref_Cr));
                have_ref = true;
                printf("  chain pkt %03d type=0x%02X: I-frame reference set\n", pkt_idx, type);
            }

            output_count++;
        } else {
            /* Still track reference for non-output frames */
            if (type != 0x06) {
                memcpy(ref_Y, cur_Y, sizeof(ref_Y));
                memcpy(ref_Cb, cur_Cb, sizeof(ref_Cb));
                memcpy(ref_Cr, cur_Cr, sizeof(ref_Cr));
                have_ref = true;
            } else if (have_ref) {
                /* Apply delta and update ref */
                for (int y = 0; y < PD_H; y++)
                    for (int x = 0; x < PD_W; x++)
                        ref_Y[y][x] = clamp8((ref_Y[y][x] + 128) + cur_Y[y][x]) - 128;
                for (int y = 0; y < PD_H/2; y++)
                    for (int x = 0; x < PD_W/2; x++) {
                        ref_Cb[y][x] = ref_Cb[y][x] + cur_Cb[y][x];
                        ref_Cr[y][x] = ref_Cr[y][x] + cur_Cr[y][x];
                    }
            }
        }
    }

    printf("\n");

    /* ================================================================
     * Summary: Overall statistics per type
     * ================================================================ */
    printf("================================================================\n");
    printf("SUMMARY: IDCT value range statistics by type (all packets)\n");
    printf("================================================================\n\n");

    printf("%-8s %-6s %-12s %-12s %-12s %-12s\n",
           "Type", "Count", "Y_min", "Y_max", "Y_mean", "Y_stddev");

    int type_seen[256] = {0};
    /* Accumulate stats per type */
    double type_sum_mean[256] = {0};
    double type_sum_sd[256] = {0};
    int type_min_all[256], type_max_all[256];
    for (int t = 0; t < 256; t++) { type_min_all[t] = 32767; type_max_all[t] = -32768; }

    for (int i = 0; i < npkt; i++) {
        uint8_t *pkt = pkts[i].data;
        if (pkt[0] != 0x00 || pkt[1] != 0x80 || pkt[2] != 0x04) continue;
        int type = pkt[39];
        int data_end = trim_padding(pkt, pkts[i].size);

        BS bs = { pkt + 40, (data_end - 40) * 8, 0 };
        int dc = decode_frame(&bs, coeff_cur);
        if (dc < PD_NBLOCKS) continue;

        coeff_to_planes_signed(coeff_cur, dc, cur_Y, cur_Cb, cur_Cr);
        int mn, mx; double mean, sd;
        compute_plane_stats((int16_t*)cur_Y, PD_H*PD_W, &mn, &mx, &mean, &sd);

        if (mn < type_min_all[type]) type_min_all[type] = mn;
        if (mx > type_max_all[type]) type_max_all[type] = mx;
        type_sum_mean[type] += mean;
        type_sum_sd[type] += sd;
        type_seen[type]++;
    }

    for (int t = 0; t < 256; t++) {
        if (type_seen[t] == 0) continue;
        printf("0x%02X     %-6d %-12d %-12d %-12.1f %-12.1f\n",
               t, type_seen[t],
               type_min_all[t], type_max_all[t],
               type_sum_mean[t] / type_seen[t],
               type_sum_sd[t] / type_seen[t]);
    }
    printf("\n");

    /* Cleanup */
    for (int i = 0; i < MAX_PACKETS; i++) free(pkts[i].data);
    free(pkts);
    free(binpath);

    printf("Done.\n");
    return 0;
}
