/*
 * DC-only decoder v2 - test plane-sequential vs MB-interleaved
 * Also test different resolutions
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
    const char *game = argc > 3 ? argv[3] : "mari";

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
    printf("Assembled %d frames\n", nf);

    for (int fi = 0; fi < 2 && fi < nf; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        int qscale = f[3];
        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        printf("\n=== Frame %d: qscale=%d, type=%d ===\n", fi, qscale, f[39]);

        /* Test: read ALL coefficients sequentially, count blocks */
        BR br; br_init(&br, bs, bslen);
        int total_coeffs = 0;
        while (!br_eof(&br)) {
            vlc_coeff(&br);
            total_coeffs++;
        }
        printf("Total coefficients decoded: %d (= %d blocks of 64, remainder %d)\n",
               total_coeffs, total_coeffs/64, total_coeffs%64);
        printf("Bits used: %d/%d\n", br.total, bslen*8);

        /* =================================================================
         * APPROACH A: Plane-sequential - all Y, then Cb, then Cr
         * Try different block counts / resolutions
         * ================================================================= */
        
        /* For 128×144 4:2:0: 16×18 Y blocks = 288, 8×9 Cb = 72, 8×9 Cr = 72, total = 432 */
        /* For 128×128 4:2:0: 16×16 Y = 256, 8×8 Cb = 64, 8×8 Cr = 64, total = 384 */
        /* For 160×120 4:2:0: 20×15 Y = 300, 10×7.5 → doesn't work */
        /* For 128×96  4:2:0: 16×12 Y = 192, 8×6 Cb = 48, 8×6 Cr = 48, total = 288 */
        
        int configs[][3] = {
            {16, 18, 0}, /* 128×144, 288 Y blocks */
            {16, 16, 0}, /* 128×128, 256 Y blocks */
            {16, 12, 0}, /* 128×96, 192 Y blocks */
            {20, 15, 0}, /* 160×120, 300 Y blocks */
            {16, 18, 1}, /* 128×144, MB-interleaved */
        };
        int nconfigs = 5;

        for (int ci = 0; ci < nconfigs; ci++) {
            int bw = configs[ci][0], bh = configs[ci][1];
            int interleaved = configs[ci][2];
            int nY = bw * bh;
            int cw = bw/2, ch = bh/2;
            int nC = cw * ch;
            int total_needed = nY + nC*2;
            
            if (interleaved) {
                /* Already tested above */
                continue;
            }

            printf("\n--- Config: %dx%d blocks (Y=%d, C=%d), total=%d blocks ---\n",
                   bw, bh, nY, nC, total_needed);

            /* Plane-sequential decode */
            br_init(&br, bs, bslen);
            int dcY[1024], dcCb[256], dcCr[256];
            int dc = 0;

            /* Decode Y DCs */
            int got_y = 0;
            for (int i = 0; i < nY && !br_eof(&br); i++) {
                int raw0 = vlc_coeff(&br);
                dc += raw0;
                dcY[i] = dc;
                got_y++;
                for (int j = 1; j < 64 && !br_eof(&br); j++)
                    vlc_coeff(&br);
            }
            int bits_after_y = br.total;
            
            /* Decode Cb DCs */
            dc = 0;
            int got_cb = 0;
            for (int i = 0; i < nC && !br_eof(&br); i++) {
                int raw0 = vlc_coeff(&br);
                dc += raw0;
                dcCb[i] = dc;
                got_cb++;
                for (int j = 1; j < 64 && !br_eof(&br); j++)
                    vlc_coeff(&br);
            }
            int bits_after_cb = br.total;
            
            /* Decode Cr DCs */
            dc = 0;
            int got_cr = 0;
            for (int i = 0; i < nC && !br_eof(&br); i++) {
                int raw0 = vlc_coeff(&br);
                dc += raw0;
                dcCr[i] = dc;
                got_cr++;
                for (int j = 1; j < 64 && !br_eof(&br); j++)
                    vlc_coeff(&br);
            }
            
            printf("Y: %d/%d blocks (%d bits), Cb: %d/%d (%d bits), Cr: %d/%d (%d bits), total bits: %d/%d\n",
                   got_y, nY, bits_after_y,
                   got_cb, nC, bits_after_cb - bits_after_y,
                   got_cr, nC, br.total - bits_after_cb,
                   br.total, bslen*8);
            
            if (got_y < nY) { printf("  Not enough data for Y, skipping\n"); continue; }

            /* Output DC image */
            int ymin=99999,ymax=-99999;
            for (int i=0;i<got_y;i++) { if(dcY[i]<ymin)ymin=dcY[i]; if(dcY[i]>ymax)ymax=dcY[i]; }
            printf("Y DC range: %d..%d\n", ymin, ymax);

            /* Map blocks to image - raster order (block 0 = top-left, etc) */
            uint8_t *img = calloc(bw * bh, 1);
            for (int i = 0; i < got_y; i++) {
                int scaled = (ymax!=ymin) ? (dcY[i]-ymin)*255/(ymax-ymin) : 128;
                img[i] = clamp8(scaled);
            }
            char path[256];
            snprintf(path, sizeof(path), OUT_DIR "dcseq_%dx%d_%s_f%d.pgm", bw, bh, game, fi);
            write_pgm(path, img, bw, bh);

            /* Upscale */
            int pw = bw*8, ph = bh*8;
            uint8_t *up = calloc(pw * ph, 1);
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++)
                    up[y*pw+x] = img[(y/8)*bw + (x/8)];
            snprintf(path, sizeof(path), OUT_DIR "dcseq_%dx%d_up_%s_f%d.pgm", bw, bh, game, fi);
            write_pgm(path, up, pw, ph);

            /* Also try: Y blocks in MB raster order (2x2 blocks per MB) */
            int mbw = bw/2, mbh2 = bh/2;
            uint8_t *img_mb = calloc(bw * bh, 1);
            int idx = 0;
            for (int mby = 0; mby < mbh2; mby++) {
                for (int mbx = 0; mbx < mbw; mbx++) {
                    /* TL, TR, BL, BR within this MB */
                    int positions[4][2] = {
                        {mbx*2, mby*2}, {mbx*2+1, mby*2},
                        {mbx*2, mby*2+1}, {mbx*2+1, mby*2+1}
                    };
                    for (int b = 0; b < 4 && idx < got_y; b++) {
                        int scaled = (ymax!=ymin) ? (dcY[idx]-ymin)*255/(ymax-ymin) : 128;
                        img_mb[positions[b][1]*bw + positions[b][0]] = clamp8(scaled);
                        idx++;
                    }
                }
            }
            snprintf(path, sizeof(path), OUT_DIR "dcseq_mb_%dx%d_%s_f%d.pgm", bw, bh, game, fi);
            write_pgm(path, img_mb, bw, bh);
            
            uint8_t *up_mb = calloc(pw * ph, 1);
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++)
                    up_mb[y*pw+x] = img_mb[(y/8)*bw + (x/8)];
            snprintf(path, sizeof(path), OUT_DIR "dcseq_mb_%dx%d_up_%s_f%d.pgm", bw, bh, game, fi);
            write_pgm(path, up_mb, pw, ph);

            free(img); free(up); free(img_mb); free(up_mb);
        }

        /* ================================================================
         * APPROACH B: Single-plane, various widths, to find right width
         * Read ALL blocks sequentially, try as single grayscale plane
         * ================================================================ */
        printf("\n--- Raw sequential decode (all blocks) ---\n");
        br_init(&br, bs, bslen);
        int allDC[2048];
        int dc2 = 0;
        int nblk = 0;
        while (!br_eof(&br) && nblk < 2048) {
            int raw0 = vlc_coeff(&br);
            dc2 += raw0;
            allDC[nblk++] = dc2;
            for (int j = 1; j < 64 && !br_eof(&br); j++)
                vlc_coeff(&br);
        }
        printf("Total blocks decoded: %d, bits=%d/%d\n", nblk, br.total, bslen*8);

        int amin=99999,amax=-99999;
        for(int i=0;i<nblk;i++){if(allDC[i]<amin)amin=allDC[i];if(allDC[i]>amax)amax=allDC[i];}
        printf("DC range: %d..%d\n", amin, amax);

        /* Try different widths */
        int widths[] = {8, 12, 16, 18, 20, 24, 32};
        for (int wi = 0; wi < 7; wi++) {
            int w = widths[wi];
            int h = nblk / w;
            if (h * w > nblk) h--;
            if (h <= 0) continue;
            uint8_t *img = calloc(w * h, 1);
            for (int i = 0; i < w*h && i < nblk; i++) {
                int scaled = (amax!=amin) ? (allDC[i]-amin)*255/(amax-amin) : 128;
                img[i] = clamp8(scaled);
            }
            char path[256];
            snprintf(path, sizeof(path), OUT_DIR "dcraw_w%d_%s_f%d.pgm", w, game, fi);
            write_pgm(path, img, w, h);
            free(img);
        }

        /* Also try absolute (non-differential) DC */
        br_init(&br, bs, bslen);
        int allAbs[2048];
        nblk = 0;
        while (!br_eof(&br) && nblk < 2048) {
            allAbs[nblk] = vlc_coeff(&br);
            nblk++;
            for (int j = 1; j < 64 && !br_eof(&br); j++)
                vlc_coeff(&br);
        }
        amin=99999;amax=-99999;
        for(int i=0;i<nblk;i++){if(allAbs[i]<amin)amin=allAbs[i];if(allAbs[i]>amax)amax=allAbs[i];}
        printf("Abs DC range: %d..%d, blocks=%d\n", amin, amax, nblk);
        
        for (int wi = 0; wi < 7; wi++) {
            int w = widths[wi];
            int h = nblk / w;
            if (h <= 0) continue;
            uint8_t *img = calloc(w * h, 1);
            for (int i = 0; i < w*h && i < nblk; i++) {
                int scaled = (amax!=amin) ? (allAbs[i]-amin)*255/(amax-amin) : 128;
                img[i] = clamp8(scaled);
            }
            char path[256];
            snprintf(path, sizeof(path), OUT_DIR "dcabs_w%d_%s_f%d.pgm", w, game, fi);
            write_pgm(path, img, w, h);
            free(img);
        }
    }

    free(disc); zip_close(z);
    return 0;
}
