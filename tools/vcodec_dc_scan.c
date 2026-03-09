/*
 * Playdia video - DC-only scan at multiple widths
 * Uses MPEG-1 DC luminance VLC to extract DC coefficients
 * Then renders at various block widths to find correct resolution
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

/* MPEG-1 DC luminance VLC decode (Table B.12) */
static int mpeg1_dc_lum(BR *b) {
    int bit = br_get1(b);
    int size;
    if (bit == 0) {
        size = br_get1(b) ? 2 : 1;
    } else {
        bit = br_get1(b);
        if (bit == 0) { size = br_get1(b) ? 3 : 0; }
        else {
            bit = br_get1(b);
            if (bit == 0) size = 4;
            else { bit = br_get1(b);
                if (bit == 0) size = 5;
                else { bit = br_get1(b);
                    if (bit == 0) size = 6;
                    else { bit = br_get1(b);
                        if (bit == 0) size = 7;
                        else size = 8;
                    }
                }
            }
        }
    }
    if (size == 0) return 0;
    int val = br_get(b, size);
    if (val < (1 << (size-1))) val = val - (1 << size) + 1;
    return val;
}

/* MPEG-1 DC chrominance VLC (Table B.13) */
static int mpeg1_dc_chr(BR *b) {
    int bit = br_get1(b);
    int size;
    if (bit == 0) { size = br_get1(b) ? 1 : 0; }
    else {
        bit = br_get1(b);
        if (bit == 0) size = 2;
        else { bit = br_get1(b);
            if (bit == 0) size = 3;
            else { bit = br_get1(b);
                if (bit == 0) size = 4;
                else { bit = br_get1(b);
                    if (bit == 0) size = 5;
                    else { bit = br_get1(b);
                        if (bit == 0) size = 6;
                        else { bit = br_get1(b);
                            size = bit ? 8 : 7;
                        }
                    }
                }
            }
        }
    }
    if (size == 0) return 0;
    int val = br_get(b, size);
    if (val < (1 << (size-1))) val = val - (1 << size) + 1;
    return val;
}

/* Signed Exp-Golomb */
static int eg_signed(BR *b) {
    int lz = 0;
    while (!br_eof(b) && br_get1(b) == 0 && lz < 24) lz++;
    int suf = lz > 0 ? br_get(b, lz) : 0;
    int cn = (1 << lz) - 1 + suf;
    return (cn & 1) ? -((cn+1)/2) : (cn/2);
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

static void render_dc(const char *tag, int *dc, int ndc, int bw) {
    int bh = ndc / bw;
    if (bh < 2) return;

    // Find range for normalization
    int mn = dc[0], mx = dc[0];
    for (int i = 1; i < bw*bh; i++) {
        if (dc[i] < mn) mn = dc[i];
        if (dc[i] > mx) mx = dc[i];
    }
    int range = mx - mn;
    if (range < 1) range = 1;

    // Scale up: each DC block → 8×8 pixels
    int W = bw * 8, H = bh * 8;
    uint8_t *img = calloc(W * H, 1);
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            int v = (dc[by*bw+bx] - mn) * 255 / range;
            if (v < 0) v = 0; if (v > 255) v = 255;
            for (int dy = 0; dy < 8; dy++)
                for (int dx = 0; dx < 8; dx++)
                    img[(by*8+dy)*W + bx*8+dx] = v;
        }
    }
    char path[256];
    snprintf(path, sizeof(path), OUT_DIR "dc_%s_w%d.pgm", tag, bw);
    write_pgm(path, img, W, H);
    free(img);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <zip> [lba]\n", argv[0]); return 1; }
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
    printf("Assembled %d frames from %s\n", nf, argv[1]);

    for (int fi = 0; fi < nf && fi < 4; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        int qscale = f[3];
        printf("\n=== Frame %d: %d bytes, qscale=%d, type=%d ===\n",
               fi, fsize, qscale, f[39]);

        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        /* ===== Method 1: MPEG-1 DC luminance VLC ===== */
        {
            BR br; br_init(&br, bs, bslen);
            int dc[4096], ndc = 0;
            int prev = 0;
            while (ndc < 4096 && !br_eof(&br)) {
                int diff = mpeg1_dc_lum(&br);
                prev += diff;
                dc[ndc++] = prev;
            }
            printf("MPEG1-DC-lum: %d values, %d bits (%.1f bits/val)\n",
                   ndc, br.total, (double)br.total/ndc);
            printf("  First 32: ");
            for (int i=0;i<32&&i<ndc;i++) printf("%d ",dc[i]);
            printf("\n  Range: ");
            int mn=dc[0],mx=dc[0];
            for(int i=1;i<ndc;i++){if(dc[i]<mn)mn=dc[i];if(dc[i]>mx)mx=dc[i];}
            printf("[%d, %d]\n", mn, mx);

            char tag[64];
            int widths[] = {8, 16, 20, 24, 32, 36, 40, 48};
            for (int wi = 0; wi < 8; wi++) {
                snprintf(tag, sizeof(tag), "%s_f%d_m1lum", game, fi);
                render_dc(tag, dc, ndc, widths[wi]);
            }
        }

        /* ===== Method 2: MPEG-1 DC with 4:2:0 macroblock structure ===== */
        /* Each macroblock: 4 Y-DC (lum), 1 Cb-DC (chr), 1 Cr-DC (chr) */
        {
            BR br; br_init(&br, bs, bslen);
            int y_dc[2048], cb_dc[512], cr_dc[512];
            int ny=0, ncb=0, ncr=0;
            int prev_y=0, prev_cb=0, prev_cr=0;
            int mb = 0;

            while (mb < 512 && !br_eof(&br)) {
                for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                    int diff = mpeg1_dc_lum(&br);
                    prev_y += diff;
                    if (ny < 2048) y_dc[ny++] = prev_y;
                }
                {
                    int diff = mpeg1_dc_chr(&br);
                    prev_cb += diff;
                    if (ncb < 512) cb_dc[ncb++] = prev_cb;
                }
                {
                    int diff = mpeg1_dc_chr(&br);
                    prev_cr += diff;
                    if (ncr < 512) cr_dc[ncr++] = prev_cr;
                }
                mb++;
            }
            printf("MPEG1-DC-420: %d MBs, %d Y-DC, %d bits (%.1f bits/MB)\n",
                   mb, ny, br.total, (double)br.total/mb);
            printf("  Y first 16: ");
            for(int i=0;i<16&&i<ny;i++) printf("%d ",y_dc[i]);
            printf("\n  Cb first 8: ");
            for(int i=0;i<8&&i<ncb;i++) printf("%d ",cb_dc[i]);
            printf("\n  Cr first 8: ");
            for(int i=0;i<8&&i<ncr;i++) printf("%d ",cr_dc[i]);
            printf("\n");

            // Render Y-DC at various MB widths (1 MB = 2×2 blocks = 16×16 pixels)
            // For 128-wide image: 128/16 = 8 MBs wide → 16 Y blocks wide
            // For 128-wide image: 128/8 = 16 Y blocks wide
            int mbwidths[] = {4, 8, 10, 12, 16, 20};
            for (int wi = 0; wi < 6; wi++) {
                int mbw = mbwidths[wi];
                // Y blocks are arranged as 2×2 within each MB
                // MB order: left-to-right, top-to-bottom
                // Within MB: top-left, top-right, bottom-left, bottom-right
                int yw = mbw * 2; // Y block width
                int total_mbs = ny / 4;
                int mbh = total_mbs / mbw;
                if (mbh < 2) continue;

                int W = mbw * 16, H = mbh * 16;
                uint8_t *img = calloc(W * H, 1);

                // Find range
                int mn=y_dc[0], mx=y_dc[0];
                for(int i=1;i<mbw*mbh*4;i++){if(y_dc[i]<mn)mn=y_dc[i];if(y_dc[i]>mx)mx=y_dc[i];}
                int range = mx-mn; if(range<1)range=1;

                for (int mby = 0; mby < mbh; mby++) {
                    for (int mbx = 0; mbx < mbw; mbx++) {
                        int mi = mby * mbw + mbx;
                        // Y blocks: [0]=TL, [1]=TR, [2]=BL, [3]=BR
                        int yvals[4];
                        for (int j=0; j<4; j++)
                            yvals[j] = (y_dc[mi*4+j] - mn) * 255 / range;

                        // Top-left 8×8 block
                        for (int dy=0;dy<8;dy++) for(int dx=0;dx<8;dx++) {
                            int v = yvals[0]; if(v<0)v=0;if(v>255)v=255;
                            img[(mby*16+dy)*W + mbx*16+dx] = v;
                        }
                        // Top-right
                        for (int dy=0;dy<8;dy++) for(int dx=0;dx<8;dx++) {
                            int v = yvals[1]; if(v<0)v=0;if(v>255)v=255;
                            img[(mby*16+dy)*W + mbx*16+8+dx] = v;
                        }
                        // Bottom-left
                        for (int dy=0;dy<8;dy++) for(int dx=0;dx<8;dx++) {
                            int v = yvals[2]; if(v<0)v=0;if(v>255)v=255;
                            img[(mby*16+8+dy)*W + mbx*16+dx] = v;
                        }
                        // Bottom-right
                        for (int dy=0;dy<8;dy++) for(int dx=0;dx<8;dx++) {
                            int v = yvals[3]; if(v<0)v=0;if(v>255)v=255;
                            img[(mby*16+8+dy)*W + mbx*16+8+dx] = v;
                        }
                    }
                }
                char path[256];
                snprintf(path, sizeof(path), OUT_DIR "dc420_%s_f%d_mb%d.pgm", game, fi, mbw);
                write_pgm(path, img, W, H);
                free(img);
            }
        }

        /* ===== Method 3: Exp-Golomb DC ===== */
        {
            BR br; br_init(&br, bs, bslen);
            int dc[4096], ndc = 0;
            int prev = 0;
            while (ndc < 4096 && !br_eof(&br)) {
                int diff = eg_signed(&br);
                prev += diff;
                dc[ndc++] = prev;
                if (abs(prev) > 10000) break; // overflow
            }
            printf("EG-DC: %d values, %d bits (%.1f bits/val)\n",
                   ndc, br.total, ndc>0?(double)br.total/ndc:0);
            printf("  First 16: ");
            for(int i=0;i<16&&i<ndc;i++) printf("%d ",dc[i]);
            printf("\n");

            if (ndc > 100) {
                char tag[64];
                snprintf(tag, sizeof(tag), "%s_f%d_eg", game, fi);
                render_dc(tag, dc, ndc, 16);
                render_dc(tag, dc, ndc, 32);
            }
        }
    }

    free(disc); zip_close(z);
    return 0;
}
