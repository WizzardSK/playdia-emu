/*
 * vcodec_padding2.c - Check for padding at end of bitstream
 * And test: what if a "length" field in the header tells us the actual data size?
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <zip.h>

#define SECTOR_RAW 2352
#define MAX_FRAME  65536

static int assemble_frames(const uint8_t *disc, int tsec, int slba,
    uint8_t fr[][MAX_FRAME], int fs[], int mx) {
    int n=0,c=0; bool inf=false;
    for(int l=slba;l<tsec&&n<mx;l++){
        const uint8_t *s=disc+(long)l*SECTOR_RAW;
        if(s[0]!=0||s[1]!=0xFF||s[15]!=2||(s[18]&4)) continue;
        if(s[24]==0xF1){if(!inf){inf=true;c=0;}if(c+2047<MAX_FRAME){memcpy(fr[n]+c,s+25,2047);c+=2047;}}
        else if(s[24]==0xF2){if(inf&&c>0){fs[n]=c;n++;inf=false;c=0;}}
    } return n;
}

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

    int lbas[] = {277, 502, 757, 1112, 1872, 3072, 5232};
    int nlbas = 7;

    for (int li = 0; li < nlbas; li++) {
        int lba = lbas[li];
        static uint8_t frames[8][MAX_FRAME]; int fsizes[8];
        int nf = assemble_frames(disc,tsec,lba,frames,fsizes,8);

        for (int fi = 0; fi < nf && fi < 4; fi++) {
            uint8_t *f = frames[fi];
            int fsize = fsizes[fi];
            int qs = f[3], type = f[39];
            const uint8_t *bs = f + 40;
            int bslen = fsize - 40;

            /* Check header bytes more carefully */
            printf("LBA %d frame %d: fsize=%d qs=%d type=%d\n", lba, fi, fsize, qs, type);
            printf("  Header: %02X %02X %02X %02X | ... | %02X %02X %02X %02X\n",
                f[0], f[1], f[2], f[3], f[36], f[37], f[38], f[39]);

            /* Check last 64 bytes of bitstream */
            printf("  Last 64 bytes: ");
            for (int i = bslen-64; i < bslen; i++)
                printf("%02X", bs[i]);
            printf("\n");

            /* Count trailing 0xFF bytes */
            int ff_count = 0;
            for (int i = bslen-1; i >= 0; i--) {
                if (bs[i] == 0xFF) ff_count++;
                else break;
            }

            /* Count trailing 0x00 bytes */
            int zero_count = 0;
            for (int i = bslen-1; i >= 0; i--) {
                if (bs[i] == 0x00) zero_count++;
                else break;
            }

            /* Find last non-0xFF byte */
            int last_data = bslen - 1;
            while (last_data >= 0 && bs[last_data] == 0xFF) last_data--;

            /* Find last non-zero byte */
            int last_nz = bslen - 1;
            while (last_nz >= 0 && bs[last_nz] == 0x00) last_nz--;

            printf("  Trailing 0xFF: %d bytes, trailing 0x00: %d bytes\n", ff_count, zero_count);
            printf("  Last non-FF byte at offset %d (of %d), last non-zero at %d\n",
                last_data, bslen, last_nz);
            printf("  Data bytes (non-FF): %d = %d bits (%.1f%%)\n",
                last_data+1, (last_data+1)*8, 100.0*(last_data+1)/bslen);

            /* Check: could header bytes 0-3 encode a length? */
            /* Format: 00 80 04 QS */
            int hdr0 = (f[0]<<8)|f[1]; /* 0x0080 = 128 */
            int hdr1 = (f[2]<<8)|f[3]; /* 0x0400|QS */
            printf("  Header words: 0x%04X, 0x%04X (0x04%02X)\n", hdr0, hdr1, qs);

            /* Check bytes 36-39 */
            int hdr2 = (f[36]<<8)|f[37]; /* 0x0080 */
            int hdr3 = (f[38]<<8)|f[39]; /* 0x2400|TYPE */
            printf("  Trailer words: 0x%04X, 0x%04X (0x24%02X)\n", hdr2, hdr3, type);

            /* Maybe the 0x04 and 0x24 encode something? */
            /* 0x0004 * 256 = 1024? 0x0024 * 256 = 9216? */
            /* Or maybe the header is: {0x0080, 0x0400+qs, qtable[16], qtable[16], 0x0080, 0x2400+type} */

            printf("\n");
        }
    }

    free(disc); zip_close(z);
    return 0;
}
