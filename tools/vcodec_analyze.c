/*
 * Playdia video - Deep bitstream analysis
 * Dump header, analyze byte distributions, find patterns
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <zip.h>

#define SECTOR_RAW 2352
#define MAX_FRAME  65536

static void write_pgm(const char *p, const uint8_t *g, int w, int h) {
    FILE *f=fopen(p,"wb"); if(!f)return;
    fprintf(f,"P5\n%d %d\n255\n",w,h); fwrite(g,1,w*h,f); fclose(f);
    printf("  -> %s (%dx%d)\n",p,w,h);
}

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
    if(argc<2){fprintf(stderr,"Usage: %s <zip> [lba]\n",argv[0]);return 1;}
    int slba=argc>2?atoi(argv[2]):502;

    int err;zip_t *z=zip_open(argv[1],ZIP_RDONLY,&err); if(!z)return 1;
    int bi=-1;zip_uint64_t bs2=0;
    for(int i=0;i<(int)zip_get_num_entries(z,0);i++){
        zip_stat_t st;if(zip_stat_index(z,i,0,&st)==0&&st.size>bs2){bs2=st.size;bi=i;}}
    zip_stat_t st;zip_stat_index(z,bi,0,&st);
    zip_file_t *zf=zip_fopen_index(z,bi,0);
    uint8_t *disc=malloc(st.size);
    zip_int64_t rd=0;
    while(rd<(zip_int64_t)st.size){zip_int64_t r=zip_fread(zf,disc+rd,st.size-rd);if(r<=0)break;rd+=r;}
    zip_fclose(zf);
    int tsec=(int)(st.size/SECTOR_RAW);

    static uint8_t frames[16][MAX_FRAME]; int fsizes[16];
    int nf=assemble_frames(disc,tsec,slba,frames,fsizes,16);
    printf("Assembled %d frames\n",nf);

    // Analyze first few frames
    for (int fi = 0; fi < nf && fi < 6; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        printf("\n========== FRAME %d: %d bytes ==========\n", fi, fsize);

        // Dump first 64 bytes as hex
        printf("Header hex:\n");
        for (int i = 0; i < 64 && i < fsize; i++) {
            printf("%02X ", f[i]);
            if ((i & 15) == 15) printf("\n");
        }
        printf("\n");

        // Analyze the header
        printf("Bytes 0-3 (main header): %02X %02X %02X %02X\n", f[0],f[1],f[2],f[3]);
        printf("Bytes 4-19 (qtable 1):  ");
        for(int i=4;i<20;i++) printf("%02X ",f[i]);
        printf("\n                   dec: ");
        for(int i=4;i<20;i++) printf("%d ",f[i]);
        printf("\n");
        printf("Bytes 20-35 (qtable 2): ");
        for(int i=20;i<36;i++) printf("%02X ",f[i]);
        printf("\n                   dec: ");
        for(int i=20;i<36;i++) printf("%d ",f[i]);
        printf("\n");
        printf("Bytes 36-39 (sub-hdr):  %02X %02X %02X %02X\n", f[36],f[37],f[38],f[39]);

        // Check if qtable 1 == qtable 2
        if (memcmp(f+4, f+20, 16) == 0)
            printf("  -> QTables are IDENTICAL\n");
        else
            printf("  -> QTables are DIFFERENT\n");

        // Byte distribution of bitstream (from offset 40)
        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        // Byte histogram
        int hist[256] = {0};
        for (int i = 0; i < bslen; i++) hist[bs[i]]++;

        printf("\nBitstream: %d bytes\n", bslen);
        printf("Top 20 most common bytes:\n");
        int hist2[256]; memcpy(hist2, hist, sizeof(hist));
        for (int t = 0; t < 20; t++) {
            int maxv = 0, maxi = 0;
            for (int i = 0; i < 256; i++) {
                if (hist2[i] > maxv) { maxv = hist2[i]; maxi = i; }
            }
            if (maxv == 0) break;
            printf("  0x%02X (%3d): %d (%.1f%%)\n", maxi, maxi, maxv, 100.0*maxv/bslen);
            hist2[maxi] = 0;
        }

        // Nibble distribution
        int nhist[16] = {0};
        for (int i = 0; i < bslen; i++) {
            nhist[bs[i] >> 4]++;
            nhist[bs[i] & 0x0F]++;
        }
        printf("\nNibble distribution:\n");
        for (int i = 0; i < 16; i++)
            printf("  %X: %d (%.1f%%)\n", i, nhist[i], 100.0*nhist[i]/(bslen*2));

        // Entropy
        {
            double ent = 0;
            for (int i = 0; i < 256; i++) {
                if (hist[i] == 0) continue;
                double p = (double)hist[i] / bslen;
                ent -= p * log2(p);
            }
            printf("\nByte entropy: %.3f bits/byte\n", ent);
        }
        {
            double ent = 0;
            for (int i = 0; i < 16; i++) {
                if (nhist[i] == 0) continue;
                double p = (double)nhist[i] / (bslen*2);
                ent -= p * log2(p);
            }
            printf("Nibble entropy: %.3f bits/nibble\n", ent);
        }

        // Autocorrelation - look for scanline periodicity
        printf("\nAutocorrelation (byte-MSE at various lags):\n");
        for (int lag = 48; lag <= 256; lag += 8) {
            if (lag >= bslen) break;
            long sum = 0;
            int cnt = bslen - lag;
            if (cnt > 8000) cnt = 8000;
            for (int i = 0; i < cnt; i++) {
                int d = (int)bs[i] - (int)bs[i+lag];
                sum += d*d;
            }
            double mse = (double)sum / cnt;
            // Also try nibble-level autocorrelation
            printf("  lag=%3d: byte-MSE=%.1f", lag, mse);
            if (mse < 6000) printf(" ***LOW***");
            printf("\n");
        }

        // Also check small lags
        printf("Small lags:\n");
        for (int lag = 1; lag <= 16; lag++) {
            long sum = 0;
            int cnt = bslen - lag;
            if (cnt > 8000) cnt = 8000;
            for (int i = 0; i < cnt; i++) {
                int d = (int)bs[i] - (int)bs[i+lag];
                sum += d*d;
            }
            double mse = (double)sum / cnt;
            printf("  lag=%2d: MSE=%.1f\n", lag, mse);
        }

        // Search for 00 80 sub-headers
        printf("\nSub-header markers (00 80 XX XX) in first 200 bytes:\n");
        for (int i = 0; i < 200 && i < fsize-3; i++) {
            if (f[i] == 0x00 && f[i+1] == 0x80) {
                printf("  offset %d: %02X %02X %02X %02X\n",
                       i, f[i], f[i+1], f[i+2], f[i+3]);
            }
        }

        // Dump the first 64 nibbles
        printf("\nFirst 128 nibbles (high first):\n");
        for (int i = 0; i < 128 && i/2 < bslen; i++) {
            int nib = (i & 1) ? (bs[i/2] & 0x0F) : (bs[i/2] >> 4);
            printf("%X", nib);
            if ((i & 63) == 63) printf("\n");
        }
        printf("\n");

        // Dump raw frame to binary file
        {
            char path[256];
            snprintf(path,sizeof(path),"/home/wizzard/share/GitHub/pd_frame%d.bin",fi);
            FILE *fp = fopen(path,"wb");
            if(fp){ fwrite(f, 1, fsize, fp); fclose(fp); printf("Wrote %s\n", path); }
        }
    }

    // Inter-frame comparison
    if (nf >= 2) {
        printf("\n========== INTER-FRAME COMPARISON ==========\n");
        for (int fi = 0; fi < nf && fi < 8; fi++) {
            printf("Frame %d: hdr=%02X %02X %02X %02X | sub=%02X %02X %02X %02X | size=%d\n",
                   fi, frames[fi][0], frames[fi][1], frames[fi][2], frames[fi][3],
                   frames[fi][36], frames[fi][37], frames[fi][38], frames[fi][39],
                   fsizes[fi]);
        }

        for (int fi = 0; fi + 1 < nf && fi < 6; fi++) {
            int len = fsizes[fi] < fsizes[fi+1] ? fsizes[fi] : fsizes[fi+1];
            len -= 40;
            int same = 0;
            long diff_sum = 0;
            for (int i = 0; i < len; i++) {
                if (frames[fi][40+i] == frames[fi+1][40+i]) same++;
                int d = (int)frames[fi][40+i] - (int)frames[fi+1][40+i];
                diff_sum += (long)d*d;
            }
            printf("Frame %d vs %d: %d/%d same (%.1f%%), MSE=%.1f\n",
                   fi, fi+1, same, len, 100.0*same/len, (double)diff_sum/len);
        }
    }

    free(disc); zip_close(z);
    return 0;
}
