#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <zip.h>

#define SECTOR_RAW 2352
#define MAX_FRAME  65536

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    int slba = argc > 2 ? atoi(argv[2]) : 502;

    int err; zip_t *z = zip_open(argv[1], ZIP_RDONLY, &err); if (!z) return 1;
    int bi=-1; zip_uint64_t bs2=0;
    for (int i=0; i<(int)zip_get_num_entries(z,0); i++) {
        zip_stat_t st; if(zip_stat_index(z,i,0,&st)==0 && st.size>bs2){bs2=st.size;bi=i;}}
    zip_stat_t st; zip_stat_index(z,bi,0,&st);
    zip_file_t *zf = zip_fopen_index(z,bi,0);
    uint8_t *disc = malloc(st.size);
    zip_int64_t rd=0;
    while(rd<(zip_int64_t)st.size){zip_int64_t r=zip_fread(zf,disc+rd,st.size-rd);if(r<=0)break;rd+=r;}
    zip_fclose(zf);
    int tsec = (int)(st.size/SECTOR_RAW);

    printf("Total sectors: %d\n", tsec);
    
    /* Scan for F1/F2/F3 markers and examine sector structure */
    int frame = 0;
    int f1_count = 0;
    for (int l = slba; l < tsec && l < slba + 100; l++) {
        const uint8_t *s = disc + (long)l * SECTOR_RAW;
        
        /* Check sync pattern */
        if (s[0] != 0 || s[1] != 0xFF) continue;
        
        /* Print sector info */
        printf("LBA %d: sync=%02X%02X mode=%02X sub=%02X%02X%02X%02X marker=%02X",
               l, s[0], s[1], s[15], s[16], s[17], s[18], s[19], s[24]);
        
        if (s[24] == 0xF1) {
            f1_count++;
            if (f1_count <= 2) {
                /* Show first bytes of payload */
                printf(" data=");
                for (int i = 25; i < 65; i++) printf("%02X", s[i]);
            }
            printf(" [F1 sector %d]", f1_count);
        } else if (s[24] == 0xF2) {
            printf(" [F2 - frame %d end, %d F1 sectors = %d bytes payload]", 
                   frame, f1_count, f1_count * 2047);
            frame++;
            f1_count = 0;
        } else if (s[24] == 0xF3) {
            printf(" [F3]");
        }
        
        /* Check if data sector (bit 2 of submode s[18]) */
        if (s[18] & 4) printf(" (audio?)");
        
        printf("\n");
    }

    /* Also show raw sector structure around first frame */
    printf("\n--- Raw sector bytes at first F1 ---\n");
    for (int l = slba; l < tsec; l++) {
        const uint8_t *s = disc + (long)l * SECTOR_RAW;
        if (s[0] == 0 && s[1] == 0xFF && s[24] == 0xF1) {
            printf("Full sector header (0-40): ");
            for (int i = 0; i < 40; i++) printf("%02X ", s[i]);
            printf("\n");
            printf("Payload start (25-85): ");
            for (int i = 25; i < 85; i++) printf("%02X ", s[i]);
            printf("\n");
            printf("Sector end (2320-2352): ");
            for (int i = 2320; i < 2352; i++) printf("%02X ", s[i]);
            printf("\n");
            
            /* Check Mode 2 Form 1 vs Form 2 */
            printf("Mode: %d, Subheader: %02X %02X %02X %02X / %02X %02X %02X %02X\n",
                   s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23]);
            printf("User data starts at offset 24 (Mode 2) or 16 (Mode 1)\n");
            
            /* Mode 2 Form 1: 12 sync + 4 header + 8 subheader + 2048 data + 4 EDC + 276 ECC = 2352 */
            /* Mode 2 Form 2: 12 sync + 4 header + 8 subheader + 2328 data + 4 spare = 2352 */
            /* Form is in submode bit 5 */
            int form = (s[18] >> 5) & 1;
            printf("Form: %d (Form 1 = 2048 data, Form 2 = 2328 data)\n", form + 1);
            printf("Data after marker byte (s[25]..): actual payload\n");
            
            /* So if Form 2: total user data = 2328 bytes, first byte is marker (F1), rest is 2327 bytes */
            /* If Form 1: total user data = 2048 bytes, first byte is marker (F1), rest is 2047 bytes */
            printf("Payload per sector: %d bytes\n", form ? 2327 : 2047);
            break;
        }
    }

    free(disc); zip_close(z);
    return 0;
}
