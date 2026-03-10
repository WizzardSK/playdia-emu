/*
 * vcodec_acstruct.c - Look for structural patterns in AC data
 * - Check for byte/word alignment of blocks
 * - Check for fixed bit budgets
 * - Look at bit patterns at regular intervals
 * - Test reversed unary (count 1s before 0) for run coding
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static uint8_t frame_buf[16384];
static int frame_len;

static int get_bit(const uint8_t *data, int bitpos) {
    return (data[bitpos >> 3] >> (7 - (bitpos & 7))) & 1;
}
static uint32_t get_bits(const uint8_t *data, int bitpos, int n) {
    uint32_t val = 0;
    for (int i = 0; i < n; i++) val = (val << 1) | get_bit(data, bitpos + i);
    return val;
}

static const struct { int len; uint32_t code; } dc_vlc[] = {
    {3, 0x4}, {2, 0x0}, {2, 0x1}, {3, 0x5}, {3, 0x6},
    {4, 0xE}, {5, 0x1E}, {6, 0x3E}, {7, 0x7E},
    {8, 0xFE}, {9, 0x1FE}, {10, 0x3FE},
};

static int decode_dc(const uint8_t *data, int bitpos, int *dc_val, int total_bits) {
    for (int i = 0; i < 12; i++) {
        if (bitpos + dc_vlc[i].len > total_bits) continue;
        uint32_t bits = get_bits(data, bitpos, dc_vlc[i].len);
        if (bits == dc_vlc[i].code) {
            int sz = i, consumed = dc_vlc[i].len;
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

static int load_nth_frame(const char *binfile, int start_lba, int target_frame) {
    FILE *fp = fopen(binfile, "rb");
    if (!fp) return -1;
    int frame_count = 0, f1_count = 0;
    frame_len = 0;
    for (int sec = 0; sec < 500; sec++) {
        long offset = (long)(start_lba + sec) * 2352;
        uint8_t sector[2352];
        fseek(fp, offset, SEEK_SET);
        if (fread(sector, 1, 2352, fp) != 2352) break;
        uint8_t type = sector[24];
        if (type == 0xF1) {
            if (frame_count == target_frame && f1_count < 6)
                memcpy(frame_buf + f1_count * 2047, sector + 25, 2047);
            f1_count++;
        } else if (type == 0xF2) {
            if (frame_count == target_frame && f1_count == 6) {
                frame_len = 6 * 2047;
                fclose(fp);
                return 0;
            }
            frame_count++;
            f1_count = 0;
        } else if (type == 0xF3) {
            f1_count = 0;
        }
    }
    fclose(fp);
    return -1;
}

/* Test: reversed unary run (count 1s until 0) + DC VLC level, level=0=EOB */
static void test_rev_unary_vlc(const uint8_t *ac_data, int ac_bits, int num_blocks, const char *label) {
    int bitpos = 0;
    int total_nz = 0, total_eob = 0;
    int bands[8] = {0};
    int blocks_ok = 0;
    int run_hist[32] = {0};
    
    for (int b = 0; b < num_blocks && bitpos < ac_bits; b++) {
        int pos = 0;
        int block_nz = 0;
        while (pos < 63 && bitpos < ac_bits) {
            /* Count 1s until 0 (reversed unary) */
            int run = 0;
            while (bitpos < ac_bits && get_bit(ac_data, bitpos) == 1) {
                run++; bitpos++;
                if (run > 63) break;
            }
            if (bitpos >= ac_bits || run > 63) break;
            bitpos++; /* skip the 0 */
            
            /* Decode level using DC VLC */
            int level;
            int consumed = decode_dc(ac_data, bitpos, &level, ac_bits);
            if (consumed < 0) break;
            bitpos += consumed;
            
            if (level == 0) {
                /* EOB */
                total_eob++;
                break;
            }
            
            pos += run;
            if (pos >= 63) break;
            block_nz++;
            if (run < 32) run_hist[run]++;
            int band = pos / 8;
            if (band < 8) bands[band]++;
            pos++;
        }
        total_nz += block_nz;
        blocks_ok++;
    }
    
    double pct = 100.0 * bitpos / ac_bits;
    printf("  RevUnary+VLC %s: %.1f%%, blocks=%d, NZ=%d, EOB=%d\n",
           label, pct, blocks_ok, total_nz, total_eob);
    printf("    bands: %d %d %d %d %d %d %d %d\n",
           bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
    printf("    runs: r0=%d r1=%d r2=%d r3=%d r4=%d r5=%d r6=%d r7=%d\n",
           run_hist[0],run_hist[1],run_hist[2],run_hist[3],
           run_hist[4],run_hist[5],run_hist[6],run_hist[7]);
}

/* Test: DC VLC for level only, skip zeros implicitly (position = cumulative) */
/* Each VLC decodes one AC coefficient in zigzag order. 0 = zero, size>0 = non-zero */
/* No explicit EOB - read until 63 coefficients or run out of bits */
static void test_vlc_sequential(const uint8_t *ac_data, int ac_bits, int num_blocks, const char *label) {
    int bitpos = 0;
    int total_nz = 0;
    int bands[8] = {0};
    int blocks_ok = 0;
    int size_hist[16] = {0};
    int fail_count = 0;
    
    for (int b = 0; b < num_blocks && bitpos < ac_bits; b++) {
        int block_nz = 0;
        int ok = 1;
        for (int pos = 0; pos < 63 && bitpos < ac_bits; pos++) {
            int val;
            int consumed = decode_dc(ac_data, bitpos, &val, ac_bits);
            if (consumed < 0) { fail_count++; ok = 0; break; }
            bitpos += consumed;
            
            /* Track size */
            for (int v = 0; v < 12; v++) {
                if (consumed == dc_vlc[v].len + v) { size_hist[v]++; break; }
            }
            
            if (val != 0) {
                block_nz++;
                int band = pos / 8;
                if (band < 8) bands[band]++;
            }
        }
        if (ok) {
            total_nz += block_nz;
            blocks_ok++;
        } else {
            /* On failure, try to find next valid VLC start? Just skip 1 bit */
            bitpos++;
        }
    }
    
    double pct = 100.0 * bitpos / ac_bits;
    printf("  VLC-seq %s: %.1f%%, blocks=%d (ok=%d fail=%d), NZ=%d\n",
           label, pct, num_blocks, blocks_ok, fail_count, total_nz);
    printf("    bands: %d %d %d %d %d %d %d %d\n",
           bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
    printf("    sizes: ");
    for (int i = 0; i < 10; i++) printf("s%d=%d ", i, size_hist[i]);
    printf("\n");
}

/* Test: N-bit value per coefficient, look for which N gives ~100% on padded frame */
static void test_fixed_bits_per_coeff(const uint8_t *ac_data, int ac_bits, int num_blocks) {
    printf("  Fixed bits per coefficient:\n");
    for (int nbits = 1; nbits <= 3; nbits++) {
        int total_needed = num_blocks * 63 * nbits;
        printf("    %d bits: need %d, have %d (%.1f%%)\n", 
               nbits, total_needed, ac_bits, 100.0 * ac_bits / total_needed);
    }
    /* 1.53 bits/coeff → not integer. Maybe some positions use 1 bit, some use 2? */
    /* What if: first K positions use 2 bits, rest use 1 bit? */
    for (int k = 0; k <= 63; k++) {
        int bits_needed = num_blocks * (k * 2 + (63 - k) * 1);
        if (abs(bits_needed - ac_bits) < 50) {
            printf("    MATCH: %d positions @2bit + %d @1bit = %d bits (diff=%d)\n",
                   k, 63-k, bits_needed, bits_needed - ac_bits);
        }
    }
    /* Or: each block has a 1-byte header + variable bits? */
    int header_bits = num_blocks * 8;
    int remaining = ac_bits - header_bits;
    printf("    With 8-bit header per block: %d remaining = %.2f bits/coeff\n",
           remaining, (double)remaining / (num_blocks * 63));
}

/* Test: Autocorrelation at block-level intervals */
static void test_block_autocorrelation(const uint8_t *ac_data, int ac_bits) {
    printf("  Autocorrelation at various offsets (normalized):\n");
    int offsets[] = {96, 97, 107, 108, 109, 192, 200, 577, 578, 640};
    int n_offsets = 10;
    
    /* Calculate mean */
    double mean = 0;
    for (int i = 0; i < ac_bits; i++) mean += get_bit(ac_data, i);
    mean /= ac_bits;
    
    for (int oi = 0; oi < n_offsets; oi++) {
        int offset = offsets[oi];
        double sum = 0, var = 0;
        int count = ac_bits - offset;
        for (int i = 0; i < count; i++) {
            double a = get_bit(ac_data, i) - mean;
            double b = get_bit(ac_data, i + offset) - mean;
            sum += a * b;
            var += a * a;
        }
        double autocorr = (var > 0) ? sum / var : 0;
        printf("    offset=%3d: %.4f%s\n", offset, autocorr,
               fabs(autocorr) > 0.01 ? " ***" : "");
    }
}

int main() {
    const char *binfile = "/tmp/Mari-nee no Heya (Japan) (Track 2).bin";
    int start_lba = 502;
    int num_blocks = 864;
    
    /* Test on both F03 (padded) and F00 (full) */
    int test_frames[] = {3, 0};
    
    for (int tf = 0; tf < 2; tf++) {
        int fi = test_frames[tf];
        printf("=== Frame F%02d ===\n", fi);
        if (load_nth_frame(binfile, start_lba, fi) < 0) {
            printf("Load failed\n"); continue;
        }
        
        int data_end = frame_len;
        while (data_end > 0 && frame_buf[data_end-1] == 0xFF) data_end--;
        int pad = frame_len - data_end;
        int total_bits = (data_end - 40) * 8;
        
        int bitpos = 0;
        for (int b = 0; b < num_blocks; b++) {
            int dc_val;
            int consumed = decode_dc(frame_buf + 40, bitpos, &dc_val, total_bits);
            if (consumed < 0) { printf("DC fail at %d\n", b); goto next_frame; }
            bitpos += consumed;
        }
        int dc_bits = bitpos;
        int ac_bits = total_bits - dc_bits;
        printf("QS=%d TYPE=%d pad=%d DC=%d AC=%d (%.2f bits/coeff)\n",
               frame_buf[3], frame_buf[39], pad, dc_bits, ac_bits,
               (double)ac_bits / (num_blocks * 63));
        
        int ac_buf_len = (ac_bits + 7) / 8 + 1;
        uint8_t *ac_data = calloc(ac_buf_len, 1);
        for (int i = 0; i < ac_bits; i++) {
            if (get_bit(frame_buf + 40, dc_bits + i))
                ac_data[i >> 3] |= (1 << (7 - (i & 7)));
        }
        
        /* Random control */
        uint8_t *rand_data = malloc(ac_buf_len);
        srand(42 + fi);
        for (int i = 0; i < ac_buf_len; i++) rand_data[i] = rand() & 0xFF;
        
        printf("\n--- Rev unary (1s→0) + VLC level ---\n");
        test_rev_unary_vlc(ac_data, ac_bits, num_blocks, "REAL");
        test_rev_unary_vlc(rand_data, ac_bits, num_blocks, "RAND");
        
        printf("\n--- VLC sequential (per position, skip on fail) ---\n");
        test_vlc_sequential(ac_data, ac_bits, num_blocks, "REAL");
        test_vlc_sequential(rand_data, ac_bits, num_blocks, "RAND");
        
        printf("\n--- Fixed bits analysis ---\n");
        test_fixed_bits_per_coeff(ac_data, ac_bits, num_blocks);
        
        printf("\n--- Block-level autocorrelation ---\n");
        test_block_autocorrelation(ac_data, ac_bits);
        
        free(ac_data);
        free(rand_data);
        next_frame:
        printf("\n");
    }
    
    return 0;
}
