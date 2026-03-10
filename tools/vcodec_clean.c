/*
 * vcodec_clean.c - Clean full decode testing multiple models side by side
 * Focus on getting a recognizable image at various scenes
 *
 * Models tested:
 * A. EOB (val=0 terminates block) - uses ~17% bits (8x8)
 * B. Per-AC bit flag - uses ~88% bits (8x8)
 * C. EOB 4x4 blocks - uses ~63% bits
 *
 * Each with DC reset per MB row and multiple dequant options.
 * Also try: normalize output per-block to fix scaling.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <zip.h>
#include <math.h>

#define SECTOR_RAW 2352
#define MAX_FRAME  65536
#define PI 3.14159265358979323846
#define OUT_DIR "/home/wizzard/share/GitHub/playdia-emu/tools/test_output/"

static void write_pgm(const char *p, const uint8_t *g, int w, int h) {
    FILE *f=fopen(p,"wb"); if(!f)return;
    fprintf(f,"P5\n%d %d\n255\n",w,h); fwrite(g,1,w*h,f); fclose(f);
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

static const int zz8[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

static void idct8x8(int block[64], double out[64]) {
    double tmp[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) {
            double sum = 0;
            for (int k = 0; k < 8; k++) {
                double ck = (k == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += ck * block[i*8+k] * cos((2*j+1)*k*PI/16.0);
            }
            tmp[i*8+j] = sum * 0.5;
        }
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) {
            double sum = 0;
            for (int k = 0; k < 8; k++) {
                double ck = (k == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += ck * tmp[k*8+j] * cos((2*i+1)*k*PI/16.0);
            }
            out[i*8+j] = sum * 0.5;
        }
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

static void place_block8(int *plane, int pw, int bx, int by, double spatial[64]) {
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            plane[(by*8+y)*pw + bx*8+x] = (int)round(spatial[y*8+x]);
}

/* Decode a single 8x8 block with EOB model */
static void decode_block_eob(BR *br, int *dc, int block[64], int qm[64], int qscale) {
    memset(block, 0, 64*sizeof(int));
    *dc += vlc_coeff(br);
    block[0] = *dc * 8;
    int pos = 1;
    while (pos < 64 && !br_eof(br)) {
        int v = vlc_coeff(br);
        if (v == 0) break; /* EOB */
        block[zz8[pos]] = v * qm[zz8[pos]] * qscale / 16;
        pos++;
    }
}

/* Decode a single 8x8 block with per-AC flag model */
static void decode_block_acflag(BR *br, int *dc, int block[64], int qm[64], int qscale) {
    memset(block, 0, 64*sizeof(int));
    *dc += vlc_coeff(br);
    block[0] = *dc * 8;
    for (int i = 1; i < 64 && !br_eof(br); i++) {
        if (br_get1(br))
            block[zz8[i]] = vlc_coeff(br) * qm[zz8[i]] * qscale / 16;
    }
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

    int imgW = 128, imgH = 144;
    int scene_lbas[] = {502, 1872, 3072, 5232, 757, 1112};

    for (int si = 0; si < 6; si++) {
        int lba = scene_lbas[si];
        static uint8_t frames[4][MAX_FRAME]; int fsizes[4];
        int nf = assemble_frames(disc, tsec, lba, frames, fsizes, 4);
        if (nf < 1) continue;

        uint8_t *f = frames[0];
        int fsize = fsizes[0];
        int qscale = f[3];
        uint8_t qt[16]; memcpy(qt, f+4, 16);
        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        int qm[64];
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 8; j++)
                qm[i*8+j] = qt[(i/2)*4 + (j/2)];

        printf("LBA %d: qs=%d type=%d\n", lba, qscale, f[39]);

        /* ===== Model A: EOB with DC reset per row ===== */
        {
            BR br; br_init(&br, bs, bslen);
            int *planeY = calloc(imgW*imgH, sizeof(int));

            for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
                int dc_y = 128, dc_cb = 128, dc_cr = 128;
                for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
                    double yspat[4][64];
                    for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                        int block[64];
                        decode_block_eob(&br, &dc_y, block, qm, qscale);
                        idct8x8(block, yspat[yb]);
                    }
                    place_block8(planeY, imgW, mbx*2,   mby*2,   yspat[0]);
                    place_block8(planeY, imgW, mbx*2+1, mby*2,   yspat[1]);
                    place_block8(planeY, imgW, mbx*2,   mby*2+1, yspat[2]);
                    place_block8(planeY, imgW, mbx*2+1, mby*2+1, yspat[3]);

                    int cb[64], cr[64]; double cs[64];
                    decode_block_eob(&br, &dc_cb, cb, qm, qscale);
                    decode_block_eob(&br, &dc_cr, cr, qm, qscale);
                }
            }
            printf("  EOB: bits %d/%d (%.1f%%)\n", br.total, bslen*8, 100.0*br.total/(bslen*8));

            /* Auto-scale output */
            int pmin=99999, pmax=-99999;
            for(int i=0;i<imgW*imgH;i++){
                if(planeY[i]<pmin)pmin=planeY[i];
                if(planeY[i]>pmax)pmax=planeY[i];
            }
            uint8_t *yimg = malloc(imgW*imgH);
            if (pmax > pmin) {
                for(int i=0;i<imgW*imgH;i++)
                    yimg[i] = clamp8(255*(planeY[i]-pmin)/(pmax-pmin));
            } else {
                memset(yimg, 128, imgW*imgH);
            }
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "clean_eob_lba%d.pgm",lba);
            write_pgm(path, yimg, imgW, imgH);
            printf("    range [%d,%d] -> %s\n", pmin, pmax, path);

            /* Also raw (not auto-scaled) */
            for(int i=0;i<imgW*imgH;i++) yimg[i]=clamp8(planeY[i]);
            snprintf(path,sizeof(path),OUT_DIR "clean_eob_raw_lba%d.pgm",lba);
            write_pgm(path, yimg, imgW, imgH);

            free(yimg); free(planeY);
        }

        /* ===== Model B: Per-AC flag with DC reset per row ===== */
        {
            BR br; br_init(&br, bs, bslen);
            int *planeY = calloc(imgW*imgH, sizeof(int));

            for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
                int dc_y = 128, dc_cb = 128, dc_cr = 128;
                for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
                    double yspat[4][64];
                    for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                        int block[64];
                        decode_block_acflag(&br, &dc_y, block, qm, qscale);
                        idct8x8(block, yspat[yb]);
                    }
                    place_block8(planeY, imgW, mbx*2,   mby*2,   yspat[0]);
                    place_block8(planeY, imgW, mbx*2+1, mby*2,   yspat[1]);
                    place_block8(planeY, imgW, mbx*2,   mby*2+1, yspat[2]);
                    place_block8(planeY, imgW, mbx*2+1, mby*2+1, yspat[3]);

                    int cb[64], cr[64];
                    decode_block_acflag(&br, &dc_cb, cb, qm, qscale);
                    decode_block_acflag(&br, &dc_cr, cr, qm, qscale);
                }
            }
            printf("  ACflag: bits %d/%d (%.1f%%)\n", br.total, bslen*8, 100.0*br.total/(bslen*8));

            int pmin=99999, pmax=-99999;
            for(int i=0;i<imgW*imgH;i++){
                if(planeY[i]<pmin)pmin=planeY[i];
                if(planeY[i]>pmax)pmax=planeY[i];
            }
            uint8_t *yimg = malloc(imgW*imgH);
            if (pmax > pmin) {
                for(int i=0;i<imgW*imgH;i++)
                    yimg[i] = clamp8(255*(planeY[i]-pmin)/(pmax-pmin));
            }
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "clean_acflag_lba%d.pgm",lba);
            write_pgm(path, yimg, imgW, imgH);
            printf("    range [%d,%d] -> %s\n", pmin, pmax, path);

            for(int i=0;i<imgW*imgH;i++) yimg[i]=clamp8(planeY[i]);
            snprintf(path,sizeof(path),OUT_DIR "clean_acflag_raw_lba%d.pgm",lba);
            write_pgm(path, yimg, imgW, imgH);

            free(yimg); free(planeY);
        }

        /* ===== Model C: EOB, NO DC reset (continuous DPCM) ===== */
        {
            BR br; br_init(&br, bs, bslen);
            int *planeY = calloc(imgW*imgH, sizeof(int));
            int dc_y=0, dc_cb=0, dc_cr=0;

            for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
                for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
                    double yspat[4][64];
                    for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                        int block[64];
                        decode_block_eob(&br, &dc_y, block, qm, qscale);
                        idct8x8(block, yspat[yb]);
                    }
                    place_block8(planeY, imgW, mbx*2,   mby*2,   yspat[0]);
                    place_block8(planeY, imgW, mbx*2+1, mby*2,   yspat[1]);
                    place_block8(planeY, imgW, mbx*2,   mby*2+1, yspat[2]);
                    place_block8(planeY, imgW, mbx*2+1, mby*2+1, yspat[3]);

                    int cb[64], cr[64];
                    decode_block_eob(&br, &dc_cb, cb, qm, qscale);
                    decode_block_eob(&br, &dc_cr, cr, qm, qscale);
                }
            }
            /* Auto-scale */
            int pmin=99999, pmax=-99999;
            for(int i=0;i<imgW*imgH;i++){
                if(planeY[i]<pmin)pmin=planeY[i];
                if(planeY[i]>pmax)pmax=planeY[i];
            }
            uint8_t *yimg = malloc(imgW*imgH);
            if (pmax > pmin) {
                for(int i=0;i<imgW*imgH;i++)
                    yimg[i] = clamp8(255*(planeY[i]-pmin)/(pmax-pmin));
            }
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "clean_eob_nodcreset_lba%d.pgm",lba);
            write_pgm(path, yimg, imgW, imgH);
            printf("  EOB_noDCrst: range [%d,%d]\n", pmin, pmax);

            free(yimg); free(planeY);
        }
    }

    free(disc); zip_close(z);
    return 0;
}
