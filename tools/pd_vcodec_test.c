// Assemble AK8000 picture packets from a MODE2/2352 track and decode them
// with the ported decoder. Mirrors PlaydiaEmu's playdia-frame so the two can
// be compared byte for byte.
//
//   pd_vcodec_test <track.bin> --check-all
//   pd_vcodec_test <track.bin> --packet N --output out.ppm

#include "../src/ak8000_pd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 2352
#define CAP    (64 * 1024)

static uint8_t acc[CAP];
static size_t  acc_len;
static int     acc_overflow;
static uint8_t rgb[PD_PIC_W * PD_PIC_H * 3];

static long packet_index;
static long want_packet = -1;
static const char *out_path;
static long valid, failed;

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", PD_PIC_W, PD_PIC_H);
    fwrite(rgb, 1, sizeof rgb, f);
    fclose(f);
    printf("wrote %s (%dx%d)\n", path, PD_PIC_W, PD_PIC_H);
}

static void flush_packet(void)
{
    int starts = acc_len >= 4 && acc[0] == 0x00 && acc[1] == 0x80 && acc[2] == 0x04;
    if (!starts || acc_len < 44 || acc_overflow) {
        if (acc_len) failed++;
        acc_len = 0; acc_overflow = 0;
        return;
    }
    packet_index++;
    if (want_packet > 0) {
        if (packet_index == want_packet) {
            pd_result r = pd_picture_decode(acc, acc_len, rgb);
            if (r.status != PD_OK) {
                fprintf(stderr, "decode failed: %s bit=%d row=%d block=%d\n",
                        pd_status_name(r.status), r.bit, r.row, r.block);
                exit(1);
            }
            write_ppm(out_path);
            exit(0);
        }
    } else {
        pd_result r = pd_picture_validate(acc, acc_len);
        if (r.status == PD_OK) {
            valid++;
        } else {
            failed++;
            if (failed <= 8)
                printf("failed packet=%ld len=%zu error=%s bit=%d row=%d block=%d\n",
                       packet_index, acc_len, pd_status_name(r.status),
                       r.bit, r.row, r.block);
        }
    }
    acc_len = 0;
    acc_overflow = 0;
}

// F1 carries the fragment from byte 1, F2 from byte 0x23.
static void append(const uint8_t *payload, size_t offset)
{
    if (offset >= 2048) return;
    size_t n = 2048 - offset;
    if (acc_overflow || acc_len + n > CAP) { acc_overflow = 1; return; }
    memcpy(acc + acc_len, payload + offset, n);
    acc_len += n;
}

static int is_padding(const uint8_t *d)
{
    if (d[0] != 0xF3) return 0;
    for (int i = 3; i < 2048; i++) if (d[i] != 0xFF) return 0;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <track.bin> --check-all | --packet N --output f.ppm\n", argv[0]);
        return 2;
    }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--packet") && i + 1 < argc) want_packet = atol(argv[++i]);
        else if (!strcmp(argv[i], "--output") && i + 1 < argc) out_path = argv[++i];
    }
    if (want_packet > 0 && !out_path) { fprintf(stderr, "--packet needs --output\n"); return 2; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }

    uint8_t raw[SECTOR];
    while (fread(raw, 1, SECTOR, f) == SECTOR) {
        uint8_t channel = raw[17];
        uint8_t submode = raw[18];
        const uint8_t *data = raw + 24;
        if (channel != 0 || !(submode & 0x08) || (submode & 0x04)) continue;
        if (data[0] == 0xF1) {
            append(data, 1);
        } else if (data[0] == 0xF2) {
            if (acc_len) append(data, 0x23);
            flush_packet();
        } else if (data[0] == 0xF3 && !is_padding(data)) {
            acc_len = 0;   // scene reset drops the partial picture
            acc_overflow = 0;
        }
    }
    fclose(f);

    if (want_packet > 0) {
        fprintf(stderr, "packet %ld not found (saw %ld)\n", want_packet, packet_index);
        return 1;
    }
    printf("packets=%ld valid=%ld failed=%ld\n", packet_index, valid, failed);
    return failed ? 1 : 0;
}
