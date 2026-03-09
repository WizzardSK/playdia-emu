/*
 * Playdia video - Full MPEG-1 decode with complete VLC tables
 * Complete Table B.14 (AC coefficients) + B.12/B.13 (DC)
 * Try both 8×8 and 4×4 block sizes
 * Try both MSB-first and LSB-first bit reading
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
#define OUT_DIR "/home/wizzard/share/GitHub/playdia-emu/tools/test_output/"

static void write_pgm(const char *p, const uint8_t *g, int w, int h) {
    FILE *f=fopen(p,"wb"); if(!f)return;
    fprintf(f,"P5\n%d %d\n255\n",w,h); fwrite(g,1,w*h,f); fclose(f);
    printf("  -> %s (%dx%d)\n",p,w,h);
}

/* ============ Bitstream reader (MSB first) ============ */
typedef struct { const uint8_t *data; int len,pos,bit,total; } BR;
static void br_init(BR *b, const uint8_t *d, int l) {
    b->data=d; b->len=l; b->pos=0; b->bit=7; b->total=0;
}
static int br_eof(BR *b) { return b->pos>=b->len; }
static int br_get1(BR *b) {
    if(b->pos>=b->len) return 0;
    int v=(b->data[b->pos]>>b->bit)&1;
    if(--b->bit<0){b->bit=7;b->pos++;}
    b->total++; return v;
}
static int br_get(BR *b, int n) {
    int v=0; for(int i=0;i<n;i++) v=(v<<1)|br_get1(b); return v;
}

/* ============ Bitstream reader (LSB first) ============ */
typedef struct { const uint8_t *data; int len,pos,bit,total; } BRL;
static void brl_init(BRL *b, const uint8_t *d, int l) {
    b->data=d; b->len=l; b->pos=0; b->bit=0; b->total=0;
}
static int brl_eof(BRL *b) { return b->pos>=b->len; }
static int brl_get1(BRL *b) {
    if(b->pos>=b->len) return 0;
    int v=(b->data[b->pos]>>b->bit)&1;
    if(++b->bit>7){b->bit=0;b->pos++;}
    b->total++; return v;
}
static int brl_get(BRL *b, int n) {
    int v=0; for(int i=0;i<n;i++) v|=(brl_get1(b)<<i); return v;
}

/* ============ MPEG-1 DC VLC ============ */
static int mpeg1_dc_lum(BR *b) {
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

static int mpeg1_dc_chr(BR *b) {
    int size;
    if (br_get1(b) == 0) { size = br_get1(b) ? 1 : 0; }
    else {
        if (br_get1(b) == 0) size = 2;
        else if (br_get1(b) == 0) size = 3;
        else if (br_get1(b) == 0) size = 4;
        else if (br_get1(b) == 0) size = 5;
        else if (br_get1(b) == 0) size = 6;
        else size = br_get1(b) ? 8 : 7;
    }
    if (size == 0) return 0;
    int val = br_get(b, size);
    if (val < (1 << (size-1))) val = val - (1 << size) + 1;
    return val;
}

/* ============ Complete MPEG-1 AC VLC (Table B.14) ============ */
/*
 * Each entry: run, level. Sign bit follows the VLC code.
 * Special: EOB = end of block, ESC = escape (6-bit run + 8/16-bit level follows)
 *
 * Table B.14 from ISO 11172-2:
 * Code          | Run | Level
 * 10            | EOB |
 * 1s            | 0   | 1     (first coeff of non-intra block)
 * 11s           | 0   | 1     (all other positions)
 * 011s          | 1   | 1
 * 0100s         | 0   | 2
 * 0101s         | 2   | 1
 * 00101s        | 0   | 3
 * 00110s        | 4   | 1
 * 00111s        | 3   | 1
 * 000100s       | 7   | 1
 * 000101s       | 6   | 1
 * 000110s       | 1   | 2
 * 000111s       | 5   | 1
 * 0000100s      | 2   | 2
 * 0000101s      | 9   | 1
 * 0000110s      | 0   | 4
 * 0000111s      | 8   | 1
 * 00100000s     | 13  | 1
 * 00100001s     | 0   | 6
 * 00100010s     | 12  | 1
 * 00100011s     | 11  | 1
 * 00100100s     | 3   | 2
 * 00100101s     | 1   | 3
 * 00100110s     | 0   | 5
 * 00100111s     | 10  | 1
 * 0000001000s   | 0   | 9
 * 0000001001s   | 27  | 1
 * 0000001010s   | 2   | 3
 * 0000001011s   | 0   | 8
 * 0000001100s   | 4   | 2
 * 0000001101s   | 26  | 1
 * 0000001110s   | 0   | 7
 * 0000001111s   | 25  | 1
 * 000000010000s | 16  | 1
 * 000000010001s | 5   | 2
 * 000000010010s | 0   | 11
 * 000000010011s | 2   | 4
 * 000000010100s | 1   | 5
 * 000000010101s | 24  | 1
 * 000000010110s | 0   | 10
 * 000000010111s | 23  | 1
 * 000000011000s | 22  | 1
 * 000000011001s | 21  | 1
 * 000000011010s | 0   | 12
 * 000000011011s | 20  | 1
 * 000000011100s | 19  | 1
 * 000000011101s | 18  | 1
 * 000000011110s | 1   | 4
 * 000000011111s | 17  | 1
 * 0000000010000s| 14  | 1
 * ... (more entries up to 16-bit codes)
 * 000001         | ESC (escape)
 */

/* Return: 1=got run/level, 0=EOB, -1=error */
static int mpeg1_ac_decode(BR *b, int *run, int *level) {
    int code = 0;
    int bits = 0;

    /* Read bits one at a time and match against table */
    code = br_get1(b); bits = 1;

    if (code == 1) {
        /* Could be EOB (10) or run=0,level=1 (11s) */
        int next = br_get1(b); bits = 2;
        if (next == 0) return 0; /* EOB */
        /* 11s */
        int s = br_get1(b);
        *run = 0; *level = s ? -1 : 1;
        return 1;
    }

    /* code starts with 0 */
    code = (code << 1) | br_get1(b); bits = 2;
    if (code == 1) { /* 01 */
        code = (code << 1) | br_get1(b); bits = 3;
        if (code == 3) { /* 011s */
            int s = br_get1(b);
            *run = 1; *level = s ? -1 : 1;
            return 1;
        }
        /* 010... */
        code = (code << 1) | br_get1(b); bits = 4;
        if (code == 4) { /* 0100s */
            int s = br_get1(b);
            *run = 0; *level = s ? -2 : 2;
            return 1;
        }
        if (code == 5) { /* 0101s */
            int s = br_get1(b);
            *run = 2; *level = s ? -1 : 1;
            return 1;
        }
    }

    if (bits == 2 && code == 0) {
        /* 00... */
        code = (code << 1) | br_get1(b); bits = 3;
        if (code == 1) { /* 001... */
            code = (code << 1) | br_get1(b); bits = 4;
            code = (code << 1) | br_get1(b); bits = 5;
            if ((code >> 0) == 5) { /* 00101s */
                int s = br_get1(b);
                *run = 0; *level = s ? -3 : 3;
                return 1;
            }
            if ((code >> 0) == 6) { /* 00110s */
                int s = br_get1(b);
                *run = 4; *level = s ? -1 : 1;
                return 1;
            }
            if ((code >> 0) == 7) { /* 00111s */
                int s = br_get1(b);
                *run = 3; *level = s ? -1 : 1;
                return 1;
            }
            if ((code >> 0) == 4) { /* 00100... 8-bit codes */
                code = (code << 1) | br_get1(b); bits = 6;
                code = (code << 1) | br_get1(b); bits = 7;
                code = (code << 1) | br_get1(b); bits = 8;
                int low3 = code & 7;
                int s = br_get1(b);
                static const int r8[] = {13,0,12,11,3,1,0,10};
                static const int l8[] = { 1,6, 1, 1,2,3,5, 1};
                *run = r8[low3]; *level = s ? -l8[low3] : l8[low3];
                return 1;
            }
        }

        if (code == 0) { /* 000... */
            code = (code << 1) | br_get1(b); bits = 4;
            if (code == 1) { /* 0001... */
                code = (code << 1) | br_get1(b); bits = 5;
                code = (code << 1) | br_get1(b); bits = 6;
                int low2 = code & 3;
                int s = br_get1(b);
                static const int r6[] = {7,6,1,5};
                static const int l6[] = {1,1,2,1};
                *run = r6[low2]; *level = s ? -l6[low2] : l6[low2];
                return 1;
            }
            if (code == 0) { /* 0000... */
                code = (code << 1) | br_get1(b); bits = 5;
                if (code == 1) { /* 00001... */
                    /* 000010 = escape, 000011xx = more VLC */
                    code = (code << 1) | br_get1(b); bits = 6;
                    if (code == 2) { /* 000010 = ESCAPE */
                        *run = br_get(b, 6);
                        int lev = br_get(b, 8);
                        if (lev == 0) {
                            lev = br_get(b, 8);
                        } else if (lev == 128) {
                            lev = br_get(b, 8) - 256;
                        } else if (lev > 128) {
                            lev = lev - 256;
                        }
                        *level = lev;
                        return 1;
                    }
                    if (code == 3) { /* 000011... */
                        code = (code << 1) | br_get1(b); bits = 7;
                        int low1 = code & 1;
                        int s = br_get1(b);
                        if (low1 == 0) { *run = 0; *level = s ? -4 : 4; }
                        else { *run = 8; *level = s ? -1 : 1; }
                        return 1;
                    }
                }
                if (code == 0) { /* 00000... */
                    code = (code << 1) | br_get1(b); bits = 6;
                    if (code == 1) { /* 000001 = escape already handled above */
                        /* Actually this path shouldn't happen - escape is 000001 */
                        *run = br_get(b, 6);
                        int lev = br_get(b, 8);
                        if (lev == 0) lev = br_get(b, 8);
                        else if (lev == 128) lev = br_get(b, 8) - 256;
                        else if (lev > 128) lev -= 256;
                        *level = lev;
                        return 1;
                    }
                    if (code == 0) { /* 000000... */
                        code = (code << 1) | br_get1(b); bits = 7;
                        if (code == 1) { /* 0000001... 10-bit codes */
                            code = (code << 1) | br_get1(b); bits = 8;
                            code = (code << 1) | br_get1(b); bits = 9;
                            code = (code << 1) | br_get1(b); bits = 10;
                            int low3 = code & 7;
                            int s = br_get1(b);
                            static const int r10[] = {0,27,2,0,4,26,0,25};
                            static const int l10[] = {9, 1,3,8,2, 1,7, 1};
                            *run = r10[low3]; *level = s ? -l10[low3] : l10[low3];
                            return 1;
                        }
                        if (code == 0) { /* 0000000... */
                            code = (code << 1) | br_get1(b); bits = 8;
                            if (code == 1) { /* 00000001... 12-bit codes */
                                code = (code << 1) | br_get1(b); bits = 9;
                                code = (code << 1) | br_get1(b); bits = 10;
                                code = (code << 1) | br_get1(b); bits = 11;
                                code = (code << 1) | br_get1(b); bits = 12;
                                int low4 = code & 15;
                                int s = br_get1(b);
                                static const int r12[] = {16,5,0,2,1,24,0,23,22,21,0,20,19,18,1,17};
                                static const int l12[] = { 1,2,11,4,5, 1,10, 1, 1, 1,12, 1, 1, 1,4, 1};
                                *run = r12[low4]; *level = s ? -l12[low4] : l12[low4];
                                return 1;
                            }
                            if (code == 0) { /* 00000000... 13+ bit codes */
                                code = (code << 1) | br_get1(b); bits = 9;
                                if (code == 1) { /* 000000001... 13-bit codes */
                                    code = (code << 1) | br_get1(b); bits = 10;
                                    code = (code << 1) | br_get1(b); bits = 11;
                                    code = (code << 1) | br_get1(b); bits = 12;
                                    code = (code << 1) | br_get1(b); bits = 13;
                                    int low4 = code & 15;
                                    int s = br_get1(b);
                                    static const int r13[] = {14,1,0,2,0,15,3,0,6,0,1,5,0,0,1,0};
                                    static const int l13[] = { 1,7,13,5,14, 1,3,16,2,15,6,2,0,0,0,0};
                                    if (l13[low4] == 0) return -1; /* invalid */
                                    *run = r13[low4]; *level = s ? -l13[low4] : l13[low4];
                                    return 1;
                                }
                                /* More leading zeros = longer codes or invalid */
                                return -1;
                            }
                        }
                    }
                }
            }
        }
    }

    return -1; /* unrecognized code */
}

/* ============ Zigzag tables ============ */
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

static const int zigzag4[16] = {
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};

/* ============ IDCT ============ */
static void idct8x8(int block[64], int out[64]) {
    double tmp[64];
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            double sum = 0;
            for (int k = 0; k < 8; k++) {
                double ck = (k == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += ck * block[i*8+k] * cos((2*j+1)*k*PI/16.0);
            }
            tmp[i*8+j] = sum / 2.0;
        }
    }
    for (int j = 0; j < 8; j++) {
        for (int i = 0; i < 8; i++) {
            double sum = 0;
            for (int k = 0; k < 8; k++) {
                double ck = (k == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += ck * tmp[k*8+j] * cos((2*i+1)*k*PI/16.0);
            }
            out[i*8+j] = (int)round(sum / 2.0);
        }
    }
}

static void idct4x4(int block[16], int out[16]) {
    double tmp[16];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            double sum = 0;
            for (int k = 0; k < 4; k++) {
                double ck = (k == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += ck * block[i*4+k] * cos((2*j+1)*k*PI/8.0);
            }
            tmp[i*4+j] = sum / 2.0;
        }
    }
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            double sum = 0;
            for (int k = 0; k < 4; k++) {
                double ck = (k == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += ck * tmp[k*4+j] * cos((2*i+1)*k*PI/8.0);
            }
            out[i*4+j] = (int)round(sum / 2.0);
        }
    }
}

/* MPEG-1 default intra quantization matrix */
static const int mpeg1_intra_qm[64] = {
     8,16,19,22,26,27,29,34,
    16,16,22,24,27,29,34,37,
    19,22,26,27,29,34,34,38,
    22,22,26,27,29,34,37,40,
    22,26,27,29,32,35,40,48,
    26,27,29,32,35,40,48,58,
    26,27,29,34,38,46,56,69,
    27,29,35,38,46,56,69,83
};

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

/*
 * Decode a full frame using MPEG-1 intra with 8×8 blocks
 * Full AC VLC table, proper dequantization
 */
static void decode_mpeg1_8x8(const uint8_t *bs, int bslen, int qscale,
                              const uint8_t qt[16], const char *tag,
                              int imgW, int imgH) {
    int bw = imgW / 8, bh = imgH / 8;
    uint8_t *img = calloc(imgW * imgH, 1);
    BR br; br_init(&br, bs, bslen);

    int dc_pred = 128; /* MPEG-1 initial DC pred = 128 for 8-bit */
    int ok = 0, fail = 0;

    /* Build 8x8 quantization matrix from 4x4 qtable */
    int qm[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            qm[i*8+j] = qt[(i/2)*4 + (j/2)];

    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            if (br_eof(&br)) goto done;

            int block[64] = {0};
            int spatial[64];

            /* DC coefficient */
            int dc_diff = mpeg1_dc_lum(&br);
            dc_pred += dc_diff;
            /* In MPEG-1: DC = dc_pred * 8 (shift to match AC scale) */
            block[0] = dc_pred * 8;

            /* AC coefficients */
            int idx = 1;
            bool block_ok = true;
            while (idx < 64 && !br_eof(&br)) {
                int run, level;
                int ret = mpeg1_ac_decode(&br, &run, &level);
                if (ret == 0) break; /* EOB */
                if (ret < 0) { block_ok = false; break; }
                idx += run;
                if (idx >= 64) { block_ok = false; break; }
                /* Dequantize: coeff = (2*level*qscale*qm[pos]) / 16 */
                int pos = zigzag8[idx];
                int dq = (2 * level * qscale * qm[pos]) / 16;
                /* Odd/even correction */
                if (dq > 0 && (dq & 1) == 0) dq--;
                if (dq < 0 && (-dq & 1) == 0) dq++;
                block[pos] = dq;
                idx++;
            }

            if (block_ok) ok++; else fail++;

            idct8x8(block, spatial);
            for (int dy = 0; dy < 8; dy++) {
                for (int dx = 0; dx < 8; dx++) {
                    int v = spatial[dy*8+dx];
                    if (v < 0) v = 0; if (v > 255) v = 255;
                    img[(by*8+dy)*imgW + bx*8+dx] = v;
                }
            }
        }
    }
done:
    printf("MPEG1-8x8 %s %dx%d: ok=%d fail=%d, %d/%d bits\n",
           tag, imgW, imgH, ok, fail, br.total, bslen*8);
    char path[256];
    snprintf(path, sizeof(path), OUT_DIR "m1full_%s_%dx%d.pgm", tag, imgW, imgH);
    write_pgm(path, img, imgW, imgH);
    free(img);
}

/*
 * Same but with 4×4 blocks
 */
static void decode_mpeg1_4x4(const uint8_t *bs, int bslen, int qscale,
                              const uint8_t qt[16], const char *tag,
                              int imgW, int imgH) {
    int bw = imgW / 4, bh = imgH / 4;
    uint8_t *img = calloc(imgW * imgH, 1);
    BR br; br_init(&br, bs, bslen);

    int dc_pred = 128;
    int ok = 0, fail = 0;

    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            if (br_eof(&br)) goto done4;

            int block[16] = {0};
            int spatial[16];

            int dc_diff = mpeg1_dc_lum(&br);
            dc_pred += dc_diff;
            block[0] = dc_pred * 8;

            int idx = 1;
            bool block_ok = true;
            while (idx < 16 && !br_eof(&br)) {
                int run, level;
                int ret = mpeg1_ac_decode(&br, &run, &level);
                if (ret == 0) break;
                if (ret < 0) { block_ok = false; break; }
                idx += run;
                if (idx >= 16) { block_ok = false; break; }
                int pos = zigzag4[idx];
                int dq = (2 * level * qscale * qt[pos]) / 16;
                if (dq > 0 && (dq & 1) == 0) dq--;
                if (dq < 0 && (-dq & 1) == 0) dq++;
                block[pos] = dq;
                idx++;
            }

            if (block_ok) ok++; else fail++;

            idct4x4(block, spatial);
            for (int dy = 0; dy < 4; dy++) {
                for (int dx = 0; dx < 4; dx++) {
                    int v = spatial[dy*4+dx];
                    if (v < 0) v = 0; if (v > 255) v = 255;
                    img[(by*4+dy)*imgW + bx*4+dx] = v;
                }
            }
        }
    }
done4:
    printf("MPEG1-4x4 %s %dx%d: ok=%d fail=%d, %d/%d bits\n",
           tag, imgW, imgH, ok, fail, br.total, bslen*8);
    char path[256];
    snprintf(path, sizeof(path), OUT_DIR "m1_4x4_%s_%dx%d.pgm", tag, imgW, imgH);
    write_pgm(path, img, imgW, imgH);
    free(img);
}

/*
 * MPEG-1 with 4:2:0 macroblock structure
 * Each MB = 4 Y blocks + 1 Cb + 1 Cr (8×8 blocks)
 */
static void decode_mpeg1_420(const uint8_t *bs, int bslen, int qscale,
                             const uint8_t qt[16], const char *tag,
                             int imgW, int imgH) {
    int mbw = imgW / 16, mbh = imgH / 16;
    uint8_t *imgY = calloc(imgW * imgH, 1);
    uint8_t *imgCb = calloc(imgW/2 * imgH/2, 1);
    uint8_t *imgCr = calloc(imgW/2 * imgH/2, 1);
    BR br; br_init(&br, bs, bslen);

    int dc_pred_y = 128, dc_pred_cb = 128, dc_pred_cr = 128;
    int ok = 0, fail = 0;

    int qm[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            qm[i*8+j] = qt[(i/2)*4 + (j/2)];

    for (int mby = 0; mby < mbh; mby++) {
        for (int mbx = 0; mbx < mbw; mbx++) {
            if (br_eof(&br)) goto done420;

            /* 4 Y blocks (TL, TR, BL, BR) */
            int yblocks[4][64];
            for (int yb = 0; yb < 4; yb++) {
                memset(yblocks[yb], 0, sizeof(yblocks[yb]));
                int dc_diff = mpeg1_dc_lum(&br);
                dc_pred_y += dc_diff;
                yblocks[yb][0] = dc_pred_y * 8;

                int idx = 1;
                bool block_ok = true;
                while (idx < 64 && !br_eof(&br)) {
                    int run, level;
                    int ret = mpeg1_ac_decode(&br, &run, &level);
                    if (ret == 0) break;
                    if (ret < 0) { block_ok = false; break; }
                    idx += run;
                    if (idx >= 64) { block_ok = false; break; }
                    int pos = zigzag8[idx];
                    int dq = (2 * level * qscale * qm[pos]) / 16;
                    if (dq > 0 && (dq & 1) == 0) dq--;
                    if (dq < 0 && (-dq & 1) == 0) dq++;
                    yblocks[yb][pos] = dq;
                    idx++;
                }
                if (block_ok) ok++; else fail++;
            }

            /* Cb block */
            int cbblock[64] = {0};
            {
                int dc_diff = mpeg1_dc_chr(&br);
                dc_pred_cb += dc_diff;
                cbblock[0] = dc_pred_cb * 8;
                int idx = 1;
                bool block_ok = true;
                while (idx < 64 && !br_eof(&br)) {
                    int run, level;
                    int ret = mpeg1_ac_decode(&br, &run, &level);
                    if (ret == 0) break;
                    if (ret < 0) { block_ok = false; break; }
                    idx += run;
                    if (idx >= 64) { block_ok = false; break; }
                    int pos = zigzag8[idx];
                    int dq = (2 * level * qscale * qm[pos]) / 16;
                    if (dq > 0 && (dq & 1) == 0) dq--;
                    if (dq < 0 && (-dq & 1) == 0) dq++;
                    cbblock[pos] = dq;
                    idx++;
                }
                if (block_ok) ok++; else fail++;
            }

            /* Cr block */
            int crblock[64] = {0};
            {
                int dc_diff = mpeg1_dc_chr(&br);
                dc_pred_cr += dc_diff;
                crblock[0] = dc_pred_cr * 8;
                int idx = 1;
                bool block_ok = true;
                while (idx < 64 && !br_eof(&br)) {
                    int run, level;
                    int ret = mpeg1_ac_decode(&br, &run, &level);
                    if (ret == 0) break;
                    if (ret < 0) { block_ok = false; break; }
                    idx += run;
                    if (idx >= 64) { block_ok = false; break; }
                    int pos = zigzag8[idx];
                    int dq = (2 * level * qscale * qm[pos]) / 16;
                    if (dq > 0 && (dq & 1) == 0) dq--;
                    if (dq < 0 && (-dq & 1) == 0) dq++;
                    crblock[pos] = dq;
                    idx++;
                }
                if (block_ok) ok++; else fail++;
            }

            /* IDCT and place Y blocks */
            int yspatial[4][64];
            for (int yb = 0; yb < 4; yb++)
                idct8x8(yblocks[yb], yspatial[yb]);

            /* Place Y: TL(0), TR(1), BL(2), BR(3) */
            int offsets[4][2] = {{0,0},{8,0},{0,8},{8,8}};
            for (int yb = 0; yb < 4; yb++) {
                for (int dy = 0; dy < 8; dy++) {
                    for (int dx = 0; dx < 8; dx++) {
                        int px = mbx*16 + offsets[yb][0] + dx;
                        int py = mby*16 + offsets[yb][1] + dy;
                        if (px < imgW && py < imgH) {
                            int v = yspatial[yb][dy*8+dx];
                            if (v < 0) v = 0; if (v > 255) v = 255;
                            imgY[py*imgW+px] = v;
                        }
                    }
                }
            }

            /* Place Cb/Cr */
            int cbspatial[64], crspatial[64];
            idct8x8(cbblock, cbspatial);
            idct8x8(crblock, crspatial);
            for (int dy = 0; dy < 8; dy++) {
                for (int dx = 0; dx < 8; dx++) {
                    int px = mbx*8+dx, py = mby*8+dy;
                    if (px < imgW/2 && py < imgH/2) {
                        int v;
                        v = cbspatial[dy*8+dx]; if(v<0)v=0;if(v>255)v=255;
                        imgCb[py*(imgW/2)+px] = v;
                        v = crspatial[dy*8+dx]; if(v<0)v=0;if(v>255)v=255;
                        imgCr[py*(imgW/2)+px] = v;
                    }
                }
            }
        }
    }
done420:
    printf("MPEG1-420 %s %dx%d: ok=%d fail=%d, %d/%d bits\n",
           tag, imgW, imgH, ok, fail, br.total, bslen*8);
    char path[256];
    snprintf(path, sizeof(path), OUT_DIR "m1_420y_%s_%dx%d.pgm", tag, imgW, imgH);
    write_pgm(path, imgY, imgW, imgH);
    snprintf(path, sizeof(path), OUT_DIR "m1_420cb_%s_%dx%d.pgm", tag, imgW/2, imgH/2);
    write_pgm(path, imgCb, imgW/2, imgH/2);
    free(imgY); free(imgCb); free(imgCr);
}

/*
 * Try MPEG-1 with DC prediction reset per macroblock row
 */
static void decode_mpeg1_420_rowreset(const uint8_t *bs, int bslen, int qscale,
                                       const uint8_t qt[16], const char *tag,
                                       int imgW, int imgH) {
    int mbw = imgW / 16, mbh = imgH / 16;
    uint8_t *imgY = calloc(imgW * imgH, 1);
    BR br; br_init(&br, bs, bslen);
    int ok = 0, fail = 0;

    int qm[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            qm[i*8+j] = qt[(i/2)*4 + (j/2)];

    for (int mby = 0; mby < mbh; mby++) {
        int dc_pred_y = 128, dc_pred_cb = 128, dc_pred_cr = 128;
        for (int mbx = 0; mbx < mbw; mbx++) {
            if (br_eof(&br)) goto done_rr;

            /* 4 Y blocks */
            for (int yb = 0; yb < 4; yb++) {
                int block[64] = {0}, spatial[64];
                int dc_diff = mpeg1_dc_lum(&br);
                dc_pred_y += dc_diff;
                block[0] = dc_pred_y * 8;

                int idx = 1;
                while (idx < 64 && !br_eof(&br)) {
                    int run, level;
                    int ret = mpeg1_ac_decode(&br, &run, &level);
                    if (ret == 0) break;
                    if (ret < 0) { fail++; goto skip_mb; }
                    idx += run;
                    if (idx >= 64) { fail++; goto skip_mb; }
                    int pos = zigzag8[idx];
                    int dq = (2 * level * qscale * qm[pos]) / 16;
                    block[pos] = dq;
                    idx++;
                }
                ok++;

                idct8x8(block, spatial);
                int offx = (yb & 1) * 8, offy = (yb >> 1) * 8;
                for (int dy = 0; dy < 8; dy++)
                    for (int dx = 0; dx < 8; dx++) {
                        int v = spatial[dy*8+dx];
                        if(v<0)v=0;if(v>255)v=255;
                        int px=mbx*16+offx+dx, py=mby*16+offy+dy;
                        if(px<imgW&&py<imgH) imgY[py*imgW+px]=v;
                    }
            }

            /* Cb */
            { int dc_diff = mpeg1_dc_chr(&br); dc_pred_cb += dc_diff;
              int block[64]={0}; block[0]=dc_pred_cb*8;
              int idx=1;
              while(idx<64&&!br_eof(&br)){int run,level;int ret=mpeg1_ac_decode(&br,&run,&level);
                if(ret==0)break;if(ret<0){fail++;goto skip_mb;}
                idx+=run;if(idx>=64){fail++;goto skip_mb;}
                block[zigzag8[idx]]=(2*level*qscale*qm[zigzag8[idx]])/16;idx++;}
              ok++;
            }
            /* Cr */
            { int dc_diff = mpeg1_dc_chr(&br); dc_pred_cr += dc_diff;
              int block[64]={0}; block[0]=dc_pred_cr*8;
              int idx=1;
              while(idx<64&&!br_eof(&br)){int run,level;int ret=mpeg1_ac_decode(&br,&run,&level);
                if(ret==0)break;if(ret<0){fail++;goto skip_mb;}
                idx+=run;if(idx>=64){fail++;goto skip_mb;}
                block[zigzag8[idx]]=(2*level*qscale*qm[zigzag8[idx]])/16;idx++;}
              ok++;
            }
            continue;
            skip_mb:;
        }
    }
done_rr:
    printf("MPEG1-420-rowreset %s %dx%d: ok=%d fail=%d, %d/%d bits\n",
           tag, imgW, imgH, ok, fail, br.total, bslen*8);
    char path[256];
    snprintf(path, sizeof(path), OUT_DIR "m1_420rr_%s_%dx%d.pgm", tag, imgW, imgH);
    write_pgm(path, imgY, imgW, imgH);
    free(imgY);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <zip> [lba] [game]\n", argv[0]); return 1; }
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

    /* Process first 2 frames */
    for (int fi = 0; fi < nf && fi < 2; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        int qscale = f[3];
        uint8_t qt[16];
        memcpy(qt, f+4, 16);

        printf("\n=== Frame %d: %d bytes, qscale=%d, type=%d ===\n",
               fi, fsize, qscale, f[39]);

        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        char tag[64];
        snprintf(tag, sizeof(tag), "%s_f%d", game, fi);

        /* Try various resolutions with 8×8 blocks */
        decode_mpeg1_8x8(bs, bslen, qscale, qt, tag, 128, 96);
        decode_mpeg1_8x8(bs, bslen, qscale, qt, tag, 128, 144);

        /* Try 4×4 blocks */
        decode_mpeg1_4x4(bs, bslen, qscale, qt, tag, 128, 144);
        decode_mpeg1_4x4(bs, bslen, qscale, qt, tag, 128, 128);

        /* Try 4:2:0 macroblock */
        decode_mpeg1_420(bs, bslen, qscale, qt, tag, 128, 144);
        decode_mpeg1_420(bs, bslen, qscale, qt, tag, 128, 128);

        /* Try with row reset */
        decode_mpeg1_420_rowreset(bs, bslen, qscale, qt, tag, 128, 144);
        decode_mpeg1_420_rowreset(bs, bslen, qscale, qt, tag, 128, 128);
    }

    free(disc); zip_close(z);
    return 0;
}
