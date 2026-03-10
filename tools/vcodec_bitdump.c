/*
 * vcodec_bitdump.c - Dump raw bitstream and look for structural patterns
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

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    int start_lba = atoi(argv[2]);

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

    static uint8_t frames[8][MAX_FRAME]; int fsizes[8];
    int nf = assemble_frames(disc, tsec, start_lba, frames, fsizes, 8);
    if (nf == 0) { printf("No frames\n"); return 1; }

    uint8_t *f = frames[0];
    int fsize = fsizes[0];
    int qs = f[3], type = f[39];
    printf("LBA %d: qs=%d type=%d fsize=%d\n", start_lba, qs, type, fsize);

    const uint8_t *bsdata = f + 40;
    int bslen = fsize - 40;
    int total_bits = bslen * 8;

    int mw = W/16, mh = H/16;
    int nblocks = mw * mh * 6;

    /* === Part 1: Decode all DCs === */
    printf("\n=== DC decode: %d blocks ===\n", nblocks);
    bitstream bs = {bsdata, total_bits, 0};
    int dc_pred[3] = {0,0,0};
    for (int b = 0; b < nblocks; b++) {
        int sub = b % 6;
        int comp = (sub < 4) ? 0 : (sub == 4) ? 1 : 2;
        int diff = read_dc_vlc(&bs);
        dc_pred[comp] += diff;
    }
    int dc_end = bs.pos;
    int ac_bits = total_bits - dc_end;
    printf("DC: %d bits (%.1f%%), avg %.1f bits/block\n",
           dc_end, 100.0*dc_end/total_bits, (double)dc_end/nblocks);
    printf("AC: %d bits remaining, %.1f bits/block, %.2f bits/coeff\n",
           ac_bits, (double)ac_bits/nblocks, (double)ac_bits/(nblocks*63.0));

    /* === Part 2: AC bit distribution === */
    int zeros = 0, ones = 0;
    for (int i = dc_end; i < total_bits; i++) {
        int bit = (bsdata[i>>3] >> (7-(i&7))) & 1;
        if (bit) ones++; else zeros++;
    }
    printf("\nAC bit distribution: %.1f%% zeros, %.1f%% ones\n",
           100.0*zeros/ac_bits, 100.0*ones/ac_bits);

    /* === Part 3: Per-block bit consumption (flag+VLC model) === */
    printf("\n=== Per-block bit consumption (flag+VLC model) ===\n");
    bs.pos = dc_end;
    int block_bits[900];
    for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
        int start = bs.pos;
        for (int k = 1; k < 64 && bs.pos < total_bits; k++) {
            int flag = bs_read(&bs, 1);
            if (flag) read_dc_vlc(&bs);
        }
        block_bits[b] = bs.pos - start;
    }
    printf("Flag+VLC total: %d/%d (%.1f%%)\n", bs.pos, total_bits, 100.0*bs.pos/total_bits);

    /* Print per-MB stats for first 20 MBs */
    printf("First 20 MBs:\n");
    for (int mb = 0; mb < 20 && mb*6+5 < nblocks; mb++) {
        int total = 0;
        for (int s = 0; s < 6; s++) total += block_bits[mb*6+s];
        printf("  MB%3d: %4d bits (", mb, total);
        for (int s = 0; s < 6; s++) printf("%s%3d", s?",":"", block_bits[mb*6+s]);
        printf(")\n");
    }

    /* Histogram of per-block bit counts */
    printf("\nPer-block bit count histogram:\n");
    int hist[20] = {0};
    for (int b = 0; b < nblocks; b++) {
        int bucket = block_bits[b] / 10;
        if (bucket >= 20) bucket = 19;
        hist[bucket]++;
    }
    for (int i = 0; i < 20; i++)
        if (hist[i] > 0)
            printf("  %3d-%3d: %d blocks\n", i*10, i*10+9, hist[i]);

    /* === Part 4: Progressive (per-coefficient-plane) === */
    printf("\n=== Progressive decode ===\n");
    bs.pos = dc_end;
    for (int k = 1; k < 64 && bs.pos < total_bits; k++) {
        int plane_start = bs.pos;
        int nz = 0;
        double sum_abs = 0;
        for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
            int flag = bs_read(&bs, 1);
            if (flag) {
                int val = read_dc_vlc(&bs);
                if (val != 0) { nz++; sum_abs += abs(val); }
            }
        }
        int plane_bits = bs.pos - plane_start;
        if (k <= 15 || k >= 58)
            printf("  ZZ%2d: %5d bits (%.1f/blk) nz=%3d (%.1f%%) avg=%.1f\n",
                   k, plane_bits, (double)plane_bits/nblocks,
                   nz, 100.0*nz/nblocks, nz ? sum_abs/nz : 0);
    }
    printf("  Progressive total: %d/%d (%.1f%%)\n",
           bs.pos, total_bits, 100.0*bs.pos/total_bits);

    /* === Part 5: Full header === */
    printf("\n=== Header ===\n");
    printf("  Bytes 0-3: %02X %02X %02X %02X\n", f[0], f[1], f[2], f[3]);
    printf("  Qtable1 (4-19): ");
    for (int i = 0; i < 16; i++) printf("%02X ", f[4+i]);
    printf("\n  Qtable2 (20-35): ");
    for (int i = 0; i < 16; i++) printf("%02X ", f[20+i]);
    printf("\n  Bytes 36-39: %02X %02X %02X %02X\n", f[36], f[37], f[38], f[39]);
    printf("  Same qtables: %s\n", memcmp(f+4, f+20, 16) == 0 ? "YES" : "NO");

    /* === Part 6: Try reading AC with different block counts === */
    printf("\n=== Flag+VLC with different block counts ===\n");
    int test_counts[] = {432, 576, 768, 864, 1024};
    for (int t = 0; t < 5; t++) {
        bs.pos = dc_end;
        int nb = test_counts[t];
        for (int b = 0; b < nb && bs.pos < total_bits; b++) {
            for (int k = 1; k < 64 && bs.pos < total_bits; k++) {
                int flag = bs_read(&bs, 1);
                if (flag) read_dc_vlc(&bs);
            }
        }
        printf("  %4d blocks: %d/%d (%.1f%%)\n",
               nb, bs.pos, total_bits, 100.0*bs.pos/total_bits);
    }

    /* === Part 7: What if DC is NOT DPCM? === */
    printf("\n=== DC as absolute (not DPCM) ===\n");
    bs.pos = 0;
    int abs_dc_bits = 0;
    for (int b = 0; b < nblocks && bs.pos < total_bits; b++) {
        read_dc_vlc(&bs);
    }
    abs_dc_bits = bs.pos;
    printf("  Absolute DC: %d bits (%.1f%%)\n", abs_dc_bits, 100.0*abs_dc_bits/total_bits);
    /* Note: read_dc_vlc is the same whether DPCM or absolute - the bit consumption is identical */

    /* === Part 8: Check if resolution might be different === */
    printf("\n=== Different resolutions ===\n");
    int res_tests[][2] = {{256,144}, {240,136}, {256,128}, {128,72}, {320,176}, {256,192}};
    for (int t = 0; t < 6; t++) {
        int tw = res_tests[t][0], th = res_tests[t][1];
        int tmw = tw/16, tmh = th/16;
        int tnb = tmw * tmh * 6;
        bs.pos = 0;
        /* DC */
        dc_pred[0] = dc_pred[1] = dc_pred[2] = 0;
        for (int b = 0; b < tnb && bs.pos < total_bits; b++) {
            int sub = b % 6;
            int comp = (sub < 4) ? 0 : (sub == 4) ? 1 : 2;
            int diff = read_dc_vlc(&bs);
            dc_pred[comp] += diff;
        }
        int tdc = bs.pos;
        /* AC flag+VLC */
        for (int b = 0; b < tnb && bs.pos < total_bits; b++) {
            for (int k = 1; k < 64 && bs.pos < total_bits; k++) {
                int flag = bs_read(&bs, 1);
                if (flag) read_dc_vlc(&bs);
            }
        }
        printf("  %3dx%3d (%3d MBs, %4d blocks): dc=%5d ac+dc=%d/%d (%.1f%%)\n",
               tw, th, tmw*tmh, tnb, tdc, bs.pos, total_bits, 100.0*bs.pos/total_bits);
    }

    free(disc); zip_close(z);
    return 0;
}
