/*
 * vcodec_headers.c - Dump frame headers for analysis
 * Check: are qtables always same? Are there per-frame differences?
 * Also check if header might be larger than 40 bytes.
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
    int slba = argc > 2 ? atoi(argv[2]) : 502;

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

    static uint8_t frames[64][MAX_FRAME]; int fsizes[64];
    int nf = assemble_frames(disc,tsec,slba,frames,fsizes,64);

    printf("Total frames: %d\n\n", nf);

    /* Check if qtables vary between frames */
    printf("=== Header bytes 0-63 for first 16 frames ===\n");
    for (int fi = 0; fi < 16 && fi < nf; fi++) {
        uint8_t *f = frames[fi];
        printf("F%2d (size=%5d): ", fi, fsizes[fi]);
        for (int i = 0; i < 40; i++) printf("%02X ", f[i]);
        printf("\n");
    }

    printf("\n=== Qtable comparison ===\n");
    for (int fi = 0; fi < 16 && fi < nf; fi++) {
        uint8_t *f = frames[fi];
        printf("F%2d: qt1=[", fi);
        for(int i=0;i<16;i++) printf("%d%s", f[4+i], i<15?",":"");
        printf("] qt2=[");
        for(int i=0;i<16;i++) printf("%d%s", f[20+i], i<15?",":"");
        printf("] %s\n", memcmp(f+4, f+20, 16)==0 ? "SAME" : "DIFFERENT");
    }

    /* Look at bytes 36-39 more carefully */
    printf("\n=== Bytes 36-39 (post-qtable header) ===\n");
    for (int fi = 0; fi < nf && fi < 32; fi++) {
        printf("F%2d: %02X %02X %02X %02X  (type=%d)\n",
               fi, frames[fi][36], frames[fi][37], frames[fi][38], frames[fi][39],
               frames[fi][39]);
    }

    /* Check first few bytes of bitstream for patterns */
    printf("\n=== First 16 bytes of bitstream (bytes 40-55) ===\n");
    for (int fi = 0; fi < 16 && fi < nf; fi++) {
        printf("F%2d: ", fi);
        for(int i=40;i<56;i++) printf("%02X ", frames[fi][i]);
        printf("\n");
    }

    /* Check what the last byte of real data is before 0xFF padding */
    printf("\n=== Trailing data before padding ===\n");
    for (int fi = 0; fi < nf && fi < 32; fi++) {
        int last = fsizes[fi]-1;
        while(last>0 && frames[fi][last]==0xFF) last--;
        int pad = fsizes[fi]-1-last;
        printf("F%2d: last_data_byte=%5d, padding=%4d bytes, last_bytes=",
               fi, last, pad);
        for(int i=(last>3?last-3:0); i<=last; i++) printf("%02X ", frames[fi][i]);
        printf("\n");
    }

    /* Are there any frames at different LBAs? Scan for more video starting points */
    printf("\n=== Scanning for video scene starts (F3 markers) ===\n");
    int scene_count = 0;
    for (int l = 0; l < tsec && scene_count < 50; l++) {
        const uint8_t *s = disc + (long)l*SECTOR_RAW;
        if(s[0]!=0||s[1]!=0xFF||s[15]!=2||(s[18]&4)) continue;
        if (s[24] == 0xF3) {
            printf("  F3 scene marker at LBA %d\n", l);
            scene_count++;
        }
    }

    free(disc); zip_close(z);
    return 0;
}
