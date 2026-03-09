/*
 * Playdia video - MPEG-1 style 8×8 DCT intra decode
 * Hypothesis: standard MPEG-1 DC/AC VLC tables, 8×8 blocks
 * Header: 00 80 = width(128), 04 = coding, byte3 = quantizer_scale
 * 16 bytes might be custom intra quantization matrix (compressed)
 * Bitstream at offset 40 is raw VLC-coded DCT coefficients
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <zip.h>

#define SECTOR_RAW 2352
#define MAX_FRAME  65536
#define PI 3.14159265358979323846

static void write_pgm(const char *p, const uint8_t *g, int w, int h) {
    FILE *f=fopen(p,"wb"); if(!f)return;
    fprintf(f,"P5\n%d %d\n255\n",w,h); fwrite(g,1,w*h,f); fclose(f);
    printf("  -> %s (%dx%d)\n",p,w,h);
}

/* Bitstream reader */
typedef struct {
    const uint8_t *data;
    int len, pos, bit;
    int total_bits;
} BR;

static void br_init(BR *b, const uint8_t *d, int l) {
    b->data=d; b->len=l; b->pos=0; b->bit=7; b->total_bits=0;
}
static int br_eof(BR *b) { return b->pos >= b->len; }
static int br_get1(BR *b) {
    if(b->pos>=b->len) return 0;
    int v=(b->data[b->pos]>>b->bit)&1;
    if(--b->bit<0){b->bit=7;b->pos++;}
    b->total_bits++;
    return v;
}
static int br_peek(BR *b, int n) {
    BR save = *b;
    int v = 0;
    for(int i=0;i<n;i++) v=(v<<1)|br_get1(&save);
    return v;
}
static int br_get(BR *b, int n) {
    int v=0;
    for(int i=0;i<n;i++) v=(v<<1)|br_get1(b);
    return v;
}

/* MPEG-1 DC luminance VLC (Table B.12 of ISO 11172-2) */
static int mpeg1_dc_lum_decode(BR *b) {
    // size 0: 100
    // size 1: 00
    // size 2: 01
    // size 3: 101
    // size 4: 110
    // size 5: 1110
    // size 6: 11110
    // size 7: 111110
    // size 8: 1111110
    int bit = br_get1(b);
    int size;
    if (bit == 0) {
        bit = br_get1(b);
        if (bit == 0) size = 1;
        else size = 2;
    } else {
        bit = br_get1(b);
        if (bit == 0) {
            bit = br_get1(b);
            if (bit == 0) size = 0;
            else size = 3;
        } else {
            bit = br_get1(b);
            if (bit == 0) size = 4;
            else {
                bit = br_get1(b);
                if (bit == 0) size = 5;
                else {
                    bit = br_get1(b);
                    if (bit == 0) size = 6;
                    else {
                        bit = br_get1(b);
                        if (bit == 0) size = 7;
                        else size = 8;
                    }
                }
            }
        }
    }
    if (size == 0) return 0;
    int val = br_get(b, size);
    // MPEG-1 DC difference decoding
    if (val < (1 << (size-1)))
        val = val - (1 << size) + 1;
    return val;
}

/* MPEG-1 DC chrominance VLC (Table B.13) */
static int mpeg1_dc_chr_decode(BR *b) {
    // size 0: 00
    // size 1: 01
    // size 2: 10
    // size 3: 110
    // size 4: 1110
    // size 5: 11110
    // size 6: 111110
    // size 7: 1111110
    // size 8: 11111110
    int bit = br_get1(b);
    int size;
    if (bit == 0) {
        bit = br_get1(b);
        if (bit == 0) size = 0;
        else size = 1;
    } else {
        bit = br_get1(b);
        if (bit == 0) size = 2;
        else {
            bit = br_get1(b);
            if (bit == 0) size = 3;
            else {
                bit = br_get1(b);
                if (bit == 0) size = 4;
                else {
                    bit = br_get1(b);
                    if (bit == 0) size = 5;
                    else {
                        bit = br_get1(b);
                        if (bit == 0) size = 6;
                        else {
                            bit = br_get1(b);
                            if (bit == 0) size = 7;
                            else size = 8;
                        }
                    }
                }
            }
        }
    }
    if (size == 0) return 0;
    int val = br_get(b, size);
    if (val < (1 << (size-1)))
        val = val - (1 << size) + 1;
    return val;
}

/* MPEG-1 AC coefficient VLC (Table B.14 - subset) */
/* Returns: run in upper 16 bits, level in lower 16 bits, or -1 for EOB, -2 for escape */
typedef struct { int bits; int code; int run; int level; } ACEntry;

/* Table B.14 entries (most common ones) */
static const ACEntry ac_table[] = {
    /* EOB */
    {2, 0x2, -1, 0},       // 10 → EOB
    /* run=0 entries */
    {3, 0x6, 0, 1},        // 110 → (0,1) [note: sign bit follows for some interpretations]
    /* Actually in MPEG-1, Table B.14 codes include the sign */
    /* Let me use the proper table format: code → (run, level), then sign bit */
    {2, 0x3, 0, 1},        // 1s → (0,1)  where s is sign
    {4, 0x6, 1, 1},        // 011s → (1,1)
    {5, 0xC, 0, 2},        // 0100s → (0,2)
    {5, 0xE, 2, 1},        // 0101s → (2,1)
    {6, 0x15, 0, 3},       // 00101s → (0,3)
    {6, 0x11, 3, 1},       // 00100 0s → hmm
    /* This is getting complex. Let me use the escape code approach */
};

/* Simplified MPEG-1 AC decode: try to match Table B.14 */
static int mpeg1_ac_decode(BR *b, int *run_out, int *level_out) {
    /* Check for EOB: "10" */
    int peek2 = br_peek(b, 2);
    if (peek2 == 2) { // 10
        br_get(b, 2);
        return -1; // EOB
    }

    /* Check for escape: "000001" */
    int peek6 = br_peek(b, 6);
    if (peek6 == 1) { // 000001
        br_get(b, 6);
        *run_out = br_get(b, 6);
        // Level: 8-bit signed for MPEG-1 or 12-bit
        int level = br_get(b, 8);
        if (level == 0) {
            level = br_get(b, 8);
        } else if (level == 128) {
            level = br_get(b, 8) - 256;
        } else if (level > 128) {
            level = level - 256;
        }
        *level_out = level;
        return 0;
    }

    /* Table B.14 VLC decoding */
    /* Format: variable length code + 1 sign bit */
    int bit;

    /* "1" prefix → short codes */
    bit = br_get1(b);
    if (bit == 1) {
        int s = br_get1(b);
        *run_out = 0;
        *level_out = s ? -1 : 1;
        return 0;
    }

    /* "01" → (1,1) */
    bit = br_get1(b);
    if (bit == 1) {
        int s = br_get1(b);
        *run_out = 1;
        *level_out = s ? -1 : 1;
        return 0;
    }

    /* "001" prefix */
    bit = br_get1(b);
    if (bit == 1) {
        bit = br_get1(b);
        if (bit == 0) {
            int s = br_get1(b);
            *run_out = 0;
            *level_out = s ? -2 : 2;
            return 0;
        } else {
            int s = br_get1(b);
            *run_out = 2;
            *level_out = s ? -1 : 1;
            return 0;
        }
    }

    /* "0001" prefix */
    bit = br_get1(b);
    if (bit == 1) {
        bit = br_get1(b);
        if (bit == 0) {
            int s = br_get1(b);
            *run_out = 0;
            *level_out = s ? -3 : 3;
            return 0;
        } else {
            int s = br_get1(b);
            *run_out = 3;
            *level_out = s ? -1 : 1;
            return 0;
        }
    }

    /* "00001" prefix */
    bit = br_get1(b);
    if (bit == 1) {
        bit = br_get1(b);
        if (bit == 0) {
            bit = br_get1(b);
            if (bit == 0) {
                int s = br_get1(b);
                *run_out = 4;
                *level_out = s ? -1 : 1;
                return 0;
            } else {
                int s = br_get1(b);
                *run_out = 1;
                *level_out = s ? -2 : 2;
                return 0;
            }
        } else {
            bit = br_get1(b);
            if (bit == 0) {
                int s = br_get1(b);
                *run_out = 5;
                *level_out = s ? -1 : 1;
                return 0;
            } else {
                int s = br_get1(b);
                *run_out = 6;
                *level_out = s ? -1 : 1;
                return 0;
            }
        }
    }

    /* Longer codes - use escape as fallback */
    /* Skip remaining bits and treat as escape */
    *run_out = 0;
    *level_out = 0;
    return -2; // error
}

/* 8×8 zigzag order */
static const int zigzag8[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* Default MPEG-1 intra quantization matrix */
static const int default_intra_qm[64] = {
     8, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83
};

/* Simple 8×8 IDCT */
static void idct8x8(const int coeff[64], int out[64]) {
    double tmp[64];
    // Row IDCT
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            double s = 0;
            for (int u = 0; u < 8; u++) {
                double cu = (u == 0) ? 1.0/sqrt(2.0) : 1.0;
                s += cu * coeff[y*8+u] * cos((2*x+1)*u*PI/16.0);
            }
            tmp[y*8+x] = s;
        }
    }
    // Column IDCT
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            double s = 0;
            for (int v = 0; v < 8; v++) {
                double cv = (v == 0) ? 1.0/sqrt(2.0) : 1.0;
                s += cv * tmp[v*8+x] * cos((2*y+1)*v*PI/16.0);
            }
            out[y*8+x] = (int)round(s / 4.0);
        }
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

int main(int argc, char **argv) {
    if(argc<2){fprintf(stderr,"Usage: %s <zip> [lba]\n",argv[0]);return 1;}
    int slba=argc>2?atoi(argv[2]):502;

    int err;zip_t *z=zip_open(argv[1],ZIP_RDONLY,&err); if(!z)return 1;
    int bi=-1;zip_uint64_t bs2=0;
    for(int i=0;i<(int)zip_get_num_entries(z,0);i++){
        zip_stat_t st;if(zip_stat_index(z,i,0,&st)==0&&st.size>bs2){bs2=st.size;bi=i;}}
    zip_stat_t st;zip_stat_index(z,bi,0,&st);
    zip_file_t *zf=zip_fopen_index(z,bi,0);
    uint8_t *disc=malloc(st.size);
    zip_int64_t rd=0;
    while(rd<(zip_int64_t)st.size){zip_int64_t r=zip_fread(zf,disc+rd,st.size-rd);if(r<=0)break;rd+=r;}
    zip_fclose(zf);
    int tsec=(int)(st.size/SECTOR_RAW);

    static uint8_t frames[8][MAX_FRAME]; int fsizes[8];
    int nf=assemble_frames(disc,tsec,slba,frames,fsizes,8);
    printf("Assembled %d frames\n",nf);

    /* Try frames 0 and 1 */
    for (int fi = 0; fi < 2 && fi < nf; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        int qscale = f[3];

        printf("\n=== Frame %d: %d bytes, qscale=%d, type=%d ===\n",
               fi, fsize, qscale, f[39]);

        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        /* Try different resolutions and block configurations */
        struct { int W, H; const char *name; } configs[] = {
            {128, 144, "128x144"},
            {128, 96,  "128x96"},
            {256, 192, "256x192"},
            {160, 120, "160x120"},
        };

        for (int ci = 0; ci < 4; ci++) {
            int W = configs[ci].W;
            int H = configs[ci].H;
            int bw = W / 8;
            int bh = H / 8;

            printf("\n--- %s: %d×%d blocks ---\n", configs[ci].name, bw, bh);

            /* MPEG-1 intra decode with DC lum VLC */
            BR br;
            br_init(&br, bs, bslen);

            uint8_t *img = calloc(W * H, 1);
            int prev_dc = 0; // DC predictor (128*8 for MPEG-1, but we'll adjust)
            int blocks_ok = 0, blocks_fail = 0;

            for (int by = 0; by < bh && !br_eof(&br); by++) {
                for (int bx = 0; bx < bw && !br_eof(&br); bx++) {
                    int coeff[64] = {0};
                    int saved = br.total_bits;

                    /* DC coefficient */
                    int dc_diff = mpeg1_dc_lum_decode(&br);
                    prev_dc += dc_diff;
                    coeff[0] = prev_dc * qscale; // dequantize DC

                    /* AC coefficients */
                    int k = 1;
                    int ac_fail = 0;
                    while (k < 64 && !br_eof(&br)) {
                        int run, level;
                        int ret = mpeg1_ac_decode(&br, &run, &level);
                        if (ret == -1) break; // EOB
                        if (ret == -2) { ac_fail = 1; break; } // error
                        k += run;
                        if (k >= 64) { ac_fail = 1; break; }
                        // Dequantize: coeff = level * qscale * quant_matrix[k] / 8
                        int qm = default_intra_qm[zigzag8[k]];
                        coeff[zigzag8[k]] = level * qscale * qm / 8;
                        k++;
                    }

                    if (ac_fail) {
                        blocks_fail++;
                        if (blocks_fail > 50) goto done;
                        continue;
                    }

                    /* IDCT */
                    int pixels[64];
                    idct8x8(coeff, pixels);

                    /* Write to image */
                    for (int dy = 0; dy < 8; dy++) {
                        for (int dx = 0; dx < 8; dx++) {
                            int px = bx*8+dx, py = by*8+dy;
                            if (px < W && py < H) {
                                int v = pixels[dy*8+dx] + 128;
                                if (v < 0) v = 0;
                                if (v > 255) v = 255;
                                img[py*W+px] = v;
                            }
                        }
                    }
                    blocks_ok++;
                }
            }
            done:

            printf("  Decoded %d blocks OK, %d failures, used %d bits (%.1f bits/block)\n",
                   blocks_ok, blocks_fail, br.total_bits,
                   blocks_ok ? (double)br.total_bits/blocks_ok : 0);

            if (blocks_ok > 10) {
                char path[256];
                snprintf(path,sizeof(path),"/home/wizzard/share/GitHub/playdia-emu/tools/test_output/pd_m1_8x8_%s_f%d.pgm",
                         configs[ci].name, fi);
                write_pgm(path, img, W, H);
            }
            free(img);
        }

        /* Also try: MPEG-1 DC-only (no AC) to get a low-res thumbnail */
        printf("\n--- DC-only decode ---\n");
        {
            BR br;
            br_init(&br, bs, bslen);
            int prev_dc = 0;
            int dc_vals[4096];
            int ndc = 0;

            while (ndc < 4096 && !br_eof(&br)) {
                int dc_diff = mpeg1_dc_lum_decode(&br);
                prev_dc += dc_diff;
                dc_vals[ndc++] = prev_dc;
            }

            printf("  Decoded %d DC values using %d bits (%.1f bits/val)\n",
                   ndc, br.total_bits, (double)br.total_bits/ndc);

            // Print first 32
            printf("  First 32 DC: ");
            for (int i = 0; i < 32 && i < ndc; i++) printf("%d ", dc_vals[i]);
            printf("\n");

            // Render as 16-wide image (for 128-pixel wide frame)
            int dw = 16, dh = ndc / dw;
            if (dh > 0) {
                uint8_t *dcimg = calloc(dw*dh, 1);
                for (int i = 0; i < dw*dh; i++) {
                    int v = dc_vals[i] * 8 + 128; // MPEG-1 DC scaling
                    if (v < 0) v = 0; if (v > 255) v = 255;
                    dcimg[i] = v;
                }
                // Scale up
                int sw = dw*8, sh = dh*8;
                uint8_t *scaled = calloc(sw*sh, 1);
                for (int y = 0; y < sh; y++)
                    for (int x = 0; x < sw; x++)
                        scaled[y*sw+x] = dcimg[(y/8)*dw + (x/8)];
                char path[256];
                snprintf(path,sizeof(path),"/home/wizzard/share/GitHub/playdia-emu/tools/test_output/pd_m1dc_%dx%d_f%d.pgm",sw,sh,fi);
                write_pgm(path, scaled, sw, sh);
                free(scaled);
                free(dcimg);
            }
        }

        /* Try: just DC with chrominance tables alternating */
        printf("\n--- DC lum+chr interleaved (4:2:0 MB) ---\n");
        {
            BR br;
            br_init(&br, bs, bslen);
            int prev_dc_y = 0, prev_dc_cb = 0, prev_dc_cr = 0;
            int mb_w = 128/16, mb_h = 144/16; // 8×9 macroblocks
            // But 144/16 = 9, which is fine

            int dc_y[1024], dc_cb[256], dc_cr[256];
            int n_mb = 0;

            for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
                for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
                    // 4 Y blocks
                    for (int yb = 0; yb < 4; yb++) {
                        int dc_diff = mpeg1_dc_lum_decode(&br);
                        prev_dc_y += dc_diff;
                        dc_y[n_mb*4+yb] = prev_dc_y;
                    }
                    // 1 Cb block
                    {
                        int dc_diff = mpeg1_dc_chr_decode(&br);
                        prev_dc_cb += dc_diff;
                        dc_cb[n_mb] = prev_dc_cb;
                    }
                    // 1 Cr block
                    {
                        int dc_diff = mpeg1_dc_chr_decode(&br);
                        prev_dc_cr += dc_diff;
                        dc_cr[n_mb] = prev_dc_cr;
                    }
                    n_mb++;
                }
            }

            printf("  Decoded %d macroblocks (DC only), %d bits (%.1f bits/MB)\n",
                   n_mb, br.total_bits, (double)br.total_bits/n_mb);
            printf("  First 8 Y DC: ");
            for (int i = 0; i < 32 && i < n_mb*4; i++) printf("%d ", dc_y[i]);
            printf("\n");
            printf("  First 8 Cb DC: ");
            for (int i = 0; i < 8 && i < n_mb; i++) printf("%d ", dc_cb[i]);
            printf("\n");
            printf("  First 8 Cr DC: ");
            for (int i = 0; i < 8 && i < n_mb; i++) printf("%d ", dc_cr[i]);
            printf("\n");
        }

        /* Also try: skip first 2 bits (might be coding type or other header info) */
        printf("\n--- With 2-bit skip ---\n");
        for (int skip = 1; skip <= 8; skip++) {
            BR br;
            br_init(&br, bs, bslen);
            br_get(&br, skip); // skip bits

            int prev_dc = 0;
            int dc_vals[256];
            int ndc = 0;
            int bad = 0;

            while (ndc < 256 && !br_eof(&br)) {
                int dc_diff = mpeg1_dc_lum_decode(&br);
                prev_dc += dc_diff;
                dc_vals[ndc++] = prev_dc;
                if (abs(prev_dc) > 500) { bad = 1; break; }
            }

            printf("  skip=%d: %d DC vals, last=%d %s | first5: ",
                   skip, ndc, ndc>0?dc_vals[ndc-1]:0, bad?"(OVERFLOW)":"(ok)");
            for (int i = 0; i < 5 && i < ndc; i++) printf("%d ", dc_vals[i]);
            printf("\n");
        }
    }

    free(disc); zip_close(z);
    return 0;
}
