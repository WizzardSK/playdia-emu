/*
 * Playdia Disc Sector Scanner
 * Shows the interleaving pattern and detailed sector info for a range of LBAs
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <zip.h>

#define SECTOR_RAW 2352

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <game.zip> [start_lba] [count]\n", argv[0]);
        return 1;
    }
    int start_lba = argc > 2 ? atoi(argv[2]) : 140;
    int count = argc > 3 ? atoi(argv[3]) : 40;

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
    printf("Scanning LBA %d to %d (of %d total)\n\n", start_lba, start_lba + count - 1, total_sectors);
    printf("LBA   Mode Sub  Cod  File Ch  Marker  First8_payload\n");
    printf("----  ---- ---  ---  ---- --  ------  ---------------\n");

    for (int lba = start_lba; lba < start_lba + count && lba < total_sectors; lba++) {
        uint8_t *s = disc + (long)lba * SECTOR_RAW;

        // Check sync
        if (s[0] != 0x00 || s[1] != 0xFF) {
            printf("%5d  (no sync)\n", lba);
            continue;
        }

        uint8_t mode = s[15];
        uint8_t file_num = s[16];    // file number
        uint8_t channel = s[17];     // channel
        uint8_t submode = s[18];     // submode flags
        uint8_t coding = s[19];      // coding info

        // Decode submode flags
        char flags[64] = "";
        if (submode & 0x80) strcat(flags, "EOF ");
        if (submode & 0x40) strcat(flags, "RT ");
        if (submode & 0x20) strcat(flags, "F2 ");  // Form 2
        if (submode & 0x10) strcat(flags, "TRIG ");
        if (submode & 0x08) strcat(flags, "DATA ");
        if (submode & 0x04) strcat(flags, "AUD ");
        if (submode & 0x02) strcat(flags, "VID ");
        if (submode & 0x01) strcat(flags, "EOR ");

        // Payload first 8 bytes
        uint8_t *p = s + 24;
        char marker[16] = "---";
        if (!(submode & 0x04)) { // not audio
            if (p[0] == 0xF1) strcpy(marker, "F1-VID");
            else if (p[0] == 0xF2) strcpy(marker, "F2-END");
            else if (p[0] == 0xF3) strcpy(marker, "F3-SCN");
            else snprintf(marker, sizeof(marker), "?%02X", p[0]);
        }

        printf("%5d  M%d  %02X   %02X   F%d  C%d  %-7s [%02X %02X %02X %02X %02X %02X %02X %02X]  %s\n",
               lba, mode, submode, coding, file_num, channel, marker,
               p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
               flags);

        // For audio sectors, show coding info
        if (submode & 0x04) {
            int stereo = coding & 1;
            int rate = (coding & 4) ? 18900 : 37800;
            int bits = (coding & 0x10) ? 8 : 4;
            printf("       Audio: %s, %dHz, %d-bit ADPCM\n",
                   stereo ? "stereo" : "mono", rate, bits);
        }
    }

    // Also: dump the full first F1 sector payload (bytes 24-2352) for the first video frame
    printf("\n\n=== Full first F1 sector payload dump ===\n");
    for (int lba = start_lba; lba < total_sectors; lba++) {
        uint8_t *s = disc + (long)lba * SECTOR_RAW;
        if (s[0] != 0x00 || s[1] != 0xFF) continue;
        if (s[15] != 2) continue;
        if (s[18] & 0x04) continue; // skip audio
        if (s[24] == 0xF1) {
            printf("LBA %d: First F1 sector, full subheader + payload:\n", lba);
            printf("  Subheader [16-23]: ");
            for (int i = 16; i < 24; i++) printf("%02X ", s[i]);
            printf("\n  Payload [24-87] (first 64 bytes):\n  ");
            for (int i = 24; i < 88 && i < 2352; i++) {
                printf("%02X ", s[i]);
                if ((i - 24) % 32 == 31) printf("\n  ");
            }
            printf("\n  Payload [2316-2351] (last 36 bytes):\n  ");
            for (int i = 2316; i < 2352; i++) printf("%02X ", s[i]);
            printf("\n");
            break;
        }
    }

    // Dump the byte right before F1 marker removal to see if we're stripping correctly
    printf("\n=== Checking F1 sector boundaries ===\n");
    int f1_count = 0;
    for (int lba = start_lba; lba < total_sectors && f1_count < 10; lba++) {
        uint8_t *s = disc + (long)lba * SECTOR_RAW;
        if (s[0] != 0x00 || s[1] != 0xFF) continue;
        if (s[15] != 2) continue;
        if (s[18] & 0x04) continue;
        if (s[24] == 0xF1 || s[24] == 0xF2) {
            printf("LBA %d [%s]: bytes 24-31: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                   lba, s[24] == 0xF1 ? "F1" : "F2",
                   s[24], s[25], s[26], s[27], s[28], s[29], s[30], s[31]);
            if (s[24] == 0xF1) f1_count++;
        }
    }

    free(disc);
    zip_close(z);
    return 0;
}
