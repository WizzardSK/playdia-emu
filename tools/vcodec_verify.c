/*
 * vcodec_verify.c - Verify JPEG AC results against random data
 * Also test progressive (band-ordered) AC coding
 *
 * If JPEG AC on random data produces the same band distribution as
 * real data, then JPEG is self-calibrating (wrong).
 * If real data has a DIFFERENT distribution than random, JPEG might be right.
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
#define W 256
#define H 144

typedef struct { const uint8_t *data; int total_bits; int pos; } bitstream;

static int bs_read(bitstream *bs, int n) {
    if (n <= 0 || bs->pos + n > bs->total_bits) return -1;
    int v = 0;
    for (int i = 0; i < n; i++) {
        int bp = bs->pos + i;
        v = (v << 1) | ((bs->data[bp >> 3] >> (7 - (bp & 7))) & 1);
    }
    bs->pos += n;
    return v;
}
static int bs_peek(bitstream *bs, int n) {
    if (n <= 0 || bs->pos + n > bs->total_bits) return -1;
    int v = 0;
    for (int i = 0; i < n; i++) {
        int bp = bs->pos + i;
        v = (v << 1) | ((bs->data[bp >> 3] >> (7 - (bp & 7))) & 1);
    }
    return v;
}

static const struct { int len; uint32_t code; int size; } dc_vlc[] = {
    {3, 0b100, 0}, {2, 0b00, 1}, {2, 0b01, 2}, {3, 0b101, 3},
    {3, 0b110, 4}, {4, 0b1110, 5}, {5, 0b11110, 6},
    {6, 0b111110, 7}, {7, 0b1111110, 8},
};
static int read_dc(bitstream *bs) {
    for (int i = 0; i < 9; i++) {
        int bits = bs_peek(bs, dc_vlc[i].len);
        if (bits == (int)dc_vlc[i].code) {
            bs->pos += dc_vlc[i].len;
            int sz = dc_vlc[i].size;
            if (sz == 0) return 0;
            int val = bs_read(bs, sz);
            if (val < (1 << (sz - 1))) val -= (1 << sz) - 1;
            return val;
        }
    }
    bs->pos += 2;
    return 0;
}

typedef struct { uint16_t code; int len; int run; int size; } jpeg_ac_entry;
static const jpeg_ac_entry jpeg_ac_table[] = {
    {0b00, 2, 0, 1},
    {0b01, 2, 0, 2},
    {0b100, 3, 0, 3},
    {0b1010, 4, 0, 0},       /* EOB */
    {0b1011, 4, 0, 4},
    {0b1100, 4, 1, 1},
    {0b11010, 5, 0, 5},
    {0b11011, 5, 1, 2},
    {0b11100, 5, 2, 1},
    {0b111010, 6, 3, 1},
    {0b111011, 6, 4, 1},
    {0b1111000, 7, 0, 6},
    {0b1111001, 7, 1, 3},
    {0b1111010, 7, 5, 1},
    {0b1111011, 7, 6, 1},
    {0b11111000, 8, 0, 7},
    {0b11111001, 8, 2, 2},
    {0b11111010, 8, 7, 1},
    {0b111110110, 9, 1, 4},
    {0b111110111, 9, 3, 2},
    {0b111111000, 9, 8, 1},
    {0b111111001, 9, 9, 1},
    {0b111111010, 9, 0, 8},
    {0b1111110110, 10, 2, 3},
    {0b1111110111, 10, 4, 2},
    {0b1111111000, 10, 10, 1},
    {0b1111111001, 10, 11, 1},
    {0b1111111010, 10, 0, 9},
    {0b11111110110, 11, 5, 2},
    {0b11111110111, 11, 6, 2},
    {0b11111111000, 11, 12, 1},
    {0b11111111001, 11, 15, 0},  /* ZRL */
    {0b111111110100, 12, 1, 5},
    {0b111111110101, 12, 3, 3},
    {0b111111110110, 12, 7, 2},
    {0b111111110111, 12, 13, 1},
    {0b1111111110000, 13, 0, 10},
    {0b1111111110001, 13, 2, 4},
    {0b1111111110010, 13, 4, 3},
    {0b1111111110011, 13, 8, 2},
    {0b1111111110100, 13, 14, 1},
};
#define JPEG_AC_COUNT (sizeof(jpeg_ac_table)/sizeof(jpeg_ac_table[0]))

/* MPEG-1 Table B.14 AC VLC (common entries + escape) */
static const struct { uint16_t code; int len; int run; int level; } mpeg_ac[] = {
    {0b10, 2, -1, 0},     /* EOB */
    {0b11, 2, 0, 1},      /* 0,1 */
    {0b011, 3, 1, 1},
    {0b0100, 4, 0, 2},
    {0b0101, 4, 2, 1},
    {0b00101, 5, 0, 3},
    {0b00111, 5, 3, 1},
    {0b00110, 5, 4, 1},
    {0b000110, 6, 1, 2},
    {0b000111, 6, 5, 1},
    {0b000101, 6, 6, 1},
    {0b000100, 6, 7, 1},
    {0b0000110, 7, 0, 4},
    {0b0000100, 7, 2, 2},
    {0b0000111, 7, 8, 1},
    {0b0000101, 7, 9, 1},
    {0b000001, 6, -2, 0},  /* Escape */
};
#define MPEG_AC_COUNT (sizeof(mpeg_ac)/sizeof(mpeg_ac[0]))

static int decode_jpeg_ac_block(bitstream *bs, int band_nz[8], int *total_nz, int *total_eob) {
    int pos = 1;
    while (pos < 64 && bs->pos < bs->total_bits) {
        int matched = 0;
        for (int i = 0; i < (int)JPEG_AC_COUNT; i++) {
            int bits = bs_peek(bs, jpeg_ac_table[i].len);
            if (bits < 0) return -1;
            if (bits == jpeg_ac_table[i].code) {
                bs->pos += jpeg_ac_table[i].len;
                int run = jpeg_ac_table[i].run;
                int size = jpeg_ac_table[i].size;
                if (run == 0 && size == 0) { (*total_eob)++; return 0; }
                if (run == 15 && size == 0) { pos += 16; matched=1; break; }
                int val = bs_read(bs, size);
                if (val < 0) return -1;
                if (val < (1 << (size - 1))) val -= (1 << size) - 1;
                pos += run;
                if (pos >= 64) break;
                if (val != 0) {
                    int band = (pos-1)/8;
                    if (band < 8) band_nz[band]++;
                    (*total_nz)++;
                }
                pos++;
                matched = 1;
                break;
            }
        }
        if (!matched) { bs->pos++; }
    }
    return 0;
}

static int decode_mpeg_ac_block(bitstream *bs, int band_nz[8], int *total_nz, int *total_eob) {
    int pos = 1;
    while (pos < 64 && bs->pos < bs->total_bits) {
        int matched = 0;
        for (int t = 0; t < (int)MPEG_AC_COUNT; t++) {
            int bits = bs_peek(bs, mpeg_ac[t].len);
            if (bits == mpeg_ac[t].code) {
                bs->pos += mpeg_ac[t].len;
                if (mpeg_ac[t].run == -1) { (*total_eob)++; return 0; }
                if (mpeg_ac[t].run == -2) { bs->pos += 14; pos++; matched=1; break; }
                bs->pos++;  /* sign */
                pos += mpeg_ac[t].run;
                if (pos < 64) {
                    int band = (pos-1)/8;
                    if (band < 8) band_nz[band]++;
                    (*total_nz)++;
                }
                pos++;
                matched = 1;
                break;
            }
        }
        if (!matched) bs->pos++;
    }
    return 0;
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

static const int zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static void idct8x8(int block[64], int out[64]) {
    double tmp[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) {
            double sum = 0;
            for (int u = 0; u < 8; u++) {
                double cu = (u == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += cu * block[i*8+u] * cos((2*j+1)*u*M_PI/16.0);
            }
            tmp[i*8+j] = sum * 0.5;
        }
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) {
            double sum = 0;
            for (int v = 0; v < 8; v++) {
                double cv = (v == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += cv * tmp[v*8+j] * cos((2*i+1)*v*M_PI/16.0);
            }
            out[i*8+j] = (int)(sum * 0.5);
        }
}

static int clamp(int v) { return v<0?0:v>255?255:v; }

static void write_ppm(const char *fn, int Y[H][W], int Cb[H/2][W/2], int Cr[H/2][W/2]) {
    FILE *fp = fopen(fn, "wb");
    fprintf(fp, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int yv = Y[y][x], cb = Cb[y/2][x/2]-128, cr = Cr[y/2][x/2]-128;
            uint8_t rgb[3];
            rgb[0] = clamp(yv + 1.402*cr);
            rgb[1] = clamp(yv - 0.344136*cb - 0.714136*cr);
            rgb[2] = clamp(yv + 1.772*cb);
            fwrite(rgb, 1, 3, fp);
        }
    fclose(fp);
}

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    int start_lba = atoi(argv[2]);

    int err; zip_t *z = zip_open(argv[1], ZIP_RDONLY, &err); if (!z) return 1;
    int bi=-1; zip_uint64_t bsz=0;
    for(int i=0;i<(int)zip_get_num_entries(z,0);i++){
        zip_stat_t st;if(zip_stat_index(z,i,0,&st)==0&&st.size>bsz){bsz=st.size;bi=i;}}
    zip_stat_t st; zip_stat_index(z,bi,0,&st);
    zip_file_t *zf=zip_fopen_index(z,bi,0);
    uint8_t *disc=malloc(st.size);
    zip_int64_t rd=0;
    while(rd<(zip_int64_t)st.size){zip_int64_t r=zip_fread(zf,disc+rd,st.size-rd);if(r<=0)break;rd+=r;}
    zip_fclose(zf);
    int tsec=(int)(st.size/SECTOR_RAW);

    static uint8_t frames[32][MAX_FRAME]; int fsizes[32];
    int nf=assemble_frames(disc,tsec,start_lba,frames,fsizes,32);
    if(nf==0){printf("No frames\n");return 1;}

    int mw=W/16, mh=H/16, nblocks=mw*mh*6;

    /* Get real frame AC data */
    uint8_t *f=frames[0]; int fsize=fsizes[0];
    int qs = f[3];
    const uint8_t *bsdata = f+40;
    int bslen = fsize-40, total_bits = bslen*8;

    bitstream bs0 = {bsdata, total_bits, 0};
    int dc_vals[900]; int dc_pred[3]={0,0,0};
    for(int b=0;b<nblocks&&bs0.pos<total_bits;b++){
        int comp=(b%6<4)?0:(b%6==4)?1:2;
        dc_pred[comp]+=read_dc(&bs0);
        dc_vals[b]=dc_pred[comp];
    }
    int dc_end = bs0.pos;
    int ac_bits = total_bits - dc_end;

    printf("Frame: qs=%d, dc=%d bits, ac=%d bits\n\n", qs, dc_end, ac_bits);

    /* ============================================================
     * Test 1: JPEG AC on REAL data
     * ============================================================ */
    printf("=== JPEG AC on REAL data ===\n");
    {
        int band_nz[8]={0}; int total_nz=0, total_eob=0;
        bitstream bs = {bsdata, total_bits, dc_end};
        for (int b = 0; b < nblocks && bs.pos < total_bits; b++)
            decode_jpeg_ac_block(&bs, band_nz, &total_nz, &total_eob);
        int used = bs.pos - dc_end;
        printf("  NZ=%d EOB=%d used=%d/%d (%.1f%%)\n", total_nz, total_eob, used, ac_bits, 100.0*used/ac_bits);
        printf("  Bands: "); for(int i=0;i<8;i++) printf("%d ", band_nz[i]); printf("\n");
    }

    /* ============================================================
     * Test 2: JPEG AC on RANDOM data (same size)
     * ============================================================ */
    printf("\n=== JPEG AC on RANDOM data (control) ===\n");
    {
        uint8_t *rnd = malloc(bslen);
        srand(12345);
        for (int i = 0; i < bslen; i++) rnd[i] = rand() & 0xFF;

        int band_nz[8]={0}; int total_nz=0, total_eob=0;
        bitstream bs = {rnd, ac_bits, 0};
        for (int b = 0; b < nblocks && bs.pos < ac_bits; b++)
            decode_jpeg_ac_block(&bs, band_nz, &total_nz, &total_eob);
        int used = bs.pos;
        printf("  NZ=%d EOB=%d used=%d/%d (%.1f%%)\n", total_nz, total_eob, used, ac_bits, 100.0*used/ac_bits);
        printf("  Bands: "); for(int i=0;i<8;i++) printf("%d ", band_nz[i]); printf("\n");
        free(rnd);
    }

    /* ============================================================
     * Test 3: MPEG-1 AC VLC on RANDOM data (second control)
     * ============================================================ */
    printf("\n=== MPEG-1 AC on RANDOM data (control) ===\n");
    {
        uint8_t *rnd = malloc(bslen);
        srand(12345);
        for (int i = 0; i < bslen; i++) rnd[i] = rand() & 0xFF;

        int band_nz[8]={0}; int total_nz=0, total_eob=0;
        bitstream bs = {rnd, ac_bits, 0};
        for (int b = 0; b < nblocks && bs.pos < ac_bits; b++)
            decode_mpeg_ac_block(&bs, band_nz, &total_nz, &total_eob);
        int used = bs.pos;
        printf("  NZ=%d EOB=%d used=%d/%d (%.1f%%)\n", total_nz, total_eob, used, ac_bits, 100.0*used/ac_bits);
        printf("  Bands: "); for(int i=0;i<8;i++) printf("%d ", band_nz[i]); printf("\n");
        free(rnd);
    }

    /* ============================================================
     * Test 4: MPEG-1 AC on REAL data (reference)
     * ============================================================ */
    printf("\n=== MPEG-1 AC on REAL data (reference) ===\n");
    {
        int band_nz[8]={0}; int total_nz=0, total_eob=0;
        bitstream bs = {bsdata, total_bits, dc_end};
        for (int b = 0; b < nblocks && bs.pos < total_bits; b++)
            decode_mpeg_ac_block(&bs, band_nz, &total_nz, &total_eob);
        int used = bs.pos - dc_end;
        printf("  NZ=%d EOB=%d used=%d/%d (%.1f%%)\n", total_nz, total_eob, used, ac_bits, 100.0*used/ac_bits);
        printf("  Bands: "); for(int i=0;i<8;i++) printf("%d ", band_nz[i]); printf("\n");
    }

    /* ============================================================
     * Test 5: Progressive/band-ordered decoding
     * What if AC data is organized by frequency band, not by block?
     * Band 1 (pos 1) for all 864 blocks, then band 2, etc.
     * ============================================================ */
    printf("\n=== Test 5: Progressive band-ordered AC ===\n");
    {
        /* Try: for each zigzag position 1..63, read a value for all 864 blocks */
        /* Use DC VLC for each value (same table) */
        static int coeffs[900][64];
        memset(coeffs, 0, sizeof(coeffs));

        bitstream bs = {bsdata, total_bits, dc_end};
        int positions_done = 0;
        int total_nz = 0;

        for (int p = 1; p < 64 && bs.pos < total_bits; p++) {
            int band_nz = 0;
            for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
                /* Try reading a DC-style VLC for this AC position */
                int val = read_dc(&bs);
                if (val != 0) {
                    coeffs[b][zigzag[p]] = val;
                    band_nz++;
                    total_nz++;
                }
            }
            positions_done++;
            if (p <= 8 || p == 16 || p == 32 || p == 63)
                printf("  Pos %2d: NZ=%d, bit pos=%d\n", p, band_nz, bs.pos);
        }
        int used = bs.pos - dc_end;
        printf("  Positions done: %d/63, NZ=%d, used=%d/%d (%.1f%%)\n",
               positions_done, total_nz, used, ac_bits, 100.0*used/ac_bits);
    }

    /* ============================================================
     * Test 6: Progressive with 1-bit flag per value
     * For each position, for each block: 0=zero, 1=read value
     * ============================================================ */
    printf("\n=== Test 6: Progressive 1-bit flag + DC VLC ===\n");
    {
        static int coeffs[900][64];
        memset(coeffs, 0, sizeof(coeffs));
        bitstream bs = {bsdata, total_bits, dc_end};
        int total_nz = 0;
        int positions_done = 0;

        for (int p = 1; p < 64 && bs.pos < total_bits; p++) {
            int band_nz = 0;
            for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
                int flag = bs_read(&bs, 1);
                if (flag < 0) break;
                if (flag == 1) {
                    int val = read_dc(&bs);
                    coeffs[b][zigzag[p]] = val;
                    if (val != 0) { band_nz++; total_nz++; }
                }
            }
            positions_done++;
            if (p <= 8 || p == 16 || p == 32 || p == 63)
                printf("  Pos %2d: NZ=%d, bit pos=%d\n", p, band_nz, bs.pos);
        }
        int used = bs.pos - dc_end;
        printf("  Positions: %d/63, NZ=%d, used=%d/%d (%.1f%%)\n",
               positions_done, total_nz, used, ac_bits, 100.0*used/ac_bits);
    }

    /* ============================================================
     * Test 7: Compare Frame 0 (with padding) vs Frame 4 (no padding)
     * Are they decoded consistently?
     * ============================================================ */
    printf("\n=== Test 7: JPEG AC consistency across frames ===\n");
    for (int fi = 0; fi < nf && fi < 8; fi++) {
        uint8_t *ff = frames[fi];
        int ffs = fsizes[fi];
        int fqs = ff[3], ft = ff[39];
        const uint8_t *fbd = ff+40;
        int fbl = ffs-40, ftb = fbl*8;

        /* Count padding */
        int pad = 0;
        for (int i = fbl-1; i >= 0; i--) {
            if (fbd[i] == 0xFF) pad++; else break;
        }

        /* DC */
        bitstream bs = {fbd, ftb, 0};
        int dp[3]={0,0,0};
        for(int b=0;b<nblocks&&bs.pos<ftb;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dp[comp]+=read_dc(&bs);
        }
        int de = bs.pos;
        int ab = ftb - de;

        /* JPEG AC */
        int band_nz[8]={0}; int tnz=0, teob=0;
        for (int b = 0; b < nblocks && bs.pos < ftb; b++)
            decode_jpeg_ac_block(&bs, band_nz, &tnz, &teob);
        int used = bs.pos - de;

        printf("  F%02d qs=%2d t=%d pad=%4d: NZ=%5d EOB=%3d used=%5d/%5d (%.1f%%) bands=%d,%d,%d,%d,%d,%d,%d,%d\n",
               fi, fqs, ft, pad, tnz, teob, used, ab, 100.0*used/ab,
               band_nz[0],band_nz[1],band_nz[2],band_nz[3],
               band_nz[4],band_nz[5],band_nz[6],band_nz[7]);
    }

    /* ============================================================
     * Test 8: What if there's a different 8×8 block size?
     * Maybe it's 4×4 blocks (256 blocks instead of 864)?
     * ============================================================ */
    printf("\n=== Test 8: Alternative block configurations ===\n");
    {
        int configs[][3] = {
            {16, 9, 6},   /* 16×9 MBs, 6 blocks each = 864 (current) */
            {32, 18, 1},  /* 32×18 blocks of 8×8 = 576 blocks */
            {16, 9, 4},   /* 16×9 MBs, 4 blocks each = 576 (Y-only) */
        };
        for (int ci = 0; ci < 3; ci++) {
            int w = configs[ci][0], h = configs[ci][1], bpb = configs[ci][2];
            int nb = w * h * bpb;
            printf("  %dx%d MBs × %d blocks = %d blocks: %.1f bits/block, %.2f bits/coeff\n",
                   w, h, bpb, nb, (double)ac_bits/nb, (double)ac_bits/(nb*63));
        }
    }

    /* ============================================================
     * Test 9: What if the padded frame (F03, 1328 bytes padding)
     * gives us the actual data size and we can figure out the structure?
     * Frame 3 has 1328 bytes of 0xFF padding = 10624 bits padding
     * So actual AC data = 93833 - 10624 = ~83209 bits for 864 blocks
     * ============================================================ */
    printf("\n=== Test 9: Padded frame analysis (F03) ===\n");
    if (nf > 3) {
        uint8_t *ff = frames[3];
        int ffs = fsizes[3];
        const uint8_t *fbd = ff+40;
        int fbl = ffs-40;
        int fqs = ff[3];

        /* Find where real data ends (before 0xFF padding) */
        int data_end = fbl;
        for (int i = fbl-1; i >= 0; i--) {
            if (fbd[i] != 0xFF) { data_end = i+1; break; }
        }
        printf("  Frame 3: qs=%d, total=%d bytes, real data=%d bytes, padding=%d bytes\n",
               fqs, fbl, data_end, fbl-data_end);

        /* DC */
        int ftb = data_end * 8;
        bitstream bs = {fbd, ftb, 0};
        int dp[3]={0,0,0};
        for(int b=0;b<nblocks&&bs.pos<ftb;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dp[comp]+=read_dc(&bs);
        }
        int de = bs.pos;
        int real_ac = ftb - de;
        printf("  DC=%d bits, real AC=%d bits\n", de, real_ac);
        printf("  Real AC per block: %.1f bits, per coeff: %.2f bits\n",
               (double)real_ac/nblocks, (double)real_ac/(nblocks*63));

        /* Last few bytes before padding */
        printf("  Last 16 bytes before padding: ");
        for (int i = data_end-16; i < data_end; i++) printf("%02X ", fbd[i]);
        printf("\n");

        /* Does it end at a specific bit boundary? */
        printf("  Data end byte: %d (mod 8=%d, mod 16=%d)\n",
               data_end, data_end%8, data_end%16);

        /* JPEG AC on just the real data */
        int band_nz[8]={0}; int tnz=0, teob=0;
        for (int b = 0; b < nblocks && bs.pos < ftb; b++)
            decode_jpeg_ac_block(&bs, band_nz, &tnz, &teob);
        int used = bs.pos - de;
        printf("  JPEG AC: NZ=%d EOB=%d used=%d/%d (%.1f%%)\n",
               tnz, teob, used, real_ac, 100.0*used/real_ac);
        printf("  Bands: "); for(int i=0;i<8;i++) printf("%d ",band_nz[i]); printf("\n");
    }

    /* ============================================================
     * Test 10: Image with JPEG AC (proper dequant) on padded frame
     * Using the padded frame where we know the exact data boundary
     * ============================================================ */
    printf("\n=== Test 10: JPEG AC image from padded frame (F03) ===\n");
    if (nf > 3) {
        uint8_t *ff = frames[3];
        int ffs = fsizes[3];
        int fqs = ff[3];
        const uint8_t *fbd = ff+40;
        int fbl = ffs-40;
        int data_end = fbl;
        for (int i = fbl-1; i >= 0; i--) {
            if (fbd[i] != 0xFF) { data_end = i+1; break; }
        }
        int ftb = data_end * 8;

        static int coeffs[900][64];
        memset(coeffs, 0, sizeof(coeffs));

        bitstream bs = {fbd, ftb, 0};
        int dp[3]={0,0,0};
        for(int b=0;b<nblocks&&bs.pos<ftb;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dp[comp]+=read_dc(&bs);
            coeffs[b][0] = dp[comp] * 8;
        }

        static const uint8_t qt[16] = {10,20,14,13,18,37,22,28,15,24,15,18,18,31,17,20};

        for (int b = 0; b < nblocks && bs.pos < ftb; b++) {
            int pos = 1;
            while (pos < 64 && bs.pos < ftb) {
                int matched = 0;
                for (int i = 0; i < (int)JPEG_AC_COUNT; i++) {
                    int bits = bs_peek(&bs, jpeg_ac_table[i].len);
                    if (bits < 0) goto done10;
                    if (bits == jpeg_ac_table[i].code) {
                        bs.pos += jpeg_ac_table[i].len;
                        int run = jpeg_ac_table[i].run;
                        int size = jpeg_ac_table[i].size;
                        if (run == 0 && size == 0) { matched=1; goto eob10; }
                        if (run == 15 && size == 0) { pos += 16; matched=1; break; }
                        int val = bs_read(&bs, size);
                        if (val < 0) goto done10;
                        if (val < (1 << (size - 1))) val -= (1 << size) - 1;
                        pos += run;
                        if (pos >= 64) break;
                        int zpos = zigzag[pos];
                        int qi = zpos % 16;
                        coeffs[b][zpos] = val * qt[qi] * fqs / 8;
                        pos++;
                        matched = 1;
                        break;
                    }
                }
                if (!matched) { bs.pos++; pos++; }
            }
            eob10:;
        }
        done10:;

        static int Yp[H][W], Cbp[H/2][W/2], Crp[H/2][W/2];
        memset(Yp,128,sizeof(Yp)); memset(Cbp,128,sizeof(Cbp)); memset(Crp,128,sizeof(Crp));
        for (int mb = 0; mb < mw*mh; mb++) {
            int mx2=mb%mw, my2=mb/mw;
            for (int s = 0; s < 6; s++) {
                int out[64];
                idct8x8(coeffs[mb*6+s], out);
                if (s < 4) {
                    int bx=(s&1)*8, by=(s>>1)*8;
                    for(int r=0;r<8;r++) for(int c=0;c<8;c++){
                        int py=my2*16+by+r, px=mx2*16+bx+c;
                        if(py<H&&px<W) Yp[py][px]=clamp(out[r*8+c]+128);
                    }
                } else if (s == 4) {
                    for(int r=0;r<8;r++) for(int c=0;c<8;c++){
                        int py=my2*8+r, px=mx2*8+c;
                        if(py<H/2&&px<W/2) Cbp[py][px]=clamp(out[r*8+c]+128);
                    }
                } else {
                    for(int r=0;r<8;r++) for(int c=0;c<8;c++){
                        int py=my2*8+r, px=mx2*8+c;
                        if(py<H/2&&px<W/2) Crp[py][px]=clamp(out[r*8+c]+128);
                    }
                }
            }
        }
        write_ppm("/tmp/ac_jpeg_f03.ppm", Yp, Cbp, Crp);
        printf("  → /tmp/ac_jpeg_f03.ppm\n");
    }

    free(disc); zip_close(z);
    return 0;
}
