/*
 * frame_type_stats.c - Scan a Playdia disc image (raw Track 2 bin)
 * and report statistics about frame types found in video packets.
 *
 * Disc format:
 *   - Raw Mode 2/2352-byte sectors
 *   - Video sectors: [0]=0x00, [1]=0xFF, [15]=2 (mode 2),
 *     [18] bit3 set & bit2 clear (data sector)
 *   - Marker byte at [24]: 0xF1=video data, 0xF2=end-of-frame, 0xF3=scene marker
 *   - F1 sectors carry 2047 bytes of payload starting at [25]
 *   - 6 x F1 sectors = one video packet (12282 bytes)
 *   - Packet header (40 bytes): [0-2]=00 80 04, [3]=QS, [4-19]=qtable,
 *     [20-35]=qtable copy, [36-38]=00 80 24, [39]=TYPE
 *
 * Usage: frame_type_stats <track2.bin>
 *        frame_type_stats <file.cue>   (auto-finds Track 2 bin)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SECTOR_SIZE   2352
#define F1_PAYLOAD    2047
#define F1_PER_PACKET 6
#define PACKET_SIZE   (F1_PAYLOAD * F1_PER_PACKET)  /* 12282 */
#define MAX_SEQ       2048

static int is_video_sector(const uint8_t *s)
{
    if (s[0] != 0x00 || s[1] != 0xFF) return 0;
    if (s[15] != 2) return 0;
    if (!(s[18] & 0x08) || (s[18] & 0x04)) return 0;
    return 1;
}

/* Try to resolve a CUE file to Track 2 bin path */
static char *resolve_cue(const char *cuepath)
{
    FILE *f = fopen(cuepath, "r");
    if (!f) return NULL;

    char line[1024];
    int track_num = 0;
    char *result = NULL;
    char current_file[1024] = {0};

    /* Get directory from cuepath */
    char dir[1024] = {0};
    const char *slash = strrchr(cuepath, '/');
    if (slash) {
        size_t dlen = slash - cuepath;
        memcpy(dir, cuepath, dlen);
        dir[dlen] = '/';
        dir[dlen + 1] = 0;
    }

    while (fgets(line, sizeof(line), f)) {
        /* Parse FILE lines */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (strncmp(p, "FILE ", 5) == 0) {
            /* Extract filename between quotes */
            char *q1 = strchr(p, '"');
            if (q1) {
                char *q2 = strchr(q1 + 1, '"');
                if (q2) {
                    size_t len = q2 - q1 - 1;
                    memcpy(current_file, q1 + 1, len);
                    current_file[len] = 0;
                }
            }
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

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <track2.bin or file.cue>\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    char *resolved = NULL;

    /* If it ends with .cue, resolve to Track 2 bin */
    size_t plen = strlen(path);
    if (plen > 4 && strcasecmp(path + plen - 4, ".cue") == 0) {
        resolved = resolve_cue(path);
        if (!resolved) {
            fprintf(stderr, "Could not find Track 2 in CUE file: %s\n", path);
            return 1;
        }
        printf("Resolved CUE -> Track 2: %s\n", resolved);
        path = resolved;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        free(resolved);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    long total_sectors = file_size / SECTOR_SIZE;
    printf("File size: %ld bytes (%ld sectors)\n\n", file_size, total_sectors);

    uint8_t sector[SECTOR_SIZE];
    uint8_t packet[PACKET_SIZE];
    int f1_count = 0;        /* F1 sectors accumulated so far */
    int total_packets = 0;
    int type_counts[256] = {0};

    /* Sequence recording */
    uint8_t seq_types[MAX_SEQ];
    uint8_t seq_qs[MAX_SEQ];
    int seq_len = 0;

    int total_f1 = 0, total_f2 = 0, total_f3 = 0;
    int total_video_sectors = 0;

    for (long s = 0; s < total_sectors; s++) {
        if (fread(sector, SECTOR_SIZE, 1, f) != 1) break;

        if (!is_video_sector(sector)) continue;
        total_video_sectors++;

        uint8_t marker = sector[24];

        if (marker == 0xF3) {
            /* Scene marker - reset packet assembly */
            f1_count = 0;
            total_f3++;
            continue;
        }

        if (marker == 0xF1) {
            total_f1++;
            if (f1_count < F1_PER_PACKET) {
                memcpy(packet + f1_count * F1_PAYLOAD, sector + 25, F1_PAYLOAD);
                f1_count++;
            }
            continue;
        }

        if (marker == 0xF2) {
            total_f2++;
            /* End of frame - if we have a full packet, process it */
            if (f1_count >= F1_PER_PACKET) {
                uint8_t qs = packet[3];
                uint8_t frame_type = packet[39];

                type_counts[frame_type]++;
                total_packets++;

                if (seq_len < MAX_SEQ) {
                    seq_types[seq_len] = frame_type;
                    seq_qs[seq_len] = qs;
                    seq_len++;
                }
            }
            f1_count = 0;
            continue;
        }
    }

    fclose(f);
    free(resolved);

    /* Report */
    printf("=== Sector Statistics ===\n");
    printf("Total video sectors: %d\n", total_video_sectors);
    printf("  F1 (video data):   %d\n", total_f1);
    printf("  F2 (end-of-frame): %d\n", total_f2);
    printf("  F3 (scene marker): %d\n", total_f3);
    printf("\n");

    printf("=== Packet Statistics ===\n");
    printf("Total assembled packets: %d\n", total_packets);
    printf("\nFrame type breakdown:\n");
    for (int i = 0; i < 256; i++) {
        if (type_counts[i] > 0) {
            printf("  Type 0x%02X: %5d packets (%5.1f%%)\n",
                   i, type_counts[i],
                   100.0 * type_counts[i] / total_packets);
        }
    }

    printf("\n=== First %d packets (Type, QS) ===\n",
           seq_len < 20 ? seq_len : 20);
    int show = seq_len < 20 ? seq_len : 20;
    for (int i = 0; i < show; i++) {
        printf("  Packet %3d: Type=0x%02X  QS=%d\n",
               i, seq_types[i], seq_qs[i]);
    }

    if (seq_len > 20) {
        printf("  ... (%d more packets)\n", seq_len - 20);
    }

    /* Show type sequence pattern for first 80 packets */
    printf("\n=== Type sequence (first %d) ===\n",
           seq_len < 80 ? seq_len : 80);
    int show2 = seq_len < 80 ? seq_len : 80;
    for (int i = 0; i < show2; i++) {
        printf("%02X ", seq_types[i]);
        if ((i + 1) % 20 == 0) printf("\n");
    }
    if (show2 % 20 != 0) printf("\n");

    return 0;
}
