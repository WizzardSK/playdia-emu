#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint8_t frame_data[16384];
static int frame_len_val;

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

static int load_nth_frame(const char *binfile, int start_lba, int target_frame) {
    FILE *fp = fopen(binfile, "rb");
    if (!fp) return -1;
    
    int frame_count = 0;
    int f1_count = 0;
    frame_len_val = 0;
    
    for (int sec = 0; sec < 500; sec++) {
        long offset = (long)(start_lba + sec) * 2352;
        uint8_t sector[2352];
        fseek(fp, offset, SEEK_SET);
        if (fread(sector, 1, 2352, fp) != 2352) break;
        
        uint8_t type_byte = sector[24];
        
        if (type_byte == 0xF1) {
            if (frame_count == target_frame) {
                memcpy(frame_data + f1_count * 2047, sector + 25, 2047);
            }
            f1_count++;
        } else if (type_byte == 0xF2) {
            if (frame_count == target_frame && f1_count == 6) {
                frame_len_val = 6 * 2047;
                fclose(fp);
                return 0;
            }
            frame_count++;
            f1_count = 0;
        } else if (type_byte == 0xF3) {
            f1_count = 0;
        }
    }
    fclose(fp);
    return -1;
}

int main() {
    const char *binfile = "/tmp/Mari-nee no Heya (Japan) (Track 1).bin";
    int start_lba = 502;
    int num_blocks = 864;
    
    /* Test frames that failed: F03, F06, F07, F09, F20, F22-25 */
    int test_frames[] = {0, 3, 6, 7, 9, 20, 22};
    int ntest = 7;
    
    for (int t = 0; t < ntest; t++) {
        int fi = test_frames[t];
        if (load_nth_frame(binfile, start_lba, fi) < 0) {
            printf("F%02d: Failed to load\n", fi);
            continue;
        }
        
        printf("\n=== F%02d ===\n", fi);
        printf("Header: %02X %02X %02X %02X ... %02X %02X %02X %02X\n",
               frame_data[0], frame_data[1], frame_data[2], frame_data[3],
               frame_data[36], frame_data[37], frame_data[38], frame_data[39]);
        printf("QS=%d TYPE=%d\n", frame_data[3], frame_data[39]);
        
        /* Check qtables */
        printf("Qtable1: ");
        for (int i = 4; i < 20; i++) printf("%02X ", frame_data[i]);
        printf("\nQtable2: ");
        for (int i = 20; i < 36; i++) printf("%02X ", frame_data[i]);
        printf("\n");
        
        int data_end = frame_len_val;
        while (data_end > 0 && frame_data[data_end-1] == 0xFF) data_end--;
        printf("Data end: %d, padding: %d\n", data_end, frame_len_val - data_end);
        
        int total_bits = (data_end - 40) * 8;
        int bitpos = 0;
        int prev_dc[3] = {0, 0, 0}; /* Y, Cb, Cr predictors */
        int block_in_mb = 0;
        
        /* Decode DC with per-component DPCM predictors */
        int failed_block = -1;
        for (int b = 0; b < num_blocks; b++) {
            int dc_val;
            int consumed = decode_dc(frame_data + 40, bitpos, &dc_val, total_bits);
            if (consumed < 0) {
                failed_block = b;
                printf("DC failed at block %d (bitpos %d/%d)\n", b, bitpos, total_bits);
                /* Show nearby bits */
                printf("Bits at failure: ");
                for (int i = -8; i < 20 && bitpos+i >= 0 && bitpos+i < total_bits; i++) {
                    if (i == 0) printf("[");
                    printf("%d", get_bit(frame_data + 40, bitpos + i));
                    if (i == 0) printf("]");
                }
                printf("\n");
                /* Show which VLC codes we tried */
                printf("Tried VLC codes: ");
                for (int v = 0; v < 9; v++) {
                    if (bitpos + dc_lum_vlc[v].len <= total_bits) {
                        uint32_t bits = get_bits(frame_data + 40, bitpos, dc_lum_vlc[v].len);
                        printf("len%d=%X(want %X) ", dc_lum_vlc[v].len, bits, dc_lum_vlc[v].code);
                    }
                }
                printf("\n");
                break;
            }
            bitpos += consumed;
            
            /* Track per-component */
            int comp;
            if (block_in_mb < 4) comp = 0; /* Y */
            else if (block_in_mb == 4) comp = 1; /* Cb */
            else comp = 2; /* Cr */
            prev_dc[comp] += dc_val;
            
            block_in_mb++;
            if (block_in_mb == 6) block_in_mb = 0;
        }
        
        if (failed_block < 0) {
            int dc_bits_val = bitpos;
            int ac_bits = total_bits - dc_bits_val;
            printf("DC OK: %d bits (%.1f/blk), AC: %d bits (%.1f/blk)\n",
                   dc_bits_val, (double)dc_bits_val/num_blocks,
                   ac_bits, (double)ac_bits/num_blocks);
        }
    }
    
    return 0;
}
