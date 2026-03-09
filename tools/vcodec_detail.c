/*
 * vcodec_detail.c - Detailed block-by-block VLC analysis
 *
 * Dump every VLC value with its bit position to spot structural patterns.
 * Try to identify EOB, block boundaries, or macroblock headers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <zip.h>

#define SECTOR_RAW 2352
#define MAX_FRAME  65536

typedef struct { const uint8_t *data; int len,pos,bit,total; } BR;
static void br_init(BR *b, const uint8_t *d, int l) { b->data=d;b->len=l;b->pos=0;b->bit=7;b->total=0; }
static int br_eof(BR *b) { return b->pos>=b->len; }
static int br_get1(BR *b) {
    if(b->pos>=b->len) return 0;
    int v=(b->data[b->pos]>>b->bit)&1;
    if(--b->bit<0){b->bit=7;b->pos++;}
    b->total++; return v;
}
static int br_get(BR *b, int n) { int v=0; for(int i=0;i<n;i++) v=(v<<1)|br_get1(b); return v; }

/* Return VLC size (number of bits for prefix) for analysis */
static int vlc_coeff_detail(BR *b, int *out_val, int *out_size) {
    int start = b->total;
    int size;
    if (br_get1(b) == 0) { size = br_get1(b) ? 2 : 1; }
    else {
        if (br_get1(b) == 0) { size = br_get1(b) ? 3 : 0; }
        else {
            if (br_get1(b) == 0) size = 4;
            else if (br_get1(b) == 0) size = 5;
            else if (br_get1(b) == 0) size = 6;
            else size = br_get1(b) ? 8 : 7;
        }
    }
    *out_size = size;
    if (size == 0) { *out_val = 0; return b->total - start; }
    int val = br_get(b, size);
    if (val < (1 << (size-1))) val = val - (1 << size) + 1;
    *out_val = val;
    return b->total - start;
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

    static uint8_t frames[16][MAX_FRAME]; int fsizes[16];
    int nf = assemble_frames(disc,tsec,slba,frames,fsizes,16);

    /* Only analyze frame 0 */
    uint8_t *f = frames[0];
    int fsize = fsizes[0];
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;

    printf("Frame 0: qscale=%d, type=%d, bslen=%d (%d bits)\n\n", f[3], f[39], bslen, bslen*8);

    /* Dump raw bits at the start */
    printf("First 256 bits:\n");
    BR br; br_init(&br, bs, bslen);
    for (int i = 0; i < 256; i++) {
        printf("%d", br_get1(&br));
        if ((i+1) % 64 == 0) printf("\n");
        else if ((i+1) % 8 == 0) printf(" ");
    }
    printf("\n");

    /* Now decode VLC values and show positions */
    br_init(&br, bs, bslen);
    printf("VLC decode (position, bits, size, value):\n");
    printf("pos     bits val  size | pos     bits val  size | pos     bits val  size\n");

    int col = 0;
    for (int i = 0; i < 500 && !br_eof(&br); i++) {
        int pos = br.total;
        int val, size;
        int bits = vlc_coeff_detail(&br, &val, &size);
        if (br.total == pos) break; /* EOF */

        printf("%5d %3d %4d s%d", pos, bits, val, size);
        col++;
        if (col == 3) { printf("\n"); col = 0; }
        else printf(" | ");
    }
    if (col) printf("\n");

    /* Now analyze: what if we look at the VALUE sequence for patterns? */
    printf("\n\nLooking for structural markers...\n");
    br_init(&br, bs, bslen);

    /* Decode all values and look for patterns */
    int positions[25000];
    int values[25000];
    int sizes[25000];
    int nbits[25000];
    int nv = 0;
    while (nv < 25000 && !br_eof(&br)) {
        positions[nv] = br.total;
        nbits[nv] = vlc_coeff_detail(&br, &values[nv], &sizes[nv]);
        if (br.total == positions[nv]) break;
        nv++;
    }
    printf("Total values: %d\n\n", nv);

    /* Look at consecutive zeros - could indicate block boundaries */
    printf("Positions of value=0 (first 100):\n");
    int zcnt = 0;
    for (int i = 0; i < nv && zcnt < 100; i++) {
        if (values[i] == 0) {
            /* Check context: how many non-zero values before this zero? */
            int prev_nonzero = 0;
            for (int j = i-1; j >= 0 && values[j] != 0; j--) prev_nonzero++;
            int next_nonzero = 0;
            for (int j = i+1; j < nv && values[j] != 0; j++) next_nonzero++;

            printf("  val[%d] @ bit %d  (prev_nz=%d, next_nz=%d)\n",
                   i, positions[i], prev_nonzero, next_nonzero);
            zcnt++;
        }
    }

    /* Look for large magnitude values that could be DC */
    printf("\nValues with |val| > 20 (potential DCs, first 50):\n");
    int dcnt = 0;
    for (int i = 0; i < nv && dcnt < 50; i++) {
        if (abs(values[i]) > 20) {
            printf("  val[%d] = %d @ bit %d  (prev=%d, next=%d)\n",
                   i, values[i], positions[i],
                   i>0 ? values[i-1] : -999,
                   i<nv-1 ? values[i+1] : -999);
            dcnt++;
        }
    }

    /* Check spacing between "large" values */
    printf("\nSpacing between values with |val| > 10:\n");
    int last_large = -1;
    int spacing[1000]; int nsp = 0;
    for (int i = 0; i < nv && nsp < 200; i++) {
        if (abs(values[i]) > 10) {
            if (last_large >= 0 && nsp < 1000) {
                spacing[nsp++] = i - last_large;
            }
            last_large = i;
        }
    }
    /* Histogram of spacings */
    printf("Spacing histogram (between large values):\n");
    int hist[100] = {0};
    for (int i = 0; i < nsp; i++) {
        if (spacing[i] < 100) hist[spacing[i]]++;
    }
    for (int i = 1; i < 100; i++) {
        if (hist[i] > 0) printf("  %2d: %d\n", i, hist[i]);
    }

    free(disc); zip_close(z);
    return 0;
}
