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

/* Returns value AND size (bits consumed) */
static int vlc_coeff_sz(BR *b, int *sz) {
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
    if (size == 0) { *sz = b->total - start; return 0; }
    int val = br_get(b, size);
    if (val < (1 << (size-1))) val = val - (1 << size) + 1;
    *sz = b->total - start;
    return val;
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

    static uint8_t frames[16][MAX_FRAME]; int fsizes[16];
    int nf = assemble_frames(disc,tsec,slba,frames,fsizes,16);

    /* Frame 0 only */
    uint8_t *f = frames[0];
    int fsize = fsizes[0];
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;

    BR br; br_init(&br, bs, bslen);

    /* Decode first 200 values and show their sizes */
    printf("First 200 VLC values (val:bits):\n");
    int size_hist[20] = {0};
    int val_hist[512] = {0}; /* shifted by 256 */
    int total_vlc = 0;
    
    for (int i = 0; i < 200 && !br_eof(&br); i++) {
        int sz;
        int val = vlc_coeff_sz(&br, &sz);
        if (i < 200) {
            if (i % 20 == 0) printf("\n  [%3d] ", i);
            printf("%d:%d ", val, sz);
        }
        if (sz < 20) size_hist[sz]++;
        val_hist[val + 256]++;
        total_vlc++;
    }
    printf("\n");

    /* Continue to count all values */
    while (!br_eof(&br)) {
        int sz;
        int val = vlc_coeff_sz(&br, &sz);
        if (sz < 20) size_hist[sz]++;
        if (val+256 >= 0 && val+256 < 512) val_hist[val+256]++;
        total_vlc++;
    }
    
    printf("\nTotal VLC values: %d, bits: %d\n", total_vlc, br.total);
    printf("Average bits/value: %.3f\n", (double)br.total / total_vlc);
    
    printf("\nSize histogram (bits:count):\n");
    for (int i = 0; i < 20; i++)
        if (size_hist[i] > 0) printf("  %2d bits: %d (%.1f%%)\n", i, size_hist[i], 100.0*size_hist[i]/total_vlc);

    printf("\nValue distribution (most common):\n");
    /* Sort by frequency */
    typedef struct { int val; int count; } VC;
    VC top[30];
    for (int i = 0; i < 30; i++) { top[i].val = 0; top[i].count = 0; }
    for (int i = 0; i < 512; i++) {
        if (val_hist[i] > top[29].count) {
            top[29].val = i - 256;
            top[29].count = val_hist[i];
            /* bubble sort */
            for (int j = 28; j >= 0; j--) {
                if (top[j+1].count > top[j].count) {
                    VC tmp = top[j]; top[j] = top[j+1]; top[j+1] = tmp;
                }
            }
        }
    }
    for (int i = 0; i < 20 && top[i].count > 0; i++)
        printf("  val=%4d: %d (%.1f%%)\n", top[i].val, top[i].count, 100.0*top[i].count/total_vlc);

    /* What percentage of values are 0? */
    int zero_count = val_hist[256]; /* val=0 is at index 256 */
    printf("\nZero values: %d/%d (%.1f%%)\n", zero_count, total_vlc, 100.0*zero_count/total_vlc);
    
    /* Expected values for 432×64 coefficients at various zero rates */
    printf("\nExpected total bits:\n");
    printf("  If all zero (3 bits each): %d\n", 27648 * 3);
    printf("  Actual average to get 27648 vals: %.2f bits/val\n", 97936.0 / 27648);

    free(disc); zip_close(z);
    return 0;
}
