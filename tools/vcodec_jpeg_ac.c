/*
 * vcodec_jpeg_ac.c - Test JPEG-style AC Huffman coding
 *
 * JPEG AC coding differs from MPEG-1:
 * - Codes represent (run_of_zeros, size_of_value) pairs
 * - After Huffman code, 'size' more bits give the actual coefficient value
 * - Standard table from JPEG Annex K, Table K.5 (luminance AC)
 *
 * Also tests:
 * - MPEG chrominance DC VLC for AC
 * - QS vs padding/data relationship across multiple frames
 * - H.261 AC VLC differences
 * - Fixed block size with QS-dependent bit allocation
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

/* MPEG-1 DC luminance VLC (confirmed working) */
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

/*
 * JPEG Standard AC Huffman Table (Annex K, Table K.5, Luminance)
 * Format: (run/size) coded as Huffman, then 'size' additional bits for value
 * Special: (0,0)=EOB, (15,0)=ZRL (skip 16 zeros)
 *
 * Building the table from the JPEG spec bit lengths and values.
 * JPEG defines tables via bit counts per length then symbol list.
 *
 * Luminance AC:
 * Bits: 0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d
 * (meaning: 0 codes of length 1, 2 of length 2, 1 of length 3, etc.)
 * Values: 01,02, 03, 00,04,11, 05,12,21, 31,41, 06,13,51,61, ...
 */

/* Pre-built JPEG luminance AC Huffman table */
/* Each entry: code, code_length, run, size */
typedef struct { uint16_t code; int len; int run; int size; } jpeg_ac_entry;

/* Generated from JPEG Annex K Table K.5 */
static const jpeg_ac_entry jpeg_ac_table[] = {
    /* len 2 */
    {0b00, 2, 0, 1},         /* 0/1 */
    {0b01, 2, 0, 2},         /* 0/2 */
    /* len 3 */
    {0b100, 3, 0, 3},        /* 0/3 */
    /* len 4 */
    {0b1010, 4, 0, 0},       /* EOB (0/0) */
    {0b1011, 4, 0, 4},       /* 0/4 */
    {0b1100, 4, 1, 1},       /* 1/1 */
    /* len 5 */
    {0b11010, 5, 0, 5},      /* 0/5 */
    {0b11011, 5, 1, 2},      /* 1/2 */
    {0b11100, 5, 2, 1},      /* 2/1 */
    /* len 6 */
    {0b111010, 6, 3, 1},     /* 3/1 */
    {0b111011, 6, 4, 1},     /* 4/1 */
    /* len 7 */
    {0b1111000, 7, 0, 6},    /* 0/6 */
    {0b1111001, 7, 1, 3},    /* 1/3 */
    {0b1111010, 7, 5, 1},    /* 5/1 */
    {0b1111011, 7, 6, 1},    /* 6/1 */
    /* len 8 */
    {0b11111000, 8, 0, 7},   /* 0/7 */
    {0b11111001, 8, 2, 2},   /* 2/2 */
    {0b11111010, 8, 7, 1},   /* 7/1 */
    /* len 9 */
    {0b111110110, 9, 1, 4},  /* 1/4 */
    {0b111110111, 9, 3, 2},  /* 3/2 */
    {0b111111000, 9, 8, 1},  /* 8/1 */
    {0b111111001, 9, 9, 1},  /* 9/1 */
    {0b111111010, 9, 0, 8},  /* 0/8 */
    /* len 10 */
    {0b1111110110, 10, 2, 3},  /* 2/3 */
    {0b1111110111, 10, 4, 2},  /* 4/2 */
    {0b1111111000, 10, 10, 1}, /* A/1 */
    {0b1111111001, 10, 11, 1}, /* B/1 */
    {0b1111111010, 10, 0, 9},  /* 0/9 */
    /* len 11 */
    {0b11111110110, 11, 5, 2},  /* 5/2 */
    {0b11111110111, 11, 6, 2},  /* 6/2 */
    {0b11111111000, 11, 12, 1}, /* C/1 */
    {0b11111111001, 11, 15, 0}, /* ZRL (F/0) */
    /* len 12 - extend further with common entries */
    {0b111111110100, 12, 1, 5},
    {0b111111110101, 12, 3, 3},
    {0b111111110110, 12, 7, 2},
    {0b111111110111, 12, 13, 1},
    /* len 13 */
    {0b1111111110000, 13, 0, 10},
    {0b1111111110001, 13, 2, 4},
    {0b1111111110010, 13, 4, 3},
    {0b1111111110011, 13, 8, 2},
    {0b1111111110100, 13, 14, 1},
};
#define JPEG_AC_COUNT (sizeof(jpeg_ac_table)/sizeof(jpeg_ac_table[0]))

static int read_jpeg_ac(bitstream *bs, int *run_out, int *level_out) {
    for (int i = 0; i < (int)JPEG_AC_COUNT; i++) {
        int bits = bs_peek(bs, jpeg_ac_table[i].len);
        if (bits < 0) return -1;
        if (bits == jpeg_ac_table[i].code) {
            bs->pos += jpeg_ac_table[i].len;
            int run = jpeg_ac_table[i].run;
            int size = jpeg_ac_table[i].size;
            if (run == 0 && size == 0) {
                *run_out = -1;  /* EOB */
                *level_out = 0;
                return 0;
            }
            if (run == 15 && size == 0) {
                *run_out = 16;  /* ZRL: 16 zeros */
                *level_out = 0;
                return 0;
            }
            /* Read 'size' bits for value */
            int val = bs_read(bs, size);
            if (val < 0) return -1;
            /* JPEG sign convention: if MSB=0, value is negative */
            if (val < (1 << (size - 1)))
                val -= (1 << size) - 1;
            *run_out = run;
            *level_out = val;
            return 0;
        }
    }
    return -1;  /* No match */
}

static const int zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static const uint8_t qtable_vals[16] = {
    10,20,14,13,18,37,22,28,15,24,15,18,18,31,17,20
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
    if (argc < 3) { printf("Usage: %s <zip> <lba>\n", argv[0]); return 1; }
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

    uint8_t *f=frames[0]; int fsize=fsizes[0];
    int qs=f[3], ftype=f[39];
    const uint8_t *bsdata=f+40;
    int bslen=fsize-40, total_bits=bslen*8;
    int mw=W/16, mh=H/16, nblocks=mw*mh*6;

    printf("Frame 0: qs=%d type=%d fsize=%d bitstream=%d bits\n\n", qs, ftype, fsize, total_bits);

    /* Decode DC */
    bitstream bs0 = {bsdata, total_bits, 0};
    int dc_vals[900]; int dc_pred[3]={0,0,0};
    for(int b=0;b<nblocks&&bs0.pos<total_bits;b++){
        int comp=(b%6<4)?0:(b%6==4)?1:2;
        dc_pred[comp]+=read_dc(&bs0);
        dc_vals[b]=dc_pred[comp];
    }
    int dc_end=bs0.pos;
    int ac_bits=total_bits-dc_end;
    printf("DC: %d bits, AC: %d bits\n\n", dc_end, ac_bits);

    /* ============================================================
     * Test 1: JPEG Standard AC Huffman Table (Luminance)
     * ============================================================ */
    printf("=== Test 1: JPEG Luminance AC Huffman ===\n");
    {
        static int coeffs[900][64];
        memset(coeffs, 0, sizeof(coeffs));
        bitstream bs = {bsdata, total_bits, dc_end};
        int completed = 0, total_nz = 0, total_eob = 0, nomatch = 0;
        int band_nz[8] = {0};

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            coeffs[b][0] = dc_vals[b] * 8;
            int pos = 1;
            while (pos < 64 && bs.pos < total_bits) {
                int run, level;
                int rc = read_jpeg_ac(&bs, &run, &level);
                if (rc < 0) { nomatch++; bs.pos++; continue; }
                if (run == -1) { total_eob++; break; }  /* EOB */
                pos += run;
                if (pos >= 64) break;
                if (level != 0) {
                    coeffs[b][zigzag[pos]] = level;
                    int band = (pos-1)/8;
                    if (band < 8) band_nz[band]++;
                    total_nz++;
                }
                pos++;
            }
            completed++;
        }
        int used = bs.pos - dc_end;
        printf("  Blocks: %d, NZ: %d, EOB: %d, NoMatch: %d\n", completed, total_nz, total_eob, nomatch);
        printf("  Used: %d/%d bits (%.1f%%)\n", used, ac_bits, 100.0*used/ac_bits);
        printf("  Avg NZ/block: %.1f, Avg bits/NZ: %.1f\n",
               (double)total_nz/completed, total_nz>0 ? (double)used/total_nz : 0);
        printf("  Bands: ");
        for (int i = 0; i < 8; i++) printf("b%d=%d ", i, band_nz[i]);
        printf("\n");
    }

    /* ============================================================
     * Test 2: JPEG AC with interleaved DC+AC (not separate DC pass)
     * ============================================================ */
    printf("\n=== Test 2: JPEG AC interleaved with DC ===\n");
    {
        static int coeffs[900][64];
        memset(coeffs, 0, sizeof(coeffs));
        bitstream bs = {bsdata, total_bits, 0};
        int dcp[3]={0,0,0};
        int completed = 0, total_nz = 0, total_eob = 0, nomatch = 0;
        int band_nz[8] = {0};

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dcp[comp]+=read_dc(&bs);
            coeffs[b][0] = dcp[comp] * 8;

            int pos = 1;
            while (pos < 64 && bs.pos < total_bits) {
                int run, level;
                int rc = read_jpeg_ac(&bs, &run, &level);
                if (rc < 0) { nomatch++; bs.pos++; continue; }
                if (run == -1) { total_eob++; break; }
                pos += run;
                if (pos >= 64) break;
                if (level != 0) {
                    coeffs[b][zigzag[pos]] = level;
                    int band = (pos-1)/8;
                    if (band < 8) band_nz[band]++;
                    total_nz++;
                }
                pos++;
            }
            completed++;
        }
        int used = bs.pos;
        printf("  Blocks: %d, NZ: %d, EOB: %d, NoMatch: %d\n", completed, total_nz, total_eob, nomatch);
        printf("  Used: %d/%d bits (%.1f%%)\n", used, total_bits, 100.0*used/total_bits);
        printf("  Bands: ");
        for (int i = 0; i < 8; i++) printf("b%d=%d ", i, band_nz[i]);
        printf("\n");
    }

    /* ============================================================
     * Test 3: QS vs padding/data analysis across all frames
     * ============================================================ */
    printf("\n=== Test 3: QS vs frame content analysis ===\n");
    {
        for (int fi = 0; fi < nf && fi < 20; fi++) {
            uint8_t *ff = frames[fi];
            int ffs = fsizes[fi];
            int fqs = ff[3], ft = ff[39];
            int fbl = ffs - 40;

            /* Count trailing 0xFF bytes */
            int ff_pad = 0;
            for (int i = fbl - 1; i >= 0; i--) {
                if (ff[40+i] == 0xFF) ff_pad++;
                else break;
            }

            /* Count trailing 0x00 bytes */
            int z_pad = 0;
            for (int i = fbl - 1; i >= 0; i--) {
                if (ff[40+i] == 0x00) z_pad++;
                else break;
            }

            /* Count DC bits */
            const uint8_t *fbd = ff + 40;
            int ftb = fbl * 8;
            bitstream bs = {fbd, ftb, 0};
            int dp[3]={0,0,0};
            for(int b=0;b<nblocks&&bs.pos<ftb;b++){
                int comp=(b%6<4)?0:(b%6==4)?1:2;
                dp[comp]+=read_dc(&bs);
            }
            int dc_bits = bs.pos;

            /* Check last 16 bytes */
            printf("  F%02d: qs=%2d type=%d dc=%4d  pad_FF=%3d pad_00=%3d  last8=",
                   fi, fqs, ft, dc_bits, ff_pad, z_pad);
            for (int i = fbl-8; i < fbl; i++)
                printf("%02X", ff[40+i]);
            printf("\n");
        }
    }

    /* ============================================================
     * Test 4: Try MPEG-1 AC VLC but with DC+AC interleaved per block
     * (We know separate DC pass + MPEG AC VLC is wrong due to uniform bands,
     *  but what about interleaved? The DC is coded first in each block,
     *  then AC follows immediately)
     * ============================================================ */
    printf("\n=== Test 4: MPEG-1 AC VLC interleaved with DC ===\n");
    {
        /* MPEG-1 Table B.14 AC VLC (first 30 most common entries) */
        static const struct { uint16_t code; int len; int run; int level; } mpeg_ac[] = {
            /* Special: EOB */
            {0b10, 2, -1, 0},
            /* Regular entries */
            {0b11, 2, 0, 1},
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
            /* Escape */
            {0b000001, 6, -2, 0},
        };
        #define MPEG_AC_COUNT (sizeof(mpeg_ac)/sizeof(mpeg_ac[0]))

        bitstream bs = {bsdata, total_bits, 0};
        int dcp[3]={0,0,0};
        int completed=0, total_nz=0, total_eob=0, nomatch=0;
        int band_nz[8]={0};

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dcp[comp]+=read_dc(&bs);

            int pos = 1;
            while (pos < 64 && bs.pos < total_bits) {
                int matched = 0;
                for (int t = 0; t < (int)MPEG_AC_COUNT; t++) {
                    int bits = bs_peek(&bs, mpeg_ac[t].len);
                    if (bits == mpeg_ac[t].code) {
                        bs.pos += mpeg_ac[t].len;
                        if (mpeg_ac[t].run == -1) { total_eob++; matched=1; goto eob_inter; }
                        if (mpeg_ac[t].run == -2) {
                            /* Escape: 6-bit run + 8-bit level */
                            int r = bs_read(&bs, 6);
                            int l = bs_read(&bs, 8);
                            if (r >= 0 && l >= 0) {
                                pos += r;
                                if (pos < 64) {
                                    int band = (pos-1)/8;
                                    if (band < 8) band_nz[band]++;
                                    total_nz++;
                                }
                            }
                            pos++;
                            matched = 1;
                            break;
                        }
                        /* Sign bit */
                        int sign = bs_read(&bs, 1);
                        int level = mpeg_ac[t].level;
                        if (sign) level = -level;
                        pos += mpeg_ac[t].run;
                        if (pos < 64) {
                            int band = (pos-1)/8;
                            if (band < 8) band_nz[band]++;
                            total_nz++;
                        }
                        pos++;
                        matched = 1;
                        break;
                    }
                }
                if (!matched) { nomatch++; bs.pos++; }
            }
            eob_inter:
            completed++;
        }
        int used = bs.pos;
        printf("  Blocks: %d, NZ: %d, EOB: %d, NoMatch: %d\n", completed, total_nz, total_eob, nomatch);
        printf("  Used: %d/%d bits (%.1f%%)\n", used, total_bits, 100.0*used/total_bits);
        printf("  Bands: ");
        for (int i = 0; i < 8; i++) printf("b%d=%d ", i, band_nz[i]);
        printf("\n");
    }

    /* ============================================================
     * Test 5: Look for block-level structure
     * What if after DC, the AC data is organized differently?
     * Try reading the raw bits and look for repeating structural patterns
     * at macroblock boundaries (every 6 blocks)
     * ============================================================ */
    printf("\n=== Test 5: Bit pattern analysis at block boundaries ===\n");
    {
        /* Decode DC and record bit positions */
        bitstream bs = {bsdata, total_bits, 0};
        int dcp[3]={0,0,0};
        int block_bit_start[900];
        for(int b=0;b<nblocks&&bs.pos<total_bits;b++){
            block_bit_start[b] = bs.pos;
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dcp[comp]+=read_dc(&bs);
        }
        int dc_end_pos = bs.pos;

        /* If DC is interleaved with AC, the "dc" we decoded would actually
           read into AC territory. Let's check what happens if we decode
           only the first macroblock's DC (6 blocks) and look at bit positions */
        printf("  DC bit positions for first 3 macroblocks:\n");
        for (int mb = 0; mb < 3; mb++) {
            printf("  MB%d: ", mb);
            for (int s = 0; s < 6; s++) {
                int b = mb*6+s;
                printf("blk%d@%d ", s, block_bit_start[b]);
            }
            printf("\n");
        }

        /* Average DC bits per block */
        int total_dc_bits = 0;
        for (int b = 1; b < nblocks; b++)
            total_dc_bits += block_bit_start[b] - block_bit_start[b-1];
        printf("  Avg DC bits/block: %.2f\n", (double)total_dc_bits/(nblocks-1));

        /* Look at bits right after DC ends - are there any marker patterns? */
        printf("  Bits after DC (first 128): ");
        bs.pos = dc_end_pos;
        for (int i = 0; i < 128 && bs.pos < total_bits; i++) {
            printf("%d", bs_read(&bs, 1));
            if (i % 8 == 7) printf(" ");
        }
        printf("\n");
    }

    /* ============================================================
     * Test 6: What if AC uses a different zigzag scan order?
     * Some codecs (H.263, MPEG-4) use alternate scan patterns
     * ============================================================ */
    printf("\n=== Test 6: Alternate scan patterns ===\n");
    {
        /* H.263/MPEG-4 alternate scan (column-major for interlaced) */
        static const int alt_zigzag[64] = {
            0, 8, 16, 24, 1, 9, 2, 3, 10, 17, 25, 32, 40, 48, 56, 33,
            26, 18, 11, 4, 5, 12, 19, 27, 34, 41, 49, 57, 58, 50, 42, 35,
            28, 20, 13, 6, 7, 14, 21, 29, 36, 43, 51, 59, 37, 44, 52, 60,
            61, 53, 45, 38, 30, 22, 15, 23, 31, 39, 46, 54, 62, 55, 47, 63
        };
        printf("  (Alternate zigzag only matters once we find the right VLC)\n");
    }

    /* ============================================================
     * Test 7: What if the entire bitstream (including DC) uses
     * a different bit ordering within each byte?
     * MSB-first is confirmed for DC, but what about nibble-swapped?
     * ============================================================ */
    printf("\n=== Test 7: Nibble-swapped AC bytes ===\n");
    {
        /* Create nibble-swapped copy of AC data */
        int ac_start_byte = dc_end / 8;
        uint8_t *swapped = malloc(bslen);
        memcpy(swapped, bsdata, bslen);
        for (int i = ac_start_byte; i < bslen; i++)
            swapped[i] = ((bsdata[i] & 0x0F) << 4) | ((bsdata[i] & 0xF0) >> 4);

        /* Try MPEG-1 AC VLC on nibble-swapped data */
        static const struct { uint16_t code; int len; int run; int level; } mpeg_ac2[] = {
            {0b10, 2, -1, 0},
            {0b11, 2, 0, 1},
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
            {0b000001, 6, -2, 0},
        };

        bitstream bs = {swapped, total_bits, dc_end};
        int total_nz=0, total_eob=0;

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int pos = 1;
            while (pos < 64 && bs.pos < total_bits) {
                int matched = 0;
                for (int t = 0; t < 17; t++) {
                    int bits = bs_peek(&bs, mpeg_ac2[t].len);
                    if (bits == mpeg_ac2[t].code) {
                        bs.pos += mpeg_ac2[t].len;
                        if (mpeg_ac2[t].run == -1) { total_eob++; matched=1; goto eob7; }
                        if (mpeg_ac2[t].run == -2) { bs.pos += 14; pos++; matched=1; break; }
                        bs.pos += 1;  /* sign */
                        pos += mpeg_ac2[t].run + 1;
                        total_nz++;
                        matched = 1;
                        break;
                    }
                }
                if (!matched) { bs.pos++; }
            }
            eob7:;
        }
        int used = bs.pos - dc_end;
        printf("  Nibble-swapped MPEG AC: NZ=%d, EOB=%d, %d/%d bits (%.1f%%)\n",
               total_nz, total_eob, used, ac_bits, 100.0*used/ac_bits);
        free(swapped);
    }

    /* ============================================================
     * Test 8: What if there's a global Huffman table in the bitstream?
     * Look for any structural patterns in the first few bytes of AC
     * that could be a table definition
     * ============================================================ */
    printf("\n=== Test 8: AC bitstream structure analysis ===\n");
    {
        /* Check if there's a length field at the start of AC data */
        bitstream bs = {bsdata, total_bits, dc_end};

        printf("  First 16 bits after DC: ");
        int first16 = bs_peek(&bs, 16);
        printf("0x%04X = %d\n", first16, first16);

        /* Try as various length fields */
        printf("  As 8-bit: %d (bytes=%d, %d%% of AC)\n", first16>>8,
               first16>>8, 100*(first16>>8)*8/ac_bits);
        printf("  As 12-bit: %d (bytes=%d, %d%% of AC)\n", first16>>4,
               (first16>>4)*8, 100*(first16>>4)/ac_bits);

        /* Look at byte values at regular intervals */
        printf("  Every 108 bits (1 block worth): ");
        for (int i = 0; i < 20 && dc_end + i*108 < total_bits; i++) {
            bs.pos = dc_end + i * 108;
            int val = bs_peek(&bs, 8);
            printf("%02X ", val);
        }
        printf("\n");

        /* Entropy of AC data in 1024-byte windows */
        printf("  Entropy by position (1KB windows):\n");
        int ac_start_byte = (dc_end + 7) / 8;
        for (int w = 0; w < 10 && ac_start_byte + (w+1)*1024 <= bslen; w++) {
            int hist[256] = {0};
            for (int i = 0; i < 1024; i++)
                hist[bsdata[ac_start_byte + w*1024 + i]]++;
            double ent = 0;
            for (int i = 0; i < 256; i++) {
                if (hist[i] == 0) continue;
                double p = hist[i] / 1024.0;
                ent -= p * log2(p);
            }
            printf("    Window %d (byte %d): entropy=%.3f bits/byte\n",
                   w, ac_start_byte + w*1024, ent);
        }
    }

    /* ============================================================
     * Test 9: Image output for JPEG AC decode
     * ============================================================ */
    printf("\n=== Test 9: JPEG AC image output ===\n");
    {
        static int coeffs[900][64];
        memset(coeffs, 0, sizeof(coeffs));
        bitstream bs = {bsdata, total_bits, dc_end};

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            coeffs[b][0] = dc_vals[b] * 8;
            int pos = 1;
            while (pos < 64 && bs.pos < total_bits) {
                int run, level;
                if (read_jpeg_ac(&bs, &run, &level) < 0) { bs.pos++; continue; }
                if (run == -1) break;
                pos += run;
                if (pos >= 64) break;
                if (level != 0) {
                    int zpos = zigzag[pos];
                    int qi = zpos % 16;  /* qtable repeats every 16 */
                    int dequant = level * qtable_vals[qi] * qs / 8;
                    coeffs[b][zpos] = dequant;
                }
                pos++;
            }
        }

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
        write_ppm("/tmp/ac_jpeg.ppm", Yp, Cbp, Crp);
        printf("  → /tmp/ac_jpeg.ppm\n");
    }

    /* ============================================================
     * Test 10: Cross-frame QS vs data relationship
     * ============================================================ */
    printf("\n=== Test 10: QS patterns across frames ===\n");
    {
        int qs_counts[64] = {0};
        int type_counts[4] = {0};
        for (int fi = 0; fi < nf; fi++) {
            int fqs = frames[fi][3];
            int ft = frames[fi][39];
            if (fqs < 64) qs_counts[fqs]++;
            if (ft < 4) type_counts[ft]++;
        }
        printf("  Total frames: %d\n", nf);
        printf("  Types: I=%d P=%d\n", type_counts[0], type_counts[1]);
        printf("  QS distribution: ");
        for (int i = 0; i < 64; i++)
            if (qs_counts[i] > 0) printf("qs%d=%d ", i, qs_counts[i]);
        printf("\n");
    }

    free(disc); zip_close(z);
    return 0;
}
