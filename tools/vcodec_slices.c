/*
 * vcodec_slices.c - Test DC reset per macroblock row (MPEG-1 slice behavior)
 * and try full decode with corrected dequantization
 *
 * Key finding: per-AC flag DC images show recognizable structure!
 * But DC drifts → need DC reset per row (slice) or per frame area.
 * Also test: DC DPCM resets for Cb/Cr separately from Y.
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

static const int zigzag8[64] = {
     0, 1, 8,16, 9, 2, 3,10,
    17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,
    27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,
    29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};

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

    int imgW = 128, imgH = 144;
    int scene_lbas[] = {502, 1872, 3072, 5232};

    for (int si = 0; si < 4; si++) {
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

        printf("\n=== LBA %d: qscale=%d, type=%d ===\n", lba, qscale, f[39]);

        /* ===== Full decode with DC reset per MB row ===== */
        /* Try multiple dequant formulas */
        struct {
            const char *name;
            int dc_scale;      /* DC = dc_coeff * dc_scale */
            int ac_num, ac_den; /* AC = coeff * qt * qscale * ac_num / ac_den */
            int dc_base;       /* DC predictor reset value */
        } modes[] = {
            {"mpeg1_std",    8, 1, 16, 128},
            {"dc8_ac_qt",    8, 1, 1,  128},
            {"dc1_ac_qt",    1, 1, 1,  0},
            {"dc_qs_ac_qs",  0, 1, 1,  128},  /* dc_scale=qscale (set below) */
            {"dc4_ac_qt8",   4, 1, 8,  128},
            {"dc1_noac",     1, 0, 1,  0},     /* DC only, no AC */
        };
        modes[3].dc_scale = qscale;

        for (int mi = 0; mi < 6; mi++) {
            BR br; br_init(&br, bs, bslen);
            int *planeY = calloc(imgW * imgH, sizeof(int));
            int *planeCb = calloc(imgW/2 * imgH/2, sizeof(int));
            int *planeCr = calloc(imgW/2 * imgH/2, sizeof(int));

            for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
                /* Reset DC predictors at start of each MB row (slice) */
                int dc_y = modes[mi].dc_base;
                int dc_cb = modes[mi].dc_base;
                int dc_cr = modes[mi].dc_base;

                for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
                    double yspat[4][64];
                    for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                        int block[64] = {0};
                        dc_y += vlc_coeff(&br);
                        block[0] = dc_y * modes[mi].dc_scale;
                        if (modes[mi].ac_num > 0) {
                            for (int i = 1; i < 64 && !br_eof(&br); i++) {
                                if (br_get1(&br)) {
                                    int v = vlc_coeff(&br);
                                    block[zigzag8[i]] = v * qm[zigzag8[i]] * qscale *
                                                        modes[mi].ac_num / modes[mi].ac_den;
                                }
                            }
                        } else {
                            /* Skip AC */
                            for (int i = 1; i < 64 && !br_eof(&br); i++)
                                if (br_get1(&br)) vlc_coeff(&br);
                        }
                        idct8x8(block, yspat[yb]);
                    }
                    int bx0 = mbx*2, by0 = mby*2;
                    place_block8(planeY, imgW, bx0,   by0,   yspat[0]);
                    place_block8(planeY, imgW, bx0+1, by0,   yspat[1]);
                    place_block8(planeY, imgW, bx0,   by0+1, yspat[2]);
                    place_block8(planeY, imgW, bx0+1, by0+1, yspat[3]);

                    /* Cb */
                    {
                        int cblock[64] = {0};
                        dc_cb += vlc_coeff(&br);
                        cblock[0] = dc_cb * modes[mi].dc_scale;
                        if (modes[mi].ac_num > 0) {
                            for (int i = 1; i < 64 && !br_eof(&br); i++)
                                if (br_get1(&br))
                                    cblock[zigzag8[i]] = vlc_coeff(&br) * qm[zigzag8[i]] *
                                                         qscale * modes[mi].ac_num / modes[mi].ac_den;
                        } else {
                            for (int i = 1; i < 64 && !br_eof(&br); i++)
                                if (br_get1(&br)) vlc_coeff(&br);
                        }
                        double cspat[64];
                        idct8x8(cblock, cspat);
                        place_block8(planeCb, imgW/2, mbx, mby, cspat);
                    }
                    /* Cr */
                    {
                        int cblock[64] = {0};
                        dc_cr += vlc_coeff(&br);
                        cblock[0] = dc_cr * modes[mi].dc_scale;
                        if (modes[mi].ac_num > 0) {
                            for (int i = 1; i < 64 && !br_eof(&br); i++)
                                if (br_get1(&br))
                                    cblock[zigzag8[i]] = vlc_coeff(&br) * qm[zigzag8[i]] *
                                                         qscale * modes[mi].ac_num / modes[mi].ac_den;
                        } else {
                            for (int i = 1; i < 64 && !br_eof(&br); i++)
                                if (br_get1(&br)) vlc_coeff(&br);
                        }
                        double cspat[64];
                        idct8x8(cblock, cspat);
                        place_block8(planeCr, imgW/2, mbx, mby, cspat);
                    }
                }
            }
            printf("  %s: bits %d/%d (%.1f%%)\n",
                   modes[mi].name, br.total, bslen*8, 100.0*br.total/(bslen*8));

            /* Y output */
            uint8_t *yimg = malloc(imgW * imgH);
            for (int i = 0; i < imgW*imgH; i++)
                yimg[i] = clamp8(planeY[i] + (modes[mi].dc_base == 0 ? 128 : 0));
            char path[256];
            snprintf(path, sizeof(path), OUT_DIR "sl_%s_%s_lba%d.pgm",
                     modes[mi].name, tag, lba);
            write_pgm(path, yimg, imgW, imgH);

            /* RGB output for the best-looking modes */
            if (mi == 0 || mi == 1 || mi == 5) {
                uint8_t *rgb = malloc(imgW*imgH*3);
                int off = (modes[mi].dc_base == 0) ? 128 : 0;
                for (int y = 0; y < imgH; y++)
                    for (int x = 0; x < imgW; x++) {
                        int yv = planeY[y*imgW+x] + off;
                        int cb = planeCb[(y/2)*(imgW/2)+(x/2)];
                        int cr = planeCr[(y/2)*(imgW/2)+(x/2)];
                        rgb[(y*imgW+x)*3+0] = clamp8(yv + 1.402*cr);
                        rgb[(y*imgW+x)*3+1] = clamp8(yv - 0.344*cb - 0.714*cr);
                        rgb[(y*imgW+x)*3+2] = clamp8(yv + 1.772*cb);
                    }
                snprintf(path, sizeof(path), OUT_DIR "sl_%s_rgb_%s_lba%d.ppm",
                         modes[mi].name, tag, lba);
                write_ppm(path, rgb, imgW, imgH);
                free(rgb);
            }

            free(yimg); free(planeY); free(planeCb); free(planeCr);
        }

        /* ===== DC-only without per-AC flags ===== */
        /* If per-AC flag model has errors, DC-only with different block counts */
        /* Try: read ONLY DC value, skip fixed number of bits, see if DC becomes clean */
        printf("\n  --- Fixed-skip DC test ---\n");
        for (int skip = 60; skip <= 250; skip += 10) {
            BR br; br_init(&br, bs, bslen);
            int dc = 128;
            uint8_t dc_img[288];
            memset(dc_img, 128, 288);
            int ni = 0;
            bool ok = true;

            for (int mby = 0; mby < 9 && ok; mby++) {
                dc = 128; /* Reset per row */
                for (int mbx = 0; mbx < 8 && ok; mbx++) {
                    for (int yb = 0; yb < 4; yb++) {
                        if (br_eof(&br)) { ok = false; break; }
                        dc += vlc_coeff(&br);
                        int bx = mbx*2 + (yb&1);
                        int by = mby*2 + (yb>>1);
                        dc_img[by*16+bx] = clamp8(dc);
                        ni++;
                        /* Skip fixed bits for AC */
                        for (int i = 0; i < skip && !br_eof(&br); i++)
                            br_get1(&br);
                    }
                    /* Skip Cb, Cr: DC + fixed AC */
                    for (int c = 0; c < 2; c++) {
                        if (!br_eof(&br)) vlc_coeff(&br);
                        for (int i = 0; i < skip && !br_eof(&br); i++)
                            br_get1(&br);
                    }
                }
            }
            /* Only output if decode completed */
            if (ok && ni >= 288) {
                /* Check if DC values look reasonable (not all clipped) */
                int nclip = 0;
                for (int i = 0; i < 288; i++)
                    if (dc_img[i] == 0 || dc_img[i] == 255) nclip++;
                if (nclip < 200) {
                    printf("  skip=%3d: bits=%5d/%5d (%.0f%%), clipped=%d\n",
                           skip, br.total, bslen*8, 100.0*br.total/(bslen*8), nclip);
                }
            }
        }
    }

    free(disc); zip_close(z);
    return 0;
}
