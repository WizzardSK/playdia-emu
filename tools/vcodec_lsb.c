/*
 * vcodec_lsb.c - Test LSB-first bit reading for AC coefficients
 * Also: run-length fingerprint on AC-only data, and arithmetic coding test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static uint8_t frame_data[16384];
static int frame_len;

/* MSB-first (standard) */
static int get_bit_msb(const uint8_t *data, int bitpos) {
    return (data[bitpos >> 3] >> (7 - (bitpos & 7))) & 1;
}
/* LSB-first */
static int get_bit_lsb(const uint8_t *data, int bitpos) {
    return (data[bitpos >> 3] >> (bitpos & 7)) & 1;
}

static uint32_t get_bits_msb(const uint8_t *data, int bitpos, int n) {
    uint32_t val = 0;
    for (int i = 0; i < n; i++)
        val = (val << 1) | get_bit_msb(data, bitpos + i);
    return val;
}
static uint32_t get_bits_lsb(const uint8_t *data, int bitpos, int n) {
    uint32_t val = 0;
    for (int i = 0; i < n; i++)
        val |= ((uint32_t)get_bit_lsb(data, bitpos + i)) << i;
    return val;
}

/* DC VLC table */
static const struct { int len; uint32_t code; } dc_lum_vlc[] = {
    {3, 0x4}, {2, 0x0}, {2, 0x1}, {3, 0x5}, {3, 0x6},
    {4, 0xE}, {5, 0x1E}, {6, 0x3E}, {7, 0x7E},
};

/* DC VLC reversed for LSB reading */
static uint32_t reverse_bits(uint32_t val, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++)
        r = (r << 1) | ((val >> i) & 1);
    return r;
}

static int decode_dc_msb(const uint8_t *data, int bitpos, int *dc_val, int total_bits) {
    for (int i = 0; i < 9; i++) {
        if (bitpos + dc_lum_vlc[i].len > total_bits) continue;
        uint32_t bits = get_bits_msb(data, bitpos, dc_lum_vlc[i].len);
        if (bits == dc_lum_vlc[i].code) {
            int sz = i, consumed = dc_lum_vlc[i].len;
            if (sz == 0) { *dc_val = 0; }
            else {
                if (bitpos + consumed + sz > total_bits) return -1;
                uint32_t raw = get_bits_msb(data, bitpos + consumed, sz);
                consumed += sz;
                *dc_val = (raw < (1u << (sz-1))) ? (int)raw - (1<<sz) + 1 : (int)raw;
            }
            return consumed;
        }
    }
    return -1;
}

static int decode_dc_lsb(const uint8_t *data, int bitpos, int *dc_val, int total_bits) {
    for (int i = 0; i < 9; i++) {
        if (bitpos + dc_lum_vlc[i].len > total_bits) continue;
        uint32_t bits = get_bits_lsb(data, bitpos, dc_lum_vlc[i].len);
        uint32_t rev_code = reverse_bits(dc_lum_vlc[i].code, dc_lum_vlc[i].len);
        if (bits == rev_code) {
            int sz = i, consumed = dc_lum_vlc[i].len;
            if (sz == 0) { *dc_val = 0; }
            else {
                if (bitpos + consumed + sz > total_bits) return -1;
                uint32_t raw = get_bits_lsb(data, bitpos + consumed, sz);
                consumed += sz;
                *dc_val = (raw < (1u << (sz-1))) ? (int)raw - (1<<sz) + 1 : (int)raw;
            }
            return consumed;
        }
    }
    return -1;
}

static int load_frame(const char *zipfile, int lba, int frame_idx) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "cd /tmp && unzip -o '%s' '*.bin' >/dev/null 2>&1", zipfile);
    system(cmd);
    FILE *fp = popen("find /tmp -maxdepth 1 -name '*.bin' -print -quit", "r");
    if (!fp) return -1;
    char binfile[512];
    if (!fgets(binfile, sizeof(binfile), fp)) { pclose(fp); return -1; }
    pclose(fp);
    binfile[strcspn(binfile, "\n")] = 0;

    fp = fopen(binfile, "rb");
    if (!fp) return -1;
    long offset = (long)(lba + frame_idx * 6) * 2352;
    fseek(fp, offset, SEEK_SET);
    frame_len = 0;
    for (int s = 0; s < 6; s++) {
        uint8_t sector[2352];
        if (fread(sector, 1, 2352, fp) != 2352) { fclose(fp); return -1; }
        memcpy(frame_data + frame_len, sector + 24, 2048);
        frame_len += 2048;
    }
    fclose(fp);
    return 0;
}

static int find_data_end(void) {
    int end = frame_len;
    while (end > 0 && frame_data[end - 1] == 0xFF) end--;
    return end;
}

/* Run-length analysis on a bit buffer */
static void analyze_runs(const uint8_t *data, int nbits, 
                         int (*get_bit_fn)(const uint8_t*, int),
                         const char *label) {
    int run0[32] = {0}, run1[32] = {0};
    int cur_val = get_bit_fn(data, 0);
    int cur_len = 1;
    int max0 = 0, max1 = 0;
    
    for (int i = 1; i < nbits; i++) {
        int b = get_bit_fn(data, i);
        if (b == cur_val) {
            cur_len++;
        } else {
            int idx = (cur_len < 32) ? cur_len : 31;
            if (cur_val == 0) { run0[idx]++; if (cur_len > max0) max0 = cur_len; }
            else { run1[idx]++; if (cur_len > max1) max1 = cur_len; }
            cur_val = b;
            cur_len = 1;
        }
    }
    /* last run */
    int idx = (cur_len < 32) ? cur_len : 31;
    if (cur_val == 0) { run0[idx]++; if (cur_len > max0) max0 = cur_len; }
    else { run1[idx]++; if (cur_len > max1) max1 = cur_len; }

    printf("  %s: max_run0=%d, max_run1=%d\n", label, max0, max1);
    printf("    0-runs: ");
    for (int r = 1; r <= 16; r++) printf("r%d=%d ", r, run0[r]);
    printf("\n    1-runs: ");
    for (int r = 1; r <= 16; r++) printf("r%d=%d ", r, run1[r]);
    printf("\n");
}

/* Test if DC decoding works with LSB-first */
static int test_dc_lsb(int num_blocks, int total_bits) {
    int bitpos = 0;
    int prev_dc = 0;
    int min_dc = 9999, max_dc = -9999;
    int ok = 0;
    
    for (int b = 0; b < num_blocks; b++) {
        int dc_val;
        int consumed = decode_dc_lsb(frame_data + 40, bitpos, &dc_val, total_bits);
        if (consumed < 0) {
            printf("  LSB DC decode failed at block %d, bitpos %d\n", b, bitpos);
            return -1;
        }
        bitpos += consumed;
        prev_dc += dc_val;
        if (prev_dc < min_dc) min_dc = prev_dc;
        if (prev_dc > max_dc) max_dc = prev_dc;
        ok++;
    }
    printf("  LSB DC: %d blocks decoded, %d bits (%.1f bits/block), range [%d, %d]\n",
           ok, bitpos, (double)bitpos/ok, min_dc, max_dc);
    return bitpos;
}

/* Byte-swapped (nibble-swapped) bit reading */
static uint8_t nibble_swap_buf[16384];
static void make_nibble_swapped(int len) {
    for (int i = 0; i < len; i++)
        nibble_swap_buf[i] = ((frame_data[i] & 0x0F) << 4) | ((frame_data[i] & 0xF0) >> 4);
}

/* Bit-reversed byte reading */
static uint8_t bitrev_buf[16384];
static void make_bitrev(int len) {
    for (int i = 0; i < len; i++) {
        uint8_t b = frame_data[i], r = 0;
        for (int j = 0; j < 8; j++) { r = (r << 1) | (b & 1); b >>= 1; }
        bitrev_buf[i] = r;
    }
}

/* Simple arithmetic coding detector: check if bitstream looks like range-coded data */
static void test_arithmetic_signature(const uint8_t *data, int nbits) {
    /* Arithmetic coding tends to produce very uniform byte distribution
       and the bit at position i is highly dependent on recent bits.
       Check conditional entropy H(X_n | X_{n-1}...X_{n-k}) for different k */
    
    int count[256] = {0};
    int nbytes = nbits / 8;
    for (int i = 0; i < nbytes; i++) count[data[i]]++;
    
    /* Chi-squared test for uniform byte distribution */
    double expected = (double)nbytes / 256;
    double chi2 = 0;
    for (int i = 0; i < 256; i++) {
        double d = count[i] - expected;
        chi2 += d * d / expected;
    }
    printf("  Byte uniformity chi2: %.1f (255 dof, expect ~255±23 for uniform)\n", chi2);
    
    /* Entropy */
    double entropy = 0;
    for (int i = 0; i < 256; i++) {
        if (count[i] > 0) {
            double p = (double)count[i] / nbytes;
            entropy -= p * log2(p);
        }
    }
    printf("  Byte entropy: %.4f bits (max 8.0)\n", entropy);
    
    /* Check 2-byte pair distribution (should be even more uniform for arithmetic) */
    int pair_count = 0;
    int pair_unique = 0;
    int pair_hist[65536];
    memset(pair_hist, 0, sizeof(pair_hist));
    for (int i = 0; i < nbytes - 1; i++) {
        int pair = (data[i] << 8) | data[i+1];
        pair_hist[pair]++;
        pair_count++;
    }
    for (int i = 0; i < 65536; i++)
        if (pair_hist[i] > 0) pair_unique++;
    
    double pair_expected = (double)pair_count / 65536;
    double pair_chi2 = 0;
    for (int i = 0; i < 65536; i++) {
        double d = pair_hist[i] - pair_expected;
        pair_chi2 += d * d / (pair_expected > 0 ? pair_expected : 1);
    }
    printf("  Byte-pair unique: %d/65536, chi2: %.1f\n", pair_unique, pair_chi2);
    
    /* Check for bit-level conditional dependencies (markov chain) */
    /* Already have transition probs from vcodec_structure. But also check 2nd order */
    int tri[8] = {0}; /* 000, 001, 010, 011, 100, 101, 110, 111 */
    for (int i = 0; i < nbits - 2; i++) {
        int b0 = (data[i/8] >> (7 - (i%8))) & 1;
        int b1 = (data[(i+1)/8] >> (7 - ((i+1)%8))) & 1;
        int b2 = (data[(i+2)/8] >> (7 - ((i+2)%8))) & 1;
        tri[(b0<<2)|(b1<<1)|b2]++;
    }
    printf("  Trigram (bit): 000=%d 001=%d 010=%d 011=%d 100=%d 101=%d 110=%d 111=%d\n",
           tri[0],tri[1],tri[2],tri[3],tri[4],tri[5],tri[6],tri[7]);
    
    /* Conditional entropy H(X2|X0,X1) */
    double h_cond = 0;
    for (int ctx = 0; ctx < 4; ctx++) {
        int n0 = tri[ctx*2];
        int n1 = tri[ctx*2+1];
        int total = n0 + n1;
        if (total > 0) {
            double p0 = (double)n0/total, p1 = (double)n1/total;
            double h = 0;
            if (p0 > 0) h -= p0 * log2(p0);
            if (p1 > 0) h -= p1 * log2(p1);
            h_cond += h * total / (nbits - 2);
        }
    }
    printf("  Conditional entropy H(b|b-1,b-2): %.4f bits (1.0 = perfectly random)\n", h_cond);
}

int main(int argc, char **argv) {
    const char *zipfile = "/home/wizzard/share/GitHub/Mari-nee no Heya (Japan).zip";
    int lba = 502;
    int num_blocks = 864;

    if (load_frame(zipfile, lba, 0) < 0) {
        fprintf(stderr, "Failed to load frame\n"); return 1;
    }
    int data_end = find_data_end();
    int pad_bytes = frame_len - data_end;
    printf("=== Frame F00: data_end=%d, padding=%d ===\n", data_end, pad_bytes);
    printf("Header bytes 0-3: %02X %02X %02X %02X\n", 
           frame_data[0], frame_data[1], frame_data[2], frame_data[3]);
    printf("Header bytes 36-39: %02X %02X %02X %02X\n",
           frame_data[36], frame_data[37], frame_data[38], frame_data[39]);

    int total_data_bits = (data_end - 40) * 8;

    /* Test 1: LSB-first DC decoding */
    printf("\n=== Test 1: LSB-first DC decoding ===\n");
    int lsb_dc_bits = test_dc_lsb(num_blocks, total_data_bits);
    
    /* Test 2: MSB DC (confirmed), then AC run-length analysis */
    printf("\n=== Test 2: MSB DC + AC run-length fingerprint ===\n");
    int bitpos = 0;
    int prev_dc = 0;
    for (int b = 0; b < num_blocks; b++) {
        int dc_val;
        int consumed = decode_dc_msb(frame_data + 40, bitpos, &dc_val, total_data_bits);
        if (consumed < 0) { printf("DC error at block %d\n", b); return 1; }
        bitpos += consumed;
        prev_dc += dc_val;
    }
    int dc_bits = bitpos;
    int ac_bits = total_data_bits - dc_bits;
    printf("DC: %d bits, AC start at bit %d, AC: %d bits\n", dc_bits, dc_bits, ac_bits);

    /* Extract AC-only bits into separate buffer */
    int ac_buf_len = (ac_bits + 7) / 8 + 1;
    uint8_t *ac_data = calloc(ac_buf_len, 1);
    for (int i = 0; i < ac_bits; i++) {
        int val = get_bit_msb(frame_data + 40, dc_bits + i);
        if (val) ac_data[i >> 3] |= (1 << (7 - (i & 7)));
    }

    printf("\n  --- AC-only run-length analysis (MSB) ---\n");
    analyze_runs(ac_data, ac_bits, get_bit_msb, "AC-MSB");
    
    /* Also make LSB version of AC data */
    printf("\n  --- AC-only run-length analysis (LSB) ---\n");
    /* For LSB, we need to re-extract from frame reading LSB-first */
    uint8_t *ac_data_lsb = calloc(ac_buf_len, 1);
    for (int i = 0; i < ac_bits; i++) {
        /* Read from original frame data MSB, but pack into LSB buffer */
        int val = get_bit_msb(frame_data + 40, dc_bits + i);
        if (val) ac_data_lsb[i >> 3] |= (1 << (i & 7));
    }
    analyze_runs(ac_data_lsb, ac_bits, get_bit_lsb, "AC-LSB");

    /* Test 3: Arithmetic coding signatures on AC data */
    printf("\n=== Test 3: Arithmetic coding signatures ===\n");
    test_arithmetic_signature(ac_data, ac_bits);
    
    /* Generate random data for comparison */
    printf("\n  --- Random data comparison ---\n");
    uint8_t *rand_data = malloc(ac_buf_len);
    srand(12345);
    for (int i = 0; i < ac_buf_len; i++) rand_data[i] = rand() & 0xFF;
    test_arithmetic_signature(rand_data, ac_bits);

    /* Test 4: Bit-reversed frame */
    printf("\n=== Test 4: Bit-reversed byte reading ===\n");
    make_bitrev(frame_len);
    printf("Bitrev header bytes 0-3: %02X %02X %02X %02X\n",
           bitrev_buf[0], bitrev_buf[1], bitrev_buf[2], bitrev_buf[3]);
    printf("Bitrev header bytes 36-39: %02X %02X %02X %02X\n",
           bitrev_buf[36], bitrev_buf[37], bitrev_buf[38], bitrev_buf[39]);

    /* Test 5: Nibble-swapped */
    printf("\n=== Test 5: Nibble-swapped reading ===\n");
    make_nibble_swapped(frame_len);
    printf("Nibswap header bytes 0-3: %02X %02X %02X %02X\n",
           nibble_swap_buf[0], nibble_swap_buf[1], nibble_swap_buf[2], nibble_swap_buf[3]);

    /* Test 6: First 200 bits of AC data - show raw pattern */
    printf("\n=== Test 6: First 200 AC bits (MSB reading) ===\n");
    for (int i = 0; i < 200 && i < ac_bits; i++) {
        printf("%d", get_bit_msb(ac_data, i));
        if ((i+1) % 64 == 0) printf("\n");
    }
    printf("\n");

    /* Test 7: Look for repeated patterns in AC data */
    printf("\n=== Test 7: Most common 8-bit patterns in AC ===\n");
    int byte_hist[256] = {0};
    int ac_bytes = ac_bits / 8;
    for (int i = 0; i < ac_bytes; i++) byte_hist[ac_data[i]]++;
    
    /* Sort and show top 10 */
    for (int top = 0; top < 10; top++) {
        int max_idx = 0, max_val = 0;
        for (int i = 0; i < 256; i++) {
            if (byte_hist[i] > max_val) { max_val = byte_hist[i]; max_idx = i; }
        }
        printf("  0x%02X (%3d): %d (%.2f%%)\n", max_idx, max_idx, max_val, 100.0*max_val/ac_bytes);
        byte_hist[max_idx] = -1;
    }

    /* Test 8: Check if AC data has structure when split by macroblock position */
    printf("\n=== Test 8: AC bit consumption per macroblock row ===\n");
    /* If there's a fixed bit budget per macroblock, we might see patterns */
    /* Count how many bits each DC-VLC-decoded macroblock uses */
    /* We don't know AC, but we can check DC bits per macroblock row */
    bitpos = 0;
    for (int row = 0; row < 9; row++) {
        int row_start = bitpos;
        for (int col = 0; col < 16; col++) {
            for (int blk = 0; blk < 6; blk++) {
                int dc_val;
                int consumed = decode_dc_msb(frame_data + 40, bitpos, &dc_val, total_data_bits);
                if (consumed < 0) { printf("  Error at row %d col %d blk %d\n", row, col, blk); goto done8; }
                bitpos += consumed;
            }
        }
        printf("  Row %d: DC bits=%d (avg %.1f/MB)\n", row, bitpos - row_start, 
               (double)(bitpos - row_start) / 16);
    }
    done8:

    /* Test 9: Check alignment - are there byte boundaries in the AC data? */
    printf("\n=== Test 9: Byte alignment patterns ===\n");
    printf("  DC ends at bit %d (byte %d, bit offset %d)\n", dc_bits, dc_bits/8, dc_bits%8);
    /* Check if DC end is byte-aligned */
    if (dc_bits % 8 == 0) printf("  DC IS byte-aligned!\n");
    else printf("  DC is NOT byte-aligned\n");
    
    /* Check if DC end is 16-bit aligned */
    if (dc_bits % 16 == 0) printf("  DC IS 16-bit aligned!\n");
    
    free(ac_data);
    free(ac_data_lsb);
    free(rand_data);
    printf("\nDone.\n");
    return 0;
}
