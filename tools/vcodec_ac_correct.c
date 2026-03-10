/*
 * vcodec_ac_correct.c - AC analysis with corrected DC decoder (extended VLC)
 * and proper Track 2 sector loading
 *
 * Focus on padded frame F03 where exact AC boundary is known (83079 bits)
 * Tests various AC coding schemes and compares real vs random data
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

/* Extended DC VLC (up to size 11) */
static const struct { int len; uint32_t code; } dc_vlc[] = {
    {3, 0x4}, {2, 0x0}, {2, 0x1}, {3, 0x5}, {3, 0x6},
    {4, 0xE}, {5, 0x1E}, {6, 0x3E}, {7, 0x7E},
    {8, 0xFE}, {9, 0x1FE}, {10, 0x3FE},
};
#define NUM_VLC 12

static int decode_dc(const uint8_t *data, int bitpos, int *dc_val, int total_bits) {
    for (int i = 0; i < NUM_VLC; i++) {
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

static void analyze_runs(const uint8_t *data, int nbits, const char *label) {
    int run0[32] = {0}, run1[32] = {0};
    int cv = get_bit(data, 0), cl = 1;
    int max0 = 0, max1 = 0;
    for (int i = 1; i < nbits; i++) {
        int b = get_bit(data, i);
        if (b == cv) cl++;
        else {
            int idx = cl < 32 ? cl : 31;
            if (cv == 0) { run0[idx]++; if (cl > max0) max0 = cl; }
            else { run1[idx]++; if (cl > max1) max1 = cl; }
            cv = b; cl = 1;
        }
    }
    int idx = cl < 32 ? cl : 31;
    if (cv == 0) { run0[idx]++; if (cl > max0) max0 = cl; }
    else { run1[idx]++; if (cl > max1) max1 = cl; }
    printf("  %s: max0=%d max1=%d\n", label, max0, max1);
    printf("    0-runs: ");
    for (int r = 1; r <= 14; r++) printf("r%d=%d ", r, run0[r]);
    printf("\n    1-runs: ");
    for (int r = 1; r <= 14; r++) printf("r%d=%d ", r, run1[r]);
    printf("\n");
}

/* Test: DC-like VLC (extended) for each AC coefficient, size=0 as EOB */
static void test_dc_vlc_ac(const uint8_t *ac_data, int ac_bits, int num_blocks, const char *label) {
    int bitpos = 0;
    int total_nz = 0, total_eob = 0;
    int bands[8] = {0};
    int blocks_ok = 0;
    int size_hist[16] = {0};
    
    for (int b = 0; b < num_blocks && bitpos < ac_bits; b++) {
        int pos = 0;
        int block_nz = 0;
        while (pos < 63 && bitpos < ac_bits) {
            /* Decode using extended DC VLC */
            int val;
            int consumed = decode_dc(ac_data, bitpos, &val, ac_bits);
            if (consumed < 0) break;
            bitpos += consumed;
            
            /* Track size */
            for (int v = 0; v < NUM_VLC; v++) {
                if (consumed == dc_vlc[v].len + v) {
                    size_hist[v]++;
                    break;
                }
            }
            
            if (val == 0) {
                /* size 0 = move to next position (zero coefficient) */
                pos++;
            } else {
                block_nz++;
                int band = pos / 8;
                if (band < 8) bands[band]++;
                pos++;
            }
        }
        total_nz += block_nz;
        blocks_ok++;
    }
    
    double pct = 100.0 * bitpos / ac_bits;
    printf("  DC-VLC-AC %s: %.1f%% (%d bits), blocks=%d, NZ=%d\n",
           label, pct, bitpos, blocks_ok, total_nz);
    printf("    bands: %d %d %d %d %d %d %d %d\n",
           bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
    printf("    sizes: s0=%d s1=%d s2=%d s3=%d s4=%d s5=%d s6=%d s7=%d s8=%d\n",
           size_hist[0],size_hist[1],size_hist[2],size_hist[3],
           size_hist[4],size_hist[5],size_hist[6],size_hist[7],size_hist[8]);
}

/* Test: DC VLC with EOB (size=0 at non-zero position = EOB) */
static void test_dc_vlc_eob(const uint8_t *ac_data, int ac_bits, int num_blocks, const char *label) {
    int bitpos = 0;
    int total_nz = 0, total_eob = 0;
    int bands[8] = {0};
    int blocks_ok = 0;
    
    for (int b = 0; b < num_blocks && bitpos < ac_bits; b++) {
        int pos = 0;
        int block_nz = 0;
        int first = 1;
        while (pos < 63 && bitpos < ac_bits) {
            int val;
            int consumed = decode_dc(ac_data, bitpos, &val, ac_bits);
            if (consumed < 0) break;
            bitpos += consumed;
            
            if (val == 0 && !first) {
                /* EOB - size 0 after first position */
                total_eob++;
                break;
            }
            first = 0;
            
            if (val == 0) {
                pos++; continue;
            }
            block_nz++;
            int band = pos / 8;
            if (band < 8) bands[band]++;
            pos++;
        }
        total_nz += block_nz;
        blocks_ok++;
    }
    
    double pct = 100.0 * bitpos / ac_bits;
    printf("  DC-VLC-EOB %s: %.1f%%, blocks=%d, NZ=%d, EOB=%d\n",
           label, pct, blocks_ok, total_nz, total_eob);
    printf("    bands: %d %d %d %d %d %d %d %d\n",
           bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
}

/* Test: Run-level with DC VLC for both run and level, (0,0)=EOB */
static void test_dc_vlc_runlevel(const uint8_t *ac_data, int ac_bits, int num_blocks, const char *label) {
    int bitpos = 0;
    int total_nz = 0, total_eob = 0;
    int bands[8] = {0};
    int blocks_ok = 0;
    int errors = 0;
    
    for (int b = 0; b < num_blocks && bitpos < ac_bits; b++) {
        int pos = 0;
        int block_nz = 0;
        while (pos < 63 && bitpos < ac_bits) {
            /* Decode run */
            int run;
            int consumed = decode_dc(ac_data, bitpos, &run, ac_bits);
            if (consumed < 0) { errors++; goto next_rl; }
            bitpos += consumed;
            
            /* Decode level */
            int level;
            consumed = decode_dc(ac_data, bitpos, &level, ac_bits);
            if (consumed < 0) { errors++; goto next_rl; }
            bitpos += consumed;
            
            if (run == 0 && level == 0) {
                total_eob++;
                goto next_rl;
            }
            
            /* Use absolute run value */
            int abs_run = run < 0 ? -run : run;
            pos += abs_run;
            if (pos >= 63) break;
            block_nz++;
            int band = pos / 8;
            if (band < 8) bands[band]++;
            pos++;
        }
        next_rl:
        total_nz += block_nz;
        blocks_ok++;
    }
    
    double pct = 100.0 * bitpos / ac_bits;
    printf("  DC-VLC-RL %s: %.1f%%, blocks=%d, NZ=%d, EOB=%d, err=%d\n",
           label, pct, blocks_ok, total_nz, total_eob, errors);
    printf("    bands: %d %d %d %d %d %d %d %d\n",
           bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
}

/* Test: Unary run (count 0s until 1) + DC VLC level */
static void test_unary_run_vlc_level(const uint8_t *ac_data, int ac_bits, int num_blocks, const char *label) {
    int bitpos = 0;
    int total_nz = 0, total_eob = 0;
    int bands[8] = {0};
    int blocks_ok = 0;
    
    for (int b = 0; b < num_blocks && bitpos < ac_bits; b++) {
        int pos = 0;
        int block_nz = 0;
        while (pos < 63 && bitpos < ac_bits) {
            /* Unary run: count 0-bits until 1 */
            int run = 0;
            while (bitpos < ac_bits && get_bit(ac_data, bitpos) == 0) {
                run++; bitpos++;
                if (run > 63) break;
            }
            if (bitpos >= ac_bits || run > 63) break;
            bitpos++; /* skip the 1 */
            
            if (run == 0) {
                /* Immediate 1: non-zero coefficient, decode level */
                int level;
                int consumed = decode_dc(ac_data, bitpos, &level, ac_bits);
                if (consumed < 0) break;
                bitpos += consumed;
                
                if (level == 0) {
                    /* EOB */
                    total_eob++;
                    goto next_url;
                }
                block_nz++;
                int band = pos / 8;
                if (band < 8) bands[band]++;
                pos++;
            } else {
                /* Skip 'run' zero positions, then decode level */
                pos += run;
                if (pos >= 63) break;
                
                int level;
                int consumed = decode_dc(ac_data, bitpos, &level, ac_bits);
                if (consumed < 0) break;
                bitpos += consumed;
                
                block_nz++;
                int band = pos / 8;
                if (band < 8) bands[band]++;
                pos++;
            }
        }
        next_url:
        total_nz += block_nz;
        blocks_ok++;
    }
    
    double pct = 100.0 * bitpos / ac_bits;
    printf("  Unary+VLC %s: %.1f%%, blocks=%d, NZ=%d, EOB=%d\n",
           label, pct, blocks_ok, total_nz, total_eob);
    printf("    bands: %d %d %d %d %d %d %d %d\n",
           bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
}

/* Test: Each AC coefficient coded as extended DC VLC (including the sign/magnitude) */
/* No EOB - process all 63 positions */
static void test_vlc_all_positions(const uint8_t *ac_data, int ac_bits, int num_blocks, const char *label) {
    int bitpos = 0;
    int total_nz = 0;
    int bands[8] = {0};
    int blocks_ok = 0;
    
    for (int b = 0; b < num_blocks && bitpos < ac_bits; b++) {
        int block_nz = 0;
        for (int pos = 0; pos < 63 && bitpos < ac_bits; pos++) {
            int val;
            int consumed = decode_dc(ac_data, bitpos, &val, ac_bits);
            if (consumed < 0) goto done_vap;
            bitpos += consumed;
            if (val != 0) {
                block_nz++;
                int band = pos / 8;
                if (band < 8) bands[band]++;
            }
        }
        total_nz += block_nz;
        blocks_ok++;
    }
    done_vap:;
    
    double pct = 100.0 * bitpos / ac_bits;
    printf("  VLC-all %s: %.1f%%, blocks=%d, NZ=%d\n",
           label, pct, blocks_ok, total_nz);
    printf("    bands: %d %d %d %d %d %d %d %d\n",
           bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
}

int main() {
    const char *binfile = "/tmp/Mari-nee no Heya (Japan) (Track 2).bin";
    int start_lba = 502;
    int num_blocks = 864;
    
    /* Load padded frame F03 */
    printf("=== Loading padded frame F03 ===\n");
    if (load_nth_frame(binfile, start_lba, 3) < 0) {
        fprintf(stderr, "Failed to load F03\n"); return 1;
    }
    
    int data_end = frame_len;
    while (data_end > 0 && frame_buf[data_end-1] == 0xFF) data_end--;
    int pad = frame_len - data_end;
    printf("QS=%d TYPE=%d pad=%d\n", frame_buf[3], frame_buf[39], pad);
    
    int total_bits = (data_end - 40) * 8;
    int bitpos = 0;
    for (int b = 0; b < num_blocks; b++) {
        int dc_val;
        int consumed = decode_dc(frame_buf + 40, bitpos, &dc_val, total_bits);
        if (consumed < 0) { printf("DC FAIL at block %d\n", b); return 1; }
        bitpos += consumed;
    }
    int dc_bits = bitpos;
    int ac_bits = total_bits - dc_bits;
    printf("DC=%d bits, AC=%d bits (%.1f/blk, %.2f/coeff)\n",
           dc_bits, ac_bits, (double)ac_bits/num_blocks, (double)ac_bits/(num_blocks*63));
    
    /* Extract AC data */
    int ac_buf_len = (ac_bits + 7) / 8 + 1;
    uint8_t *ac_data = calloc(ac_buf_len, 1);
    for (int i = 0; i < ac_bits; i++) {
        if (get_bit(frame_buf + 40, dc_bits + i))
            ac_data[i >> 3] |= (1 << (7 - (i & 7)));
    }
    
    /* Random control */
    uint8_t *rand_data = malloc(ac_buf_len);
    srand(42);
    for (int i = 0; i < ac_buf_len; i++) rand_data[i] = rand() & 0xFF;
    
    printf("\n--- AC run-length ---\n");
    analyze_runs(ac_data, ac_bits, "F03-AC");
    
    printf("\n=== Test 1: Extended DC VLC per AC position (no EOB) ===\n");
    test_dc_vlc_ac(ac_data, ac_bits, num_blocks, "REAL");
    test_dc_vlc_ac(rand_data, ac_bits, num_blocks, "RAND");
    
    printf("\n=== Test 2: Extended DC VLC per AC with EOB ===\n");
    test_dc_vlc_eob(ac_data, ac_bits, num_blocks, "REAL");
    test_dc_vlc_eob(rand_data, ac_bits, num_blocks, "RAND");
    
    printf("\n=== Test 3: DC VLC run-level (0,0)=EOB ===\n");
    test_dc_vlc_runlevel(ac_data, ac_bits, num_blocks, "REAL");
    test_dc_vlc_runlevel(rand_data, ac_bits, num_blocks, "RAND");
    
    printf("\n=== Test 4: Unary run + DC VLC level ===\n");
    test_unary_run_vlc_level(ac_data, ac_bits, num_blocks, "REAL");
    test_unary_run_vlc_level(rand_data, ac_bits, num_blocks, "RAND");
    
    printf("\n=== Test 5: VLC all 63 positions (no EOB) ===\n");
    test_vlc_all_positions(ac_data, ac_bits, num_blocks, "REAL");
    test_vlc_all_positions(rand_data, ac_bits, num_blocks, "RAND");
    
    /* Also test F00 for cross-check */
    printf("\n\n=== F00 cross-check ===\n");
    if (load_nth_frame(binfile, start_lba, 0) < 0) {
        fprintf(stderr, "Failed to load F00\n"); goto done;
    }
    data_end = frame_len;
    while (data_end > 0 && frame_buf[data_end-1] == 0xFF) data_end--;
    total_bits = (data_end - 40) * 8;
    bitpos = 0;
    for (int b = 0; b < num_blocks; b++) {
        int dc_val;
        int consumed = decode_dc(frame_buf + 40, bitpos, &dc_val, total_bits);
        if (consumed < 0) { printf("F00 DC FAIL\n"); goto done; }
        bitpos += consumed;
    }
    dc_bits = bitpos;
    ac_bits = total_bits - dc_bits;
    printf("F00: QS=%d DC=%d AC=%d\n", frame_buf[3], dc_bits, ac_bits);
    
    uint8_t *ac_f00 = calloc((ac_bits+7)/8+1, 1);
    for (int i = 0; i < ac_bits; i++) {
        if (get_bit(frame_buf + 40, dc_bits + i))
            ac_f00[i >> 3] |= (1 << (7 - (i & 7)));
    }
    
    test_dc_vlc_ac(ac_f00, ac_bits, num_blocks, "F00");
    test_dc_vlc_eob(ac_f00, ac_bits, num_blocks, "F00");
    test_vlc_all_positions(ac_f00, ac_bits, num_blocks, "F00");
    
    free(ac_f00);
    
done:
    free(ac_data);
    free(rand_data);
    printf("\nDone.\n");
    return 0;
}
