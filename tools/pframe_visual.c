/*
 * pframe_visual.c — Render frames with different type bytes side by side
 *
 * For each of type=0x00, 0x06, 0x07, decode as I-frame and output PPM.
 * For type=0x06 and 0x07, also try decoding with DC DPCM predictors
 * carried from the preceding I-frame (simple P-frame model).
 *
 * Usage: ./pframe_visual <file.cue or Track2.bin>
 *
 * Disc format:
 *   Raw Mode 2/2352 sectors. F1 sectors (marker 0xF1) carry 2047 bytes
 *   at offset [25]. 6 F1 sectors per packet. F2 = end-of-frame.
 *   F3 = scene marker.
 *
 * Packet header (40 bytes):
 *   [0-2]=00 80 04, [3]=QS, [4-19]=qtable, [20-35]=qtable copy,
 *   [36-38]=00 80 24, [39]=TYPE
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
#define MAX_PACKETS 512
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
    printf("  -> %s (%dx%d)\n", p, w, h);
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
/* dc_pred_init: initial DC predictors [Y, Cb, Cr] — if non-NULL, use them
 * and also write the final predictor state back into them */
static int decode_frame(BS *b, int coeff[PD_NBLOCKS][64], int dc_pred_init[3]) {
    int dc_count = 0;
    memset(coeff, 0, sizeof(int) * PD_NBLOCKS * 64);
    int dc_pred[3];
    if (dc_pred_init) {
        dc_pred[0] = dc_pred_init[0];
        dc_pred[1] = dc_pred_init[1];
        dc_pred[2] = dc_pred_init[2];
    } else {
        dc_pred[0] = dc_pred[1] = dc_pred[2] = 0;
    }

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
    /* Write back final predictor state */
    if (dc_pred_init) {
        dc_pred_init[0] = dc_pred[0];
        dc_pred_init[1] = dc_pred[1];
        dc_pred_init[2] = dc_pred[2];
    }

    if (dc_count < 6) return 0;

    /* Phase 2: AC coefficients */
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

/* ---------- Clamp ---------- */
static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* ---------- Render coefficients to RGB ---------- */
static void render_frame(int coeff[PD_NBLOCKS][64], int dc_count,
                          uint8_t *rgb) {
    uint8_t Y[PD_H][PD_W], Cb[PD_H/2][PD_W/2], Cr[PD_H/2][PD_W/2];
    memset(Y, 128, sizeof(Y));
    memset(Cb, 128, sizeof(Cb));
    memset(Cr, 128, sizeof(Cr));

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

        /* IDCT: row pass */
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

        /* IDCT: column pass */
        uint8_t block[8][8];
        for (int c = 0; c < 8; c++)
            for (int r = 0; r < 8; r++) {
                double sum = 0;
                for (int k = 0; k < 8; k++) {
                    double ck = (k == 0) ? (1.0 / sqrt(2.0)) : 1.0;
                    sum += ck * temp[k][c] * cos((2*r+1) * k * PI / 16.0);
                }
                block[r][c] = clamp8((int)round(sum / 2.0) + 128);
            }

        /* Place block into Y/Cb/Cr planes */
        if (bl < 4) {
            int bx = mx * 16 + (bl & 1) * 8;
            int by = my * 16 + (bl >> 1) * 8;
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++)
                    if (by+r < PD_H && bx+c < PD_W)
                        Y[by+r][bx+c] = block[r][c];
        } else if (bl == 4) {
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++)
                    if (my*8+r < PD_H/2 && mx*8+c < PD_W/2)
                        Cb[my*8+r][mx*8+c] = block[r][c];
        } else {
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++)
                    if (my*8+r < PD_H/2 && mx*8+c < PD_W/2)
                        Cr[my*8+r][mx*8+c] = block[r][c];
        }
    }

    /* YCbCr -> RGB */
    for (int y = 0; y < PD_H; y++)
        for (int x = 0; x < PD_W; x++) {
            int yv = Y[y][x], cb = Cb[y/2][x/2] - 128, cr = Cr[y/2][x/2] - 128;
            int idx = (y * PD_W + x) * 3;
            rgb[idx + 0] = clamp8(yv + (int)(1.402 * cr));
            rgb[idx + 1] = clamp8(yv - (int)(0.344 * cb + 0.714 * cr));
            rgb[idx + 2] = clamp8(yv + (int)(1.772 * cb));
        }
}

/* ---------- Packet info ---------- */
typedef struct {
    uint8_t data[MAX_FRAME];
    int     size;
    int     index;  /* sequential packet number */
} Packet;

/* ---------- Assemble all packets from disc ---------- */
static int assemble_packets(const uint8_t *disc, int total_sectors,
                             Packet *pkts, int max_pkts) {
    int n = 0, pos = 0;
    bool in_frame = false;

    for (int lba = 0; lba < total_sectors && n < max_pkts; lba++) {
        const uint8_t *sec = disc + (long)lba * SECTOR_RAW;
        /* Check sync + mode 2 + video subheader */
        if (sec[0] != 0x00 || sec[1] != 0xFF || sec[15] != 2) continue;
        if (sec[18] & 0x04) continue;       /* skip audio */
        if (!(sec[18] & 0x08)) continue;     /* must be data */

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
                pkts[n].index = n;
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

/* ---------- Trim trailing 0xFF padding ---------- */
static int trim_padding(const uint8_t *pkt, int size) {
    int end = size;
    while (end > 40 && pkt[end - 1] == 0xFF) end--;
    return end;
}

/* ---------- main ---------- */
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

    /* Assemble all packets */
    static Packet *pkts;
    pkts = calloc(MAX_PACKETS, sizeof(Packet));
    if (!pkts) { fprintf(stderr, "Out of memory\n"); free(disc); free(resolved); return 1; }
    int npkt = assemble_packets(disc, total_sectors, pkts, MAX_PACKETS);
    printf("Assembled %d video packets\n\n", npkt);
    free(disc);

    /* Scan for type byte distribution */
    int type_counts[256] = {0};
    for (int i = 0; i < npkt; i++) {
        if (pkts[i].data[0] == 0x00 && pkts[i].data[1] == 0x80 && pkts[i].data[2] == 0x04)
            type_counts[pkts[i].data[39]]++;
    }
    printf("Type byte distribution:\n");
    for (int t = 0; t < 256; t++)
        if (type_counts[t] > 0)
            printf("  Type 0x%02X: %d packets\n", t, type_counts[t]);
    printf("\n");

    /* Find first packet of each target type */
    int target_types[] = {0x00, 0x06, 0x07};
    int ntargets = 3;
    int found_idx[3] = {-1, -1, -1};

    for (int ti = 0; ti < ntargets; ti++) {
        for (int i = 0; i < npkt; i++) {
            uint8_t *p = pkts[i].data;
            if (p[0] != 0x00 || p[1] != 0x80 || p[2] != 0x04) continue;
            if (p[39] == target_types[ti]) {
                found_idx[ti] = i;
                break;
            }
        }
    }

    for (int ti = 0; ti < ntargets; ti++) {
        if (found_idx[ti] < 0)
            printf("Type 0x%02X: NOT FOUND\n", target_types[ti]);
        else
            printf("Type 0x%02X: first at packet #%d (QS=%d)\n",
                   target_types[ti], found_idx[ti], pkts[found_idx[ti]].data[3]);
    }
    printf("\n");

    /* Show context: type sequence around each found packet */
    printf("=== Type sequence around found packets ===\n");
    for (int ti = 0; ti < ntargets; ti++) {
        int idx = found_idx[ti];
        if (idx < 0) continue;
        printf("Around type 0x%02X (packet #%d):\n  ", target_types[ti], idx);
        int lo = idx - 5; if (lo < 0) lo = 0;
        int hi = idx + 5; if (hi >= npkt) hi = npkt - 1;
        for (int i = lo; i <= hi; i++) {
            uint8_t *p = pkts[i].data;
            if (p[0] == 0x00 && p[1] == 0x80 && p[2] == 0x04)
                printf("[%d]type=%02X,QS=%d ", i, p[39], p[3]);
        }
        printf("\n");
    }
    printf("\n");

    static int coeff[PD_NBLOCKS][64];
    static uint8_t rgb[PD_W * PD_H * 3];

    /* ========== 1) Render each type as I-frame (DC predictors reset to 0) ========== */
    printf("=== Rendering each type as independent I-frame ===\n");
    for (int ti = 0; ti < ntargets; ti++) {
        int idx = found_idx[ti];
        if (idx < 0) continue;
        uint8_t *pkt = pkts[idx].data;
        int QS = pkt[3];
        int type = pkt[39];
        int data_end = trim_padding(pkt, pkts[idx].size);
        BS bs = { pkt + 40, (data_end - 40) * 8, 0 };

        int dc = decode_frame(&bs, coeff, NULL);
        printf("Type 0x%02X (pkt#%d): decoded %d/%d blocks, bits used %d/%d\n",
               type, idx, dc, PD_NBLOCKS, bs.pos, bs.total_bits);

        if (dc >= PD_NBLOCKS) {
            render_frame(coeff, dc, rgb);
            char path_buf[512];
            snprintf(path_buf, sizeof(path_buf),
                     OUT_DIR "pframe_type%02X_iframe_pkt%03d.ppm", type, idx);
            write_ppm(path_buf, rgb, PD_W, PD_H);
        }
    }
    printf("\n");

    /* ========== 2) For type 0x06 and 0x07: try P-frame model ==========
     * Find the I-frame (type=0x00) immediately preceding each, decode it
     * to get the final DC predictor state, then decode the target frame
     * using those predictors as starting values. */
    printf("=== P-frame model: carry DC predictors from preceding I-frame ===\n");
    for (int ti = 1; ti < ntargets; ti++) {  /* skip type 0x00 */
        int idx = found_idx[ti];
        if (idx < 0) continue;

        /* Find the most recent type=0x00 packet before this one */
        int iframe_idx = -1;
        for (int i = idx - 1; i >= 0; i--) {
            uint8_t *p = pkts[i].data;
            if (p[0] == 0x00 && p[1] == 0x80 && p[2] == 0x04 && p[39] == 0x00) {
                iframe_idx = i;
                break;
            }
        }

        if (iframe_idx < 0) {
            printf("Type 0x%02X: no preceding I-frame found, skipping P-frame test\n",
                   target_types[ti]);
            continue;
        }

        printf("Type 0x%02X (pkt#%d): preceding I-frame at pkt#%d\n",
               target_types[ti], idx, iframe_idx);

        /* Decode the I-frame to capture final DC predictor state */
        {
            uint8_t *ipkt = pkts[iframe_idx].data;
            int iend = trim_padding(ipkt, pkts[iframe_idx].size);
            BS ibs = { ipkt + 40, (iend - 40) * 8, 0 };
            int dc_pred_state[3] = {0, 0, 0};
            int idc = decode_frame(&ibs, coeff, dc_pred_state);
            printf("  I-frame decoded %d blocks, final DC preds: Y=%d Cb=%d Cr=%d\n",
                   idc, dc_pred_state[0], dc_pred_state[1], dc_pred_state[2]);

            /* Also render the I-frame for reference */
            if (idc >= PD_NBLOCKS) {
                render_frame(coeff, idc, rgb);
                char path_buf[512];
                snprintf(path_buf, sizeof(path_buf),
                         OUT_DIR "pframe_ref_iframe_pkt%03d.ppm", iframe_idx);
                write_ppm(path_buf, rgb, PD_W, PD_H);
            }

            /* Now decode the target frame with carried predictors */
            uint8_t *tpkt = pkts[idx].data;
            int tend = trim_padding(tpkt, pkts[idx].size);
            BS tbs = { tpkt + 40, (tend - 40) * 8, 0 };

            int tdc = decode_frame(&tbs, coeff, dc_pred_state);
            printf("  P-frame decoded %d blocks, bits used %d/%d\n",
                   tdc, tbs.pos, tbs.total_bits);

            if (tdc >= PD_NBLOCKS) {
                render_frame(coeff, tdc, rgb);
                char path_buf[512];
                snprintf(path_buf, sizeof(path_buf),
                         OUT_DIR "pframe_type%02X_pmodel_pkt%03d.ppm",
                         target_types[ti], idx);
                write_ppm(path_buf, rgb, PD_W, PD_H);
            }
        }

        /* Also try: carry predictors through ALL frames from the
         * preceding I-frame up to this target frame */
        printf("  Trying chained decode from I-frame through all intermediate frames...\n");
        {
            int dc_pred_chain[3] = {0, 0, 0};
            for (int i = iframe_idx; i <= idx; i++) {
                uint8_t *p = pkts[i].data;
                if (p[0] != 0x00 || p[1] != 0x80 || p[2] != 0x04) continue;
                int pend = trim_padding(p, pkts[i].size);
                BS pbs = { p + 40, (pend - 40) * 8, 0 };
                int pdc = decode_frame(&pbs, coeff, dc_pred_chain);

                if (i == idx) {
                    printf("  Chained: decoded %d blocks, final DC preds: Y=%d Cb=%d Cr=%d\n",
                           pdc, dc_pred_chain[0], dc_pred_chain[1], dc_pred_chain[2]);
                    if (pdc >= PD_NBLOCKS) {
                        render_frame(coeff, pdc, rgb);
                        char path_buf[512];
                        snprintf(path_buf, sizeof(path_buf),
                                 OUT_DIR "pframe_type%02X_chained_pkt%03d.ppm",
                                 target_types[ti], idx);
                        write_ppm(path_buf, rgb, PD_W, PD_H);
                    }
                }
            }
        }
    }

    printf("\n=== Done ===\n");
    free(pkts);
    free(resolved);
    return 0;
}
