/*
 * vcodec_dcdump.c - Dump raw DC values and test various scalings
 * to find the correct DC representation
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

static int clamp8(int v) { return v<0?0:v>255?255:v; }

static void upscale(const uint8_t *src, int sw, int sh, uint8_t *dst, int factor) {
    int dw = sw * factor;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++)
            for (int dy = 0; dy < factor; dy++)
                for (int dx = 0; dx < factor; dx++)
                    dst[(y*factor+dy)*dw + x*factor+dx] = src[y*sw+x];
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    int slba = argc > 2 ? atoi(argv[2]) : 502;
    const char *tag = argc > 3 ? argv[3] : "m";

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

    /* Try multiple scenes */
    int scene_lbas[] = {277, 502, 757, 1112, 1872, 3072, 5232};

    for (int si = 0; si < 7; si++) {
        int lba = scene_lbas[si];
        static uint8_t frames[4][MAX_FRAME]; int fsizes[4];
        int nf = assemble_frames(disc,tsec,lba,frames,fsizes,4);
        if (nf < 1) continue;

        uint8_t *f = frames[0];
        int fsize = fsizes[0];
        int qscale = f[3];
        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        printf("\n=== LBA %d: qscale=%d, type=%d ===\n", lba, qscale, f[39]);

        /* Extract DC values using per-AC bit flag model (8x8 blocks) */
        BR br; br_init(&br, bs, bslen);
        int dc_vals[288]; /* Y blocks: 16x18 = 288 */
        int dc_y = 0;
        int ndv = 0;

        for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
            for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
                for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                    dc_y += vlc_coeff(&br);
                    int bx = mbx*2 + (yb&1);
                    int by = mby*2 + (yb>>1);
                    if (ndv < 288) dc_vals[by*16+bx] = dc_y;
                    ndv++;
                    /* Skip AC */
                    for (int i=1; i<64 && !br_eof(&br); i++)
                        if (br_get1(&br)) vlc_coeff(&br);
                }
                /* Skip Cb, Cr */
                for (int c = 0; c < 2 && !br_eof(&br); c++) {
                    vlc_coeff(&br);
                    for (int i=1; i<64 && !br_eof(&br); i++)
                        if (br_get1(&br)) vlc_coeff(&br);
                }
            }
        }

        /* Print DC value range */
        int dmin=99999, dmax=-99999;
        for (int i = 0; i < 288; i++) {
            if (dc_vals[i] < dmin) dmin = dc_vals[i];
            if (dc_vals[i] > dmax) dmax = dc_vals[i];
        }
        printf("  DC range: [%d, %d]\n", dmin, dmax);

        /* Print first row of DC values */
        printf("  DC row 0: ");
        for (int x = 0; x < 16; x++) printf("%4d ", dc_vals[x]);
        printf("\n  DC row 1: ");
        for (int x = 0; x < 16; x++) printf("%4d ", dc_vals[16+x]);
        printf("\n");

        /* Generate images with different DC scalings */
        int scales[] = {1, 2, 4, 8};
        for (int sci = 0; sci < 4; sci++) {
            int s = scales[sci];
            uint8_t dc_img[288];
            for (int i = 0; i < 288; i++)
                dc_img[i] = clamp8(dc_vals[i] * s + 128);

            uint8_t *big = malloc(128 * 144);
            upscale(dc_img, 16, 18, big, 8);
            char path[256];
            snprintf(path, sizeof(path), OUT_DIR "dc_%s_lba%d_s%d.pgm", tag, lba, s);
            write_pgm(path, big, 128, 144);
            free(big);
        }

        /* Also try: DC is NOT DPCM, just absolute */
        {
            BR br2; br_init(&br2, bs, bslen);
            uint8_t dc_abs[288];
            int ni = 0;
            for (int mby=0; mby<9 && !br_eof(&br2); mby++) {
                for (int mbx=0; mbx<8 && !br_eof(&br2); mbx++) {
                    for (int yb=0; yb<4 && !br_eof(&br2); yb++) {
                        int v = vlc_coeff(&br2);
                        int bx = mbx*2 + (yb&1);
                        int by = mby*2 + (yb>>1);
                        if (ni < 288)
                            dc_abs[by*16+bx] = clamp8(v * 4 + 128);
                        ni++;
                        for (int i=1; i<64 && !br_eof(&br2); i++)
                            if (br_get1(&br2)) vlc_coeff(&br2);
                    }
                    for (int c=0; c<2 && !br_eof(&br2); c++) {
                        vlc_coeff(&br2);
                        for (int i=1; i<64 && !br_eof(&br2); i++)
                            if (br_get1(&br2)) vlc_coeff(&br2);
                    }
                }
            }
            uint8_t *big = malloc(128*144);
            upscale(dc_abs, 16, 18, big, 8);
            char path[256];
            snprintf(path, sizeof(path), OUT_DIR "dc_abs_%s_lba%d.pgm", tag, lba);
            write_pgm(path, big, 128, 144);
            free(big);
        }

        /* Try EOB model and show DC */
        {
            BR br2; br_init(&br2, bs, bslen);
            int dc = 0;
            uint8_t dc_img[288];
            int ni = 0;
            for (int mby=0; mby<9 && !br_eof(&br2); mby++) {
                for (int mbx=0; mbx<8 && !br_eof(&br2); mbx++) {
                    for (int yb=0; yb<4 && !br_eof(&br2); yb++) {
                        dc += vlc_coeff(&br2);
                        int bx = mbx*2 + (yb&1);
                        int by = mby*2 + (yb>>1);
                        if (ni < 288) dc_img[by*16+bx] = clamp8(dc + 128);
                        ni++;
                        /* EOB model: skip until value=0 */
                        int ac = 0;
                        while (ac < 63 && !br_eof(&br2)) {
                            int v = vlc_coeff(&br2);
                            if (v == 0) break;
                            ac++;
                        }
                    }
                    for (int c=0; c<2 && !br_eof(&br2); c++) {
                        dc += vlc_coeff(&br2);
                        int ac=0;
                        while(ac<63 && !br_eof(&br2)){
                            int v=vlc_coeff(&br2); if(v==0)break; ac++;
                        }
                    }
                }
            }
            uint8_t *big = malloc(128*144);
            upscale(dc_img, 16, 18, big, 8);
            char path[256];
            snprintf(path, sizeof(path), OUT_DIR "dc_eob_%s_lba%d.pgm", tag, lba);
            write_pgm(path, big, 128, 144);
            free(big);
        }
    }

    free(disc); zip_close(z);
    return 0;
}
