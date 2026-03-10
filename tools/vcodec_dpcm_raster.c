/*
 * vcodec_dpcm_raster.c - Test pure DPCM (no DCT) hypothesis
 *
 * Key insight: 128*144 = 18432 Y pixels at ~4.67 bits/VLC = ~86000 bits = ~88%
 * This matches the observed bit usage!
 *
 * Test various DPCM modes:
 * 1. Left-predict: pixel = prev_pixel + VLC * qscale
 * 2. Top-predict: pixel = above_pixel + VLC * qscale
 * 3. Row-reset DPCM: reset accumulator each row
 * 4. Continuous DPCM: never reset
 * 5. 2D predict: pixel = (left + top) / 2 + VLC * qscale
 * 6. Y then Cb then Cr (plane sequential)
 * 7. MB-interleaved DPCM
 * 8. Different scale factors
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
static void write_ppm(const char *p, const uint8_t *rgb, int w, int h) {
    FILE *f=fopen(p,"wb"); if(!f)return;
    fprintf(f,"P6\n%d %d\n255\n",w,h); fwrite(rgb,1,w*h*3,f); fclose(f);
    printf("  -> %s (%dx%d RGB)\n",p,w,h);
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
static int clamp16(int v) { return v<-32768?-32768:v>32767?32767:v; }

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

    static uint8_t frames[16][MAX_FRAME]; int fsizes[16];
    int nf = assemble_frames(disc,tsec,slba,frames,fsizes,16);
    if (nf < 1) return 1;

    int imgW = 128, imgH = 144;

    for (int fi = 0; fi < 1; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        int qscale = f[3];
        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        printf("Frame %d: qscale=%d, type=%d, bslen=%d\n\n", fi, qscale, f[39], bslen);

        /* Decode all VLC values first */
        BR br; br_init(&br, bs, bslen);
        int vals[25000]; int nv = 0;
        while (nv < 25000 && !br_eof(&br)) {
            int old = br.total; vals[nv] = vlc_coeff(&br);
            if (br.total == old) break; nv++;
        }
        printf("Total VLC values: %d (bits: %d)\n\n", nv, br.total);

        /* ===== TEST 1: Left-predict DPCM, row-reset, Y only ===== */
        printf("--- Test 1: Left-predict DPCM, row-reset ---\n");
        for (int scale = 1; scale <= 16; scale *= 2) {
            uint8_t *img = malloc(imgW * imgH);
            int vi = 0;
            for (int y = 0; y < imgH; y++) {
                int acc = 128;
                for (int x = 0; x < imgW && vi < nv; x++, vi++) {
                    acc += vals[vi] * scale;
                    img[y * imgW + x] = clamp8(acc);
                }
            }
            printf("  scale=%d: used %d/%d values (%.1f%%)\n",
                   scale, vi < nv ? vi : nv, nv, 100.0 * vi / nv);
            char path[256];
            snprintf(path, sizeof(path), OUT_DIR "dpcm1_%s_s%d.pgm", tag, scale);
            write_pgm(path, img, imgW, imgH);
            free(img);
        }

        /* ===== TEST 2: Continuous DPCM (no row reset), various scales ===== */
        printf("\n--- Test 2: Continuous DPCM ---\n");
        for (int scale = 1; scale <= 16; scale *= 2) {
            uint8_t *img = malloc(imgW * imgH);
            int acc = 128;
            int vi = 0;
            for (int y = 0; y < imgH; y++) {
                for (int x = 0; x < imgW && vi < nv; x++, vi++) {
                    acc = clamp16(acc + vals[vi] * scale);
                    img[y * imgW + x] = clamp8(acc);
                }
            }
            char path[256];
            snprintf(path, sizeof(path), OUT_DIR "dpcm2_%s_s%d.pgm", tag, scale);
            write_pgm(path, img, imgW, imgH);
            free(img);
        }

        /* ===== TEST 3: 2D predict: (left + top) / 2 + delta ===== */
        printf("\n--- Test 3: 2D predict DPCM ---\n");
        for (int scale = 1; scale <= 8; scale *= 2) {
            int *buf = calloc(imgW * imgH, sizeof(int));
            int vi = 0;
            for (int y = 0; y < imgH; y++) {
                for (int x = 0; x < imgW && vi < nv; x++, vi++) {
                    int left = (x > 0) ? buf[y*imgW+x-1] : 128;
                    int top = (y > 0) ? buf[(y-1)*imgW+x] : 128;
                    int pred = (left + top) / 2;
                    buf[y*imgW+x] = pred + vals[vi] * scale;
                }
            }
            uint8_t *img = malloc(imgW * imgH);
            for (int i = 0; i < imgW*imgH; i++) img[i] = clamp8(buf[i]);
            char path[256];
            snprintf(path, sizeof(path), OUT_DIR "dpcm3_%s_s%d.pgm", tag, scale);
            write_pgm(path, img, imgW, imgH);
            free(img); free(buf);
        }

        /* ===== TEST 4: Plane-sequential Y+Cb+Cr with DPCM ===== */
        printf("\n--- Test 4: Plane-sequential DPCM (Y+Cb+Cr) ---\n");
        {
            int vi = 0;
            /* Y plane - row-reset DPCM */
            int *Yp = calloc(imgW * imgH, sizeof(int));
            for (int y = 0; y < imgH; y++) {
                int acc = 0;
                for (int x = 0; x < imgW && vi < nv; x++, vi++) {
                    acc += vals[vi] * qscale;
                    Yp[y*imgW+x] = acc;
                }
            }
            int y_vals = vi;
            printf("  Y: %d values\n", y_vals);

            /* Cb plane */
            int chW = imgW/2, chH = imgH/2;
            int *Cb = calloc(chW * chH, sizeof(int));
            for (int y = 0; y < chH; y++) {
                int acc = 0;
                for (int x = 0; x < chW && vi < nv; x++, vi++) {
                    acc += vals[vi] * qscale;
                    Cb[y*chW+x] = acc;
                }
            }
            printf("  Cb: %d values\n", vi - y_vals);

            /* Cr plane */
            int *Cr = calloc(chW * chH, sizeof(int));
            int cb_vals = vi;
            for (int y = 0; y < chH; y++) {
                int acc = 0;
                for (int x = 0; x < chW && vi < nv; x++, vi++) {
                    acc += vals[vi] * qscale;
                    Cr[y*chW+x] = acc;
                }
            }
            printf("  Cr: %d values\n", vi - cb_vals);
            printf("  Total: %d/%d values\n", vi, nv);

            /* Output Y */
            uint8_t *yimg = malloc(imgW*imgH);
            for (int i = 0; i < imgW*imgH; i++) yimg[i] = clamp8(Yp[i] + 128);
            char path[256];
            snprintf(path, sizeof(path), OUT_DIR "dpcm4_y_%s.pgm", tag);
            write_pgm(path, yimg, imgW, imgH);

            /* Output RGB */
            uint8_t *rgb = malloc(imgW*imgH*3);
            for (int y = 0; y < imgH; y++) {
                for (int x = 0; x < imgW; x++) {
                    int yv = Yp[y*imgW+x] + 128;
                    int cb = (y/2 < chH && x/2 < chW) ? Cb[(y/2)*chW+(x/2)] : 0;
                    int cr = (y/2 < chH && x/2 < chW) ? Cr[(y/2)*chW+(x/2)] : 0;
                    rgb[(y*imgW+x)*3+0] = clamp8(yv + 1.402*cr);
                    rgb[(y*imgW+x)*3+1] = clamp8(yv - 0.344*cb - 0.714*cr);
                    rgb[(y*imgW+x)*3+2] = clamp8(yv + 1.772*cb);
                }
            }
            snprintf(path, sizeof(path), OUT_DIR "dpcm4_rgb_%s.ppm", tag);
            write_ppm(path, rgb, imgW, imgH);
            free(yimg); free(rgb); free(Yp); free(Cb); free(Cr);
        }

        /* ===== TEST 5: Try at multiple widths ===== */
        printf("\n--- Test 5: DPCM at various widths ---\n");
        int widths[] = {64, 96, 112, 128, 144, 160, 176, 192, 256};
        for (int wi = 0; wi < 9; wi++) {
            int w = widths[wi];
            int h = 18432 / w;
            if (h * w > nv) h = nv / w;

            uint8_t *img = malloc(w * h);
            int vi = 0;
            for (int y = 0; y < h; y++) {
                int acc = 128;
                for (int x = 0; x < w; x++, vi++) {
                    acc += vals[vi] * qscale;
                    img[y*w+x] = clamp8(acc);
                }
            }
            char path[256];
            snprintf(path, sizeof(path), OUT_DIR "dpcm5_%s_w%d.pgm", tag, w);
            write_pgm(path, img, w, h);
            free(img);
        }

        /* ===== TEST 6: DPCM with clamp to 0-255 range (not wrapping) ===== */
        printf("\n--- Test 6: Clamped DPCM ---\n");
        {
            uint8_t *img = malloc(imgW * imgH);
            int vi = 0;
            for (int y = 0; y < imgH; y++) {
                int acc = 128;
                for (int x = 0; x < imgW && vi < nv; x++, vi++) {
                    acc = clamp8(acc + vals[vi] * qscale);
                    img[y*imgW+x] = acc;
                }
            }
            char path[256];
            snprintf(path, sizeof(path), OUT_DIR "dpcm6_clamp_%s.pgm", tag);
            write_pgm(path, img, imgW, imgH);
            free(img);
        }

        /* ===== TEST 7: Absolute values (not DPCM) ===== */
        printf("\n--- Test 7: Absolute (not DPCM) ---\n");
        {
            uint8_t *img = malloc(imgW * imgH);
            int vi = 0;
            for (int y = 0; y < imgH; y++) {
                for (int x = 0; x < imgW && vi < nv; x++, vi++) {
                    img[y*imgW+x] = clamp8(vals[vi] * qscale + 128);
                }
            }
            char path[256];
            snprintf(path, sizeof(path), OUT_DIR "dpcm7_abs_%s.pgm", tag);
            write_pgm(path, img, imgW, imgH);
            free(img);
        }

        /* ===== TEST 8: DYUV-style (CD-i compatible) ===== */
        /* DYUV: each byte encodes 2 pixels. Upper nibble = delta-Y for even pixel,
         * lower nibble = delta-U or delta-V alternating.
         * But VLC is variable-length, so maybe it's VLC-coded DYUV */
        printf("\n--- Test 8: Interleaved Y/U/V DPCM (CD-i DYUV style) ---\n");
        {
            /* Every 2 pixels: Y0, Y1, share U and V */
            /* Bitstream order: U, Y0, V, Y1 (like YUV422) */
            int *Y = calloc(imgW*imgH, sizeof(int));
            int *U = calloc(imgW*imgH/4, sizeof(int));
            int *V = calloc(imgW*imgH/4, sizeof(int));
            int vi = 0;
            int yacc = 0, uacc = 0, vacc = 0;

            for (int y = 0; y < imgH && vi+3 < nv; y++) {
                yacc = 0; uacc = 0; vacc = 0; /* Row reset */
                for (int x = 0; x < imgW && vi+3 < nv; x += 2) {
                    /* Order: Y0 Y1 U V */
                    yacc += vals[vi++] * qscale;
                    Y[y*imgW+x] = yacc;
                    yacc += vals[vi++] * qscale;
                    Y[y*imgW+x+1] = yacc;
                    uacc += vals[vi++] * qscale;
                    U[y/2*(imgW/2)+x/2] = uacc;
                    vacc += vals[vi++] * qscale;
                    V[y/2*(imgW/2)+x/2] = vacc;
                }
            }
            printf("  Used %d/%d values\n", vi, nv);

            uint8_t *yimg = malloc(imgW*imgH);
            for (int i = 0; i < imgW*imgH; i++) yimg[i] = clamp8(Y[i]+128);
            char path[256];
            snprintf(path, sizeof(path), OUT_DIR "dpcm8_dyuv_y_%s.pgm", tag);
            write_pgm(path, yimg, imgW, imgH);

            /* RGB */
            uint8_t *rgb = malloc(imgW*imgH*3);
            for (int y = 0; y < imgH; y++)
                for (int x = 0; x < imgW; x++) {
                    int yv = Y[y*imgW+x]+128;
                    int u = U[(y/2)*(imgW/2)+(x/2)];
                    int v = V[(y/2)*(imgW/2)+(x/2)];
                    rgb[(y*imgW+x)*3+0] = clamp8(yv + 1.402*v);
                    rgb[(y*imgW+x)*3+1] = clamp8(yv - 0.344*u - 0.714*v);
                    rgb[(y*imgW+x)*3+2] = clamp8(yv + 1.772*u);
                }
            snprintf(path, sizeof(path), OUT_DIR "dpcm8_dyuv_rgb_%s.ppm", tag);
            write_ppm(path, rgb, imgW, imgH);
            free(yimg); free(rgb); free(Y); free(U); free(V);
        }
    }

    free(disc); zip_close(z);
    return 0;
}
