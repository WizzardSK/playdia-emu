/*
 * Playdia video - CD-i QHY-style decode
 * Hypothesis: 4bpp per pixel, each nibble indexes QTable for DPCM delta
 * Header byte 04 = "4 bits per pixel"
 * Sub-header 00 80 24 XX = width=128, height_blocks=36, type=XX
 *
 * Also try: the QTable as a non-linear DPCM quantizer
 * with various sign conventions
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
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

    static uint8_t frames[8][MAX_FRAME]; int fsizes[8];
    int nf=assemble_frames(disc,tsec,slba,frames,fsizes,8);

    int fi = nf > 1 ? 1 : 0;
    uint8_t *f = frames[fi];
    int fsize = fsizes[fi];
    uint8_t qtab[16]; memcpy(qtab,f+4,16);
    int qscale = f[3];
    printf("Frame %d: %d bytes, qscale=%d\n", fi, fsize, qscale);
    printf("QTable: "); for(int i=0;i<16;i++) printf("%d ",qtab[i]); printf("\n");

    int W = 128, H = 144;

    // Build signed delta tables from QTable
    // Version 1: first 8 entries are positive deltas, last 8 are negative
    int dtab1[16];
    for (int i = 0; i < 8; i++) { dtab1[i] = qtab[i]; dtab1[i+8] = -qtab[i]; }

    // Version 2: interleaved signs: 0→0, 1→+qt[0], 2→-qt[0], 3→+qt[1], ...
    int dtab2[16];
    dtab2[0] = 0;
    for (int i = 1; i < 16; i++) {
        int mag = qtab[(i-1)/2];
        dtab2[i] = (i & 1) ? mag : -mag;
    }

    // Version 3: symmetric around 0: i<8 → -(8-i)*scale, i>=8 → (i-7)*scale
    // Using qtab[0] as scale
    int dtab3[16];
    for (int i = 0; i < 16; i++) dtab3[i] = (i - 8) * qtab[0];

    // Version 4: direct signed = qtab[i] - 128 (treating as signed byte)
    int dtab4[16];
    for (int i = 0; i < 16; i++) dtab4[i] = (int8_t)qtab[i]; // values are 10-37, all positive...

    // Version 5: qtab as absolute luminance values (not deltas)
    // nibble → pixel value = qtab[nibble] * qscale

    // Version 6: index 0-7 = +delta, index 8-15 = -delta, using REVERSED qtab
    int dtab6[16];
    for (int i = 0; i < 8; i++) { dtab6[i] = qtab[7-i]; dtab6[i+8] = -qtab[7-i]; }

    // Version 7: Build a balanced signed table where indices 0-7 map to negative deltas
    // and 8-15 map to positive deltas (sorted by magnitude)
    // Sort qtab first
    uint8_t sorted[16]; memcpy(sorted, qtab, 16);
    for(int i=0;i<16;i++) for(int j=i+1;j<16;j++) if(sorted[i]>sorted[j]){uint8_t t=sorted[i];sorted[i]=sorted[j];sorted[j]=t;}
    int dtab7[16];
    for(int i=0;i<8;i++) dtab7[i] = -sorted[7-i]; // largest negative first
    for(int i=0;i<8;i++) dtab7[i+8] = sorted[i];  // smallest positive first

    int *dtabs[] = {dtab1, dtab2, dtab3, dtab6, dtab7};
    const char *dnames[] = {"pos8neg8", "interleaved", "linear", "rev_pos8neg8", "sorted_sym"};
    int ndtabs = 5;

    // Try different offsets
    int offsets[] = {40, 36, 4, 20};
    int noff = 4;

    for (int oi = 0; oi < noff; oi++) {
        int off = offsets[oi];
        const uint8_t *bs = f + off;
        int bslen = fsize - off;

        printf("\n=== Offset %d ===\n", off);

        // Try each delta table interpretation
        for (int di = 0; di < ndtabs; di++) {
            int *dtab = dtabs[di];

            // DPCM left prediction, reset each row, high nibble first
            {
                uint8_t *img = calloc(W*H, 1);
                for (int y = 0; y < H; y++) {
                    int prev = 128;
                    for (int x = 0; x < W; x++) {
                        int idx = (x & 1) ? (bs[(y*W+x)/2] & 0x0F) : (bs[(y*W+x)/2] >> 4);
                        // Use offset from start of bitstream
                        int byte_off = (y * W + x) / 2;
                        if (byte_off >= bslen) goto done1;
                        idx = (x & 1) ? (bs[byte_off] & 0x0F) : (bs[byte_off] >> 4);
                        prev += dtab[idx];
                        if (prev < 0) prev = 0; if (prev > 255) prev = 255;
                        img[y*W+x] = prev;
                    }
                }
                done1:;
                char path[256];
                snprintf(path,sizeof(path),"/home/wizzard/share/GitHub/pd_qhy_%s_lr_o%d.pgm",
                         dnames[di], off);
                write_pgm(path, img, W, H);
                free(img);
            }

            // DPCM left prediction, continuous across rows
            if (off == 40 && di < 3) {
                uint8_t *img = calloc(W*H, 1);
                int prev = 128;
                for (int i = 0; i < W*H; i++) {
                    int byte_off = i / 2;
                    if (byte_off >= bslen) break;
                    int idx = (i & 1) ? (bs[byte_off] & 0x0F) : (bs[byte_off] >> 4);
                    prev += dtab[idx];
                    if (prev < 0) prev = 0; if (prev > 255) prev = 255;
                    img[i] = prev;
                }
                char path[256];
                snprintf(path,sizeof(path),"/home/wizzard/share/GitHub/pd_qhy_%s_lc_o%d.pgm",
                         dnames[di], off);
                write_pgm(path, img, W, H);
                free(img);
            }
        }

        // Also try: 4-bit value directly as pixel = qtab[nibble] * qscale
        if (off == 40) {
            uint8_t *img = calloc(W*H, 1);
            for (int i = 0; i < W*H; i++) {
                int byte_off = i / 2;
                if (byte_off >= bslen) break;
                int idx = (i & 1) ? (bs[byte_off] & 0x0F) : (bs[byte_off] >> 4);
                int v = qtab[idx] * qscale;
                if (v > 255) v = 255;
                img[i] = v;
            }
            write_pgm("/home/wizzard/share/GitHub/pd_qhy_abs_o40.pgm", img, W, H);
            free(img);
        }

        // Low nibble first instead of high nibble first
        if (off == 40) {
            uint8_t *img = calloc(W*H, 1);
            for (int y = 0; y < H; y++) {
                int prev = 128;
                for (int x = 0; x < W; x++) {
                    int byte_off = (y * W + x) / 2;
                    if (byte_off >= bslen) goto done2;
                    int idx = (x & 1) ? (bs[byte_off] >> 4) : (bs[byte_off] & 0x0F); // swapped
                    prev += dtab1[idx];
                    if (prev < 0) prev = 0; if (prev > 255) prev = 255;
                    img[y*W+x] = prev;
                }
            }
            done2:;
            write_pgm("/home/wizzard/share/GitHub/pd_qhy_lnf_o40.pgm", img, W, H);
            free(img);
        }
    }

    // Also: what if the QTable values ARE the palette directly?
    // And qscale is used to shift them: pixel = qtab[nibble] << (qscale - N)?
    printf("\n=== QTable as direct palette ===\n");
    {
        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        // Scale 1: qtab * 8 (since max qtab=37, 37*8=296 → clip at 255)
        uint8_t *img = calloc(W*H, 1);
        for (int i = 0; i < W*H; i++) {
            int byte_off = i / 2;
            if (byte_off >= bslen) break;
            int idx = (i & 1) ? (bs[byte_off] & 0x0F) : (bs[byte_off] >> 4);
            int v = qtab[idx] * 8;
            if (v > 255) v = 255;
            img[i] = v;
        }
        write_pgm("/home/wizzard/share/GitHub/pd_pal8.pgm", img, W, H);
        free(img);
    }

    free(disc); zip_close(z);
    return 0;
}
