/*
 * vcodec_sector_fix.c - Fix sector loading: skip type byte (F1/F2/F3)
 * 
 * DISCOVERY: Each sector's first user data byte is a type marker:
 * F1=video data, F2=end of frame, F3=scene marker
 * Current code includes this byte, corrupting the bitstream at
 * positions 2048, 4096, 6144, 8192, 10240.
 * 
 * With correct loading: 6 sectors × 2047 bytes = 12282 bytes per frame
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static uint8_t frame_data_old[16384];  /* old: includes type bytes */
static uint8_t frame_data_new[16384];  /* new: type bytes stripped */
static int frame_len_old, frame_len_new;

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

static int load_frame_both(const char *zipfile, int lba, int frame_idx) {
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

    frame_len_old = 0;
    frame_len_new = 0;
    for (int s = 0; s < 6; s++) {
        uint8_t sector[2352];
        if (fread(sector, 1, 2352, fp) != 2352) { fclose(fp); return -1; }
        /* Old: copy all 2048 bytes */
        memcpy(frame_data_old + frame_len_old, sector + 24, 2048);
        frame_len_old += 2048;
        /* New: skip first byte (type marker), copy 2047 bytes */
        printf("  Sector %d type byte: 0x%02X\n", s, sector[24]);
        memcpy(frame_data_new + frame_len_new, sector + 25, 2047);
        frame_len_new += 2047;
    }
    fclose(fp);
    return 0;
}

static int find_data_end(const uint8_t *data, int len) {
    int end = len;
    while (end > 0 && data[end - 1] == 0xFF) end--;
    return end;
}

static void analyze_runs(const uint8_t *data, int nbits, const char *label) {
    int run0[32] = {0}, run1[32] = {0};
    int cur_val = get_bit(data, 0), cur_len = 1;
    int max0 = 0, max1 = 0;
    for (int i = 1; i < nbits; i++) {
        int b = get_bit(data, i);
        if (b == cur_val) { cur_len++; }
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
    for (int r = 1; r <= 16; r++) printf("r%d=%d ", r, run0[r]);
    printf("\n    1-runs: ");
    for (int r = 1; r <= 16; r++) printf("r%d=%d ", r, run1[r]);
    printf("\n");
}

int main() {
    const char *zipfile = "/home/wizzard/share/GitHub/Mari-nee no Heya (Japan).zip";
    int lba = 502;
    int num_blocks = 864;

    printf("=== Loading Frame 0 (both old and new method) ===\n");
    if (load_frame_both(zipfile, lba, 0) < 0) {
        fprintf(stderr, "Failed to load frame\n"); return 1;
    }

    printf("\nOLD: %d bytes, NEW: %d bytes\n", frame_len_old, frame_len_new);

    /* Verify old data at corruption points */
    printf("\n=== Corruption verification ===\n");
    printf("Old data at sector boundaries:\n");
    for (int s = 0; s < 6; s++) {
        int pos = s * 2048;
        printf("  Sector %d start (old[%d]): %02X %02X %02X %02X %02X\n",
               s, pos, frame_data_old[pos], frame_data_old[pos+1], 
               frame_data_old[pos+2], frame_data_old[pos+3], frame_data_old[pos+4]);
    }

    /* New data header check */
    printf("\nNEW header bytes 0-5: %02X %02X %02X %02X %02X %02X\n",
           frame_data_new[0], frame_data_new[1], frame_data_new[2],
           frame_data_new[3], frame_data_new[4], frame_data_new[5]);
    printf("NEW header bytes 36-41: %02X %02X %02X %02X %02X %02X\n",
           frame_data_new[36], frame_data_new[37], frame_data_new[38],
           frame_data_new[39], frame_data_new[40], frame_data_new[41]);

    int qs = frame_data_new[3];
    int type = frame_data_new[39];
    printf("QS=%d, TYPE=%d\n", qs, type);

    /* Find padding */
    int data_end_new = find_data_end(frame_data_new, frame_len_new);
    int data_end_old = find_data_end(frame_data_old, frame_len_old);
    printf("\nOLD data_end=%d (padding=%d)\nNEW data_end=%d (padding=%d)\n",
           data_end_old, frame_len_old - data_end_old,
           data_end_new, frame_len_new - data_end_new);

    /* DC decode with new data */
    printf("\n=== DC decode with CORRECTED data ===\n");
    int total_bits_new = (data_end_new - 40) * 8;
    int bitpos = 0;
    int prev_dc = 0, min_dc = 9999, max_dc = -9999;
    for (int b = 0; b < num_blocks; b++) {
        int dc_val;
        int consumed = decode_dc(frame_data_new + 40, bitpos, &dc_val, total_bits_new);
        if (consumed < 0) {
            printf("DC FAILED at block %d, bitpos %d\n", b, bitpos);
            return 1;
        }
        bitpos += consumed;
        prev_dc += dc_val;
        if (prev_dc < min_dc) min_dc = prev_dc;
        if (prev_dc > max_dc) max_dc = prev_dc;
    }
    int dc_bits = bitpos;
    int ac_bits = total_bits_new - dc_bits;
    printf("DC: %d bits (%.1f bits/block), range [%d,%d]\n", 
           dc_bits, (double)dc_bits/num_blocks, min_dc, max_dc);
    printf("AC: %d bits (%.1f bits/block, %.2f bits/coeff)\n",
           ac_bits, (double)ac_bits/num_blocks, (double)ac_bits/(num_blocks*63));

    /* AC run-length analysis */
    printf("\n=== AC run-length (CORRECTED data) ===\n");
    int ac_buf_len = (ac_bits + 7) / 8 + 1;
    uint8_t *ac_new = calloc(ac_buf_len, 1);
    for (int i = 0; i < ac_bits; i++) {
        int val = get_bit(frame_data_new + 40, dc_bits + i);
        if (val) ac_new[i >> 3] |= (1 << (7 - (i & 7)));
    }
    analyze_runs(ac_new, ac_bits, "NEW-AC");

    /* Compare first 200 AC bits old vs new */
    printf("\n=== First 200 AC bits comparison ===\n");
    
    /* Old AC data */
    int total_bits_old = (data_end_old - 41) * 8; /* old offset was 40 but header shifted by 1 */
    bitpos = 0;
    for (int b = 0; b < num_blocks; b++) {
        int dc_val;
        int consumed = decode_dc(frame_data_old + 41, bitpos, &dc_val, total_bits_old);
        if (consumed < 0) { break; }
        bitpos += consumed;
    }
    int old_dc_bits = bitpos;
    printf("OLD DC (offset 41): %d bits\n", old_dc_bits);
    
    printf("NEW first 200 AC bits:\n");
    for (int i = 0; i < 200 && i < ac_bits; i++) {
        printf("%d", get_bit(frame_data_new + 40, dc_bits + i));
        if ((i+1) % 64 == 0) printf("\n");
    }
    printf("\n");

    /* Byte alignment check */
    printf("\nDC ends at bit %d (byte %d.%d)\n", dc_bits, dc_bits/8, dc_bits%8);

    /* Entropy of NEW AC data */
    int byte_count[256] = {0};
    int ac_bytes = ac_bits / 8;
    for (int i = 0; i < ac_bytes; i++) byte_count[ac_new[i]]++;
    double entropy = 0;
    for (int i = 0; i < 256; i++) {
        if (byte_count[i] > 0) {
            double p = (double)byte_count[i] / ac_bytes;
            entropy -= p * log2(p);
        }
    }
    printf("NEW AC byte entropy: %.4f bits\n", entropy);

    /* Now test multiple frames with corrected loading */
    printf("\n=== Cross-frame analysis (corrected) ===\n");
    for (int f = 0; f < 8; f++) {
        if (load_frame_both(zipfile, lba, f) < 0) continue;
        
        data_end_new = find_data_end(frame_data_new, frame_len_new);
        qs = frame_data_new[3];
        type = frame_data_new[39];
        int pad = frame_len_new - data_end_new;
        
        total_bits_new = (data_end_new - 40) * 8;
        bitpos = 0;
        int ok = 1;
        for (int b = 0; b < num_blocks; b++) {
            int dc_val;
            int consumed = decode_dc(frame_data_new + 40, bitpos, &dc_val, total_bits_new);
            if (consumed < 0) { ok = 0; break; }
            bitpos += consumed;
        }
        if (ok) {
            dc_bits = bitpos;
            ac_bits = total_bits_new - dc_bits;
            printf("  F%02d: QS=%2d TYPE=%d pad=%4d DC=%4d AC=%5d (%.1f/blk %.2f/coeff)\n",
                   f, qs, type, pad, dc_bits, ac_bits,
                   (double)ac_bits/num_blocks, (double)ac_bits/(num_blocks*63));
        } else {
            printf("  F%02d: QS=%2d TYPE=%d pad=%4d DC FAILED at block %d\n",
                   f, qs, type, pad, (int)(bitpos));
        }
    }

    free(ac_new);
    printf("\nDone.\n");
    return 0;
}
