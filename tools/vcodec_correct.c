/*
 * vcodec_correct.c - Correct frame loading from Track 2
 * Uses proper sector parsing: skip type byte, use 2047 bytes per sector
 * Scan for F1/F2/F3 markers to delimit frames
 * Then re-analyze AC bitstream with correct data
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define MAX_FRAMES 64
#define FRAME_SIZE (6 * 2047)

static uint8_t frames[MAX_FRAMES][FRAME_SIZE + 256];
static int frame_sizes[MAX_FRAMES];
static int num_frames;

static int get_bit(const uint8_t *data, int bitpos) {
    return (data[bitpos >> 3] >> (7 - (bitpos & 7))) & 1;
}
static uint32_t get_bits(const uint8_t *data, int bitpos, int n) {
    uint32_t val = 0;
    for (int i = 0; i < n; i++) val = (val << 1) | get_bit(data, bitpos + i);
    return val;
}

static const struct { int len; uint32_t code; } dc_lum_vlc[] = {
    {3, 0x4}, {2, 0x0}, {2, 0x1}, {3, 0x5}, {3, 0x6},
    {4, 0xE}, {5, 0x1E}, {6, 0x3E}, {7, 0x7E},
};

static int decode_dc(const uint8_t *data, int bitpos, int *dc_val, int total_bits) {
    for (int i = 0; i < 9; i++) {
        if (bitpos + dc_lum_vlc[i].len > total_bits) continue;
        uint32_t bits = get_bits(data, bitpos, dc_lum_vlc[i].len);
        if (bits == dc_lum_vlc[i].code) {
            int sz = i, consumed = dc_lum_vlc[i].len;
            if (sz == 0) { *dc_val = 0; }
            else {
                if (bitpos + consumed + sz > total_bits) return -1;
                uint32_t raw = get_bits(data, bitpos + consumed, sz);
                consumed += sz;
                *dc_val = (raw < (1u << (sz-1))) ? (int)raw - (1<<sz) + 1 : (int)raw;
            }
            return consumed;
        }
    }
    return -1;
}

static int find_data_end(int fi) {
    int end = frame_sizes[fi];
    while (end > 0 && frames[fi][end - 1] == 0xFF) end--;
    return end;
}

static void analyze_runs(const uint8_t *data, int nbits, const char *label) {
    int run0[32] = {0}, run1[32] = {0};
    int cur_val = get_bit(data, 0), cur_len = 1;
    int max0 = 0, max1 = 0;
    for (int i = 1; i < nbits; i++) {
        int b = get_bit(data, i);
        if (b == cur_val) cur_len++;
        else {
            int idx = (cur_len < 32) ? cur_len : 31;
            if (cur_val == 0) { run0[idx]++; if (cur_len > max0) max0 = cur_len; }
            else { run1[idx]++; if (cur_len > max1) max1 = cur_len; }
            cur_val = b; cur_len = 1;
        }
    }
    int idx = (cur_len < 32) ? cur_len : 31;
    if (cur_val == 0) { run0[idx]++; if (cur_len > max0) max0 = cur_len; }
    else { run1[idx]++; if (cur_len > max1) max1 = cur_len; }
    printf("  %s: max0=%d max1=%d\n", label, max0, max1);
    printf("    0-runs: ");
    for (int r = 1; r <= 14; r++) printf("r%d=%d ", r, run0[r]);
    printf("\n    1-runs: ");
    for (int r = 1; r <= 14; r++) printf("r%d=%d ", r, run1[r]);
    printf("\n");
}

static int scan_frames(const char *binfile, int start_lba) {
    FILE *fp = fopen(binfile, "rb");
    if (!fp) { fprintf(stderr, "Can't open %s\n", binfile); return -1; }
    
    num_frames = 0;
    int f1_count = 0;
    
    for (int sec = 0; sec < 500 && num_frames < MAX_FRAMES; sec++) {
        long offset = (long)(start_lba + sec) * 2352;
        uint8_t sector[2352];
        fseek(fp, offset, SEEK_SET);
        if (fread(sector, 1, 2352, fp) != 2352) break;
        
        uint8_t type_byte = sector[24]; /* first user data byte */
        
        if (type_byte == 0xF1) {
            /* Video data sector */
            if (f1_count < 6) {
                memcpy(frames[num_frames] + f1_count * 2047, sector + 25, 2047);
            }
            f1_count++;
        } else if (type_byte == 0xF2) {
            /* End of frame */
            if (f1_count == 6) {
                frame_sizes[num_frames] = 6 * 2047;
                num_frames++;
            }
            f1_count = 0;
        } else if (type_byte == 0xF3) {
            f1_count = 0;
        } else {
            /* Audio or other - don't reset f1_count */
        }
    }
    
    fclose(fp);
    return num_frames;
}

int main() {
    /* Use Track 2 explicitly */
    const char *binfile = "/tmp/Mari-nee no Heya (Japan) (Track 2).bin";
    int start_lba = 502;
    int num_blocks = 864;
    
    printf("=== Scanning frames from LBA %d ===\n", start_lba);
    int nf = scan_frames(binfile, start_lba);
    printf("Found %d video frames\n\n", nf);
    
    /* Analyze all frames */
    printf("=== Frame summary ===\n");
    int padded_frame = -1;
    for (int f = 0; f < nf && f < 32; f++) {
        uint8_t *fd = frames[f];
        
        /* Verify header */
        if (fd[0] != 0x00 || fd[1] != 0x80 || fd[2] != 0x04) {
            printf("  F%02d: BAD HEADER %02X %02X %02X %02X\n", f, fd[0], fd[1], fd[2], fd[3]);
            continue;
        }
        
        int qs = fd[3];
        int type = fd[39];
        int data_end = find_data_end(f);
        int pad = frame_sizes[f] - data_end;
        
        int total_bits = (data_end - 40) * 8;
        int bitpos = 0;
        int ok = 1;
        for (int b = 0; b < num_blocks; b++) {
            int dc_val;
            int consumed = decode_dc(fd + 40, bitpos, &dc_val, total_bits);
            if (consumed < 0) { ok = 0; break; }
            bitpos += consumed;
        }
        
        if (ok) {
            int dc_bits = bitpos;
            int ac_bits = total_bits - dc_bits;
            printf("  F%02d: QS=%2d T=%d pad=%4d DC=%4d AC=%5d (%.1f/blk %.2f/c)%s\n",
                   f, qs, type, pad, dc_bits, ac_bits,
                   (double)ac_bits/num_blocks, (double)ac_bits/(num_blocks*63),
                   dc_bits % 8 == 0 ? " ALIGNED" : "");
            if (pad > 500 && padded_frame < 0) padded_frame = f;
        } else {
            printf("  F%02d: QS=%2d T=%d pad=%4d DC-FAIL at bit %d\n",
                   f, qs, type, pad, bitpos);
        }
    }
    
    /* Detailed analysis on F00 */
    printf("\n=== Detailed analysis F00 ===\n");
    {
        uint8_t *fd = frames[0];
        int data_end = find_data_end(0);
        int total_bits = (data_end - 40) * 8;
        int bitpos = 0;
        int dc_vals[864];
        for (int b = 0; b < num_blocks; b++) {
            int consumed = decode_dc(fd + 40, bitpos, &dc_vals[b], total_bits);
            bitpos += consumed;
        }
        int dc_bits = bitpos;
        int ac_bits = total_bits - dc_bits;
        
        /* Extract AC data */
        int ac_buf_len = (ac_bits + 7) / 8 + 1;
        uint8_t *ac_data = calloc(ac_buf_len, 1);
        for (int i = 0; i < ac_bits; i++) {
            if (get_bit(fd + 40, dc_bits + i))
                ac_data[i >> 3] |= (1 << (7 - (i & 7)));
        }
        
        printf("DC bits: %d, AC bits: %d\n", dc_bits, ac_bits);
        printf("DC byte alignment: bit %d = byte %d + %d bits\n", 
               dc_bits, dc_bits/8, dc_bits%8);
        
        analyze_runs(ac_data, ac_bits, "F00-AC");
        
        /* First 64 AC bits */
        printf("First 64 AC bits: ");
        for (int i = 0; i < 64 && i < ac_bits; i++) printf("%d", get_bit(ac_data, i));
        printf("\n");
        
        /* Entropy */
        int byte_count[256] = {0};
        int ac_bytes = ac_bits / 8;
        for (int i = 0; i < ac_bytes; i++) byte_count[ac_data[i]]++;
        double entropy = 0;
        for (int i = 0; i < 256; i++) {
            if (byte_count[i] > 0) {
                double p = (double)byte_count[i] / ac_bytes;
                entropy -= p * log2(p);
            }
        }
        printf("AC byte entropy: %.4f bits (max 8.0)\n", entropy);
        
        /* Chi-squared for uniformity */
        double expected = (double)ac_bytes / 256;
        double chi2 = 0;
        for (int i = 0; i < 256; i++) {
            double d = byte_count[i] - expected;
            chi2 += d * d / expected;
        }
        printf("AC byte chi2: %.1f (uniform=~255)\n", chi2);
        
        free(ac_data);
    }
    
    /* Detailed analysis on padded frame */
    if (padded_frame >= 0) {
        printf("\n=== Detailed analysis padded F%02d ===\n", padded_frame);
        uint8_t *fd = frames[padded_frame];
        int data_end = find_data_end(padded_frame);
        int pad = frame_sizes[padded_frame] - data_end;
        int total_bits = (data_end - 40) * 8;
        int bitpos = 0;
        for (int b = 0; b < num_blocks; b++) {
            int dc_val;
            int consumed = decode_dc(fd + 40, bitpos, &dc_val, total_bits);
            if (consumed < 0) { printf("DC failed at block %d\n", b); goto done; }
            bitpos += consumed;
        }
        int dc_bits = bitpos;
        int ac_bits = total_bits - dc_bits;
        printf("QS=%d, padding=%d bytes\n", fd[3], pad);
        printf("DC bits: %d, AC bits: %d (%.1f/blk, %.2f/coeff)\n",
               dc_bits, ac_bits, (double)ac_bits/num_blocks, (double)ac_bits/(num_blocks*63));
        printf("DC alignment: bit %d = byte %d + %d\n", dc_bits, dc_bits/8, dc_bits%8);
        
        /* Last 16 bytes of data */
        printf("Last 16 bytes before pad: ");
        for (int i = data_end - 16; i < data_end; i++) printf("%02X ", fd[i]);
        printf("\n");
        
        /* Extract and analyze AC */
        int ac_buf_len = (ac_bits + 7) / 8 + 1;
        uint8_t *ac_data = calloc(ac_buf_len, 1);
        for (int i = 0; i < ac_bits; i++) {
            if (get_bit(fd + 40, dc_bits + i))
                ac_data[i >> 3] |= (1 << (7 - (i & 7)));
        }
        
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "F%02d-AC", padded_frame);
        analyze_runs(ac_data, ac_bits, lbl);
        
        /* Last 64 AC bits */
        printf("Last 64 AC bits: ");
        for (int i = ac_bits - 64; i < ac_bits; i++) printf("%d", get_bit(ac_data, i));
        printf("\n");
        
        free(ac_data);
    }
    done:
    
    /* Compare run-length fingerprint across multiple frames */
    printf("\n=== Run-length fingerprint across frames ===\n");
    printf("Frame  max0 max1  r6_0  r12_0  r6_1  r7_1\n");
    for (int f = 0; f < nf && f < 16; f++) {
        uint8_t *fd = frames[f];
        if (fd[0] != 0x00 || fd[1] != 0x80) continue;
        
        int data_end = find_data_end(f);
        int total_bits = (data_end - 40) * 8;
        int bitpos = 0;
        int ok = 1;
        for (int b = 0; b < num_blocks; b++) {
            int dc_val;
            int consumed = decode_dc(fd + 40, bitpos, &dc_val, total_bits);
            if (consumed < 0) { ok = 0; break; }
            bitpos += consumed;
        }
        if (!ok) continue;
        
        int dc_bits = bitpos;
        int ac_bits = total_bits - dc_bits;
        int ac_buf_len = (ac_bits + 7) / 8 + 1;
        uint8_t *ac_data = calloc(ac_buf_len, 1);
        for (int i = 0; i < ac_bits; i++) {
            if (get_bit(fd + 40, dc_bits + i))
                ac_data[i >> 3] |= (1 << (7 - (i & 7)));
        }
        
        int run0[32] = {0}, run1[32] = {0};
        int cur_val = get_bit(ac_data, 0), cur_len = 1;
        int max0 = 0, max1 = 0;
        for (int i = 1; i < ac_bits; i++) {
            int b = get_bit(ac_data, i);
            if (b == cur_val) cur_len++;
            else {
                int idx = (cur_len < 32) ? cur_len : 31;
                if (cur_val == 0) { run0[idx]++; if (cur_len > max0) max0 = cur_len; }
                else { run1[idx]++; if (cur_len > max1) max1 = cur_len; }
                cur_val = b; cur_len = 1;
            }
        }
        int idx = (cur_len < 32) ? cur_len : 31;
        if (cur_val == 0) { run0[idx]++; if (cur_len > max0) max0 = cur_len; }
        else { run1[idx]++; if (cur_len > max1) max1 = cur_len; }
        
        printf("  F%02d  %3d  %3d  %4d  %5d  %4d  %4d\n",
               f, max0, max1, run0[6], run0[12], run1[6], run1[7]);
        
        free(ac_data);
    }
    
    printf("\nDone.\n");
    return 0;
}
