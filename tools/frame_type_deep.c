/*
 * frame_type_deep.c - Deep analysis of frame type byte [39] and bitstream content
 *
 * Assembles 6 F1 sectors per packet (raw Mode 2/2352, F1 video at offset 25, 2047 bytes each),
 * producing a 40-byte header + bitstream. Analyzes byte [39] as potential bitfield,
 * attempts I-frame DC decode, and classifies frames.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SECTOR_SIZE     2352
#define F1_OFFSET       25
#define F1_PAYLOAD      2047
#define SECTORS_PER_PKT 6
#define PKT_SIZE        (SECTORS_PER_PKT * F1_PAYLOAD)  /* 12282 */
#define HDR_SIZE        40
#define MAX_PKTS        500
#define ANALYSIS_PKTS   50

/* DC VLC table: extended MPEG-1 luminance DC sizes 0..16 */
static const struct { int len; uint32_t code; } vlc_t[17] = {
    {3,0x4},{2,0x0},{2,0x1},{3,0x5},{3,0x6},{4,0xE},{5,0x1E},{6,0x3E},{7,0x7E},
    {8,0xFE},{9,0x1FE},{10,0x3FE},{11,0x7FE},{12,0xFFE},{13,0x1FFE},{14,0x3FFE},{15,0x7FFE}
};

/* Bitstream reader */
typedef struct {
    const uint8_t *data;
    int size;       /* in bytes */
    int bit_pos;    /* current bit position */
} bitstream_t;

static inline int bs_bits_left(bitstream_t *bs) {
    return bs->size * 8 - bs->bit_pos;
}

static inline uint32_t bs_peek(bitstream_t *bs, int n) {
    if (n == 0) return 0;
    uint32_t val = 0;
    int pos = bs->bit_pos;
    for (int i = 0; i < n; i++) {
        int byte_idx = (pos + i) / 8;
        int bit_idx  = 7 - ((pos + i) % 8);  /* MSB first */
        if (byte_idx < bs->size)
            val = (val << 1) | ((bs->data[byte_idx] >> bit_idx) & 1);
        else
            val = (val << 1);
    }
    return val;
}

static inline void bs_skip(bitstream_t *bs, int n) {
    bs->bit_pos += n;
}

static inline uint32_t bs_read(bitstream_t *bs, int n) {
    uint32_t v = bs_peek(bs, n);
    bs_skip(bs, n);
    return v;
}

/* Decode one DC size via VLC, return size or -1 on failure */
static int decode_dc_size(bitstream_t *bs) {
    if (bs_bits_left(bs) < 2) return -1;

    for (int s = 0; s <= 16; s++) {
        int len = vlc_t[s].len;
        if (bs_bits_left(bs) < len) continue;
        uint32_t bits = bs_peek(bs, len);
        if (bits == vlc_t[s].code) {
            bs_skip(bs, len);
            return s;
        }
    }
    return -1;
}

/* Decode one DC coefficient: VLC size + value bits, return differential */
static int decode_dc_coeff(bitstream_t *bs, int *ok) {
    int size = decode_dc_size(bs);
    if (size < 0) { *ok = 0; return 0; }
    if (size == 0) { *ok = 1; return 0; }
    if (bs_bits_left(bs) < size) { *ok = 0; return 0; }
    uint32_t val = bs_read(bs, size);
    /* MPEG-1 style: if MSB is 0, value is negative */
    if (!(val & (1 << (size - 1)))) {
        val = val - (1 << size) + 1;  /* two's complement style */
    }
    *ok = 1;
    return (int)val;
}

/*
 * Try to decode 864 DC coefficients (I-frame: 36x24 blocks = 864 for Y,
 * but with 4:2:0 it's 36*24*6/4 = ... let's do: 144 macroblocks * 6 blocks = 864)
 * Using DPCM with 3 predictors (Y, Cb, Cr).
 * Returns number of DCs successfully decoded.
 */
static int try_decode_dcs(const uint8_t *bitstream, int bs_size, int *dc_values, int max_dcs) {
    bitstream_t bs;
    bs.data = bitstream;
    bs.size = bs_size;
    bs.bit_pos = 0;

    int pred[3] = {0, 0, 0};  /* Y, Cb, Cr predictors */
    int count = 0;

    for (int mb = 0; mb < max_dcs / 6 && count < max_dcs; mb++) {
        /* 4 Y blocks, 1 Cb, 1 Cr per macroblock */
        for (int blk = 0; blk < 6 && count < max_dcs; blk++) {
            int comp;
            if (blk < 4) comp = 0;       /* Y */
            else if (blk == 4) comp = 1;  /* Cb */
            else comp = 2;                /* Cr */

            int ok = 0;
            int diff = decode_dc_coeff(&bs, &ok);
            if (!ok) return count;

            pred[comp] += diff;
            dc_values[count++] = pred[comp];
        }
    }
    return count;
}

/* Parse CUE file to get Track 2 bin filename */
static char *parse_cue_track2(const char *cue_path) {
    FILE *f = fopen(cue_path, "r");
    if (!f) return NULL;

    static char binfile[1024];
    char line[1024];
    char current_file[1024] = {0};
    int in_track2 = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "FILE ", 5) == 0) {
            /* Extract filename between quotes */
            char *q1 = strchr(line, '"');
            if (q1) {
                char *q2 = strchr(q1 + 1, '"');
                if (q2) {
                    int len = q2 - q1 - 1;
                    strncpy(current_file, q1 + 1, len);
                    current_file[len] = 0;
                }
            }
        }
        if (strstr(line, "TRACK 02")) {
            in_track2 = 1;
        }
        if (in_track2 && strstr(line, "INDEX 01")) {
            /* Build full path */
            char *dir_end = strrchr(cue_path, '/');
            if (dir_end) {
                int dlen = dir_end - cue_path + 1;
                strncpy(binfile, cue_path, dlen);
                binfile[dlen] = 0;
                strcat(binfile, current_file);
            } else {
                strcpy(binfile, current_file);
            }
            fclose(f);
            return binfile;
        }
    }
    fclose(f);
    return NULL;
}

int main(int argc, char **argv) {
    const char *cue_path = "/home/wizzard/share/GitHub/playdia-roms/"
        "Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku - Uchuu-hen (Japan).cue";

    char *bin_path = parse_cue_track2(cue_path);
    if (!bin_path) {
        fprintf(stderr, "Failed to parse CUE for Track 2\n");
        return 1;
    }
    printf("BIN file: %s\n\n", bin_path);

    FILE *f = fopen(bin_path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open: %s\n", bin_path);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    long num_sectors = file_size / SECTOR_SIZE;
    fseek(f, 0, SEEK_SET);

    printf("File size: %ld bytes, %ld sectors\n\n", file_size, num_sectors);

    /* Read all sectors */
    uint8_t *sector_buf = malloc(SECTOR_SIZE);
    if (!sector_buf) { perror("malloc"); return 1; }

    /* Collect F1 video sectors and assemble packets */
    uint8_t *pkt_buf = malloc(PKT_SIZE);
    int dc_values[864];

    /* Bitfield frequency tracking for byte[39] across ALL packets */
    int bit_freq[8] = {0};
    int type_counts[256] = {0};
    int total_packets = 0;

    /* Track decode results per type */
    typedef struct {
        uint8_t type;
        int dc_count;
        int dc_vals[864];
    } pkt_info_t;

    pkt_info_t *all_info = malloc(MAX_PKTS * sizeof(pkt_info_t));

    int f1_count = 0;
    int pkt_idx = 0;

    for (long s = 0; s < num_sectors && pkt_idx < MAX_PKTS; s++) {
        fseek(f, s * SECTOR_SIZE, SEEK_SET);
        if (fread(sector_buf, 1, SECTOR_SIZE, f) != SECTOR_SIZE) break;

        /* Check submode byte for Form 1 video - sector[18] typically has submode */
        /* For raw Mode 2, data starts at offset 16 (after sync+header) */
        /* F1 video data at offset 25 with 2047 bytes payload */
        /* We check if this looks like a video sector by subheader */
        uint8_t subheader_file = sector_buf[16];
        uint8_t subheader_chan = sector_buf[17];
        uint8_t subheader_sub  = sector_buf[18];
        uint8_t subheader_ci   = sector_buf[19];

        /* Video sectors typically have specific subheader patterns */
        /* Let's just collect sectors that look like they have video subheader */
        /* Form 1 = bit 5 of submode is 0, video = bit 1 of submode */
        int is_video = (subheader_sub & 0x02) && !(subheader_sub & 0x20);
        /* Also accept data sectors */
        int is_data = (subheader_sub & 0x08) && !(subheader_sub & 0x20);

        if (!is_video && !is_data) {
            /* Reset assembly if we hit non-video */
            if (f1_count > 0 && f1_count < SECTORS_PER_PKT) {
                f1_count = 0;
            }
            continue;
        }

        /* Copy F1 payload */
        memcpy(pkt_buf + f1_count * F1_PAYLOAD, sector_buf + F1_OFFSET, F1_PAYLOAD);
        f1_count++;

        if (f1_count == SECTORS_PER_PKT) {
            /* Packet assembled */
            uint8_t type_byte = pkt_buf[39];
            type_counts[type_byte]++;
            for (int b = 0; b < 8; b++) {
                if (type_byte & (1 << b)) bit_freq[b]++;
            }

            /* Try DC decode */
            const uint8_t *bitstream = pkt_buf + HDR_SIZE;
            int bs_size = PKT_SIZE - HDR_SIZE;
            int ndcs = try_decode_dcs(bitstream, bs_size, dc_values, 864);

            all_info[pkt_idx].type = type_byte;
            all_info[pkt_idx].dc_count = ndcs;
            if (ndcs > 0) memcpy(all_info[pkt_idx].dc_vals, dc_values, ndcs * sizeof(int));

            /* Print full header for first ANALYSIS_PKTS packets */
            if (pkt_idx < ANALYSIS_PKTS) {
                printf("=== Packet %3d | type=0x%02X | DCs decoded: %d/864 ===\n",
                       pkt_idx, type_byte, ndcs);
                printf("  Header: ");
                for (int i = 0; i < HDR_SIZE; i++) {
                    printf("%02X ", pkt_buf[i]);
                    if (i == 19) printf("\n          ");
                }
                printf("\n");

                /* DC statistics */
                if (ndcs >= 864) {
                    int min_dc = 9999, max_dc = -9999;
                    double sum = 0, sum2 = 0;
                    int zero_count = 0;
                    int small_count = 0;  /* |val| <= 5 */
                    for (int i = 0; i < ndcs; i++) {
                        if (dc_values[i] < min_dc) min_dc = dc_values[i];
                        if (dc_values[i] > max_dc) max_dc = dc_values[i];
                        sum += dc_values[i];
                        sum2 += (double)dc_values[i] * dc_values[i];
                        if (dc_values[i] == 0) zero_count++;
                        if (abs(dc_values[i]) <= 5) small_count++;
                    }
                    double mean = sum / ndcs;
                    double var = sum2 / ndcs - mean * mean;
                    printf("  DC stats: min=%d max=%d mean=%.1f stddev=%.1f zeros=%d small(<=5)=%d\n",
                           min_dc, max_dc, mean, sqrt(var > 0 ? var : 0), zero_count, small_count);
                }

                printf("\n");
            }

            pkt_idx++;
            total_packets++;
            f1_count = 0;
        }
    }

    /* Continue counting remaining packets */
    /* (already counted up to MAX_PKTS) */

    printf("========================================\n");
    printf("TOTAL PACKETS ANALYZED: %d\n\n", pkt_idx);

    /* Type byte frequency */
    printf("--- Type byte [39] frequency ---\n");
    for (int t = 0; t < 256; t++) {
        if (type_counts[t] > 0) {
            printf("  type=0x%02X: %d packets", t, type_counts[t]);
            printf("  (bits: ");
            for (int b = 7; b >= 0; b--) printf("%d", (t >> b) & 1);
            printf(")\n");
        }
    }

    /* Bit frequency */
    printf("\n--- Bit frequencies in byte[39] (across %d packets) ---\n", pkt_idx);
    for (int b = 0; b < 8; b++) {
        printf("  bit %d: set in %d/%d packets (%.1f%%)\n",
               b, bit_freq[b], pkt_idx, 100.0 * bit_freq[b] / (pkt_idx ? pkt_idx : 1));
    }

    /* DC decode success per type */
    printf("\n--- DC decode success per type ---\n");
    for (int t = 0; t < 256; t++) {
        if (type_counts[t] == 0) continue;
        int full_decode = 0, partial = 0, fail = 0;
        for (int p = 0; p < pkt_idx; p++) {
            if (all_info[p].type != t) continue;
            if (all_info[p].dc_count >= 864) full_decode++;
            else if (all_info[p].dc_count > 100) partial++;
            else fail++;
        }
        printf("  type=0x%02X: full(864)=%d  partial(>100)=%d  fail(<=100)=%d  total=%d\n",
               t, full_decode, partial, fail, type_counts[t]);
    }

    /* I-frame vs P-frame analysis for packets with full DC decode */
    printf("\n--- I-frame vs P-frame classification (full DC decode only) ---\n");
    for (int t = 0; t < 256; t++) {
        if (type_counts[t] == 0) continue;
        int iframe_like = 0, pframe_like = 0;
        for (int p = 0; p < pkt_idx; p++) {
            if (all_info[p].type != t) continue;
            if (all_info[p].dc_count < 864) continue;

            /* Classify: I-frame has wide range, P-frame is centered near 0 */
            int min_dc = 9999, max_dc = -9999;
            int small = 0;
            double sum = 0;
            for (int i = 0; i < 864; i++) {
                int v = all_info[p].dc_vals[i];
                if (v < min_dc) min_dc = v;
                if (v > max_dc) max_dc = v;
                sum += v;
                if (abs(v) <= 3) small++;
            }
            double mean = sum / 864.0;
            int range = max_dc - min_dc;

            /* I-frame: range > 50, not mostly small values */
            /* P-frame: range small or mostly near zero */
            if (range > 50 && small < 600) iframe_like++;
            else pframe_like++;
        }
        if (iframe_like + pframe_like > 0) {
            printf("  type=0x%02X: I-frame-like=%d  P-frame-like=%d\n",
                   t, iframe_like, pframe_like);
        }
    }

    /* Dump first 20 DC values for P-frame candidates (types 0x06, 0x07) */
    printf("\n--- First 20 DC values for types 0x06 and 0x07 ---\n");
    int dumped_06 = 0, dumped_07 = 0;
    for (int p = 0; p < pkt_idx; p++) {
        uint8_t t = all_info[p].type;
        if (t != 0x06 && t != 0x07) continue;
        if (t == 0x06 && dumped_06 >= 3) continue;
        if (t == 0x07 && dumped_07 >= 3) continue;

        int ndcs = all_info[p].dc_count;
        int show = ndcs < 20 ? ndcs : 20;
        printf("  Pkt %d (type=0x%02X, %d DCs): ", p, t, ndcs);
        for (int i = 0; i < show; i++) {
            printf("%d ", all_info[p].dc_vals[i]);
        }
        if (ndcs < 20) printf("[decode stopped]");
        printf("\n");

        if (t == 0x06) dumped_06++;
        if (t == 0x07) dumped_07++;
    }

    /* Also dump first 20 DCs for type 0x00 for comparison */
    printf("\n--- First 20 DC values for type 0x00 (I-frame reference) ---\n");
    int dumped_00 = 0;
    for (int p = 0; p < pkt_idx && dumped_00 < 3; p++) {
        if (all_info[p].type != 0x00) continue;
        int ndcs = all_info[p].dc_count;
        int show = ndcs < 20 ? ndcs : 20;
        printf("  Pkt %d (type=0x00, %d DCs): ", p, ndcs);
        for (int i = 0; i < show; i++) {
            printf("%d ", all_info[p].dc_vals[i]);
        }
        printf("\n");
        dumped_00++;
    }

    /* Additional: check byte[39] relationship with other header bytes */
    printf("\n--- Byte[39] vs byte[38] correlation (first 50 packets) ---\n");
    for (int p = 0; p < pkt_idx && p < ANALYSIS_PKTS; p++) {
        /* Re-read header bytes from stored info - we need to re-examine */
        /* We only stored type and dc info, so let's print from stored packets */
    }

    /* Let's do another pass for header correlation, examining bytes 36-39 */
    printf("  (See full headers above for correlation)\n");

    /* Summary */
    printf("\n========================================\n");
    printf("SUMMARY\n");
    printf("========================================\n");

    int type00_iframe = 0, type00_total = 0;
    int type06_iframe = 0, type06_total = 0;
    int type07_iframe = 0, type07_total = 0;

    for (int p = 0; p < pkt_idx; p++) {
        if (all_info[p].dc_count < 864) continue;

        int small = 0, range;
        int min_dc = 9999, max_dc = -9999;
        for (int i = 0; i < 864; i++) {
            int v = all_info[p].dc_vals[i];
            if (v < min_dc) min_dc = v;
            if (v > max_dc) max_dc = v;
            if (abs(v) <= 3) small++;
        }
        range = max_dc - min_dc;
        int is_iframe = (range > 50 && small < 600);

        if (all_info[p].type == 0x00) { type00_total++; if (is_iframe) type00_iframe++; }
        if (all_info[p].type == 0x06) { type06_total++; if (is_iframe) type06_iframe++; }
        if (all_info[p].type == 0x07) { type07_total++; if (is_iframe) type07_iframe++; }
    }

    printf("type=0x00: %d/%d full-decode are I-frame-like\n", type00_iframe, type00_total);
    printf("type=0x06: %d/%d full-decode are I-frame-like\n", type06_iframe, type06_total);
    printf("type=0x07: %d/%d full-decode are I-frame-like\n", type07_iframe, type07_total);

    free(all_info);
    free(pkt_buf);
    free(sector_buf);
    fclose(f);
    return 0;
}
