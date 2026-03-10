/*
 * vcodec_framescan.c - Properly scan sectors to find video frames
 * Skip F2/F3/audio sectors, collect 6 consecutive F1 sectors per frame
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define MAX_FRAMES 64

static uint8_t frame_data[MAX_FRAMES][16384];
static int frame_len[MAX_FRAMES];
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
    int end = frame_len[fi];
    while (end > 0 && frame_data[fi][end - 1] == 0xFF) end--;
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

int main() {
    const char *zipfile = "/home/wizzard/share/GitHub/Mari-nee no Heya (Japan).zip";
    int start_lba = 502;
    int num_blocks = 864;
    
    /* Extract bin file */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "cd /tmp && unzip -o '%s' '*.bin' >/dev/null 2>&1", zipfile);
    system(cmd);
    FILE *fp = popen("find /tmp -maxdepth 1 -name '*.bin' -print -quit", "r");
    if (!fp) return 1;
    char binfile[512];
    if (!fgets(binfile, sizeof(binfile), fp)) { pclose(fp); return 1; }
    pclose(fp);
    binfile[strcspn(binfile, "\n")] = 0;
    
    fp = fopen(binfile, "rb");
    if (!fp) return 1;
    
    /* Scan sectors from start_lba */
    printf("=== Scanning sectors from LBA %d ===\n", start_lba);
    num_frames = 0;
    int f1_count = 0;  /* consecutive F1 sectors for current frame */
    
    for (int sec = 0; sec < 200 && num_frames < MAX_FRAMES; sec++) {
        long offset = (long)(start_lba + sec) * 2352;
        uint8_t sector[2352];
        fseek(fp, offset, SEEK_SET);
        if (fread(sector, 1, 2352, fp) != 2352) break;
        
        /* Check subheader */
        uint8_t file_num = sector[16];
        uint8_t channel = sector[17];
        uint8_t submode = sector[18];
        uint8_t coding = sector[19];
        uint8_t type_byte = sector[24]; /* first byte of user data */
        
        if (sec < 30) {
            printf("  LBA %d: file=%02X chan=%02X sub=%02X cod=%02X type=%02X",
                   start_lba + sec, file_num, channel, submode, coding, type_byte);
            if (type_byte == 0xF1) printf(" [VIDEO]");
            else if (type_byte == 0xF2) printf(" [END]");
            else if (type_byte == 0xF3) printf(" [SCENE]");
            else if (channel == 1 || (submode & 0x04)) printf(" [AUDIO]");
            printf("\n");
        }
        
        /* Collect video sectors */
        if (type_byte == 0xF1 && file_num == 1 && (submode & 0x08)) {
            /* Video data sector - skip type byte, copy 2047 bytes */
            memcpy(frame_data[num_frames] + f1_count * 2047, sector + 25, 2047);
            f1_count++;
        } else if (type_byte == 0xF2) {
            /* End of frame marker */
            if (f1_count == 6) {
                frame_len[num_frames] = f1_count * 2047;
                num_frames++;
            } else if (f1_count > 0) {
                printf("  WARNING: frame with %d F1 sectors (expected 6)\n", f1_count);
                if (f1_count == 6) {
                    frame_len[num_frames] = f1_count * 2047;
                    num_frames++;
                }
            }
            f1_count = 0;
        } else if (type_byte == 0xF3) {
            /* Scene marker, reset */
            f1_count = 0;
        } else {
            /* Audio or other sector, ignore but don't reset frame */
        }
    }
    /* Handle last frame if no F2 */
    if (f1_count == 6) {
        frame_len[num_frames] = f1_count * 2047;
        num_frames++;
    }
    
    fclose(fp);
    printf("\nFound %d complete video frames\n", num_frames);
    
    /* Analyze each frame */
    printf("\n=== Frame analysis ===\n");
    for (int f = 0; f < num_frames && f < 32; f++) {
        uint8_t *fd = frame_data[f];
        int qs = fd[3];
        int type = fd[39];
        int data_end = find_data_end(f);
        int pad = frame_len[f] - data_end;
        
        /* Verify header */
        if (fd[0] != 0x00 || fd[1] != 0x80 || fd[2] != 0x04) {
            printf("  F%02d: BAD HEADER: %02X %02X %02X %02X\n", f, fd[0], fd[1], fd[2], fd[3]);
            continue;
        }
        if (fd[36] != 0x00 || fd[37] != 0x80 || fd[38] != 0x24) {
            printf("  F%02d: BAD 2nd HEADER: %02X %02X %02X %02X\n", f, fd[36], fd[37], fd[38], fd[39]);
            continue;
        }
        
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
            printf("  F%02d: QS=%2d TYPE=%d pad=%4d DC=%4d AC=%5d (%.1f/blk %.2f/coeff)",
                   f, qs, type, pad, dc_bits, ac_bits,
                   (double)ac_bits/num_blocks, (double)ac_bits/(num_blocks*63));
            
            /* Check DC byte alignment */
            if (dc_bits % 8 == 0) printf(" DC-ALIGNED");
            printf("\n");
        } else {
            printf("  F%02d: QS=%2d TYPE=%d pad=%4d DC FAILED\n", f, qs, type, pad);
        }
    }
    
    /* Detailed analysis on first valid frame */
    printf("\n=== Detailed AC analysis on F00 ===\n");
    {
        uint8_t *fd = frame_data[0];
        int data_end = find_data_end(0);
        int total_bits = (data_end - 40) * 8;
        int bitpos = 0;
        for (int b = 0; b < num_blocks; b++) {
            int dc_val;
            int consumed = decode_dc(fd + 40, bitpos, &dc_val, total_bits);
            bitpos += consumed;
        }
        int dc_bits = bitpos;
        int ac_bits = total_bits - dc_bits;
        
        /* Extract AC data */
        int ac_buf_len = (ac_bits + 7) / 8 + 1;
        uint8_t *ac_data = calloc(ac_buf_len, 1);
        for (int i = 0; i < ac_bits; i++) {
            int val = get_bit(fd + 40, dc_bits + i);
            if (val) ac_data[i >> 3] |= (1 << (7 - (i & 7)));
        }
        
        analyze_runs(ac_data, ac_bits, "F00-AC");
        
        /* Also analyze padded frame if available */
        for (int f = 0; f < num_frames; f++) {
            int pad = frame_len[f] - find_data_end(f);
            if (pad > 500) {
                printf("\n=== Detailed AC analysis on padded F%02d (pad=%d) ===\n", f, pad);
                fd = frame_data[f];
                if (fd[0] != 0x00 || fd[1] != 0x80) continue;
                data_end = find_data_end(f);
                total_bits = (data_end - 40) * 8;
                bitpos = 0;
                int ok = 1;
                for (int b = 0; b < num_blocks; b++) {
                    int dc_val;
                    int consumed = decode_dc(fd + 40, bitpos, &dc_val, total_bits);
                    if (consumed < 0) { ok = 0; break; }
                    bitpos += consumed;
                }
                if (!ok) { printf("  DC failed\n"); continue; }
                dc_bits = bitpos;
                ac_bits = total_bits - dc_bits;
                printf("  DC=%d, AC=%d bits (%.1f/blk, %.2f/coeff)\n",
                       dc_bits, ac_bits, (double)ac_bits/num_blocks, (double)ac_bits/(num_blocks*63));
                
                uint8_t *ac2 = calloc((ac_bits+7)/8+1, 1);
                for (int i = 0; i < ac_bits; i++) {
                    int val = get_bit(fd + 40, dc_bits + i);
                    if (val) ac2[i >> 3] |= (1 << (7 - (i & 7)));
                }
                char lbl[32];
                snprintf(lbl, sizeof(lbl), "F%02d-AC", f);
                analyze_runs(ac2, ac_bits, lbl);
                
                /* Last 16 bytes before padding */
                printf("  Last 16 bytes before pad: ");
                for (int i = data_end - 16; i < data_end; i++)
                    printf("%02X ", fd[i]);
                printf("\n");
                
                free(ac2);
                break;
            }
        }
        
        free(ac_data);
    }
    
    printf("\nDone.\n");
    return 0;
}
