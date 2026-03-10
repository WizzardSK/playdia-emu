/*
 * vcodec_runlevel_eg.c - Test run-level coding with exp-Golomb and other schemes
 *
 * Hypothesis: AC bitstream uses (run, level) pairs where:
 *   - run = number of zero coefficients before next non-zero
 *   - level = value of non-zero coefficient
 * The ~27000 exp-Golomb values for ~54000 positions suggests run-level pairs.
 *
 * Also tests: Rice coding, fixed-length with length prefix, Golomb-Rice hybrid
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

/* Read unsigned exp-Golomb */
static int read_ue(bitstream *bs) {
    int zeros = 0;
    while (bs->pos < bs->total_bits) {
        if (bs_read(bs, 1) == 1) break;
        zeros++;
        if (zeros > 24) return -1;
    }
    int val = (1 << zeros) - 1;
    if (zeros > 0) {
        int extra = bs_read(bs, zeros);
        if (extra < 0) return -1;
        val += extra;
    }
    return val;
}

/* Read signed exp-Golomb: 0→0, 1→+1, 2→-1, 3→+2, 4→-2 */
static int read_se(bitstream *bs) {
    int code = read_ue(bs);
    if (code < 0) return -9999;
    if (code == 0) return 0;
    int val = (code + 1) / 2;
    if (code % 2 == 0) val = -val;
    return val;
}

/* Read unary code (count of 1s terminated by 0, or count of 0s terminated by 1) */
static int read_unary0(bitstream *bs) {  /* count 0s, terminated by 1 */
    int n = 0;
    while (bs->pos < bs->total_bits) {
        if (bs_read(bs, 1) == 1) return n;
        n++;
        if (n > 63) return -1;
    }
    return -1;
}
static int read_unary1(bitstream *bs) {  /* count 1s, terminated by 0 */
    int n = 0;
    while (bs->pos < bs->total_bits) {
        if (bs_read(bs, 1) == 0) return n;
        n++;
        if (n > 63) return -1;
    }
    return -1;
}

/* Rice code: unary(q) + fixed(r) where val = q*M + r */
static int read_rice(bitstream *bs, int k) {
    int q = read_unary0(bs);
    if (q < 0) return -1;
    int r = (k > 0) ? bs_read(bs, k) : 0;
    if (r < 0) return -1;
    return q * (1 << k) + r;
}

static const int zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static const uint8_t qtable[64] = {
    10,20,14,13,18,37,22,28,15,24,15,18,18,31,17,20,
    10,20,14,13,18,37,22,28,15,24,15,18,18,31,17,20,
    10,20,14,13,18,37,22,28,15,24,15,18,18,31,17,20,
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
    for (int i=0;i<(int)zip_get_num_entries(z,0);i++){
        zip_stat_t st;if(zip_stat_index(z,i,0,&st)==0&&st.size>bsz){bsz=st.size;bi=i;}}
    zip_stat_t st; zip_stat_index(z,bi,0,&st);
    zip_file_t *zf=zip_fopen_index(z,bi,0);
    uint8_t *disc=malloc(st.size);
    zip_int64_t rd=0;
    while(rd<(zip_int64_t)st.size){zip_int64_t r=zip_fread(zf,disc+rd,st.size-rd);if(r<=0)break;rd+=r;}
    zip_fclose(zf);
    int tsec=(int)(st.size/SECTOR_RAW);

    static uint8_t frames[8][MAX_FRAME]; int fsizes[8];
    int nf=assemble_frames(disc,tsec,start_lba,frames,fsizes,8);
    if(nf==0){printf("No frames\n");return 1;}

    uint8_t *f=frames[0]; int fsize=fsizes[0];
    int qs=f[3];
    const uint8_t *bsdata=f+40;
    int bslen=fsize-40, total_bits=bslen*8;
    int mw=W/16, mh=H/16, nblocks=mw*mh*6;

    printf("Frame 0: qs=%d, fsize=%d, bitstream=%d bits\n\n", qs, fsize, total_bits);

    /* Decode DC for all tests */
    bitstream bs0 = {bsdata, total_bits, 0};
    int dc_vals[900];
    int dc_pred[3]={0,0,0};
    for(int b=0;b<nblocks&&bs0.pos<total_bits;b++){
        int comp=(b%6<4)?0:(b%6==4)?1:2;
        dc_pred[comp]+=read_dc(&bs0);
        dc_vals[b]=dc_pred[comp];
    }
    int dc_end=bs0.pos;
    int ac_bits=total_bits-dc_end;
    printf("DC: %d bits (%.1f%%), AC start at bit %d, %d bits remaining\n\n",
           dc_end, 100.0*dc_end/total_bits, dc_end, ac_bits);

    /* ============================================================
     * Test 1: Exp-Golomb run-level pairs
     * Each pair: run = ue(), level = se()
     * EOB when position reaches 63 or special marker
     * ============================================================ */
    printf("=== Test 1: Exp-Golomb run-level (ue=run, se=level) ===\n");
    {
        static int coeffs[900][64];
        memset(coeffs, 0, sizeof(coeffs));
        bitstream bs = {bsdata, total_bits, dc_end};
        int completed = 0, errors = 0;
        int run_hist[64] = {0};
        int level_hist[32] = {0};
        int total_nz = 0;

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            coeffs[b][0] = dc_vals[b] * 8;
            int pos = 1;
            while (pos < 64 && bs.pos < total_bits) {
                int run = read_ue(&bs);
                if (run < 0 || run > 63) { errors++; break; }
                pos += run;
                if (pos >= 64) break;
                int level = read_se(&bs);
                if (level == -9999) { errors++; break; }
                if (level == 0) break;  /* EOB? */
                coeffs[b][zigzag[pos]] = level;
                if (run < 64) run_hist[run]++;
                if (abs(level) < 32) level_hist[abs(level)]++;
                total_nz++;
                pos++;
            }
            if (errors == 0) completed++;
        }
        int used = bs.pos - dc_end;
        printf("  Completed %d/%d blocks, errors=%d\n", completed, nblocks, errors);
        printf("  Used %d/%d AC bits (%.1f%%)\n", used, ac_bits, 100.0*used/ac_bits);
        printf("  Total NZ: %d (%.1f per block)\n", total_nz, (double)total_nz/nblocks);
        printf("  Runs: "); for(int i=0;i<10;i++) printf("r%d=%d ",i,run_hist[i]); printf("\n");
        printf("  Levels: "); for(int i=0;i<10;i++) printf("l%d=%d ",i,level_hist[i]); printf("\n");
    }

    /* ============================================================
     * Test 2: Rice coding for run-level
     * run = rice(k=1), level = rice(k=0) + sign bit
     * ============================================================ */
    printf("\n=== Test 2: Rice run-level (k_run=1, k_level=0 + sign) ===\n");
    for (int kr = 0; kr <= 2; kr++) {
        for (int kl = 0; kl <= 2; kl++) {
            bitstream bs = {bsdata, total_bits, dc_end};
            int completed = 0, errors = 0, total_nz = 0;

            for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
                int pos = 1;
                while (pos < 64 && bs.pos < total_bits) {
                    int run = read_rice(&bs, kr);
                    if (run < 0 || run > 63) { errors++; break; }
                    pos += run;
                    if (pos >= 64) break;
                    int level = read_rice(&bs, kl);
                    if (level < 0) { errors++; break; }
                    if (level == 0) break;  /* EOB */
                    int sign = bs_read(&bs, 1);
                    if (sign) level = -level;
                    total_nz++;
                    pos++;
                }
                if (errors == 0) completed++;
                if (errors > 10) break;
            }
            int used = bs.pos - dc_end;
            if (errors <= 10)
                printf("  kr=%d kl=%d: %d blocks, %d NZ, %d/%d bits (%.1f%%), errs=%d\n",
                       kr, kl, completed, total_nz, used, ac_bits, 100.0*used/ac_bits, errors);
        }
    }

    /* ============================================================
     * Test 3: Simple fixed-width per block
     * What if each block has a length prefix saying how many AC coefficients?
     * Or what if there's a block-level bit budget?
     * ============================================================ */
    printf("\n=== Test 3: Fixed bits per block ===\n");
    {
        /* Try different fixed bit counts per block */
        for (int bpb = 80; bpb <= 140; bpb += 10) {
            int total_needed = nblocks * bpb;
            printf("  %d bits/block: need %d bits, have %d (%.1f%%)\n",
                   bpb, total_needed, ac_bits, 100.0*total_needed/ac_bits);
        }
        /* ac_bits / nblocks */
        printf("  Exact: %.1f bits/block available\n", (double)ac_bits/nblocks);
        /* ac_bits / (nblocks * 63) */
        printf("  Exact: %.2f bits/coeff available\n", (double)ac_bits/(nblocks*63));
    }

    /* ============================================================
     * Test 4: Length-prefixed blocks
     * Each block starts with N bits telling how many AC bytes follow
     * ============================================================ */
    printf("\n=== Test 4: Length-prefixed blocks ===\n");
    for (int prefix_bits = 4; prefix_bits <= 8; prefix_bits++) {
        bitstream bs = {bsdata, total_bits, dc_end};
        int completed = 0;
        int total_ac_data = 0;
        int max_len = 0, min_len = 9999;

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int len = bs_read(&bs, prefix_bits);
            if (len < 0) break;
            total_ac_data += len;
            if (len > max_len) max_len = len;
            if (len < min_len) min_len = len;
            bs.pos += len;  /* skip the AC data */
            if (bs.pos <= total_bits) completed++;
        }
        int used = bs.pos - dc_end;
        printf("  %d-bit prefix: %d blocks, used %d/%d bits (%.1f%%), len range [%d,%d]\n",
               prefix_bits, completed, used, ac_bits, 100.0*used/ac_bits, min_len, max_len);
    }

    /* ============================================================
     * Test 5: What if AC data starts at a byte boundary after DC?
     * Try aligning to next byte after DC
     * ============================================================ */
    printf("\n=== Test 5: Byte-aligned AC start ===\n");
    {
        int aligned_start = (dc_end + 7) & ~7;  /* round up to byte */
        printf("  DC ends at bit %d, byte-aligned AC at bit %d (byte %d)\n",
               dc_end, aligned_start, aligned_start/8);
        printf("  Wasted bits: %d\n", aligned_start - dc_end);

        /* Look at first bytes after alignment */
        printf("  First 32 bytes of AC data: ");
        for (int i = 0; i < 32; i++) {
            int byte_pos = aligned_start/8 + i;
            if (byte_pos < bslen)
                printf("%02X ", bsdata[byte_pos]);
        }
        printf("\n");
    }

    /* ============================================================
     * Test 6: Truncated unary + fixed suffix (Golomb-like)
     * For AC, try: unary prefix gives magnitude range, then fixed bits
     * 0 → value 0 (1 bit)
     * 10 + 1 bit → ±1 (3 bits)
     * 110 + 2 bits → ±2..±3 (5 bits)
     * 1110 + 3 bits → ±4..±7 (7 bits)
     * etc. (Like MPEG DC VLC but for AC values!)
     * ============================================================ */
    printf("\n=== Test 6: Unary-prefix AC coding (like DC VLC for each AC) ===\n");
    {
        /* Version A: 0=zero, 1x=value */
        bitstream bs = {bsdata, total_bits, dc_end};
        static int coeffs[900][64];
        memset(coeffs, 0, sizeof(coeffs));
        int completed = 0, errors = 0, total_nz = 0;
        int total_zeros = 0;

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            coeffs[b][0] = dc_vals[b] * 8;
            for (int p = 1; p < 64 && bs.pos < total_bits; p++) {
                int flag = bs_read(&bs, 1);
                if (flag < 0) { errors++; break; }
                if (flag == 0) { total_zeros++; continue; }
                /* 1 + size_prefix + value */
                int size = 0;
                while (bs.pos < total_bits && size < 8) {
                    int bit = bs_read(&bs, 1);
                    if (bit == 0) break;
                    size++;
                }
                if (size == 0) {
                    /* just 1,0 → ±1 */
                    int sign = bs_read(&bs, 1);
                    coeffs[b][zigzag[p]] = sign ? -1 : 1;
                } else {
                    int mag = (1 << size) + bs_read(&bs, size);
                    int sign = bs_read(&bs, 1);
                    coeffs[b][zigzag[p]] = sign ? -mag : mag;
                }
                total_nz++;
            }
            if (errors == 0) completed++;
            if (errors > 5) break;
        }
        int used = bs.pos - dc_end;
        printf("  A (0=zero, 1+unary+val): %d blocks, %d NZ, %d zeros, %d/%d bits (%.1f%%)\n",
               completed, total_nz, total_zeros, used, ac_bits, 100.0*used/ac_bits);
    }

    /* ============================================================
     * Test 7: DC VLC table reused for AC (per coefficient)
     * Each of 63 AC positions coded with same VLC as DC
     * This already failed (needs 163K bits) but try with run-length EOB:
     * Special case: size=0 code (100) → skip remaining AC positions
     * ============================================================ */
    printf("\n=== Test 7: DC VLC for AC with EOB ===\n");
    {
        bitstream bs = {bsdata, total_bits, dc_end};
        static int coeffs[900][64];
        memset(coeffs, 0, sizeof(coeffs));
        int completed = 0, total_nz = 0;

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            coeffs[b][0] = dc_vals[b] * 8;
            for (int p = 1; p < 64 && bs.pos < total_bits; p++) {
                /* Try reading DC VLC */
                int saved = bs.pos;
                int matched = 0;
                for (int i = 0; i < 9; i++) {
                    int bits = bs_peek(&bs, dc_vlc[i].len);
                    if (bits == (int)dc_vlc[i].code) {
                        bs.pos += dc_vlc[i].len;
                        int sz = dc_vlc[i].size;
                        if (sz == 0) { matched = 1; break; }  /* EOB */
                        int val = bs_read(&bs, sz);
                        if (val >= 0) {
                            if (val < (1 << (sz - 1))) val -= (1 << sz) - 1;
                            coeffs[b][zigzag[p]] = val;
                            total_nz++;
                        }
                        matched = 1;
                        break;
                    }
                }
                if (!matched) { bs.pos = saved + 1; }
                if (matched && dc_vlc[0].size == 0 && bs_peek(&bs, 3) == 4) {
                    /* Hit size=0 → EOB */
                }
            }
            completed++;
        }
        int used = bs.pos - dc_end;
        printf("  DC VLC+EOB: %d blocks, %d NZ, %d/%d bits (%.1f%%)\n",
               completed, total_nz, used, ac_bits, 100.0*used/ac_bits);

        /* Band analysis */
        int band_nz[8] = {0};
        for (int b = 0; b < nblocks; b++) {
            for (int p = 1; p < 64; p++) {
                int band = (p-1) / 8;
                if (coeffs[b][zigzag[p]] != 0) band_nz[band]++;
            }
        }
        printf("  Bands: ");
        for (int i = 0; i < 8; i++) printf("b%d=%d ", i, band_nz[i]);
        printf("\n");
    }

    /* ============================================================
     * Test 8: What about simple 2-bit coding with run-length?
     * Since avg is 1.72 bits/coeff, maybe:
     * 00 = zero
     * 01 = +1 (or run of N zeros)
     * 10 = -1 (or larger value follows)
     * 11 = escape (read more bits)
     * ============================================================ */
    printf("\n=== Test 8: 2-bit prefix AC coding variants ===\n");
    {
        /* Variant A: 00=zero, 01=+1, 10=-1, 11=escape(read 4 more bits: signed nibble) */
        bitstream bs = {bsdata, total_bits, dc_end};
        static int coeffs[900][64];
        memset(coeffs, 0, sizeof(coeffs));
        int nz = 0, zeros = 0;
        int used_at_block[10];

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            if (b < 10) used_at_block[b] = bs.pos - dc_end;
            coeffs[b][0] = dc_vals[b] * 8;
            for (int p = 1; p < 64 && bs.pos < total_bits; p++) {
                int code = bs_read(&bs, 2);
                if (code < 0) break;
                switch(code) {
                    case 0: zeros++; break;
                    case 1: coeffs[b][zigzag[p]] = 1; nz++; break;
                    case 2: coeffs[b][zigzag[p]] = -1; nz++; break;
                    case 3: {
                        int val = bs_read(&bs, 4);
                        if (val >= 8) val -= 16;
                        coeffs[b][zigzag[p]] = val;
                        nz++;
                    } break;
                }
            }
        }
        int used = bs.pos - dc_end;
        printf("  A (00=0,01=+1,10=-1,11+4bit): %d NZ, %d zeros, %d/%d bits (%.1f%%)\n",
               nz, zeros, used, ac_bits, 100.0*used/ac_bits);
        printf("  Bits at block 0-9: ");
        for(int i=0;i<10;i++) printf("%d ",used_at_block[i]);
        printf("\n");
    }

    /* ============================================================
     * Test 9: Output images for best candidates
     * Generate PPM image for exp-Golomb run-level decode
     * ============================================================ */
    printf("\n=== Test 9: Exp-Golomb run-level image output ===\n");
    {
        static int coeffs[900][64];
        memset(coeffs, 0, sizeof(coeffs));
        bitstream bs = {bsdata, total_bits, dc_end};

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            coeffs[b][0] = dc_vals[b] * 8;
            int pos = 1;
            while (pos < 64 && bs.pos < total_bits) {
                int run = read_ue(&bs);
                if (run < 0 || run > 63) break;
                pos += run;
                if (pos >= 64) break;
                int level = read_se(&bs);
                if (level == -9999) break;
                if (level == 0) break;
                /* Dequantize: level * qtable[zigzag[pos]] * qs / 8 */
                int zpos = zigzag[pos];
                int qi = zpos < 16 ? zpos : zpos % 16;
                int dequant = level * qtable[qi] * qs / 8;
                coeffs[b][zpos] = dequant;
                pos++;
            }
        }

        /* IDCT and assemble */
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
        write_ppm("/tmp/ac_eg_runlevel.ppm", Yp, Cbp, Crp);
        printf("  → /tmp/ac_eg_runlevel.ppm\n");
    }

    /* ============================================================
     * Test 10: Interleaved DC+AC per block (not separate DC pass)
     * What if DC and AC are coded together per block, not DC-first?
     * Try: for each block, read DC VLC, then AC exp-Golomb run-level
     * ============================================================ */
    printf("\n=== Test 10: Interleaved DC+AC per block ===\n");
    {
        static int coeffs[900][64];
        memset(coeffs, 0, sizeof(coeffs));
        bitstream bs = {bsdata, total_bits, 0};  /* Start from beginning */
        int dc_p[3]={0,0,0};
        int completed = 0, total_nz = 0;

        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            int diff = read_dc(&bs);
            dc_p[comp] += diff;
            coeffs[b][0] = dc_p[comp] * 8;

            int pos = 1;
            while (pos < 64 && bs.pos < total_bits) {
                int run = read_ue(&bs);
                if (run < 0 || run > 63) break;
                pos += run;
                if (pos >= 64) break;
                int level = read_se(&bs);
                if (level == -9999) break;
                if (level == 0) break;
                int zpos = zigzag[pos];
                int qi = zpos < 16 ? zpos : zpos % 16;
                coeffs[b][zpos] = level * qtable[qi] * qs / 8;
                total_nz++;
                pos++;
            }
            completed++;
        }
        int used = bs.pos;
        printf("  %d blocks, %d NZ, %d/%d bits (%.1f%%)\n",
               completed, total_nz, used, total_bits, 100.0*used/total_bits);

        /* Band analysis */
        int band_nz[8] = {0};
        for (int b = 0; b < nblocks; b++) {
            for (int p = 1; p < 64; p++) {
                int band = (p-1)/8;
                if (coeffs[b][zigzag[p]] != 0) band_nz[band]++;
            }
        }
        printf("  Bands: ");
        for (int i = 0; i < 8; i++) printf("b%d=%d ", i, band_nz[i]);
        printf("\n");

        /* Write image */
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
        write_ppm("/tmp/ac_interleaved_eg.ppm", Yp, Cbp, Crp);
        printf("  → /tmp/ac_interleaved_eg.ppm\n");
    }

    /* ============================================================
     * Test 11: Cross-frame validation
     * Try exp-Golomb run-level on frames 0,1,2 to see if consumption is consistent
     * ============================================================ */
    printf("\n=== Test 11: Cross-frame exp-Golomb run-level ===\n");
    for (int fi = 0; fi < nf && fi < 4; fi++) {
        uint8_t *ff = frames[fi];
        int ffs = fsizes[fi];
        int fqs = ff[3];
        int ftype = ff[39];
        const uint8_t *fbd = ff + 40;
        int fbl = ffs - 40;
        int ftb = fbl * 8;

        bitstream bs = {fbd, ftb, 0};
        int dcp[3]={0,0,0};
        for(int b=0;b<nblocks&&bs.pos<ftb;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dcp[comp]+=read_dc(&bs);
        }
        int de = bs.pos;

        int nz = 0, blk_done = 0;
        for (int b = 0; b < nblocks && bs.pos < ftb; b++) {
            int pos = 1;
            while (pos < 64 && bs.pos < ftb) {
                int run = read_ue(&bs);
                if (run < 0 || run > 63) goto next_frame;
                pos += run;
                if (pos >= 64) break;
                int level = read_se(&bs);
                if (level == -9999) goto next_frame;
                if (level == 0) break;
                nz++;
                pos++;
            }
            blk_done++;
        }
        next_frame:;
        int used = bs.pos - de;
        printf("  Frame %d (type=%d qs=%d): %d blocks, %d NZ, %d/%d bits (%.1f%%)\n",
               fi, ftype, fqs, blk_done, nz, used, ftb-de, 100.0*used/(ftb-de));
    }

    free(disc); zip_close(z);
    return 0;
}
