/*
 * vcodec_remain.c - Investigate the remaining ~12% of bits after per-AC flag decode
 *
 * Questions:
 * 1. Are the remaining bits all zeros (padding)?
 * 2. Do they contain more VLC-decodable data?
 * 3. Is there a pattern at specific bit positions?
 * 4. What if we're reading blocks wrong and the error accumulates?
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

    static uint8_t frames[16][MAX_FRAME]; int fsizes[16];
    int nf = assemble_frames(disc,tsec,slba,frames,fsizes,16);

    for (int fi = 0; fi < 4 && fi < nf; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        printf("\n=== Frame %d: qscale=%d, type=%d ===\n", fi, f[3], f[39]);

        /* Decode with per-AC bit flag, MB-interleaved 4:2:0 */
        BR br; br_init(&br, bs, bslen);
        int dc_y=0, dc_cb=0, dc_cr=0;
        int mbw=8, mbh=9;
        int block_bits[432]; /* bits consumed per block */
        int block_count = 0;

        for (int mby=0; mby<mbh && !br_eof(&br); mby++) {
            for (int mbx=0; mbx<mbw && !br_eof(&br); mbx++) {
                for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                    int start = br.total;
                    dc_y += vlc_coeff(&br);
                    for (int i=1; i<64 && !br_eof(&br); i++) {
                        if (br_get1(&br)) vlc_coeff(&br);
                    }
                    if (block_count < 432) block_bits[block_count++] = br.total - start;
                }
                {
                    int start = br.total;
                    dc_cb += vlc_coeff(&br);
                    for(int i=1;i<64 && !br_eof(&br);i++) if(br_get1(&br)) vlc_coeff(&br);
                    if (block_count < 432) block_bits[block_count++] = br.total - start;
                }
                {
                    int start = br.total;
                    dc_cr += vlc_coeff(&br);
                    for(int i=1;i<64 && !br_eof(&br);i++) if(br_get1(&br)) vlc_coeff(&br);
                    if (block_count < 432) block_bits[block_count++] = br.total - start;
                }
            }
        }

        int used = br.total;
        int remain = bslen*8 - used;
        printf("Bits used: %d/%d (%.1f%%), remaining: %d bits\n", used, bslen*8, 100.0*used/(bslen*8), remain);
        printf("Blocks decoded: %d\n", block_count);

        /* Block bits distribution */
        printf("Block bits: min=%d max=%d\n", block_bits[0], block_bits[0]);
        int bmin=9999, bmax=0;
        double bavg=0;
        for(int i=0;i<block_count;i++){
            if(block_bits[i]<bmin)bmin=block_bits[i];
            if(block_bits[i]>bmax)bmax=block_bits[i];
            bavg+=block_bits[i];
        }
        printf("Block bits: min=%d, max=%d, avg=%.1f\n", bmin, bmax, bavg/block_count);

        /* Bits per block type */
        double y_avg=0, cb_avg=0, cr_avg=0;
        int y_cnt=0, cb_cnt=0, cr_cnt=0;
        for(int i=0;i<block_count;i++){
            int mb = i/6;
            int btype = i%6;
            if(btype < 4){y_avg+=block_bits[i]; y_cnt++;}
            else if(btype==4){cb_avg+=block_bits[i]; cb_cnt++;}
            else{cr_avg+=block_bits[i]; cr_cnt++;}
        }
        printf("Avg bits: Y=%.1f, Cb=%.1f, Cr=%.1f\n",
               y_avg/y_cnt, cb_avg/cb_cnt, cr_avg/cr_cnt);

        /* Examine remaining bits */
        printf("\nRemaining bits (%d):\n", remain);

        /* Count zeros vs ones in remaining */
        int zeros=0, ones=0;
        BR br2 = br; /* Save position */
        for(int i=0; i<remain && !br_eof(&br); i++){
            if(br_get1(&br)) ones++; else zeros++;
        }
        printf("  Zeros: %d (%.1f%%), Ones: %d (%.1f%%)\n",
               zeros, 100.0*zeros/remain, ones, 100.0*ones/remain);

        /* Show first 128 remaining bits */
        printf("  First 128 remaining bits: ");
        br = br2;
        for(int i=0; i<128 && !br_eof(&br); i++) printf("%d", br_get1(&br));
        printf("\n");

        /* Try decoding remaining as VLC values */
        br = br2;
        printf("  Remaining as VLC values (first 50): ");
        for(int i=0; i<50 && !br_eof(&br); i++){
            int old=br.total;
            int v=vlc_coeff(&br);
            if(br.total==old)break;
            printf("%d ", v);
        }
        printf("\n");

        /* Check if remaining bytes are all 0xFF or 0x00 */
        int byte_pos = (used + 7) / 8;
        printf("  Remaining byte offset: %d (of %d)\n", byte_pos, bslen);
        printf("  Last 32 bytes of bitstream: ");
        for(int i=bslen-32;i<bslen;i++) printf("%02X ", bs[i]);
        printf("\n");

        /* Show bytes around the 88% mark */
        printf("  Bytes around 88%% mark (%d): ", byte_pos);
        for(int i=byte_pos-4;i<byte_pos+16 && i<bslen;i++) printf("%02X ", bs[i]);
        printf("\n");

        /* Are remaining bits periodic? Check for byte-alignment patterns */
        printf("\n  Byte-aligned remaining data (from byte %d):\n  ", byte_pos);
        for(int i=byte_pos; i<byte_pos+64 && i<bslen; i++){
            printf("%02X ", bs[i]);
            if((i-byte_pos+1)%16==0) printf("\n  ");
        }
        printf("\n");
    }

    free(disc); zip_close(z);
    return 0;
}
