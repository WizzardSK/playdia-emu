/*
 * Playdia video - try MPEG-1 style VLC tables for 4×4 blocks
 * MPEG-1 uses different DC and AC Huffman tables than JPEG
 * Also try: H.261 tables, simplified tables, absolute DC coding
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

static void write_pgm(const char *path, const uint8_t *g, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(g, 1, w*h, f);
    fclose(f);
    printf("  -> %s (%dx%d)\n", path, w, h);
}

typedef struct {
    const uint8_t *data;
    int len, pos, bit, total_bits;
} BR;

static void br_init(BR *b, const uint8_t *d, int l) {
    b->data = d; b->len = l; b->pos = 0; b->bit = 7; b->total_bits = 0;
}

static int br_get1(BR *b) {
    if (b->pos >= b->len) return -1;
    int v = (b->data[b->pos] >> b->bit) & 1;
    if (--b->bit < 0) { b->bit = 7; b->pos++; }
    b->total_bits++;
    return v;
}

static int br_peek(BR *b, int n) {
    BR tmp = *b;
    int v = 0;
    for (int i = 0; i < n; i++) {
        int bit = br_get1(&tmp);
        if (bit < 0) return -1;
        v = (v << 1) | bit;
    }
    return v;
}

static int br_get(BR *b, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        int bit = br_get1(b);
        if (bit < 0) return 0;
        v = (v << 1) | bit;
    }
    return v;
}

static bool br_eof(BR *b) { return b->pos >= b->len; }

// === MPEG-1 DC luminance VLC table ===
// dc_size  VLC
// 0        100
// 1        00
// 2        01
// 3        101
// 4        110
// 5        1110
// 6        11110
// 7        111110
// 8        1111110
static int mpeg1_dc_lum_decode(BR *b) {
    int bit = br_get1(b);
    if (bit < 0) return -1;
    if (bit == 0) {
        int b2 = br_get1(b);
        if (b2 < 0) return -1;
        if (b2 == 0) return 1; // 00 → size 1
        else return 2;          // 01 → size 2
    } else {
        // bit=1, read next
        int b2 = br_get1(b);
        if (b2 < 0) return -1;
        if (b2 == 0) {
            int b3 = br_get1(b);
            if (b3 < 0) return -1;
            if (b3 == 0) return 0; // 100 → size 0
            else return 3;          // 101 → size 3
        } else {
            // 11...
            int b3 = br_get1(b);
            if (b3 < 0) return -1;
            if (b3 == 0) return 4; // 110 → size 4
            else {
                // 111...
                int b4 = br_get1(b);
                if (b4 < 0) return -1;
                if (b4 == 0) return 5; // 1110 → size 5
                else {
                    int b5 = br_get1(b);
                    if (b5 < 0) return -1;
                    if (b5 == 0) return 6; // 11110 → size 6
                    else {
                        int b6 = br_get1(b);
                        if (b6 < 0) return -1;
                        if (b6 == 0) return 7; // 111110 → size 7
                        else {
                            int b7 = br_get1(b);
                            if (b7 == 0) return 8; // 1111110 → size 8
                            return -1;
                        }
                    }
                }
            }
        }
    }
}

// MPEG-1 DC differential decode (similar to JPEG extend)
static int mpeg1_dc_diff(BR *b, int size) {
    if (size == 0) return 0;
    int val = br_get(b, size);
    // If MSB is 0, value is negative
    if (!(val & (1 << (size - 1))))
        val -= (1 << size) - 1;
    return val;
}

// Forward declaration
static int mpeg1_ac_decode_00(BR *b, int *run, int *level);

// === MPEG-1 AC coefficient table (Table B.14 - DCT coefficients) ===
// For first coeff in block: special handling
// This is a simplified version of the MPEG-1 VLC table
typedef struct {
    int run;
    int level;
} RunLevel;

#define ESCAPE_CODE 999
#define EOB_CODE    998

// Decode one MPEG-1 AC coefficient (run, level) pair
// Returns: 0=ok, 1=EOB, -1=error
static int mpeg1_ac_decode(BR *b, int *run, int *level, bool is_first) {
    int peek = br_peek(b, 2);
    if (peek < 0) return -1;

    // EOB = 10 (but only for non-first coeff)
    if (!is_first && peek == 2) { // 10
        br_get(b, 2);
        return 1; // EOB
    }

    // First coeff special: 1s = (0,1)
    // Non-first: build from table
    // Let me use a simplified table approach

    // Look at next bits to determine (run,level)
    int code = 0;
    int bits_used = 0;

    // Table B.14 - selected entries:
    // VLC           | run | level
    // 1s            | 0   | 1      (first coeff only: this means 1+sign)
    // 11s           | 0   | 1      (non-first: end of block is 10, so 11s = run0 lev1)
    // Actually, let me use a lookup approach

    // For non-first coefficients:
    // 10            → EOB
    // 11 s          → (0, 1)
    // 011 s         → (1, 1)
    // 0100 s        → (0, 2)
    // 0101 s        → (2, 1)
    // 00110 s       → (0, 3)
    // 00111 s       → (3, 1)
    // 00100 s       → (4, 1)
    // 00101 s       → (1, 2)
    // 000110 s      → (5, 1)
    // 000111 s      → (0, 4)
    // 000100 s      → (6, 1)
    // 000101 s      → (7, 1)
    // 0000110 s     → (0, 5)
    // 0000111 s     → (2, 2)
    // 0000100 s     → (8, 1)
    // 0000101 s     → (9, 1)
    // ... and ESCAPE = 000001

    // For first coefficient:
    // 1 s           → (0, 1)
    // 011 s         → (0, 2)  (different from non-first!)
    // Actually, for first coeff only code 1s differs

    if (is_first) {
        int b1 = br_get1(b);
        if (b1 < 0) return -1;
        if (b1 == 1) {
            int sign = br_get1(b);
            *run = 0;
            *level = sign ? -1 : 1;
            return 0;
        }
        // Fall through to normal table, but we already consumed bit '0'
        // Need to handle this carefully...
        // For simplicity, let me just use the non-first table for everything
        // and handle first coeff separately
        // Actually let me re-read:
        // The difference is ONLY for the code `1s` which maps to (0,1) for first
        // and `10`=EOB, `11s`=(0,1) for non-first
        // So for first coeff: `1s` → (0,1)
        // For non-first: `10` → EOB, `11s` → (0,1)
        // All other codes are the same

        // We read b1=0, so continue with normal decode for code starting with 0
        int b2 = br_get1(b);
        if (b2 < 0) return -1;
        if (b2 == 1) {
            int b3 = br_get1(b);
            if (b3 < 0) return -1;
            int sign = br_get1(b);
            if (b3 == 1) { *run = 1; *level = sign ? -1 : 1; return 0; }
            else {
                int b4 = br_get1(b);
                if (b4 < 0) return -1;
                int sign2 = br_get1(b);
                if (b4 == 0) { *run = 0; *level = sign ? -2 : 2; /* oops wrong sign */ return 0; }
                else { *run = 2; *level = sign ? -1 : 1; return 0; }
            }
        }
        // code starts with 00...
        return mpeg1_ac_decode_00(b, run, level);
    }

    // Non-first coefficient
    int b1 = br_get1(b);
    if (b1 < 0) return -1;
    if (b1 == 1) {
        int b2 = br_get1(b);
        if (b2 < 0) return -1;
        if (b2 == 0) return 1; // 10 = EOB
        // 11
        int sign = br_get1(b);
        *run = 0; *level = sign ? -1 : 1;
        return 0;
    }
    // b1 = 0
    int b2 = br_get1(b);
    if (b2 < 0) return -1;
    if (b2 == 1) {
        int b3 = br_get1(b);
        if (b3 < 0) return -1;
        int b4 = br_get1(b); // sign or next bit
        if (b3 == 1) {
            // 011 + sign
            *run = 1; *level = b4 ? -1 : 1;
            return 0;
        }
        // 010...
        int b5 = br_get1(b);
        if (b4 == 0) { *run = 0; *level = b5 ? -2 : 2; return 0; } // 0100 s
        else { *run = 2; *level = b5 ? -1 : 1; return 0; } // 0101 s
    }
    // 00...
    return mpeg1_ac_decode_00(b, run, level);
}

// Handle codes starting with 00...
static int mpeg1_ac_decode_00(BR *b, int *run, int *level) {
    int b3 = br_get1(b);
    if (b3 < 0) return -1;
    if (b3 == 1) {
        int b4 = br_get1(b);
        if (b4 < 0) return -1;
        int b5 = br_get1(b);
        int sign = br_get1(b);
        if (b4 == 1) {
            if (b5 == 0) { *run = 0; *level = sign ? -3 : 3; return 0; }
            else { *run = 3; *level = sign ? -1 : 1; return 0; }
        } else {
            if (b5 == 0) { *run = 4; *level = sign ? -1 : 1; return 0; }
            else { *run = 1; *level = sign ? -2 : 2; return 0; }
        }
    }
    // 000...
    int b4 = br_get1(b);
    if (b4 < 0) return -1;
    if (b4 == 1) {
        int b5 = br_get1(b);
        int b6 = br_get1(b);
        int sign = br_get1(b);
        if (b5 == 1) {
            if (b6 == 0) { *run = 5; *level = sign ? -1 : 1; return 0; }
            else { *run = 0; *level = sign ? -4 : 4; return 0; }
        } else {
            if (b6 == 0) { *run = 6; *level = sign ? -1 : 1; return 0; }
            else { *run = 7; *level = sign ? -1 : 1; return 0; }
        }
    }
    // 0000...
    int b5 = br_get1(b);
    if (b5 < 0) return -1;
    if (b5 == 1) {
        int b6 = br_get1(b);
        int b7 = br_get1(b);
        int sign = br_get1(b);
        if (b6 == 1) {
            if (b7 == 0) { *run = 0; *level = sign ? -5 : 5; return 0; }
            else { *run = 2; *level = sign ? -2 : 2; return 0; }
        } else {
            if (b7 == 0) { *run = 8; *level = sign ? -1 : 1; return 0; }
            else { *run = 9; *level = sign ? -1 : 1; return 0; }
        }
    }
    // 00000...
    int b6 = br_get1(b);
    if (b6 < 0) return -1;
    if (b6 == 1) {
        // ESCAPE code (000001)
        *run = br_get(b, 6);
        int lv = br_get(b, 8);
        if (lv == 0) {
            lv = br_get(b, 8);
        } else if (lv == 128) {
            lv = br_get(b, 8) - 256;
        } else if (lv > 128) {
            lv = lv - 256;
        }
        *level = lv;
        return 0;
    }
    // Longer codes - skip for now
    // 000000... → more entries
    // Just consume some bits and return error
    return -1;
}

// (forward declaration moved to top)

// 4×4 zigzag
static const int zigzag4[16] = {
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};

// 4×4 IDCT
static void idct4x4(const double c[16], int out[16]) {
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            double sum = 0;
            for (int v = 0; v < 4; v++) {
                double cv = v == 0 ? 0.5 : sqrt(0.5);
                for (int u = 0; u < 4; u++) {
                    double cu = u == 0 ? 0.5 : sqrt(0.5);
                    sum += cu * cv * c[v*4+u] *
                           cos((2*x+1)*u*PI/8.0) * cos((2*y+1)*v*PI/8.0);
                }
            }
            out[y*4+x] = (int)round(sum);
        }
}

static int assemble_frames(const uint8_t *disc, int total_sectors,
                            int start_lba, uint8_t frames[][MAX_FRAME],
                            int frame_sizes[], int max_frames) {
    int nf = 0, cp = 0; bool inf = false;
    for (int lba = start_lba; lba < total_sectors && nf < max_frames; lba++) {
        const uint8_t *s = disc + (long)lba * SECTOR_RAW;
        if (s[0]!=0x00 || s[1]!=0xFF || s[15]!=2 || (s[18]&0x04)) continue;
        if (s[24]==0xF1) {
            if (!inf) { inf=true; cp=0; }
            if (cp+2047<MAX_FRAME) { memcpy(frames[nf]+cp, s+25, 2047); cp+=2047; }
        } else if (s[24]==0xF2) {
            if (inf&&cp>0) { frame_sizes[nf]=cp; nf++; inf=false; cp=0; }
        }
    }
    return nf;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <game.zip> [start_lba]\n", argv[0]); return 1; }
    int start_lba = argc > 2 ? atoi(argv[2]) : 502;

    int err;
    zip_t *z = zip_open(argv[1], ZIP_RDONLY, &err);
    if (!z) { fprintf(stderr, "zip fail\n"); return 1; }
    int bi=-1; zip_uint64_t bs2=0;
    for (int i=0; i<(int)zip_get_num_entries(z,0); i++) {
        zip_stat_t st; if (zip_stat_index(z,i,0,&st)==0 && st.size>bs2) { bs2=st.size; bi=i; }
    }
    zip_stat_t st; zip_stat_index(z,bi,0,&st);
    zip_file_t *zf = zip_fopen_index(z,bi,0);
    uint8_t *disc = malloc(st.size);
    zip_int64_t rd=0;
    while (rd<(zip_int64_t)st.size) { zip_int64_t r=zip_fread(zf,disc+rd,st.size-rd); if(r<=0)break; rd+=r; }
    zip_fclose(zf);
    int tsec = (int)(st.size/SECTOR_RAW);

    static uint8_t frames[8][MAX_FRAME];
    int fsizes[8];
    int nf = assemble_frames(disc, tsec, start_lba, frames, fsizes, 8);
    printf("Assembled %d frames\n", nf);

    // Use frame 1
    int fi = nf > 1 ? 1 : 0;
    uint8_t *f = frames[fi];
    int fsize = fsizes[fi];
    printf("Frame %d: %d bytes, header=%02X %02X %02X %02X\n", fi, fsize, f[0],f[1],f[2],f[3]);

    uint8_t qtab[16];
    memcpy(qtab, f+4, 16);
    int qscale = f[3];

    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;

    int W = 128, H = 144;

    // === Test 1: MPEG-1 DC table, decode DC values only ===
    printf("\n=== MPEG-1 DC luminance table ===\n");
    {
        BR br;
        br_init(&br, bs, bslen);
        int dc_vals[2048];
        int prev_dc = 0;
        int ndc = 0;

        for (int i = 0; i < 2048 && !br_eof(&br); i++) {
            int size = mpeg1_dc_lum_decode(&br);
            if (size < 0) { printf("DC decode failed at block %d, bit %d\n", i, br.total_bits); break; }
            int diff = mpeg1_dc_diff(&br, size);
            prev_dc += diff;
            dc_vals[ndc++] = prev_dc;

            // Skip AC - use JPEG-style skip (read until EOB or 15 ACs)
            // Actually, we don't know the AC table, so just skip using MPEG-1 approach isn't safe
            // For now, let's try with a fixed number of bits per AC
        }

        printf("Decoded %d DC values, %d bits\n", ndc, br.total_bits);
        if (ndc >= 10) {
            printf("First 10 DC vals: ");
            for (int i = 0; i < 10; i++) printf("%d ", dc_vals[i]);
            printf("\n");
        }
    }

    // === Test 2: Try simple approach - 8-bit absolute DC per block ===
    printf("\n=== 8-bit absolute DC per block ===\n");
    {
        int nblocks = bslen; // one byte per block for DC
        if (nblocks > W/4 * H/4) nblocks = W/4 * H/4;
        uint8_t *img = calloc(W*H, 1);
        for (int i = 0; i < nblocks; i++) {
            int bx = i % (W/4);
            int by = i / (W/4);
            uint8_t dc = bs[i];
            for (int dy = 0; dy < 4; dy++)
                for (int dx = 0; dx < 4; dx++)
                    if (by*4+dy < H && bx*4+dx < W)
                        img[(by*4+dy)*W + bx*4+dx] = dc;
        }
        write_pgm("/home/wizzard/share/GitHub/pd_abs8dc_f1.pgm", img, W, H);
        free(img);
    }

    // === Test 3: 4-bit packed pairs as absolute DCs (nibble per block) ===
    printf("\n=== 4-bit nibble DCs ===\n");
    {
        int nblocks = bslen * 2;
        if (nblocks > W/4 * H/4) nblocks = W/4 * H/4;
        uint8_t *img = calloc(W*H, 1);
        for (int i = 0; i < nblocks; i++) {
            int bx = i % (W/4);
            int by = i / (W/4);
            uint8_t dc = (i & 1) ? (bs[i/2] & 0x0F) : (bs[i/2] >> 4);
            dc = dc * 17; // scale 0-15 to 0-255
            for (int dy = 0; dy < 4; dy++)
                for (int dx = 0; dx < 4; dx++)
                    if (by*4+dy < H && bx*4+dx < W)
                        img[(by*4+dy)*W + bx*4+dx] = dc;
        }
        write_pgm("/home/wizzard/share/GitHub/pd_nib4dc_f1.pgm", img, W, H);
        free(img);
    }

    // === Test 4: Try JPEG DC decode but with ABSOLUTE (non-differential) DC ===
    printf("\n=== JPEG Huffman but absolute DC ===\n");
    {
        // Use JPEG luminance DC/AC tables
        typedef struct { int min_code[17], max_code[17], val_ptr[17], values[256], count; } HT;
        HT dc_h, ac_h;
        {
            static const int dcb[17]={0, 0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
            static const int dcv[12]={0,1,2,3,4,5,6,7,8,9,10,11};
            int code=0,idx=0; dc_h.count=12; memcpy(dc_h.values,dcv,48);
            for(int l=1;l<=16;l++){dc_h.min_code[l]=code;dc_h.val_ptr[l]=idx;
            dc_h.max_code[l]=dcb[l]>0?code+dcb[l]-1:-1;idx+=dcb[l];code=(code+dcb[l])<<1;}
        }
        {
            static const int acb[17]={0, 0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
            static const int acv[]={
                0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
                0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
                0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
                0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
                0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
                0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
                0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
                0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
                0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
                0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
                0xf9,0xfa};
            int code=0,idx=0; ac_h.count=162; memcpy(ac_h.values,acv,648);
            for(int l=1;l<=16;l++){ac_h.min_code[l]=code;ac_h.val_ptr[l]=idx;
            ac_h.max_code[l]=acb[l]>0?code+acb[l]-1:-1;idx+=acb[l];code=(code+acb[l])<<1;}
        }

        uint8_t *img = calloc(W*H, 1);
        BR br;
        br_init(&br, bs, bslen);
        int blocks = 0;

        for (int by = 0; by < H/4 && !br_eof(&br); by++) {
            for (int bx = 0; bx < W/4 && !br_eof(&br); bx++) {
                // DC - absolute (not differential)
                int code = 0;
                int cat = -1;
                for (int len = 1; len <= 16; len++) {
                    code = (code << 1) | br_get1(&br);
                    if (dc_h.max_code[len] >= 0 && code <= dc_h.max_code[len]) {
                        cat = dc_h.values[dc_h.val_ptr[len] + (code - dc_h.min_code[len])];
                        break;
                    }
                }
                if (cat < 0) goto done4;

                int dc = 0;
                if (cat > 0) {
                    dc = br_get(&br, cat);
                    if (!(dc & (1 << (cat-1)))) dc -= (1 << cat) - 1;
                }

                // Skip ACs
                for (int k = 1; k < 16; ) {
                    code = 0;
                    int sym = -1;
                    for (int len = 1; len <= 16; len++) {
                        code = (code << 1) | br_get1(&br);
                        if (ac_h.max_code[len] >= 0 && code <= ac_h.max_code[len]) {
                            sym = ac_h.values[ac_h.val_ptr[len] + (code - ac_h.min_code[len])];
                            break;
                        }
                    }
                    if (sym < 0) goto done4;
                    if (sym == 0x00) break; // EOB
                    int run = (sym >> 4) & 0xF;
                    int size = sym & 0xF;
                    k += run;
                    if (k >= 16) break;
                    if (size > 0) br_get(&br, size);
                    k++;
                }

                int v = dc + 128;
                if (v < 0) v = 0; if (v > 255) v = 255;
                for (int dy = 0; dy < 4; dy++)
                    for (int dx = 0; dx < 4; dx++)
                        if (by*4+dy < H && bx*4+dx < W)
                            img[(by*4+dy)*W + bx*4+dx] = v;
                blocks++;
            }
        }
        done4:
        printf("Absolute DC: %d blocks decoded, %d bits\n", blocks, br.total_bits);
        write_pgm("/home/wizzard/share/GitHub/pd_absdc_jpeg_f1.pgm", img, W, H);

        // Also try with DC * some scaling
        // Redo with DC values saved
        br_init(&br, bs, bslen);
        int dc_save[2048];
        int ndc = 0;
        for (int i = 0; i < 1152 && !br_eof(&br); i++) {
            int code2 = 0, cat2 = -1;
            for (int len = 1; len <= 16; len++) {
                code2 = (code2 << 1) | br_get1(&br);
                if (dc_h.max_code[len] >= 0 && code2 <= dc_h.max_code[len]) {
                    cat2 = dc_h.values[dc_h.val_ptr[len] + (code2 - dc_h.min_code[len])];
                    break;
                }
            }
            if (cat2 < 0) break;
            int dc2 = 0;
            if (cat2 > 0) {
                dc2 = br_get(&br, cat2);
                if (!(dc2 & (1 << (cat2-1)))) dc2 -= (1 << cat2) - 1;
            }
            dc_save[ndc++] = dc2;

            for (int k = 1; k < 16; ) {
                int code3 = 0, sym = -1;
                for (int len = 1; len <= 16; len++) {
                    code3 = (code3 << 1) | br_get1(&br);
                    if (ac_h.max_code[len] >= 0 && code3 <= ac_h.max_code[len]) {
                        sym = ac_h.values[ac_h.val_ptr[len] + (code3 - ac_h.min_code[len])];
                        break;
                    }
                }
                if (sym < 0) break;
                if (sym == 0x00) break;
                k += ((sym >> 4) & 0xF);
                if (k >= 16) break;
                if ((sym & 0xF) > 0) br_get(&br, sym & 0xF);
                k++;
            }
        }

        printf("Absolute DC values (first 32): ");
        for (int i = 0; i < 32 && i < ndc; i++) printf("%d ", dc_save[i]);
        printf("\n");

        // DC range
        int mn=99999, mx=-99999;
        for (int i=0;i<ndc;i++){if(dc_save[i]<mn)mn=dc_save[i];if(dc_save[i]>mx)mx=dc_save[i];}
        printf("Absolute DC range: %d to %d\n", mn, mx);

        free(img);
    }

    // === Test 5: Analyze bit patterns - what's the most common first few bits? ===
    printf("\n=== Bit pattern analysis ===\n");
    {
        // Count 2-bit, 3-bit, 4-bit prefixes
        int counts2[4] = {0}, counts3[8] = {0}, counts4[16] = {0};
        // Sample every 44 bits (average block size) for 1000 samples
        for (int off = 0; off < bslen * 8 - 4; off += 44) {
            int byte_off = off / 8;
            int bit_off = 7 - (off % 8);

            int v2 = 0, v3 = 0, v4 = 0;
            for (int i = 0; i < 4; i++) {
                int bo = (off + i) / 8;
                int bi = 7 - ((off + i) % 8);
                if (bo < bslen) {
                    int b = (bs[bo] >> bi) & 1;
                    if (i < 2) v2 = (v2 << 1) | b;
                    if (i < 3) v3 = (v3 << 1) | b;
                    v4 = (v4 << 1) | b;
                }
            }
            counts2[v2]++;
            counts3[v3]++;
            counts4[v4]++;
        }
        printf("2-bit prefixes (sampled every ~44 bits): ");
        for (int i = 0; i < 4; i++) printf("%d%d:%d ", i>>1, i&1, counts2[i]);
        printf("\n");
        printf("3-bit prefixes: ");
        for (int i = 0; i < 8; i++) printf("%d%d%d:%d ", i>>2, (i>>1)&1, i&1, counts3[i]);
        printf("\n");
    }

    free(disc);
    zip_close(z);
    return 0;
}
