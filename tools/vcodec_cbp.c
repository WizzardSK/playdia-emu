/*
 * Test coded-block-pattern: 1-bit flag before each block or MB
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <zip.h>

#define SECTOR_RAW 2352
#define MAX_FRAME  65536
#define OUT_DIR "/home/wizzard/share/GitHub/playdia-emu/tools/test_output/"

static void write_pgm(const char *p, const uint8_t *g, int w, int h) {
    FILE *f=fopen(p,"wb"); if(!f)return;
    fprintf(f,"P5\n%d %d\n255\n",w,h); fwrite(g,1,w*h,f); fclose(f);
    printf("  -> %s (%dx%d)\n",p,w,h);
}

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

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

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

    for (int fi = 0; fi < 4 && fi < nf; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        int qscale = f[3];
        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        printf("\n=== Frame %d: qscale=%d, type=%d ===\n", fi, qscale, f[39]);

        BR br;
        int mbw = 8, mbh = 9;

        /* ===== Test 1: 1-bit flag per BLOCK (432 blocks = 4Y+Cb+Cr) ===== */
        printf("\n--- Test 1: 1-bit flag per block ---\n");
        br_init(&br, bs, bslen);
        int coded_blocks = 0, skip_blocks = 0;
        int dcY[288]; int bi_y = 0;
        int dc_y = 0;
        
        for (int mby = 0; mby < mbh && !br_eof(&br); mby++) {
            for (int mbx = 0; mbx < mbw && !br_eof(&br); mbx++) {
                /* 4 Y blocks */
                for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                    int flag = br_get1(&br);
                    if (flag) {
                        int raw0 = vlc_coeff(&br);
                        dc_y += raw0;
                        dcY[bi_y] = dc_y;
                        for (int i = 1; i < 64 && !br_eof(&br); i++)
                            vlc_coeff(&br);
                        coded_blocks++;
                    } else {
                        dcY[bi_y] = dc_y; /* repeat last DC */
                        skip_blocks++;
                    }
                    bi_y++;
                }
                /* Cb */
                for (int ch = 0; ch < 2 && !br_eof(&br); ch++) {
                    int flag = br_get1(&br);
                    if (flag) {
                        for (int i = 0; i < 64 && !br_eof(&br); i++)
                            vlc_coeff(&br);
                        coded_blocks++;
                    } else {
                        skip_blocks++;
                    }
                }
            }
        }
        printf("Coded: %d, Skipped: %d, Total: %d, bits=%d/%d\n",
               coded_blocks, skip_blocks, coded_blocks+skip_blocks, br.total, bslen*8);

        /* Show DC image */
        if (bi_y >= 288) {
            int yw=16, yh2=18;
            int ymin=99999,ymax=-99999;
            for(int i=0;i<288;i++){if(dcY[i]<ymin)ymin=dcY[i];if(dcY[i]>ymax)ymax=dcY[i];}
            uint8_t *img = calloc(yw*yh2,1);
            int idx=0;
            for(int mby=0;mby<9;mby++)
                for(int mbx=0;mbx<8;mbx++){
                    int bx[4]={mbx*2,mbx*2+1,mbx*2,mbx*2+1};
                    int by[4]={mby*2,mby*2,mby*2+1,mby*2+1};
                    for(int yb=0;yb<4&&idx<288;yb++){
                        int v=dcY[idx++];
                        int s2=(ymax!=ymin)?(v-ymin)*255/(ymax-ymin):128;
                        img[by[yb]*yw+bx[yb]]=clamp8(s2);
                    }
                }
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "dcflag1_%s_f%d.pgm","test",fi);
            write_pgm(path,img,yw,yh2);

            uint8_t *up=calloc(128*144,1);
            for(int y=0;y<144;y++)for(int x=0;x<128;x++)
                up[y*128+x]=img[(y/8)*yw+(x/8)];
            snprintf(path,sizeof(path),OUT_DIR "dcflag1_up_%s_f%d.pgm","test",fi);
            write_pgm(path,up,128,144);
            free(img);free(up);
        }

        /* ===== Test 2: 1-bit flag per MACROBLOCK ===== */
        printf("\n--- Test 2: 1-bit flag per macroblock ---\n");
        br_init(&br, bs, bslen);
        int coded_mb = 0, skip_mb = 0;
        memset(dcY, 0, sizeof(dcY));
        bi_y = 0; dc_y = 0;
        
        for (int mby = 0; mby < mbh && !br_eof(&br); mby++) {
            for (int mbx = 0; mbx < mbw && !br_eof(&br); mbx++) {
                int flag = br_get1(&br);
                if (flag) {
                    for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                        int raw0 = vlc_coeff(&br);
                        dc_y += raw0;
                        dcY[bi_y++] = dc_y;
                        for (int i = 1; i < 64 && !br_eof(&br); i++)
                            vlc_coeff(&br);
                    }
                    for (int ch = 0; ch < 2 && !br_eof(&br); ch++)
                        for (int i = 0; i < 64 && !br_eof(&br); i++)
                            vlc_coeff(&br);
                    coded_mb++;
                } else {
                    for (int yb = 0; yb < 4; yb++)
                        dcY[bi_y++] = dc_y;
                    skip_mb++;
                }
            }
        }
        printf("Coded MBs: %d, Skipped MBs: %d, Total: %d, Y blocks: %d, bits=%d/%d\n",
               coded_mb, skip_mb, coded_mb+skip_mb, bi_y, br.total, bslen*8);

        /* ===== Test 3: 6-bit CBP per macroblock ===== */
        printf("\n--- Test 3: 6-bit CBP per macroblock ---\n");
        br_init(&br, bs, bslen);
        coded_blocks = 0; skip_blocks = 0;
        memset(dcY, 0, sizeof(dcY));
        bi_y = 0; dc_y = 0;
        
        for (int mby = 0; mby < mbh && !br_eof(&br); mby++) {
            for (int mbx = 0; mbx < mbw && !br_eof(&br); mbx++) {
                int cbp = br_get(&br, 6);
                for (int blk = 0; blk < 6 && !br_eof(&br); blk++) {
                    if (cbp & (1 << (5-blk))) {
                        if (blk < 4) {
                            int raw0 = vlc_coeff(&br);
                            dc_y += raw0;
                            dcY[bi_y] = dc_y;
                            for (int i = 1; i < 64 && !br_eof(&br); i++)
                                vlc_coeff(&br);
                        } else {
                            for (int i = 0; i < 64 && !br_eof(&br); i++)
                                vlc_coeff(&br);
                        }
                        coded_blocks++;
                    } else {
                        if (blk < 4) dcY[bi_y] = dc_y;
                        skip_blocks++;
                    }
                    if (blk < 4) bi_y++;
                }
            }
        }
        printf("Coded: %d, Skipped: %d, Y blocks: %d, bits=%d/%d\n",
               coded_blocks, skip_blocks, bi_y, br.total, bslen*8);

        /* ===== Test 4: NO per-block flag, but DC=0 means "skip rest" ===== */
        /* Maybe: read DC; if DC==0 and first AC==0, entire block is zero? */
        /* Actually, test: what if coefficient count is NOT 64 but variable? */
        
        /* ===== Test 5: Read first few bits to look for pattern ===== */
        printf("\n--- First 200 bits of bitstream ---\n");
        br_init(&br, bs, bslen);
        for (int i = 0; i < 200 && !br_eof(&br); i++) {
            if (i % 8 == 0 && i > 0) printf(" ");
            if (i % 64 == 0 && i > 0) printf("\n");
            printf("%d", br_get1(&br));
        }
        printf("\n");
        
        /* ===== Test 6: What if blocks are NOT 64 coefficients? ===== */
        /* The qtable has 16 entries. What if it's really 16 coefficients per block? */
        printf("\n--- Test 6: 16 coefficients per block ---\n");
        br_init(&br, bs, bslen);
        int nblk16 = 0;
        while (!br_eof(&br) && nblk16 < 5000) {
            vlc_coeff(&br);
            nblk16++;
        }
        /* nblk16 is really total coefficients */
        printf("Total coefficients: %d\n", nblk16);
        printf("  /16 = %d blocks (rem %d)\n", nblk16/16, nblk16%16);
        printf("  /32 = %d blocks (rem %d)\n", nblk16/32, nblk16%32);
        printf("  /48 = %d blocks (rem %d)\n", nblk16/48, nblk16%48);
        printf("  /64 = %d blocks (rem %d)\n", nblk16/64, nblk16%64);

        /* If 16 coefficients per block: total blocks = 20967/16 = 1310 remainder 7 */
        /* For 128×144 4:2:0 with 4×4 blocks:
           Y: 32×36 = 1152, Cb: 16×18 = 288, Cr: 16×18 = 288, total = 1728 → too many */
        /* For 128×144 4:2:0 with 8×8 blocks but only 16 coeff (AC truncated at 16):
           288+72+72 = 432 blocks × 16 = 6912 coefficients → way less than 20967 */
    }

    free(disc); zip_close(z);
    return 0;
}
