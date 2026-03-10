/*
 * vcodec_rawbits.c - Analyze raw bitstream patterns after DC decode
 * Look for the real AC coding by examining bit-level patterns
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

static const struct { int len; uint32_t code; int size; } dc_vlc[] = {
    {3, 0b100, 0}, {2, 0b00, 1}, {2, 0b01, 2}, {3, 0b101, 3},
    {3, 0b110, 4}, {4, 0b1110, 5}, {5, 0b11110, 6},
    {6, 0b111110, 7}, {7, 0b1111110, 8},
};
#define DC_VLC_COUNT 9

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

static int read_dc_vlc(bitstream *bs) {
    for (int i = 0; i < DC_VLC_COUNT; i++) {
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

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    int start_lba = atoi(argv[2]);

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

    static uint8_t frames[8][MAX_FRAME]; int fsizes[8];
    int nf = assemble_frames(disc, tsec, start_lba, frames, fsizes, 8);
    if (nf == 0) { printf("No frames\n"); return 1; }

    uint8_t *f = frames[0];
    int fsize = fsizes[0];
    int qs = f[3], type = f[39];
    const uint8_t *bsdata = f + 40;
    int bslen = fsize - 40;
    int total_bits = bslen * 8;
    int mw = W/16, mh = H/16, nblocks = mw*mh*6;

    printf("LBA %d: qs=%d type=%d fsize=%d\n", start_lba, qs, type, fsize);

    /* Decode DC to find AC start point */
    bitstream bs = {bsdata, total_bits, 0};
    int dc_pred[3] = {0,0,0};
    int dc_vals[900];
    for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
        int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
        int diff = read_dc_vlc(&bs);
        dc_pred[comp] += diff;
        dc_vals[b] = dc_pred[comp];
    }
    int dc_end = bs.pos;
    printf("DC: %d bits (%.1f%%)\n\n", dc_end, 100.0*dc_end/total_bits);

    /* === Analysis 1: Byte-level patterns after DC === */
    printf("=== Bytes after DC (at byte %d) ===\n", dc_end/8);
    int start_byte = dc_end / 8;
    printf("  First 64 bytes: ");
    for (int i = start_byte; i < start_byte + 64 && i < bslen; i++)
        printf("%02X ", bsdata[i]);
    printf("\n");

    printf("  Bytes +128: ");
    for (int i = start_byte+128; i < start_byte+192 && i < bslen; i++)
        printf("%02X ", bsdata[i]);
    printf("\n");

    printf("  Bytes +256: ");
    for (int i = start_byte+256; i < start_byte+320 && i < bslen; i++)
        printf("%02X ", bsdata[i]);
    printf("\n");

    /* === Analysis 2: Look for zero bytes (potential block boundaries) === */
    printf("\n=== Zero byte positions after DC ===\n");
    int zero_count = 0;
    for (int i = start_byte; i < bslen && zero_count < 20; i++) {
        if (bsdata[i] == 0x00) {
            printf("  0x00 at byte %d (offset %d from DC end)\n", i, i - start_byte);
            zero_count++;
        }
    }

    /* === Analysis 3: Byte histogram after DC === */
    printf("\n=== Byte histogram (after DC, first 4096 bytes) ===\n");
    int hist[256] = {0};
    int range = 4096;
    if (start_byte + range > bslen) range = bslen - start_byte;
    for (int i = start_byte; i < start_byte + range; i++)
        hist[bsdata[i]]++;
    printf("  Most common bytes: ");
    for (int pass = 0; pass < 10; pass++) {
        int best = -1, bestc = 0;
        for (int i = 0; i < 256; i++) {
            if (hist[i] > bestc) { bestc = hist[i]; best = i; }
        }
        if (best >= 0) {
            printf("0x%02X(%d) ", best, bestc);
            hist[best] = 0;
        }
    }
    printf("\n");

    /* === Analysis 4: N-gram patterns (2-byte) === */
    printf("\n=== 2-byte patterns (first 2048 bytes after DC) ===\n");
    int pg[65536] = {0};
    int range2 = 2048;
    if (start_byte + range2 + 1 > bslen) range2 = bslen - start_byte - 1;
    for (int i = start_byte; i < start_byte + range2; i++)
        pg[(bsdata[i]<<8)|bsdata[i+1]]++;
    printf("  Most common 2-byte patterns: ");
    for (int pass = 0; pass < 8; pass++) {
        int best = -1, bestc = 0;
        for (int i = 0; i < 65536; i++) {
            if (pg[i] > bestc) { bestc = pg[i]; best = i; }
        }
        if (best >= 0) {
            printf("%04X(%d) ", best, bestc);
            pg[best] = 0;
        }
    }
    printf("\n");

    /* === Analysis 5: Try reading AC as fixed-width packed fields === */
    printf("\n=== Fixed-width AC tests ===\n");
    for (int nbits = 1; nbits <= 4; nbits++) {
        bs.pos = dc_end;
        int nz = 0, zero = 0;
        long sum_abs = 0;
        int hist2[32] = {0};
        for (int b = 0; b < nblocks && bs.pos + nbits <= total_bits; b++) {
            for (int p = 1; p < 64 && bs.pos + nbits <= total_bits; p++) {
                int val = bs_read(&bs, nbits);
                if (nbits == 1) {
                    if (val) nz++; else zero++;
                    hist2[val]++;
                } else {
                    /* signed: MSB = sign */
                    int sval;
                    if (nbits == 2) {
                        sval = (val == 0) ? 0 : (val == 1) ? 1 : (val == 2) ? -1 : 0; /* 00=0, 01=+1, 10=-1, 11=special */
                        if (val == 3) sval = 99; /* marker? */
                    } else {
                        int sign = val >> (nbits-1);
                        int mag = val & ((1<<(nbits-1))-1);
                        sval = sign ? -mag : mag;
                    }
                    if (sval == 0) zero++; else nz++;
                    sum_abs += abs(sval);
                    if (val < 32) hist2[val]++;
                }
            }
        }
        int total = nz + zero;
        printf("  %d-bit: %d values, %.1f%% consumed, %.1f%% zero, avg|val|=%.2f\n",
               nbits, total, 100.0*bs.pos/total_bits, 100.0*zero/total,
               total > 0 ? (double)sum_abs/total : 0);
        if (nbits <= 2) {
            printf("    hist: ");
            for (int i = 0; i < (1<<nbits); i++)
                printf("[%d]=%d(%.1f%%) ", i, hist2[i], 100.0*hist2[i]/total);
            printf("\n");
        }
    }

    /* === Analysis 6: Per-row DC reset === */
    printf("\n=== DC with per-row reset ===\n");
    {
        bs.pos = 0;
        memset(dc_pred, 0, sizeof(dc_pred));
        int dc_min = 99999, dc_max = -99999;
        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
            /* Reset DC at start of each MB row */
            int mb = b / 6;
            if (mb > 0 && (mb % mw) == 0) {
                dc_pred[0] = dc_pred[1] = dc_pred[2] = 0;
            }
            int diff = read_dc_vlc(&bs);
            dc_pred[comp] += diff;
            dc_vals[b] = dc_pred[comp];
            if (dc_pred[comp] < dc_min) dc_min = dc_pred[comp];
            if (dc_pred[comp] > dc_max) dc_max = dc_pred[comp];
        }
        printf("  DC range with row reset: [%d, %d], bits=%d\n", dc_min, dc_max, bs.pos);
    }

    /* === Analysis 7: DC with per-MB reset (every block) === */
    printf("\n=== DC with per-MB component reset ===\n");
    {
        bs.pos = 0;
        int dc_min = 99999, dc_max = -99999;
        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
            /* Reset DC at start of each MB (every 6 blocks) */
            if (b % 6 == 0) {
                dc_pred[0] = dc_pred[1] = dc_pred[2] = 0;
            }
            int diff = read_dc_vlc(&bs);
            dc_pred[comp] += diff;
            dc_vals[b] = dc_pred[comp];
            if (dc_pred[comp] < dc_min) dc_min = dc_pred[comp];
            if (dc_pred[comp] > dc_max) dc_max = dc_pred[comp];
        }
        printf("  DC range with MB reset: [%d, %d], bits=%d\n", dc_min, dc_max, bs.pos);
    }

    /* === Output: DC with per-row reset === */
    printf("\n=== Output: DC-only with per-row reset ===\n");
    {
        bs.pos = 0;
        static int blocks[900][64];
        memset(blocks, 0, sizeof(blocks));
        dc_pred[0] = dc_pred[1] = dc_pred[2] = 0;
        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
            int mb = b / 6;
            if (mb > 0 && (mb % mw) == 0)
                dc_pred[0] = dc_pred[1] = dc_pred[2] = 0;
            int diff = read_dc_vlc(&bs);
            dc_pred[comp] += diff;
            blocks[b][0] = dc_pred[comp] * 8;
        }
        int Y[H][W];
        memset(Y, 0, sizeof(Y));
        for (int mb = 0; mb < mw*mh; mb++) {
            int mx2 = mb%mw, my2 = mb/mw;
            int out[64];
            for (int s = 0; s < 4; s++) {
                idct8x8(blocks[mb*6+s], out);
                int bx=(s&1)*8, by=(s>>1)*8;
                for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
                    int py=my2*16+by+r, px=mx2*16+bx+c;
                    if(py<H&&px<W) Y[py][px]=clamp(out[r*8+c]+128);
                }
            }
        }
        uint8_t *gray = malloc(W*H);
        for (int y=0;y<H;y++) for (int x=0;x<W;x++) gray[y*W+x]=Y[y][x];
        FILE *fp = fopen("/tmp/dc_rowreset.ppm", "wb");
        fprintf(fp, "P5\n%d %d\n255\n", W, H);
        fwrite(gray, 1, W*H, fp);
        fclose(fp);
        printf("  → /tmp/dc_rowreset.ppm\n");
        free(gray);
    }

    /* === Analysis 8: Try treating AC bits as 2-bit signed values === */
    printf("\n=== Output: 2-bit signed AC (00=0, 01=+1, 10=-1, 11=+2) ===\n");
    {
        static int blocks[900][64];
        memset(blocks, 0, sizeof(blocks));
        bs.pos = 0;
        dc_pred[0] = dc_pred[1] = dc_pred[2] = 0;
        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
            int diff = read_dc_vlc(&bs);
            dc_pred[comp] += diff;
            blocks[b][0] = dc_pred[comp] * 8;
        }
        /* Now read 2-bit values for each AC position */
        for (int b = 0; b < nblocks && bs.pos + 2 <= total_bits; b++) {
            for (int p = 1; p < 64 && bs.pos + 2 <= total_bits; p++) {
                int v = bs_read(&bs, 2);
                int val;
                switch(v) {
                    case 0: val = 0; break;
                    case 1: val = 1; break;
                    case 2: val = -1; break;
                    case 3: val = 2; break; /* or -2? */
                    default: val = 0;
                }
                blocks[b][zigzag[p]] = val * qs;
            }
        }
        printf("  bits used: %d/%d (%.1f%%)\n", bs.pos, total_bits, 100.0*bs.pos/total_bits);

        int Y[H][W];
        memset(Y, 0, sizeof(Y));
        for (int mb = 0; mb < mw*mh; mb++) {
            int mx2 = mb%mw, my2 = mb/mw;
            int out[64];
            for (int s = 0; s < 4; s++) {
                idct8x8(blocks[mb*6+s], out);
                int bx=(s&1)*8, by=(s>>1)*8;
                for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
                    int py=my2*16+by+r, px=mx2*16+bx+c;
                    if(py<H&&px<W) Y[py][px]=clamp(out[r*8+c]+128);
                }
            }
        }
        uint8_t *gray = malloc(W*H);
        for (int y=0;y<H;y++) for (int x=0;x<W;x++) gray[y*W+x]=Y[y][x];
        FILE *fp = fopen("/tmp/ac_2bit.ppm", "wb");
        fprintf(fp, "P5\n%d %d\n255\n", W, H);
        fwrite(gray, 1, W*H, fp);
        fclose(fp);
        double smooth = 0; int cnt = 0;
        for (int y=0;y<H;y++) for (int x=0;x<W-1;x++) {
            smooth += abs(Y[y][x]-Y[y][x+1]); cnt++;
        }
        printf("  smoothness=%.1f → /tmp/ac_2bit.ppm\n", smooth/cnt);
        free(gray);
    }

    /* === Analysis 9: Autocorrelation of bit stream after DC === */
    printf("\n=== Bit-level autocorrelation ===\n");
    {
        int ac_start = dc_end;
        int ac_len = total_bits - ac_start;
        if (ac_len > 50000) ac_len = 50000;
        /* Check for periodicity */
        for (int period = 63; period <= 130; period++) {
            int match = 0, total = 0;
            for (int i = 0; i + period < ac_len; i++) {
                int bp1 = ac_start + i;
                int bp2 = ac_start + i + period;
                int b1 = (bsdata[bp1>>3] >> (7-(bp1&7))) & 1;
                int b2 = (bsdata[bp2>>3] >> (7-(bp2&7))) & 1;
                if (b1 == b2) match++;
                total++;
            }
            double corr = (2.0*match/total - 1.0);
            if (fabs(corr) > 0.02)
                printf("  period=%d: corr=%.4f (match=%d/%d)\n", period, corr, match, total);
        }
        /* Also check byte-level periods */
        printf("  Byte periods:\n");
        int byte_start = dc_end / 8 + 1;
        int byte_end = bslen < byte_start + 8000 ? bslen : byte_start + 8000;
        for (int period = 107; period <= 115; period++) {
            int match = 0, total = 0;
            for (int i = byte_start; i + period < byte_end; i++) {
                if (bsdata[i] == bsdata[i+period]) match++;
                total++;
            }
            double corr = (double)match/total;
            if (corr > 0.02)
                printf("    period=%d bytes: %.3f match\n", period, corr);
        }
    }

    free(disc); zip_close(z);
    return 0;
}
