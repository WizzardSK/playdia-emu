/*
 * vcodec_groundtruth.c - Use padded frames as ground truth
 * 
 * Frames at LBA 502 (qs=8) have trailing 0xFF bytes.
 * By stripping padding, we know EXACTLY how long the real data is.
 * This lets us test which AC model terminates at the right position.
 *
 * Strategy: try MANY VLC variants, see which matches the true data length.
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

/* Get real data length by stripping trailing 0xFF */
static int get_real_len(const uint8_t *data, int len) {
    while (len > 0 && data[len-1] == 0xFF) len--;
    return len;
}

/* Also try stripping trailing 0x00 */
static int get_real_len_nozero(const uint8_t *data, int len) {
    while (len > 0 && data[len-1] == 0x00) len--;
    return len;
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;

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

    /* Test multiple LBAs including padded ones */
    int lbas[] = {277, 502, 757, 1112, 1872, 3072, 5232};
    int nlbas = 7;

    for (int li = 0; li < nlbas; li++) {
        int lba = lbas[li];
        static uint8_t frames[8][MAX_FRAME]; int fsizes[8];
        int nf = assemble_frames(disc, tsec, lba, frames, fsizes, 8);
        if (nf == 0) continue;

        for (int fi = 0; fi < nf && fi < 4; fi++) {
            uint8_t *f = frames[fi];
            int fsize = fsizes[fi];
            int qs = f[3], type = f[39];

            const uint8_t *bsdata = f + 40;
            int bslen = fsize - 40;
            int real_len = get_real_len(bsdata, bslen);
            int real_len_nz = get_real_len_nozero(bsdata, bslen);
            int total_bits = bslen * 8;
            int real_bits = real_len * 8;

            if (real_len == bslen && real_len_nz == bslen) continue; /* no padding */

            int nblocks = (W/16) * (H/16) * 6;

            printf("LBA %d frame %d: qs=%d type=%d bslen=%d real_len=%d (%.1f%%) real_nz=%d\n",
                   lba, fi, qs, type, bslen, real_len, 100.0*real_len/bslen, real_len_nz);

            /* Decode DC */
            bitstream bs = {bsdata, total_bits, 0};
            int dc_pred[3] = {0,0,0};
            for (int b = 0; b < nblocks; b++) {
                int comp = (b%6 < 4) ? 0 : (b%6 == 4) ? 1 : 2;
                read_dc_vlc(&bs);
            }
            int dc_end = bs.pos;
            printf("  DC: %d bits. Real AC bits: %d. AC bits/block: %.1f, bits/coeff: %.2f\n",
                   dc_end, real_bits - dc_end, (double)(real_bits - dc_end)/nblocks,
                   (double)(real_bits - dc_end)/(nblocks * 63.0));

            /* Dump first bytes of AC data and last bytes before padding */
            printf("  First 32 AC bytes: ");
            int byte_off = (dc_end + 7) / 8;
            for (int i = 0; i < 32 && byte_off+i < bslen; i++)
                printf("%02X ", bsdata[byte_off+i]);
            printf("\n");

            printf("  Last 32 data bytes: ");
            for (int i = real_len-32; i < real_len; i++)
                if (i >= 0) printf("%02X ", bsdata[i]);
            printf("\n");

            printf("  First 4 padding bytes: ");
            for (int i = real_len; i < real_len+4 && i < bslen; i++)
                printf("%02X ", bsdata[i]);
            printf("\n");

            /* Dump the last few bits before padding */
            int last_real_bit = real_len * 8;
            printf("  Last 64 bits before padding: ");
            for (int i = last_real_bit - 64; i < last_real_bit; i++) {
                if (i >= 0) printf("%d", (bsdata[i>>3] >> (7-(i&7))) & 1);
                if (i > 0 && (i+1) % 8 == 0) printf(" ");
            }
            printf("\n");

            /* === Test models against ground truth === */
            printf("\n  Model tests (target: %d bits = byte %d):\n", real_bits, real_len);

            /* Model 1: flag + DC VLC (original) */
            bs.pos = dc_end;
            for (int b = 0; b < nblocks && bs.pos < real_bits; b++) {
                for (int k = 1; k < 64 && bs.pos < real_bits; k++) {
                    int flag = bs_read(&bs, 1);
                    if (flag) read_dc_vlc(&bs);
                }
            }
            int m1_blocks = 0;
            /* Count how many blocks completed */
            {
                bitstream bs2 = {bsdata, real_bits, dc_end};
                for (m1_blocks = 0; m1_blocks < nblocks; m1_blocks++) {
                    int start = bs2.pos;
                    for (int k = 1; k < 64; k++) {
                        if (bs2.pos >= real_bits) goto m1_done;
                        int flag = bs_read(&bs2, 1);
                        if (flag) read_dc_vlc(&bs2);
                    }
                }
                m1_done:;
            }
            printf("    flag+DC_VLC:     %d bits (%.1f%% of real), ~%d blocks\n",
                   bs.pos, bs.pos <= real_bits ? 100.0*bs.pos/real_bits : 100.0*real_bits/bs.pos,
                   m1_blocks);

            /* Model 2: no-flag DC VLC (each position gets VLC) */
            bs.pos = dc_end;
            int m2_blocks = 0;
            for (int b = 0; b < nblocks && bs.pos < real_bits; b++) {
                for (int k = 1; k < 64 && bs.pos < real_bits; k++) {
                    read_dc_vlc(&bs);
                }
                m2_blocks = b + 1;
            }
            printf("    no-flag DC_VLC:  %d bits (%.1f%% of real), %d blocks\n",
                   bs.pos, bs.pos <= real_bits ? 100.0*bs.pos/real_bits : 100.0*real_bits/bs.pos,
                   m2_blocks);

            /* Model 3: Exp-Golomb signed per position */
            bs.pos = dc_end;
            int m3_blocks = 0;
            for (int b = 0; b < nblocks && bs.pos < real_bits; b++) {
                for (int k = 1; k < 64 && bs.pos < real_bits; k++) {
                    /* Exp-golomb signed */
                    int zeros = 0;
                    while (bs.pos < real_bits) {
                        int bit = bs_read(&bs, 1);
                        if (bit == 1) break;
                        zeros++;
                        if (zeros > 20) break;
                    }
                    if (zeros > 0 && bs.pos + zeros <= real_bits)
                        bs_read(&bs, zeros);
                }
                m3_blocks = b + 1;
            }
            printf("    exp-golomb:      %d bits (%.1f%% of real), %d blocks\n",
                   bs.pos, bs.pos <= real_bits ? 100.0*bs.pos/real_bits : 100.0*real_bits/bs.pos,
                   m3_blocks);

            /* Model 4: fixed 2-bit per position */
            {
                int fb = dc_end + nblocks * 63 * 2;
                printf("    fixed 2-bit:     %d bits (%.1f%% of real)\n",
                       fb, 100.0*fb/real_bits);
            }

            /* Model 5: fixed 1-bit per position (just flags) */
            {
                int fb = dc_end + nblocks * 63 * 1;
                printf("    fixed 1-bit:     %d bits (%.1f%% of real)\n",
                       fb, 100.0*fb/real_bits);
            }

            /* Model 6: inverted flag + DC VLC */
            bs.pos = dc_end;
            for (int b = 0; b < nblocks && bs.pos < real_bits; b++) {
                for (int k = 1; k < 64 && bs.pos < real_bits; k++) {
                    int flag = bs_read(&bs, 1);
                    if (!flag) read_dc_vlc(&bs); /* inverted: 0=nonzero */
                }
            }
            printf("    inv_flag+VLC:    %d bits (%.1f%% of real)\n",
                   bs.pos, bs.pos <= real_bits ? 100.0*bs.pos/real_bits : 100.0*real_bits/bs.pos);

            /* Model 7: flag + 1-bit value (±1) */
            {
                /* Each position: flag(1bit), if 1: sign(1bit) = 2 bits for nonzero */
                /* This is equivalent to 2-bit code: 0x=zero, 1s=±1 */
                /* Hmm, with ~50% ones, about 50% nonzero → 1.5 bits/pos avg */
                int fb = dc_end + nblocks * 63; /* flags only */
                /* Need to count how many flags are 1 in the real data */
                bs.pos = dc_end;
                int nz = 0;
                for (int b = 0; b < nblocks && bs.pos < real_bits; b++) {
                    for (int k = 1; k < 64 && bs.pos < real_bits; k++) {
                        int flag = bs_read(&bs, 1);
                        if (flag) nz++;
                    }
                }
                printf("    flag+sign only:  %d + %d = %d bits (%.1f%% of real)\n",
                       fb, nz, fb+nz, 100.0*(fb+nz)/real_bits);
            }

            /* Model 8: Unary-size VLC per position */
            bs.pos = dc_end;
            for (int b = 0; b < nblocks && bs.pos < real_bits; b++) {
                for (int k = 1; k < 64 && bs.pos < real_bits; k++) {
                    int sz = 0;
                    while (bs.pos < real_bits) {
                        int bit = bs_read(&bs, 1);
                        if (bit == 0) break;
                        sz++;
                        if (sz > 10) break;
                    }
                    if (sz > 0 && bs.pos + sz <= real_bits)
                        bs_read(&bs, sz);
                }
            }
            printf("    unary_size:      %d bits (%.1f%% of real)\n",
                   bs.pos, bs.pos <= real_bits ? 100.0*bs.pos/real_bits : 100.0*real_bits/bs.pos);

            /* Model 9: MPEG-1 chrominance DC VLC instead */
            /* 00→0, 01→1, 10→2, 110→3, 1110→4, 11110→5, 111110→6, 1111110→7, 11111110→8 */
            bs.pos = dc_end;
            for (int b = 0; b < nblocks && bs.pos < real_bits; b++) {
                for (int k = 1; k < 64 && bs.pos < real_bits; k++) {
                    int flag = bs_read(&bs, 1);
                    if (flag) {
                        /* Read chrominance DC VLC */
                        int peek2 = bs_peek(&bs, 2);
                        if (peek2 == 0b00) { bs.pos += 2; } /* size 0 */
                        else if (peek2 == 0b01) { bs.pos += 2; bs_read(&bs, 1); } /* size 1 */
                        else if (peek2 == 0b10) { bs.pos += 2; bs_read(&bs, 2); } /* size 2 */
                        else {
                            /* size >= 3: unary 1s then 0 */
                            int sz = 2;
                            bs.pos += 2; /* skip '11' */
                            while (bs.pos < real_bits) {
                                int bit = bs_read(&bs, 1);
                                if (bit == 0) break;
                                sz++;
                                if (sz > 8) break;
                            }
                            if (bs.pos + sz <= real_bits) bs_read(&bs, sz);
                        }
                    }
                }
            }
            printf("    flag+chrom_VLC:  %d bits (%.1f%% of real)\n",
                   bs.pos, bs.pos <= real_bits ? 100.0*bs.pos/real_bits : 100.0*real_bits/bs.pos);

            /* Model 10: Try different AC VLC - what about H.261 style? */
            /* flag + sign + magnitude using rice code (quotient unary, remainder fixed) */
            /* Rice parameter k=1: quotient=unary, 1-bit remainder */
            bs.pos = dc_end;
            for (int b = 0; b < nblocks && bs.pos < real_bits; b++) {
                for (int k = 1; k < 64 && bs.pos < real_bits; k++) {
                    int flag = bs_read(&bs, 1);
                    if (flag) {
                        int sign = bs_read(&bs, 1);
                        /* Rice k=1: unary quotient + 1 bit remainder */
                        while (bs.pos < real_bits) {
                            int bit = bs_read(&bs, 1);
                            if (bit == 0) break;
                        }
                        bs_read(&bs, 1); /* remainder */
                    }
                }
            }
            printf("    flag+rice(k=1):  %d bits (%.1f%% of real)\n",
                   bs.pos, bs.pos <= real_bits ? 100.0*bs.pos/real_bits : 100.0*real_bits/bs.pos);

            printf("\n");
        }
    }

    free(disc); zip_close(z);
    return 0;
}
