/*
 * vcodec_padding.c - Check for 0xFF padding at end of bitstream
 * and calculate actual data size vs padded size
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

static int vlc_coeff(BR *b) {
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
    if (size == 0) return 0;
    int val = br_get(b, size);
    if (val < (1 << (size-1))) val = val - (1 << size) + 1;
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

    static uint8_t frames[32][MAX_FRAME]; int fsizes[32];
    int nf = assemble_frames(disc,tsec,slba,frames,fsizes,32);

    printf("Frames found: %d\n\n", nf);

    for (int fi = 0; fi < nf && fi < 16; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        /* Find trailing 0xFF padding */
        int last_non_ff = bslen - 1;
        while (last_non_ff >= 0 && bs[last_non_ff] == 0xFF) last_non_ff--;
        int pad_bytes = bslen - 1 - last_non_ff;
        int data_bytes = last_non_ff + 1;
        int data_bits = data_bytes * 8;

        /* Decode with per-AC bit flag */
        BR br; br_init(&br, bs, bslen);
        int dc[3] = {0};
        int mbw=8, mbh=9;

        for (int mby=0; mby<mbh && !br_eof(&br); mby++) {
            for (int mbx=0; mbx<mbw && !br_eof(&br); mbx++) {
                for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                    dc[0] += vlc_coeff(&br);
                    for (int i=1; i<64 && !br_eof(&br); i++)
                        if (br_get1(&br)) vlc_coeff(&br);
                }
                dc[1] += vlc_coeff(&br);
                for(int i=1;i<64 && !br_eof(&br);i++) if(br_get1(&br)) vlc_coeff(&br);
                dc[2] += vlc_coeff(&br);
                for(int i=1;i<64 && !br_eof(&br);i++) if(br_get1(&br)) vlc_coeff(&br);
            }
        }

        int used = br.total;
        printf("F%2d: type=%d qs=%2d | data=%5d pad=%4d total=%5d | used=%5d/%5d "
               "(%4.1f%% of total, %4.1f%% of data)\n",
               fi, f[39], f[3],
               data_bytes, pad_bytes, bslen,
               used/8, bslen, 100.0*used/(bslen*8), 100.0*used/(data_bits));
    }

    free(disc); zip_close(z);
    return 0;
}
