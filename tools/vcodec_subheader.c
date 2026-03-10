/*
 * vcodec_subheader.c - Examine raw CD-ROM XA sector subheaders
 *
 * CD-ROM XA sectors have:
 * - 12 bytes sync
 * - 4 bytes header (minute, second, frame, mode)
 * - 4 bytes subheader (file#, channel#, submode, coding)
 * - 4 bytes subheader copy
 * - 2048 bytes user data (or 2324 for Form 2)
 * - 4 bytes EDC/ECC
 *
 * The subheader coding byte might contain information about the video format.
 * Also look for any structure in the first few bytes of user data beyond
 * the type byte (F1/F2/F3).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int main() {
    const char *binfile = "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin";
    FILE *fp = fopen(binfile, "rb");
    if(!fp) { printf("Can't open\n"); return 1; }

    printf("=== Sector analysis around LBA 502 ===\n");
    printf("LBA  Sync  Hdr(MMSF)     SubHdr(FN CH SM CI)  Type  First16 UserData\n");

    for(int lba = 500; lba < 530; lba++) {
        long off = (long)lba * 2352;
        uint8_t sec[2352];
        fseek(fp, off, SEEK_SET);
        if(fread(sec, 1, 2352, fp) != 2352) break;

        /* Sync pattern check */
        int sync_ok = (sec[0]==0x00 && sec[1]==0xFF && sec[2]==0xFF &&
                       sec[11]==0x00);

        /* Header */
        printf("%4d  %s  %02X:%02X:%02X:%02X  ",
               lba, sync_ok ? "OK" : "NO",
               sec[12], sec[13], sec[14], sec[15]);

        /* Subheader (bytes 16-19, copy at 20-23) */
        printf("%02X %02X %02X %02X  ",
               sec[16], sec[17], sec[18], sec[19]);

        /* Type byte (byte 24 = first user data byte) */
        printf("%02X    ", sec[24]);

        /* First 16 bytes of user data (24-39) */
        for(int i = 24; i < 40; i++) printf("%02X ", sec[i]);
        printf("\n");
    }

    /* Also check sector structure around LBA 150 (logo frames) */
    printf("\n=== Sector analysis around LBA 150 ===\n");
    for(int lba = 148; lba < 170; lba++) {
        long off = (long)lba * 2352;
        uint8_t sec[2352];
        fseek(fp, off, SEEK_SET);
        if(fread(sec, 1, 2352, fp) != 2352) break;

        int sync_ok = (sec[0]==0x00 && sec[1]==0xFF && sec[2]==0xFF && sec[11]==0x00);

        printf("%4d  %s  %02X:%02X:%02X:%02X  %02X %02X %02X %02X  %02X    ",
               lba, sync_ok?"OK":"NO",
               sec[12],sec[13],sec[14],sec[15],
               sec[16],sec[17],sec[18],sec[19],
               sec[24]);
        for(int i=25;i<41;i++) printf("%02X ",sec[i]);
        printf("\n");
    }

    /* Examine the FULL structure of a video frame's sectors */
    printf("\n=== Full sector details for first frame at LBA 502 ===\n");
    int f1_count = 0;
    for(int lba = 502; lba < 520; lba++) {
        long off = (long)lba * 2352;
        uint8_t sec[2352];
        fseek(fp, off, SEEK_SET);
        if(fread(sec, 1, 2352, fp) != 2352) break;

        uint8_t type = sec[24];
        printf("LBA %d: subhdr=[%02X %02X %02X %02X] type=%02X",
               lba, sec[16],sec[17],sec[18],sec[19], type);

        if(type == 0xF1) {
            printf(" (VIDEO DATA #%d)", f1_count);
            if(f1_count == 0) {
                /* First sector of frame - show header */
                printf("\n  Frame header: ");
                for(int i=25;i<65;i++) printf("%02X ",sec[i]);
                printf("\n  QS=%d, type=%d", sec[25+3], sec[25+39]);
                printf("\n  QtableA: ");
                for(int i=0;i<16;i++) printf("%02X ",sec[25+4+i]);
                printf("\n  QtableB: ");
                for(int i=0;i<16;i++) printf("%02X ",sec[25+20+i]);
            }
            f1_count++;
        } else if(type == 0xF2) {
            printf(" (END OF FRAME)");
            f1_count = 0;
            /* Check what's in the F2 sector */
            printf("\n  F2 data: ");
            for(int i=25;i<57;i++) printf("%02X ",sec[i]);
        } else if(type == 0xF3) {
            printf(" (SCENE MARKER)");
            printf("\n  F3 data: ");
            for(int i=25;i<57;i++) printf("%02X ",sec[i]);
        } else if(sec[18] == 0x64) {
            printf(" (AUDIO)");
        } else {
            printf(" (OTHER)");
        }
        printf("\n");
    }

    /* Check if there's any data BETWEEN the type byte and frame header */
    printf("\n=== Checking for extra bytes between type and header ===\n");
    /* In our current assumption: type byte is at offset 24, header starts at 25 */
    /* What if there are additional bytes? */
    for(int lba = 502; lba < 510; lba++) {
        long off = (long)lba * 2352;
        uint8_t sec[2352];
        fseek(fp, off, SEEK_SET);
        if(fread(sec, 1, 2352, fp) != 2352) break;

        if(sec[24] == 0xF1) {
            printf("LBA %d F1: bytes 24-31: ", lba);
            for(int i=24;i<32;i++) printf("%02X ",sec[i]);

            /* Check how many usable bytes per sector */
            /* Mode 2 Form 1: 2048 user data (bytes 24-2071) */
            /* Mode 2 Form 2: 2328 user data (bytes 24-2351) */
            /* With type byte at 24: 2047 or 2327 usable bytes */

            /* Check form from submode byte */
            int submode = sec[18];
            printf("  submode=%02X form=%d", submode, (submode & 0x20) ? 2 : 1);
            printf("\n");
        }
    }

    /* Check Chougoukin Selections (single-track game) */
    printf("\n=== Chougoukin Selections (single .bin file) ===\n");
    const char *chou = "/home/wizzard/share/GitHub/playdia-roms/Chougoukin Selections (Japan).bin";
    FILE *fp2 = fopen(chou, "rb");
    if(fp2) {
        for(int lba = 148; lba < 165; lba++) {
            long off = (long)lba * 2352;
            uint8_t sec[2352];
            fseek(fp2, off, SEEK_SET);
            if(fread(sec, 1, 2352, fp2) != 2352) break;

            int sync_ok = (sec[0]==0x00 && sec[1]==0xFF);
            printf("%4d  %s  %02X:%02X:%02X:%02X  %02X %02X %02X %02X  %02X    ",
                   lba, sync_ok?"OK":"NO",
                   sec[12],sec[13],sec[14],sec[15],
                   sec[16],sec[17],sec[18],sec[19],
                   sec[24]);
            for(int i=25;i<41;i++) printf("%02X ",sec[i]);
            printf("\n");
        }
        fclose(fp2);
    }

    fclose(fp);
    printf("\nDone.\n");
    return 0;
}
