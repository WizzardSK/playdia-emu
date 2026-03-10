#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <zip.h>

#define SECTOR_RAW 2352

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    int err; zip_t *z = zip_open(argv[1], ZIP_RDONLY, &err); if (!z) return 1;
    int bi2=-1; zip_uint64_t bs2=0;
    for (int i=0; i<(int)zip_get_num_entries(z,0); i++) {
        zip_stat_t st; if(zip_stat_index(z,i,0,&st)==0 && st.size>bs2){bs2=st.size;bi2=i;}}
    zip_stat_t st; zip_stat_index(z,bi2,0,&st);
    zip_file_t *zf = zip_fopen_index(z,bi2,0);
    uint8_t *disc = malloc(st.size);
    zip_int64_t rd=0;
    while(rd<(zip_int64_t)st.size){zip_int64_t r=zip_fread(zf,disc+rd,st.size-rd);if(r<=0)break;rd+=r;}
    zip_fclose(zf);
    int tsec = (int)(st.size/SECTOR_RAW);
    printf("Total sectors: %d\n", tsec);
    int found = 0;
    for (int l = 0; l < tsec && found < 30; l++) {
        const uint8_t *s = disc + (long)l * SECTOR_RAW;
        if (s[0] != 0 || s[1] != 0xFF || s[15] != 2 || (s[18] & 4)) continue;
        if (s[24] != 0xF1) continue;
        if (l > 0) {
            const uint8_t *sp = disc + (long)(l-1) * SECTOR_RAW;
            if (sp[0]==0 && sp[1]==0xFF && sp[15]==2 && !(sp[18]&4) && sp[24]==0xF1) continue;
        }
        if (s[25]==0x00 && s[26]==0x80 && s[27]==0x04) {
            int nf1 = 1;
            for (int l2 = l+1; l2 < tsec; l2++) {
                const uint8_t *s2 = disc + (long)l2 * SECTOR_RAW;
                if (s2[0]!=0||s2[1]!=0xFF||s2[15]!=2||(s2[18]&4)) continue;
                if (s2[24]==0xF1) nf1++;
                else break;
            }
            printf("LBA %5d: %dxF1 qs=%d type=%d qt=", l, nf1, s[28], s[25+39]);
            for (int i=0; i<16; i++) printf("%02X",s[25+4+i]);
            printf("\n");
            found++;
        }
    }
    if (!found) {
        printf("No valid headers. Scanning all F1...\n");
        for (int l=0; l<tsec; l++) {
            const uint8_t *s=disc+(long)l*SECTOR_RAW;
            if(s[0]==0&&s[1]==0xFF&&s[15]==2&&!(s[18]&4)&&s[24]==0xF1) {
                if(found<10) printf("F1@%d: %02X%02X%02X%02X %02X%02X%02X%02X\n",
                    l,s[25],s[26],s[27],s[28],s[29],s[30],s[31],s[32]);
                found++;
            }
        }
        printf("Total F1: %d\n",found);
    }
    free(disc); zip_close(z);
    return 0;
}
