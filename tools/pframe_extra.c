/*
 * pframe_extra.c — Examine extra F1 sectors beyond 6 in longer video packets.
 *
 * For the DBZ Uchuu-hen disc, most packets have 8 F1 sectors (not 6).
 * This tool investigates what the extra 2 sectors (4094 bytes) contain.
 *
 * Usage: ./pframe_extra <file.cue or Track2.bin>
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
#define MAX_FRAME   (16 * F1_PAYLOAD)   /* 16 F1 sectors max */
#define MAX_PACKETS 8192
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

static void bs_align(BS *b) {
    if (b->pos & 7) b->pos = (b->pos + 7) & ~7;
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

/* ---------- Decode one I-frame (864 blocks: DC DPCM + AC run-level) ----------
 * Returns number of complete frames decoded.
 * bits_consumed is set to bits used. */
static int decode_iframe_multi(BS *b, int max_frames) {
    int frames = 0;

    for (int fr = 0; fr < max_frames; fr++) {
        int dc_pred[3] = {0, 0, 0};
        int dc_ok = 1;

        /* Phase 1: DC DPCM via VLC for 864 blocks */
        for (int mb = 0; mb < PD_MW * PD_MH && b->pos < b->total_bits; mb++) {
            for (int bl = 0; bl < 6 && b->pos < b->total_bits; bl++) {
                int comp = (bl < 4) ? 0 : (bl == 4) ? 1 : 2;
                int diff = read_vlc(b);
                if (diff == -9999) { dc_ok = 0; break; }
                dc_pred[comp] += diff;
            }
            if (!dc_ok) break;
        }
        if (!dc_ok) break;

        /* Phase 2: AC run-level for 864 blocks */
        int ac_ok = 1;
        for (int bi = 0; bi < PD_NBLOCKS && b->pos < b->total_bits; bi++) {
            int k = 1;
            while (k < 64 && b->pos < b->total_bits) {
                /* 6-bit zero EOB */
                int peek = bs_peek(b, 6);
                if (peek < 0) { ac_ok = 0; break; }
                if (peek == 0) { b->pos += 6; break; }
                /* Unary run */
                int run = 0, ok = 1;
                while (run < 5 && b->pos < b->total_bits) {
                    int bit = bs_bit(b);
                    if (bit < 0) { ok = 0; break; }
                    if (bit == 1) break;
                    run++;
                }
                if (!ok) { ac_ok = 0; break; }
                /* Alternate EOB: "100" */
                int p3 = bs_peek(b, 3);
                if (p3 == 4) { b->pos += 3; break; }
                /* VLC level */
                int level = read_vlc(b);
                if (level == -9999) { ac_ok = 0; break; }
                k += run + 1;
            }
            if (!ac_ok) break;
        }
        if (!ac_ok) break;

        frames++;
        /* Byte-align between frames */
        bs_align(b);
    }

    return frames;
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
    int      f1_count;
    int      pkt_index;  /* sequential packet number */
} Packet;

/* ---------- Assemble all packets, tracking F1 count (up to 16) ---------- */
static int assemble_packets(const uint8_t *disc, int total_sectors,
                             Packet *pkts, int max_pkts) {
    int n = 0, pos = 0, f1_count = 0;
    bool in_frame = false;

    for (int lba = 0; lba < total_sectors && n < max_pkts; lba++) {
        const uint8_t *sec = disc + (long)lba * SECTOR_RAW;
        if (sec[0] != 0x00 || sec[1] != 0xFF || sec[15] != 2) continue;
        if (sec[18] & 0x04) continue;   /* skip audio */
        if (!(sec[18] & 0x08)) continue; /* skip non-video */

        uint8_t marker = sec[24];
        if (marker == 0xF1) {
            if (!in_frame) { in_frame = true; pos = 0; f1_count = 0; }
            if (pos + F1_PAYLOAD <= MAX_FRAME) {
                memcpy(pkts[n].data + pos, sec + 25, F1_PAYLOAD);
                pos += F1_PAYLOAD;
                f1_count++;
            }
        } else if (marker == 0xF2) {
            if (in_frame && pos > 0) {
                pkts[n].size = pos;
                pkts[n].f1_count = f1_count;
                pkts[n].pkt_index = n;
                n++;
                in_frame = false;
                pos = 0;
                f1_count = 0;
            }
        } else if (marker == 0xF3) {
            in_frame = false;
            pos = 0;
            f1_count = 0;
        }
    }
    return n;
}

/* ---------- hex dump helper ---------- */
static void hexdump(const uint8_t *data, int offset, int len, const char *label) {
    printf("  %s (offset %d-%d):\n    ", label, offset, offset + len - 1);
    for (int i = 0; i < len; i++) {
        printf("%02X ", data[offset + i]);
        if ((i + 1) % 20 == 0 && i + 1 < len) printf("\n    ");
    }
    printf("\n");
}

int main(int argc, char **argv) {
    const char *cue_default = "/home/wizzard/share/GitHub/playdia-roms/"
        "Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku - Uchuu-hen (Japan).cue";

    const char *path = (argc > 1) ? argv[1] : cue_default;
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
    for (int i = 0; i < MAX_PACKETS; i++) {
        pkts[i].data = malloc(MAX_FRAME);
        if (!pkts[i].data) { fprintf(stderr, "OOM at packet %d\n", i); return 1; }
    }

    int npkt = assemble_packets(disc, total_sectors, pkts, MAX_PACKETS);
    printf("Assembled %d total video packets\n\n", npkt);
    free(disc);

    /* ================================================================
     * SECTION 1: Packet size distribution
     * ================================================================ */
    printf("========================================================\n");
    printf("SECTION 1: PACKET SIZE DISTRIBUTION (F1 sector count)\n");
    printf("========================================================\n");

    int f1_hist[17] = {0};
    for (int i = 0; i < npkt; i++) {
        int c = pkts[i].f1_count;
        if (c > 16) c = 16;
        f1_hist[c]++;
    }
    for (int c = 0; c <= 16; c++) {
        if (f1_hist[c] > 0)
            printf("  %2d F1 sectors (%5d bytes): %4d packets (%.1f%%)\n",
                   c, c * F1_PAYLOAD, f1_hist[c], 100.0 * f1_hist[c] / npkt);
    }
    printf("  Total: %d packets\n\n", npkt);

    /* ================================================================
     * SECTION 2: Header analysis for 8-F1 packets
     * ================================================================ */
    printf("========================================================\n");
    printf("SECTION 2: 8-F1 PACKET ANALYSIS\n");
    printf("========================================================\n");

    int shown_8 = 0;
    for (int i = 0; i < npkt && shown_8 < 10; i++) {
        if (pkts[i].f1_count != 8) continue;
        uint8_t *d = pkts[i].data;

        printf("\n--- 8-F1 Packet #%d (pkt index %d, %d bytes) ---\n",
               shown_8, i, pkts[i].size);

        /* 2a: Main header at offset 0 */
        hexdump(d, 0, 40, "Main header (offset 0)");
        printf("  byte[39] type = 0x%02X\n", d[39]);

        /* 2b: Check for patterns at 6*2047 and 7*2047 */
        int off6 = 6 * F1_PAYLOAD;  /* 12282 */
        int off7 = 7 * F1_PAYLOAD;  /* 14329 */

        if (pkts[i].size > off6 + 40) {
            hexdump(d, off6, 40, "Data at offset 6*2047=12282");
            /* Check for header pattern */
            if (d[off6] == 0x00 && d[off6+1] == 0x80)
                printf("  ** HEADER PATTERN at offset %d: 00 80 %02X\n", off6, d[off6+2]);
        }
        if (pkts[i].size > off7 + 40) {
            hexdump(d, off7, 40, "Data at offset 7*2047=14329");
            if (d[off7] == 0x00 && d[off7+1] == 0x80)
                printf("  ** HEADER PATTERN at offset %d: 00 80 %02X\n", off7, d[off7+2]);
        }

        /* Scan extra data for 00 80 04 or 00 80 24 pattern */
        for (int scan = off6; scan < pkts[i].size - 3; scan++) {
            if (d[scan] == 0x00 && d[scan+1] == 0x80 &&
                (d[scan+2] == 0x04 || d[scan+2] == 0x24)) {
                printf("  ** Found 00 80 %02X at offset %d (relative to 6*2047: %+d)\n",
                       d[scan+2], scan, scan - off6);
            }
        }

        shown_8++;
    }
    printf("\n");

    /* ================================================================
     * SECTION 3: 7-F1 packet analysis
     * ================================================================ */
    printf("========================================================\n");
    printf("SECTION 3: 7-F1 PACKET ANALYSIS\n");
    printf("========================================================\n");

    int shown_7 = 0;
    for (int i = 0; i < npkt && shown_7 < 10; i++) {
        if (pkts[i].f1_count != 7) continue;
        uint8_t *d = pkts[i].data;

        printf("\n--- 7-F1 Packet #%d (pkt index %d, %d bytes) ---\n",
               shown_7, i, pkts[i].size);

        hexdump(d, 0, 40, "Main header (offset 0)");
        printf("  byte[39] type = 0x%02X\n", d[39]);

        int off6 = 6 * F1_PAYLOAD;
        if (pkts[i].size > off6 + 40) {
            hexdump(d, off6, 40, "Data at offset 12282");
            if (d[off6] == 0x00 && d[off6+1] == 0x80)
                printf("  ** HEADER PATTERN at 12282: 00 80 %02X\n", d[off6+2]);
        }
        /* Scan for patterns */
        for (int scan = off6; scan < pkts[i].size - 3; scan++) {
            if (d[scan] == 0x00 && d[scan+1] == 0x80 &&
                (d[scan+2] == 0x04 || d[scan+2] == 0x24)) {
                printf("  ** Found 00 80 %02X at offset %d\n", d[scan+2], scan);
            }
        }

        shown_7++;
    }
    printf("\n");

    /* ================================================================
     * SECTION 4: Type comparison: 6-F1 (type=0x00) vs 8-F1 (type=0x07)
     * ================================================================ */
    printf("========================================================\n");
    printf("SECTION 4: TYPE COMPARISON — 6-F1 vs 8-F1\n");
    printf("========================================================\n");

    /* Gather type distribution per F1-count */
    printf("\n  Type distribution by F1 count:\n");
    int type_by_f1[17][256];
    memset(type_by_f1, 0, sizeof(type_by_f1));
    for (int i = 0; i < npkt; i++) {
        uint8_t *d = pkts[i].data;
        int fc = pkts[i].f1_count;
        if (fc > 16) fc = 16;
        if (d[0] == 0x00 && d[1] == 0x80 && d[2] == 0x04 &&
            pkts[i].size >= 40 && d[36] == 0x00 && d[37] == 0x80 && d[38] == 0x24) {
            type_by_f1[fc][d[39]]++;
        }
    }
    for (int fc = 1; fc <= 16; fc++) {
        int total = 0;
        for (int t = 0; t < 256; t++) total += type_by_f1[fc][t];
        if (total == 0) continue;
        printf("    %2d F1: ", fc);
        for (int t = 0; t < 256; t++) {
            if (type_by_f1[fc][t] > 0)
                printf("type=0x%02X:%d ", t, type_by_f1[fc][t]);
        }
        printf("(total %d)\n", total);
    }

    /* Compare header structures */
    printf("\n  Header comparison (first 3 of each):\n");

    printf("\n  -- 6-F1, type=0x00 --\n");
    int cnt = 0;
    for (int i = 0; i < npkt && cnt < 3; i++) {
        if (pkts[i].f1_count != 6) continue;
        uint8_t *d = pkts[i].data;
        if (pkts[i].size < 40) continue;
        if (d[39] != 0x00) continue;
        printf("    pkt#%d: ", i);
        for (int j = 0; j < 40; j++) printf("%02X ", d[j]);
        printf("\n");
        cnt++;
    }

    printf("\n  -- 8-F1, type=0x07 --\n");
    cnt = 0;
    for (int i = 0; i < npkt && cnt < 3; i++) {
        if (pkts[i].f1_count != 8) continue;
        uint8_t *d = pkts[i].data;
        if (pkts[i].size < 40) continue;
        if (d[39] != 0x07) continue;
        printf("    pkt#%d: ", i);
        for (int j = 0; j < 40; j++) printf("%02X ", d[j]);
        printf("\n");
        cnt++;
    }

    printf("\n  -- 8-F1, type=0x00 --\n");
    cnt = 0;
    for (int i = 0; i < npkt && cnt < 3; i++) {
        if (pkts[i].f1_count != 8) continue;
        uint8_t *d = pkts[i].data;
        if (pkts[i].size < 40) continue;
        if (d[39] != 0x00) continue;
        printf("    pkt#%d: ", i);
        for (int j = 0; j < 40; j++) printf("%02X ", d[j]);
        printf("\n");
        cnt++;
    }
    printf("\n");

    /* ================================================================
     * SECTION 5: Multi-frame decode comparison (6*2047 vs 8*2047)
     * ================================================================ */
    printf("========================================================\n");
    printf("SECTION 5: MULTI-FRAME DECODE — 6 vs 8 F1 sectors\n");
    printf("========================================================\n");

    /* For each 8-F1 packet, decode with 6-sector data vs full 8-sector data */
    int test_count = 0;
    int more_frames_count = 0, same_frames_count = 0, fewer_frames_count = 0;
    int total_frames_6 = 0, total_frames_8 = 0;

    printf("\n  %-6s %-6s %-8s %-8s %-10s %-10s %-10s %-10s\n",
           "Pkt#", "Type", "Fr(6)", "Fr(8)", "Bits(6)", "Bits(8)",
           "DataSz(6)", "DataSz(8)");

    for (int i = 0; i < npkt && test_count < 50; i++) {
        if (pkts[i].f1_count != 8) continue;
        uint8_t *d = pkts[i].data;
        if (pkts[i].size < 40) continue;
        if (d[0] != 0x00 || d[1] != 0x80 || d[2] != 0x04) continue;

        uint8_t type = (pkts[i].size >= 40) ? d[39] : 0xFF;

        /* Decode with 6*2047 bytes */
        int data6 = 6 * F1_PAYLOAD - 40;
        int trim6 = trim_padding(d, 6 * F1_PAYLOAD) - 40;
        if (trim6 < 0) trim6 = 0;
        BS bs6 = { d + 40, trim6 * 8, 0 };
        int frames6 = decode_iframe_multi(&bs6, 8);
        int bits6 = bs6.pos;

        /* Decode with full 8*2047 bytes */
        int trim8 = trim_padding(d, pkts[i].size) - 40;
        if (trim8 < 0) trim8 = 0;
        BS bs8 = { d + 40, trim8 * 8, 0 };
        int frames8 = decode_iframe_multi(&bs8, 8);
        int bits8 = bs8.pos;

        if (test_count < 30) {
            printf("  %-6d 0x%02X   %-8d %-8d %-10d %-10d %-10d %-10d",
                   i, type, frames6, frames8, bits6, bits8, trim6, trim8);
            if (frames8 > frames6) printf(" MORE!");
            else if (frames8 == frames6 && bits8 > bits6) printf(" more-bits");
            printf("\n");
        }

        total_frames_6 += frames6;
        total_frames_8 += frames8;
        if (frames8 > frames6) more_frames_count++;
        else if (frames8 == frames6) same_frames_count++;
        else fewer_frames_count++;
        test_count++;
    }

    printf("\n  Summary over %d 8-F1 packets:\n", test_count);
    printf("    More frames with 8 sectors: %d\n", more_frames_count);
    printf("    Same frames:                %d\n", same_frames_count);
    printf("    Fewer frames (unlikely):    %d\n", fewer_frames_count);
    printf("    Total frames (6-sector):    %d  (avg %.2f/pkt)\n",
           total_frames_6, test_count > 0 ? (double)total_frames_6 / test_count : 0);
    printf("    Total frames (8-sector):    %d  (avg %.2f/pkt)\n",
           total_frames_8, test_count > 0 ? (double)total_frames_8 / test_count : 0);
    printf("\n");

    /* ================================================================
     * SECTION 6: Deep dive — what IS the extra data?
     * ================================================================ */
    printf("========================================================\n");
    printf("SECTION 6: EXTRA DATA BYTE ANALYSIS\n");
    printf("========================================================\n");

    /* For 8-F1 packets: analyze the bytes past 6*2047 */
    int extra_ff = 0, extra_zero = 0, extra_other = 0, extra_total = 0;
    int extra_starts_ff = 0, extra_starts_header = 0, extra_starts_other = 0;

    for (int i = 0; i < npkt; i++) {
        if (pkts[i].f1_count != 8) continue;
        uint8_t *d = pkts[i].data;
        int off6 = 6 * F1_PAYLOAD;

        /* What does extra data start with? */
        if (d[off6] == 0xFF) extra_starts_ff++;
        else if (d[off6] == 0x00 && d[off6+1] == 0x80) extra_starts_header++;
        else extra_starts_other++;

        for (int j = off6; j < pkts[i].size; j++) {
            if (d[j] == 0xFF) extra_ff++;
            else if (d[j] == 0x00) extra_zero++;
            else extra_other++;
            extra_total++;
        }
    }

    printf("  Extra data (bytes past 6*2047) in 8-F1 packets:\n");
    printf("    Total extra bytes: %d across %d packets\n",
           extra_total, f1_hist[8]);
    printf("    0xFF bytes: %d (%.1f%%)\n", extra_ff,
           extra_total > 0 ? 100.0 * extra_ff / extra_total : 0);
    printf("    0x00 bytes: %d (%.1f%%)\n", extra_zero,
           extra_total > 0 ? 100.0 * extra_zero / extra_total : 0);
    printf("    Other:      %d (%.1f%%)\n", extra_other,
           extra_total > 0 ? 100.0 * extra_other / extra_total : 0);
    printf("\n    Extra data starts with:\n");
    printf("      0xFF (padding):    %d\n", extra_starts_ff);
    printf("      00 80 (header):    %d\n", extra_starts_header);
    printf("      Other:             %d\n", extra_starts_other);

    /* Check where bitstream data actually ends vs 6*2047 boundary */
    printf("\n  Bitstream end position analysis (8-F1 packets, first 20):\n");
    int bs_end_count = 0;
    for (int i = 0; i < npkt && bs_end_count < 20; i++) {
        if (pkts[i].f1_count != 8) continue;
        uint8_t *d = pkts[i].data;
        if (d[0] != 0x00 || d[1] != 0x80 || d[2] != 0x04) continue;

        int trimmed = trim_padding(d, pkts[i].size);
        BS bs = { d + 40, (trimmed - 40) * 8, 0 };
        int frames = decode_iframe_multi(&bs, 8);
        int bytes_consumed = (bs.pos + 7) / 8;
        int abs_end = 40 + bytes_consumed;
        int off6 = 6 * F1_PAYLOAD;

        printf("    pkt#%d (type=0x%02X): %d frames, bitstream ends at byte %d, 6*2047=%d, diff=%+d, trimmed=%d\n",
               i, d[39], frames, abs_end, off6, abs_end - off6, trimmed);
        bs_end_count++;
    }

    /* ================================================================
     * SECTION 7: Check if extra sectors contain a SECOND packet header
     * ================================================================ */
    printf("\n========================================================\n");
    printf("SECTION 7: SECOND HEADER SEARCH IN EXTRA DATA\n");
    printf("========================================================\n");

    int second_header_count = 0;
    for (int i = 0; i < npkt; i++) {
        if (pkts[i].f1_count < 7) continue;
        uint8_t *d = pkts[i].data;
        int off6 = 6 * F1_PAYLOAD;

        /* Search from byte off6 to end for 00 80 04 ... 00 80 24 pattern (40-byte header) */
        for (int scan = off6; scan < pkts[i].size - 40; scan++) {
            if (d[scan] == 0x00 && d[scan+1] == 0x80 && d[scan+2] == 0x04 &&
                d[scan+36] == 0x00 && d[scan+37] == 0x80 && d[scan+38] == 0x24) {
                if (second_header_count < 20) {
                    printf("  pkt#%d (f1=%d, type1=0x%02X): SECOND HEADER at offset %d, type2=0x%02X\n",
                           i, pkts[i].f1_count, d[39], scan, d[scan+39]);
                    printf("    Hdr1: ");
                    for (int j = 0; j < 40; j++) printf("%02X ", d[j]);
                    printf("\n    Hdr2: ");
                    for (int j = 0; j < 40; j++) printf("%02X ", d[scan+j]);
                    printf("\n");
                }
                second_header_count++;
                break;  /* only report first match per packet */
            }
        }
    }
    printf("  Total packets with second AK8000 header in extra data: %d\n\n", second_header_count);

    /* Cleanup */
    for (int i = 0; i < MAX_PACKETS; i++) free(pkts[i].data);
    free(pkts);
    free(resolved);
    return 0;
}
