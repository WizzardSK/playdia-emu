/*
 * pframe_fullpkt.c — Analyze ONLY full-size video packets (exactly 6 F1 sectors
 * = 12282 bytes) and classify their frame types.
 *
 * Usage: ./pframe_fullpkt <file.cue or Track2.bin>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <strings.h>

#define SECTOR_RAW  2352
#define F1_PAYLOAD  2047
#define FULL_F1     6
#define FULL_SIZE   (FULL_F1 * F1_PAYLOAD)  /* 12282 */
#define MAX_FRAME   65536
#define MAX_PACKETS 4096
#define PD_W        256
#define PD_H        144
#define PD_MW       (PD_W / 16)
#define PD_MH       (PD_H / 16)
#define PD_NBLOCKS  (PD_MW * PD_MH * 6)  /* 16 * 9 * 6 = 864 */

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

/* ---------- Decode DC + AC from a packet ---------- */
/* Returns DC count. dc_vals[] filled with DC values, total_ac set to AC count */
static int decode_dc_ac(BS *b, int dc_vals[], int max_dc,
                         int *total_ac, double *sum_abs_ac) {
    int dc_count = 0;
    int dc_pred[3] = {0, 0, 0};
    *total_ac = 0;
    *sum_abs_ac = 0;

    /* Phase 1: DC coefficients via VLC DPCM */
    for (int mb = 0; mb < PD_MW * PD_MH && b->pos < b->total_bits; mb++)
        for (int bl = 0; bl < 6 && b->pos < b->total_bits; bl++) {
            int comp = (bl < 4) ? 0 : (bl == 4) ? 1 : 2;
            int diff = read_vlc(b);
            if (diff == -9999) goto dc_done;
            dc_pred[comp] += diff;
            if (dc_count < max_dc)
                dc_vals[dc_count] = dc_pred[comp];
            dc_count++;
        }
dc_done:
    if (dc_count < 6) return dc_count;

    /* Phase 2: AC coefficients */
    for (int bi = 0; bi < dc_count && b->pos < b->total_bits; bi++) {
        int k = 1;
        while (k < 64 && b->pos < b->total_bits) {
            /* Check 6-bit zero EOB */
            int peek = bs_peek(b, 6);
            if (peek < 0) break;
            if (peek == 0) { b->pos += 6; break; }
            /* Unary run */
            int run = 0, ok = 1;
            while (run < 5 && b->pos < b->total_bits) {
                int bit = bs_bit(b);
                if (bit < 0) { ok = 0; break; }
                if (bit == 1) break;
                run++;
            }
            if (!ok) break;
            /* Check alternate EOB: "100" = 0x4 in 3 bits */
            int p3 = bs_peek(b, 3);
            if (p3 == 4) { b->pos += 3; break; }
            /* VLC level */
            int level = read_vlc(b);
            if (level == -9999) break;
            k += run;
            if (k < 64) {
                (*total_ac)++;
                *sum_abs_ac += abs(level);
            }
            k++;
        }
    }
    return dc_count;
}

/* ---------- Trim trailing 0xFF padding ---------- */
static int trim_padding(const uint8_t *pkt, int size) {
    int end = size;
    while (end > 40 && pkt[end - 1] == 0xFF) end--;
    return end;
}

/* ---------- Packet info ---------- */
typedef struct {
    uint8_t *data;
    int      size;
    int      f1_count;    /* number of F1 sectors in this packet */
    bool     f3_before;   /* was there an F3 scene marker before this? */
} Packet;

/* ---------- Assemble all packets, tracking F1 count ---------- */
static int assemble_packets(const uint8_t *disc, int total_sectors,
                             Packet *pkts, int max_pkts) {
    int n = 0, pos = 0, f1_count = 0;
    bool in_frame = false;
    bool saw_f3 = false;

    for (int lba = 0; lba < total_sectors && n < max_pkts; lba++) {
        const uint8_t *sec = disc + (long)lba * SECTOR_RAW;
        if (sec[0] != 0x00 || sec[1] != 0xFF || sec[15] != 2) continue;
        if (sec[18] & 0x04) continue;   /* skip audio */
        if (!(sec[18] & 0x08)) continue; /* skip non-video */

        uint8_t marker = sec[24];
        if (marker == 0xF1) {
            if (!in_frame) { in_frame = true; pos = 0; f1_count = 0; }
            if (pos + F1_PAYLOAD < MAX_FRAME) {
                memcpy(pkts[n].data + pos, sec + 25, F1_PAYLOAD);
                pos += F1_PAYLOAD;
                f1_count++;
            }
        } else if (marker == 0xF2) {
            if (in_frame && pos > 0) {
                pkts[n].size = pos;
                pkts[n].f1_count = f1_count;
                pkts[n].f3_before = saw_f3;
                saw_f3 = false;
                n++;
                in_frame = false;
                pos = 0;
                f1_count = 0;
            }
        } else if (marker == 0xF3) {
            in_frame = false;
            pos = 0;
            f1_count = 0;
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

    /* Allocate packet storage */
    Packet *pkts = calloc(MAX_PACKETS, sizeof(Packet));
    if (!pkts) { fprintf(stderr, "Out of memory for packets\n"); free(disc); free(resolved); return 1; }
    for (int i = 0; i < MAX_PACKETS; i++) {
        pkts[i].data = malloc(MAX_FRAME);
        if (!pkts[i].data) { fprintf(stderr, "Out of memory for packet %d\n", i); return 1; }
    }

    int npkt = assemble_packets(disc, total_sectors, pkts, MAX_PACKETS);
    printf("Assembled %d total video packets\n\n", npkt);
    free(disc);

    /* Separate full-size (6 F1) and short packets */
    int full_count = 0, short_count = 0;
    int full_indices[MAX_PACKETS], short_indices[MAX_PACKETS];
    for (int i = 0; i < npkt; i++) {
        if (pkts[i].f1_count == FULL_F1) {
            full_indices[full_count++] = i;
        } else {
            short_indices[short_count++] = i;
        }
    }

    printf("====================================================\n");
    printf("PACKET SIZE BREAKDOWN\n");
    printf("====================================================\n");
    printf("  Full-size packets (6 F1 = %d bytes): %d\n", FULL_SIZE, full_count);
    printf("  Short packets (< 6 F1):              %d\n", short_count);
    printf("\n");

    /* ================================================================
     * SECTION 5: Short packet info
     * ================================================================ */
    printf("====================================================\n");
    printf("SHORT PACKETS (< 6 F1)\n");
    printf("====================================================\n");
    if (short_count == 0) {
        printf("  None found.\n");
    } else {
        /* Count by F1 count */
        int f1_hist[16] = {0};
        int short_type_counts[256] = {0};
        for (int s = 0; s < short_count; s++) {
            int idx = short_indices[s];
            Packet *p = &pkts[idx];
            if (p->f1_count < 16) f1_hist[p->f1_count]++;
            uint8_t *d = p->data;
            if (d[0] == 0x00 && d[1] == 0x80 && d[2] == 0x04 &&
                d[36] == 0x00 && d[37] == 0x80 && d[38] == 0x24)
                short_type_counts[d[39]]++;
        }
        printf("  F1-count distribution:\n");
        for (int c = 0; c < 16; c++)
            if (f1_hist[c] > 0)
                printf("    %d F1 sectors: %d packets (%d bytes)\n", c, f1_hist[c], c * F1_PAYLOAD);
        printf("  Type distribution:\n");
        for (int t = 0; t < 256; t++)
            if (short_type_counts[t] > 0)
                printf("    Type 0x%02X: %d\n", t, short_type_counts[t]);
        /* Print details of first few */
        int show = short_count < 20 ? short_count : 20;
        printf("  First %d short packets:\n", show);
        for (int s = 0; s < show; s++) {
            int idx = short_indices[s];
            Packet *p = &pkts[idx];
            uint8_t *d = p->data;
            printf("    pkt#%03d: %d F1, %d bytes", idx, p->f1_count, p->size);
            if (d[0] == 0x00 && d[1] == 0x80 && d[2] == 0x04)
                printf(", type=0x%02X", d[39]);
            if (p->f3_before) printf(" [F3]");
            printf(", hdr: %02X %02X %02X %02X ... [36-39]: %02X %02X %02X %02X\n",
                   d[0], d[1], d[2], d[3], d[36], d[37], d[38], d[39]);
        }
    }
    printf("\n");

    /* ================================================================
     * SECTION 1: Full-size packet type distribution
     * ================================================================ */
    printf("====================================================\n");
    printf("FULL-SIZE PACKET TYPE DISTRIBUTION\n");
    printf("====================================================\n");

    int type_counts[256] = {0};
    int valid_full = 0;
    for (int fi = 0; fi < full_count; fi++) {
        int idx = full_indices[fi];
        uint8_t *d = pkts[idx].data;
        if (d[0] == 0x00 && d[1] == 0x80 && d[2] == 0x04 &&
            d[36] == 0x00 && d[37] == 0x80 && d[38] == 0x24) {
            type_counts[d[39]]++;
            valid_full++;
        }
    }
    printf("Total full-size with valid header: %d / %d\n", valid_full, full_count);
    for (int t = 0; t < 256; t++)
        if (type_counts[t] > 0)
            printf("  Type 0x%02X: %4d packets (%5.1f%%)\n",
                   t, type_counts[t], 100.0 * type_counts[t] / valid_full);
    printf("\n");

    /* ================================================================
     * SECTION 2: For EACH distinct type, decode first full-size packet as I-frame
     * ================================================================ */
    printf("====================================================\n");
    printf("FIRST FULL-SIZE PACKET OF EACH TYPE (decoded as I-frame)\n");
    printf("====================================================\n");

    static int dc_vals[PD_NBLOCKS + 64];

    for (int t = 0; t < 256; t++) {
        if (type_counts[t] == 0) continue;

        /* Find first full-size packet of this type */
        int found = -1;
        for (int fi = 0; fi < full_count; fi++) {
            int idx = full_indices[fi];
            uint8_t *d = pkts[idx].data;
            if (d[0] == 0x00 && d[1] == 0x80 && d[2] == 0x04 &&
                d[36] == 0x00 && d[37] == 0x80 && d[38] == 0x24 &&
                d[39] == t) {
                found = idx;
                break;
            }
        }
        if (found < 0) continue;

        uint8_t *pkt = pkts[found].data;
        int data_end = trim_padding(pkt, pkts[found].size);
        BS bs = { pkt + 40, (data_end - 40) * 8, 0 };

        int total_ac = 0;
        double sum_abs_ac = 0;
        int dc = decode_dc_ac(&bs, dc_vals, PD_NBLOCKS, &total_ac, &sum_abs_ac);

        /* DC stats */
        double sum_abs_dc = 0;
        int dc_min = 0, dc_max = 0;
        int dc_use = dc < PD_NBLOCKS ? dc : PD_NBLOCKS;
        for (int i = 0; i < dc_use; i++) {
            sum_abs_dc += abs(dc_vals[i]);
            if (dc_vals[i] < dc_min) dc_min = dc_vals[i];
            if (dc_vals[i] > dc_max) dc_max = dc_vals[i];
        }
        double mean_abs_dc = dc_use > 0 ? sum_abs_dc / dc_use : 0;
        double mean_abs_ac_val = total_ac > 0 ? sum_abs_ac / total_ac : 0;

        const char *classification;
        if (mean_abs_dc > 20) classification = "I-frame-like";
        else if (mean_abs_dc < 5) classification = "delta-like";
        else classification = "intermediate";

        printf("\nType 0x%02X (pkt#%03d, QS=%d):\n", t, found, pkt[3]);
        printf("  DC: count=%d/%d, mean|DC|=%.2f, range=[%d..%d]\n",
               dc, PD_NBLOCKS, mean_abs_dc, dc_min, dc_max);
        printf("  AC: total=%d, mean|AC|=%.2f\n", total_ac, mean_abs_ac_val);
        printf("  Bits used: %d / %d (%.1f%%)\n", bs.pos, bs.total_bits,
               100.0 * bs.pos / bs.total_bits);
        printf("  Classification: %s\n", classification);
    }
    printf("\n");

    /* ================================================================
     * SECTION 3: Types 0x00, 0x06, 0x07 - decode first 5 of each
     * ================================================================ */
    printf("====================================================\n");
    printf("DETAILED DECODE: Types 0x00, 0x06, 0x07 (first 5 each)\n");
    printf("====================================================\n");

    int target_types[] = {0x00, 0x06, 0x07};
    for (int ti = 0; ti < 3; ti++) {
        int tgt = target_types[ti];
        if (type_counts[tgt] == 0) {
            printf("\nType 0x%02X: no full-size packets found\n", tgt);
            continue;
        }

        printf("\n--- Type 0x%02X ---\n", tgt);
        int decoded = 0;
        for (int fi = 0; fi < full_count && decoded < 5; fi++) {
            int idx = full_indices[fi];
            uint8_t *d = pkts[idx].data;
            if (d[0] != 0x00 || d[1] != 0x80 || d[2] != 0x04 ||
                d[36] != 0x00 || d[37] != 0x80 || d[38] != 0x24 ||
                d[39] != tgt)
                continue;

            int data_end = trim_padding(d, pkts[idx].size);
            BS bs = { d + 40, (data_end - 40) * 8, 0 };

            int total_ac = 0;
            double sum_abs_ac = 0;
            int dc = decode_dc_ac(&bs, dc_vals, PD_NBLOCKS, &total_ac, &sum_abs_ac);

            int dc_use = dc < PD_NBLOCKS ? dc : PD_NBLOCKS;
            double sum_abs_dc = 0;
            for (int i = 0; i < dc_use; i++) sum_abs_dc += abs(dc_vals[i]);
            double mean_abs_dc = dc_use > 0 ? sum_abs_dc / dc_use : 0;
            double mean_abs_ac_val = total_ac > 0 ? sum_abs_ac / total_ac : 0;

            printf("  Packet #%03d (QS=%d): DC=%d/%d, mean|DC|=%.2f, AC=%d, mean|AC|=%.2f\n",
                   idx, d[3], dc, PD_NBLOCKS, mean_abs_dc, total_ac, mean_abs_ac_val);

            /* Print first 20 DC values */
            int show_dc = dc_use < 20 ? dc_use : 20;
            printf("    First %d DCs:", show_dc);
            for (int i = 0; i < show_dc; i++)
                printf(" %d", dc_vals[i]);
            printf("\n");

            decoded++;
        }
    }
    printf("\n");

    /* ================================================================
     * SECTION 4: Sequence of first 100 full-size packets with F3 markers
     * ================================================================ */
    printf("====================================================\n");
    printf("TYPE SEQUENCE: first 100 full-size packets\n");
    printf("====================================================\n");

    int seq_limit = full_count < 100 ? full_count : 100;
    int col = 0;
    for (int fi = 0; fi < seq_limit; fi++) {
        int idx = full_indices[fi];
        uint8_t *d = pkts[idx].data;

        if (pkts[idx].f3_before) {
            if (col > 0) { printf("\n"); col = 0; }
            printf("[F3]\n");
        }

        if (d[0] == 0x00 && d[1] == 0x80 && d[2] == 0x04 &&
            d[36] == 0x00 && d[37] == 0x80 && d[38] == 0x24) {
            printf("%02X ", d[39]);
        } else {
            printf("?? ");
        }
        col++;
        if (col >= 20) { printf("\n"); col = 0; }
    }
    if (col > 0) printf("\n");
    printf("\n");

    /* Cleanup */
    for (int i = 0; i < MAX_PACKETS; i++) free(pkts[i].data);
    free(pkts);
    free(resolved);
    return 0;
}
