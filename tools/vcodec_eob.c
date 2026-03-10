/*
 * vcodec_eob.c - Test flag+value AC coding WITH End-of-Block optimization
 * 
 * Previous flag+value tests consumed exactly 100% because every block decoded
 * exactly 63 AC positions. With EOB, blocks with few non-zero coefficients
 * stop early, making it variable-rate per block.
 *
 * Tests against padded frame F03 where exact data boundary is known:
 * data_end=10914 bytes, DC=4233 bits, real AC=83079 bits
 *
 * Also tests Golomb-Rice with k=6 (suggested by run-length fingerprint:
 * max run=12, r6 anomaly → unary prefix up to 6 + 6-bit suffix = 12 max)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* MPEG-1 DC luminance VLC table (Table B.12) */
static const struct { int len; uint32_t code; } dc_lum_vlc[] = {
    {3, 0x4}, /* 100 -> size 0 */
    {2, 0x0}, /* 00  -> size 1 */
    {2, 0x1}, /* 01  -> size 2 */
    {3, 0x5}, /* 101 -> size 3 */
    {3, 0x6}, /* 110 -> size 4 */
    {4, 0xE}, /* 1110 -> size 5 */
    {5, 0x1E},/* 11110 -> size 6 */
    {6, 0x3E},/* 111110 -> size 7 */
    {7, 0x7E},/* 1111110 -> size 8 */
};

static uint8_t frame_data[16384];
static int frame_len;

static int get_bit(const uint8_t *data, int bitpos) {
    return (data[bitpos >> 3] >> (7 - (bitpos & 7))) & 1;
}

static uint32_t get_bits(const uint8_t *data, int bitpos, int n) {
    uint32_t val = 0;
    for (int i = 0; i < n; i++)
        val = (val << 1) | get_bit(data, bitpos + i);
    return val;
}

/* Decode DC using MPEG-1 luminance VLC, return bits consumed */
static int decode_dc(const uint8_t *data, int bitpos, int *dc_val, int total_bits) {
    for (int i = 0; i < 9; i++) {
        if (bitpos + dc_lum_vlc[i].len > total_bits) continue;
        uint32_t bits = get_bits(data, bitpos, dc_lum_vlc[i].len);
        if (bits == dc_lum_vlc[i].code) {
            int sz = i;
            int consumed = dc_lum_vlc[i].len;
            if (sz == 0) {
                *dc_val = 0;
            } else {
                if (bitpos + consumed + sz > total_bits) return -1;
                uint32_t raw = get_bits(data, bitpos + consumed, sz);
                consumed += sz;
                if (raw < (1u << (sz - 1)))
                    *dc_val = (int)raw - (1 << sz) + 1;
                else
                    *dc_val = (int)raw;
            }
            return consumed;
        }
    }
    return -1;
}

/* Load frame from zip */
static int load_frame(const char *zipfile, int lba, int frame_idx) {
    /* First extract */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "cd /tmp && unzip -o '%s' '*.bin' >/dev/null 2>&1", zipfile);
    system(cmd);

    /* Then find the bin file */
    FILE *fp = popen("find /tmp -maxdepth 1 -name '*.bin' -print -quit", "r");
    if (!fp) return -1;
    char binfile[512];
    if (!fgets(binfile, sizeof(binfile), fp)) { pclose(fp); return -1; }
    pclose(fp);
    binfile[strcspn(binfile, "\n")] = 0;

    fp = fopen(binfile, "rb");
    if (!fp) return -1;

    /* Seek to LBA, read 6 F1 sectors */
    long offset = (long)(lba + frame_idx * 6) * 2352;
    fseek(fp, offset, SEEK_SET);

    frame_len = 0;
    for (int s = 0; s < 6; s++) {
        uint8_t sector[2352];
        if (fread(sector, 1, 2352, fp) != 2352) { fclose(fp); return -1; }
        /* Skip 12 sync + 4 header + 8 subheader = 24 bytes */
        memcpy(frame_data + frame_len, sector + 24, 2048);
        frame_len += 2048;
    }
    fclose(fp);
    return 0;
}

/* Find padding end (scan from end for 0xFF bytes) */
static int find_data_end(void) {
    int end = frame_len;
    while (end > 0 && frame_data[end - 1] == 0xFF)
        end--;
    return end;
}

/* Skip DC coefficients for all blocks, return bit position after all DCs */
static int skip_all_dc(int num_blocks, int total_bits) {
    int bitpos = 0;
    int prev_dc = 0;
    for (int b = 0; b < num_blocks; b++) {
        int dc_val;
        int consumed = decode_dc(frame_data + 40, bitpos, &dc_val, total_bits);
        if (consumed < 0) return -1;
        bitpos += consumed;
        prev_dc += dc_val;
    }
    return bitpos;
}

/*
 * Scheme A: Per AC position: 0=zero, 1+Nbit=value (value=0 → EOB)
 * With N-bit values, value 0 is reserved as EOB marker
 */
static void test_scheme_a(const uint8_t *ac_data, int ac_bits, int num_blocks, int vbits, const char *label) {
    int bitpos = 0;
    int total_nz = 0, total_eob = 0;
    int bands[8] = {0};
    int blocks_ok = 0;
    int errors = 0;

    for (int b = 0; b < num_blocks && bitpos < ac_bits; b++) {
        int pos = 0;
        int block_nz = 0;
        while (pos < 63 && bitpos < ac_bits) {
            int flag = get_bit(ac_data, bitpos);
            bitpos++;
            if (flag == 0) {
                pos++;
                continue;
            }
            /* flag=1, read value */
            if (bitpos + vbits > ac_bits) { errors++; goto next_a; }
            uint32_t val = get_bits(ac_data, bitpos, vbits);
            bitpos += vbits;
            if (val == 0) {
                /* EOB */
                total_eob++;
                break;
            }
            block_nz++;
            int band = pos / 8;
            if (band < 8) bands[band]++;
            pos++;
        }
        total_nz += block_nz;
        blocks_ok++;
        next_a:;
    }

    double pct = 100.0 * bitpos / ac_bits;
    printf("  SchemeA %s (%d-bit val): %.1f%% consumed, blocks=%d, NZ=%d, EOB=%d, errors=%d\n",
           label, vbits, pct, blocks_ok, total_nz, total_eob, errors);
    printf("    bands: %d %d %d %d %d %d %d %d\n",
           bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
}

/*
 * Scheme B: 0=zero, 10=EOB, 11+Nbit=value
 * More explicit EOB signaling
 */
static void test_scheme_b(const uint8_t *ac_data, int ac_bits, int num_blocks, int vbits, const char *label) {
    int bitpos = 0;
    int total_nz = 0, total_eob = 0;
    int bands[8] = {0};
    int blocks_ok = 0;

    for (int b = 0; b < num_blocks && bitpos < ac_bits; b++) {
        int pos = 0;
        int block_nz = 0;
        while (pos < 63 && bitpos < ac_bits) {
            int b0 = get_bit(ac_data, bitpos);
            bitpos++;
            if (b0 == 0) {
                pos++;
                continue;
            }
            /* b0=1 */
            if (bitpos >= ac_bits) break;
            int b1 = get_bit(ac_data, bitpos);
            bitpos++;
            if (b1 == 0) {
                /* 10 = EOB */
                total_eob++;
                goto next_b;
            }
            /* 11 + value */
            if (bitpos + vbits > ac_bits) break;
            uint32_t val = get_bits(ac_data, bitpos, vbits);
            bitpos += vbits;
            block_nz++;
            int band = pos / 8;
            if (band < 8) bands[band]++;
            pos++;
        }
        next_b:
        total_nz += block_nz;
        blocks_ok++;
    }

    double pct = 100.0 * bitpos / ac_bits;
    printf("  SchemeB %s (%d-bit val): %.1f%% consumed, blocks=%d, NZ=%d, EOB=%d\n",
           label, vbits, pct, blocks_ok, total_nz, total_eob);
    printf("    bands: %d %d %d %d %d %d %d %d\n",
           bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
}

/*
 * Scheme C: 0=zero, 1+DC_VLC(value), size=0=EOB
 * Uses DC luminance VLC for the value part
 */
static void test_scheme_c(const uint8_t *ac_data, int ac_bits, int num_blocks, const char *label) {
    int bitpos = 0;
    int total_nz = 0, total_eob = 0;
    int bands[8] = {0};
    int blocks_ok = 0;
    int errors = 0;

    for (int b = 0; b < num_blocks && bitpos < ac_bits; b++) {
        int pos = 0;
        int block_nz = 0;
        while (pos < 63 && bitpos < ac_bits) {
            int flag = get_bit(ac_data, bitpos);
            bitpos++;
            if (flag == 0) {
                pos++;
                continue;
            }
            /* flag=1, decode DC VLC for value */
            int val;
            int consumed = decode_dc(ac_data, bitpos, &val, ac_bits);
            if (consumed < 0) { errors++; goto next_c; }
            bitpos += consumed;
            if (val == 0) {
                /* size=0 → EOB */
                total_eob++;
                goto next_c;
            }
            block_nz++;
            int band = pos / 8;
            if (band < 8) bands[band]++;
            pos++;
        }
        total_nz += block_nz;
        blocks_ok++;
        next_c:;
    }

    double pct = 100.0 * bitpos / ac_bits;
    printf("  SchemeC %s (flag+DC_VLC): %.1f%% consumed, blocks=%d, NZ=%d, EOB=%d, errors=%d\n",
           label, pct, blocks_ok, total_nz, total_eob, errors);
    printf("    bands: %d %d %d %d %d %d %d %d\n",
           bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
}

/*
 * Scheme D: 00=zero, 01=EOB, 1+Nbit=value
 * 2-bit prefix for zero/EOB, 1-bit prefix for value
 */
static void test_scheme_d(const uint8_t *ac_data, int ac_bits, int num_blocks, int vbits, const char *label) {
    int bitpos = 0;
    int total_nz = 0, total_eob = 0;
    int bands[8] = {0};
    int blocks_ok = 0;

    for (int b = 0; b < num_blocks && bitpos < ac_bits; b++) {
        int pos = 0;
        int block_nz = 0;
        while (pos < 63 && bitpos < ac_bits) {
            int b0 = get_bit(ac_data, bitpos);
            bitpos++;
            if (b0 == 1) {
                /* 1 + value */
                if (bitpos + vbits > ac_bits) break;
                uint32_t val = get_bits(ac_data, bitpos, vbits);
                bitpos += vbits;
                block_nz++;
                int band = pos / 8;
                if (band < 8) bands[band]++;
                pos++;
                continue;
            }
            /* b0=0 */
            if (bitpos >= ac_bits) break;
            int b1 = get_bit(ac_data, bitpos);
            bitpos++;
            if (b1 == 1) {
                /* 01 = EOB */
                total_eob++;
                goto next_d;
            }
            /* 00 = zero */
            pos++;
        }
        next_d:
        total_nz += block_nz;
        blocks_ok++;
    }

    double pct = 100.0 * bitpos / ac_bits;
    printf("  SchemeD %s (%d-bit val): %.1f%% consumed, blocks=%d, NZ=%d, EOB=%d\n",
           label, vbits, pct, blocks_ok, total_nz, total_eob);
    printf("    bands: %d %d %d %d %d %d %d %d\n",
           bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
}

/*
 * Golomb-Rice coding: unary prefix (count of 1s before 0) + k fixed bits
 * With k=6: max prefix=6 (before 0 terminator) + 6 bits = 13 bits max
 * This could explain the run-length fingerprint (max run 12, r6 anomaly)
 */
static void test_golomb_rice(const uint8_t *ac_data, int ac_bits, int num_blocks, int k, const char *label) {
    int bitpos = 0;
    int total_nz = 0, total_eob = 0;
    int bands[8] = {0};
    int blocks_ok = 0;
    int value_hist[32] = {0};

    for (int b = 0; b < num_blocks && bitpos < ac_bits; b++) {
        int pos = 0;
        int block_nz = 0;
        while (pos < 63 && bitpos < ac_bits) {
            /* Read unary part: count 1s until 0 */
            int q = 0;
            while (bitpos < ac_bits && get_bit(ac_data, bitpos) == 1) {
                q++;
                bitpos++;
                if (q > 20) break; /* safety */
            }
            if (bitpos >= ac_bits || q > 20) break;
            bitpos++; /* skip the 0 terminator */

            /* Read k fixed bits */
            if (bitpos + k > ac_bits) break;
            uint32_t r = get_bits(ac_data, bitpos, k);
            bitpos += k;

            uint32_t value = (q << k) | r;
            if (value == 0) {
                /* value 0 = zero coefficient */
                pos++;
            } else if (value == 1) {
                /* value 1 = EOB? Or just coeff=1? Test both interpretations */
                /* For now: value > 0 = non-zero coefficient */
                block_nz++;
                int band = pos / 8;
                if (band < 8) bands[band]++;
                if (value < 32) value_hist[value]++;
                pos++;
            } else {
                block_nz++;
                int band = pos / 8;
                if (band < 8) bands[band]++;
                if (value < 32) value_hist[value]++;
                pos++;
            }
        }
        total_nz += block_nz;
        blocks_ok++;
    }

    double pct = 100.0 * bitpos / ac_bits;
    printf("  GolombRice %s (k=%d): %.1f%% consumed, blocks=%d, NZ=%d\n",
           label, k, pct, blocks_ok, total_nz);
    printf("    bands: %d %d %d %d %d %d %d %d\n",
           bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
    printf("    value hist: v1=%d v2=%d v3=%d v4=%d v5=%d v6=%d v7=%d v8=%d\n",
           value_hist[1],value_hist[2],value_hist[3],value_hist[4],
           value_hist[5],value_hist[6],value_hist[7],value_hist[8]);
}

/*
 * Golomb-Rice with EOB: value=0 at position 0 means EOB
 * Separate zero-run + level coding using Golomb-Rice
 */
static void test_golomb_rice_runlevel(const uint8_t *ac_data, int ac_bits, int num_blocks, 
                                       int kr, int kl, const char *label) {
    int bitpos = 0;
    int total_nz = 0, total_eob = 0;
    int bands[8] = {0};
    int blocks_ok = 0;

    for (int b = 0; b < num_blocks && bitpos < ac_bits; b++) {
        int pos = 0;
        int block_nz = 0;
        while (pos < 63 && bitpos < ac_bits) {
            /* Decode run using Golomb-Rice(kr) */
            int q = 0;
            while (bitpos < ac_bits && get_bit(ac_data, bitpos) == 1) {
                q++; bitpos++;
                if (q > 20) break;
            }
            if (bitpos >= ac_bits || q > 20) break;
            bitpos++;
            if (bitpos + kr > ac_bits) break;
            uint32_t r_part = get_bits(ac_data, bitpos, kr);
            bitpos += kr;
            int run = (q << kr) | r_part;

            /* Decode level using Golomb-Rice(kl) */
            q = 0;
            while (bitpos < ac_bits && get_bit(ac_data, bitpos) == 1) {
                q++; bitpos++;
                if (q > 20) break;
            }
            if (bitpos >= ac_bits || q > 20) break;
            bitpos++;
            if (bitpos + kl > ac_bits) break;
            uint32_t l_part = get_bits(ac_data, bitpos, kl);
            bitpos += kl;
            int level = (q << kl) | l_part;

            if (run == 0 && level == 0) {
                /* EOB */
                total_eob++;
                goto next_gr;
            }

            pos += run;
            if (pos >= 63) break;
            block_nz++;
            int band = pos / 8;
            if (band < 8) bands[band]++;
            pos++;
        }
        next_gr:
        total_nz += block_nz;
        blocks_ok++;
    }

    double pct = 100.0 * bitpos / ac_bits;
    printf("  GR RunLevel %s (kr=%d,kl=%d): %.1f%% consumed, blocks=%d, NZ=%d, EOB=%d\n",
           label, kr, kl, pct, blocks_ok, total_nz, total_eob);
    printf("    bands: %d %d %d %d %d %d %d %d\n",
           bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
}

/*
 * Exp-Golomb run-level with EOB: (0,0) = EOB
 */
static void test_expgolomb_eob(const uint8_t *ac_data, int ac_bits, int num_blocks, int order, const char *label) {
    int bitpos = 0;
    int total_nz = 0, total_eob = 0;
    int bands[8] = {0};
    int blocks_ok = 0;
    int errors = 0;

    for (int b = 0; b < num_blocks && bitpos < ac_bits; b++) {
        int pos = 0;
        int block_nz = 0;
        while (pos < 63 && bitpos < ac_bits) {
            /* Decode exp-Golomb(order) for run */
            int leading = 0;
            while (bitpos < ac_bits && get_bit(ac_data, bitpos) == 0) {
                leading++; bitpos++;
                if (leading > 20) break;
            }
            if (bitpos >= ac_bits || leading > 20) { errors++; goto next_eg; }
            bitpos++; /* skip 1 */
            int nbits = leading + order;
            if (bitpos + nbits > ac_bits) { errors++; goto next_eg; }
            uint32_t suffix = 0;
            if (nbits > 0) suffix = get_bits(ac_data, bitpos, nbits);
            bitpos += nbits;
            int run = (1 << nbits) - (1 << order) + suffix;

            /* Decode exp-Golomb(order) for level */
            leading = 0;
            while (bitpos < ac_bits && get_bit(ac_data, bitpos) == 0) {
                leading++; bitpos++;
                if (leading > 20) break;
            }
            if (bitpos >= ac_bits || leading > 20) { errors++; goto next_eg; }
            bitpos++;
            nbits = leading + order;
            if (bitpos + nbits > ac_bits) { errors++; goto next_eg; }
            suffix = 0;
            if (nbits > 0) suffix = get_bits(ac_data, bitpos, nbits);
            bitpos += nbits;
            int level = (1 << nbits) - (1 << order) + suffix;

            if (run == 0 && level == 0) {
                total_eob++;
                goto next_eg;
            }

            pos += run;
            if (pos >= 63) break;
            block_nz++;
            int band = pos / 8;
            if (band < 8) bands[band]++;
            pos++;
        }
        total_nz += block_nz;
        blocks_ok++;
        next_eg:;
    }

    double pct = 100.0 * bitpos / ac_bits;
    printf("  ExpGolomb(%d) %s: %.1f%% consumed, blocks=%d, NZ=%d, EOB=%d, errors=%d\n",
           order, label, pct, blocks_ok, total_nz, total_eob, errors);
    printf("    bands: %d %d %d %d %d %d %d %d\n",
           bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
}

/* Generate random data for control test */
static void fill_random(uint8_t *buf, int len) {
    srand(12345);
    for (int i = 0; i < len; i++)
        buf[i] = rand() & 0xFF;
}

int main(int argc, char **argv) {
    const char *zipfile = "/home/wizzard/share/GitHub/Mari-nee no Heya (Japan).zip";
    int lba = 502;

    /* Load frame 3 (padded frame) */
    printf("=== Loading frame F03 (padded frame) ===\n");
    if (load_frame(zipfile, lba, 3) < 0) {
        fprintf(stderr, "Failed to load frame\n");
        return 1;
    }

    int data_end = find_data_end();
    int pad_bytes = frame_len - data_end;
    printf("Frame size: %d bytes, data_end: %d, padding: %d bytes\n",
           frame_len, data_end, pad_bytes);

    /* Parse header */
    int qs = frame_data[3];
    int type = frame_data[39];
    printf("QS=%d, type=%d\n", qs, type);

    int total_data_bits = (data_end - 40) * 8;
    int num_blocks = 864; /* 16×9 MBs × 6 blocks */

    /* Skip DC to find AC start */
    int dc_bits = skip_all_dc(num_blocks, total_data_bits);
    if (dc_bits < 0) {
        fprintf(stderr, "DC decode failed\n");
        return 1;
    }

    int ac_bits = total_data_bits - dc_bits;
    printf("DC: %d bits, AC: %d bits (%.1f bits/block, %.2f bits/coeff)\n",
           dc_bits, ac_bits, (double)ac_bits/num_blocks, (double)ac_bits/(num_blocks*63));

    /* AC data starts at byte 40, bit offset dc_bits */
    const uint8_t *base_data = frame_data + 40;

    /* Create AC-only buffer starting after DC */
    int ac_byte_start = dc_bits / 8;
    int ac_bit_offset = dc_bits % 8;
    /* We'll work with base_data + dc_bits as bit offset */

    /* For simplicity, create a shifted buffer */
    int ac_buf_len = (ac_bits + 7) / 8 + 1;
    uint8_t *ac_data = malloc(ac_buf_len);
    for (int i = 0; i < ac_bits; i++) {
        int src_bit = dc_bits + i;
        int val = get_bit(base_data, src_bit);
        if (val)
            ac_data[i >> 3] |= (1 << (7 - (i & 7)));
        else
            ac_data[i >> 3] &= ~(1 << (7 - (i & 7)));
    }

    /* Random control data */
    uint8_t *rand_data = malloc(ac_buf_len);
    fill_random(rand_data, ac_buf_len);

    printf("\n=== Scheme A: 0=zero, 1+Nbit=value (val=0→EOB) ===\n");
    for (int vb = 3; vb <= 7; vb++) {
        test_scheme_a(ac_data, ac_bits, num_blocks, vb, "REAL");
        test_scheme_a(rand_data, ac_bits, num_blocks, vb, "RAND");
    }

    printf("\n=== Scheme B: 0=zero, 10=EOB, 11+Nbit=value ===\n");
    for (int vb = 3; vb <= 6; vb++) {
        test_scheme_b(ac_data, ac_bits, num_blocks, vb, "REAL");
        test_scheme_b(rand_data, ac_bits, num_blocks, vb, "RAND");
    }

    printf("\n=== Scheme C: 0=zero, 1+DC_VLC(value), size=0=EOB ===\n");
    test_scheme_c(ac_data, ac_bits, num_blocks, "REAL");
    test_scheme_c(rand_data, ac_bits, num_blocks, "RAND");

    printf("\n=== Scheme D: 00=zero, 01=EOB, 1+Nbit=value ===\n");
    for (int vb = 3; vb <= 6; vb++) {
        test_scheme_d(ac_data, ac_bits, num_blocks, vb, "REAL");
        test_scheme_d(rand_data, ac_bits, num_blocks, vb, "RAND");
    }

    printf("\n=== Golomb-Rice per-coefficient (value=0→zero, >0→coeff) ===\n");
    for (int k = 1; k <= 6; k++) {
        test_golomb_rice(ac_data, ac_bits, num_blocks, k, "REAL");
        test_golomb_rice(rand_data, ac_bits, num_blocks, k, "RAND");
    }

    printf("\n=== Golomb-Rice Run-Level (0,0)=EOB ===\n");
    int kr_kl[][2] = {{1,1},{1,2},{2,1},{2,2},{2,3},{3,2},{3,3},{1,3},{3,1},{0,1},{0,2},{0,3},{1,0},{2,0},{3,0}};
    for (int i = 0; i < 15; i++) {
        test_golomb_rice_runlevel(ac_data, ac_bits, num_blocks, kr_kl[i][0], kr_kl[i][1], "REAL");
        test_golomb_rice_runlevel(rand_data, ac_bits, num_blocks, kr_kl[i][0], kr_kl[i][1], "RAND");
    }

    printf("\n=== Exp-Golomb Run-Level (0,0)=EOB ===\n");
    for (int o = 0; o <= 3; o++) {
        test_expgolomb_eob(ac_data, ac_bits, num_blocks, o, "REAL");
        test_expgolomb_eob(rand_data, ac_bits, num_blocks, o, "RAND");
    }

    /* Now also test Frame 0 for comparison */
    printf("\n\n=== Loading frame F00 for cross-check ===\n");
    if (load_frame(zipfile, lba, 0) < 0) {
        fprintf(stderr, "Failed to load F00\n");
        free(ac_data); free(rand_data);
        return 1;
    }
    data_end = find_data_end();
    pad_bytes = frame_len - data_end;
    qs = frame_data[3];
    type = frame_data[39];
    printf("F00: data_end=%d, padding=%d, QS=%d, type=%d\n", data_end, pad_bytes, qs, type);

    total_data_bits = (data_end - 40) * 8;
    dc_bits = skip_all_dc(num_blocks, total_data_bits);
    ac_bits = total_data_bits - dc_bits;
    printf("F00 DC: %d bits, AC: %d bits\n", dc_bits, ac_bits);

    /* Rebuild AC buffer for F00 */
    ac_buf_len = (ac_bits + 7) / 8 + 1;
    ac_data = realloc(ac_data, ac_buf_len);
    memset(ac_data, 0, ac_buf_len);
    for (int i = 0; i < ac_bits; i++) {
        int src_bit = dc_bits + i;
        int val = get_bit(frame_data + 40, src_bit);
        if (val)
            ac_data[i >> 3] |= (1 << (7 - (i & 7)));
    }

    /* Test the most promising schemes on F00 */
    printf("\n--- Best schemes on F00 ---\n");
    test_scheme_b(ac_data, ac_bits, num_blocks, 4, "F00-REAL");
    test_scheme_c(ac_data, ac_bits, num_blocks, "F00-REAL");
    test_scheme_d(ac_data, ac_bits, num_blocks, 4, "F00-REAL");
    for (int k = 2; k <= 4; k++)
        test_golomb_rice(ac_data, ac_bits, num_blocks, k, "F00-REAL");
    test_golomb_rice_runlevel(ac_data, ac_bits, num_blocks, 2, 2, "F00-REAL");
    test_golomb_rice_runlevel(ac_data, ac_bits, num_blocks, 1, 2, "F00-REAL");
    test_expgolomb_eob(ac_data, ac_bits, num_blocks, 0, "F00-REAL");
    test_expgolomb_eob(ac_data, ac_bits, num_blocks, 1, "F00-REAL");

    free(ac_data);
    free(rand_data);
    printf("\nDone.\n");
    return 0;
}
