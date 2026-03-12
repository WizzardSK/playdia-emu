/*
 * header_verify.c - Verify AK8000 video packet headers across entire disc
 *
 * Assembles ALL video packets from Track 2 (F1 sectors terminated by F2),
 * validates the 40-byte AK8000 header structure, and performs I-frame
 * DC decode analysis per frame type.
 *
 * Header layout:
 *   [0-2]   = 00 80 04
 *   [3]     = QS (quantization scale, 1-100)
 *   [4-19]  = qtable (16 bytes)
 *   [20-35] = qtable copy (should match [4-19])
 *   [36-38] = 00 80 24
 *   [39]    = frame type
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SECTOR_SIZE     2352
#define F1_OFFSET       25
#define F1_PAYLOAD      2047
#define MAX_PKT_SECTORS 6
#define PKT_SIZE        (MAX_PKT_SECTORS * F1_PAYLOAD)
#define HDR_SIZE        40
#define MAX_PKTS        65536

/* ============ Bitstream reader (MSB first) ============ */
typedef struct {
    const uint8_t *data;
    int size;
    int bit_pos;
} bitstream_t;

static inline int bs_bits_left(bitstream_t *bs) {
    return bs->size * 8 - bs->bit_pos;
}

static inline uint32_t bs_peek(bitstream_t *bs, int n) {
    if (n == 0) return 0;
    uint32_t val = 0;
    int pos = bs->bit_pos;
    for (int i = 0; i < n; i++) {
        int byte_idx = (pos + i) / 8;
        int bit_idx  = 7 - ((pos + i) % 8);
        if (byte_idx < bs->size)
            val = (val << 1) | ((bs->data[byte_idx] >> bit_idx) & 1);
        else
            val <<= 1;
    }
    return val;
}

static inline void bs_skip(bitstream_t *bs, int n) {
    bs->bit_pos += n;
}

static inline uint32_t bs_read(bitstream_t *bs, int n) {
    uint32_t v = bs_peek(bs, n);
    bs_skip(bs, n);
    return v;
}

/* ============ DC VLC (MPEG-1 luminance) ============ */
static const struct { int len; uint32_t code; } dc_vlc[17] = {
    {3,0x4},{2,0x0},{2,0x1},{3,0x5},{3,0x6},{4,0xE},{5,0x1E},{6,0x3E},{7,0x7E},
    {8,0xFE},{9,0x1FE},{10,0x3FE},{11,0x7FE},{12,0xFFE},{13,0x1FFE},{14,0x3FFE},{15,0x7FFE}
};

static int decode_dc_size(bitstream_t *bs) {
    if (bs_bits_left(bs) < 2) return -1;
    for (int s = 0; s <= 16; s++) {
        int len = dc_vlc[s].len;
        if (bs_bits_left(bs) < len) continue;
        if (bs_peek(bs, len) == dc_vlc[s].code) {
            bs_skip(bs, len);
            return s;
        }
    }
    return -1;
}

static int decode_dc_coeff(bitstream_t *bs, int *ok) {
    int size = decode_dc_size(bs);
    if (size < 0) { *ok = 0; return 0; }
    if (size == 0) { *ok = 1; return 0; }
    if (bs_bits_left(bs) < size) { *ok = 0; return 0; }
    uint32_t val = bs_read(bs, size);
    if (!(val & (1 << (size - 1))))
        val = val - (1 << size) + 1;
    *ok = 1;
    return (int)val;
}

/* ============ AC VLC (MPEG-1 Table B.14) ============ */
static int decode_ac(bitstream_t *bs, int *run, int *level) {
    if (bs_bits_left(bs) < 2) return -1;

    uint32_t b0 = bs_read(bs, 1);
    if (b0 == 1) {
        uint32_t b1 = bs_read(bs, 1);
        if (b1 == 0) return 0; /* EOB */
        int s = bs_read(bs, 1);
        *run = 0; *level = s ? -1 : 1;
        return 1;
    }
    uint32_t b1 = bs_read(bs, 1);
    if (b1 == 1) {
        uint32_t b2 = bs_read(bs, 1);
        if (b2 == 1) { int s=bs_read(bs,1); *run=1; *level=s?-1:1; return 1; }
        uint32_t b3 = bs_read(bs, 1);
        if (b3 == 0) { int s=bs_read(bs,1); *run=0; *level=s?-2:2; return 1; }
        { int s=bs_read(bs,1); *run=2; *level=s?-1:1; return 1; }
    }
    uint32_t b2 = bs_read(bs, 1);
    if (b2 == 1) {
        uint32_t b3=bs_read(bs,1), b4=bs_read(bs,1);
        int c5 = (b3<<1)|b4;
        if (c5==1) { int s=bs_read(bs,1); *run=0; *level=s?-3:3; return 1; }
        if (c5==2) { int s=bs_read(bs,1); *run=4; *level=s?-1:1; return 1; }
        if (c5==3) { int s=bs_read(bs,1); *run=3; *level=s?-1:1; return 1; }
        { uint32_t l3=bs_read(bs,3); int s=bs_read(bs,1);
          static const int r8[]={13,0,12,11,3,1,0,10}, l8[]={1,6,1,1,2,3,5,1};
          *run=r8[l3]; *level=s?-l8[l3]:l8[l3]; return 1; }
    }
    uint32_t b3 = bs_read(bs, 1);
    if (b3 == 1) {
        uint32_t l2=bs_read(bs,2); int s=bs_read(bs,1);
        static const int r6[]={7,6,1,5}, l6[]={1,1,2,1};
        *run=r6[l2]; *level=s?-l6[l2]:l6[l2]; return 1;
    }
    uint32_t b4 = bs_read(bs, 1);
    if (b4 == 1) {
        uint32_t b5=bs_read(bs,1);
        if (b5==0) {
            *run=bs_read(bs,6);
            int lev=(int)bs_read(bs,8);
            if (lev==0) lev=(int)bs_read(bs,8);
            else if (lev==128) lev=(int)bs_read(bs,8)-256;
            else if (lev>128) lev-=256;
            *level=lev; return 1;
        }
        uint32_t b6=bs_read(bs,1); int s=bs_read(bs,1);
        if (b6==0) { *run=2; *level=s?-2:2; } else { *run=8; *level=s?-1:1; }
        return 1;
    }
    uint32_t b5 = bs_read(bs, 1);
    if (b5 == 1) {
        uint32_t l3=bs_read(bs,3); int s=bs_read(bs,1);
        static const int r10[]={0,27,2,0,4,26,0,25}, l10[]={9,1,3,8,2,1,7,1};
        *run=r10[l3]; *level=s?-l10[l3]:l10[l3]; return 1;
    }
    uint32_t b6 = bs_read(bs, 1);
    if (b6 == 1) {
        uint32_t l4=bs_read(bs,4); int s=bs_read(bs,1);
        static const int r12[]={16,5,0,2,1,24,0,23,22,21,0,20,19,18,1,17};
        static const int l12[]={1,2,11,4,5,1,10,1,1,1,12,1,1,1,4,1};
        *run=r12[l4]; *level=s?-l12[l4]:l12[l4]; return 1;
    }
    uint32_t b7 = bs_read(bs, 1);
    if (b7 == 1) {
        uint32_t l4=bs_read(bs,4); int s=bs_read(bs,1);
        static const int r13[]={14,1,0,2,0,15,3,0,6,0,1,5,0,0,1,0};
        static const int l13[]={1,7,13,5,14,1,3,16,2,15,6,2,0,0,0,0};
        if (l13[l4]==0) return -1;
        *run=r13[l4]; *level=s?-l13[l4]:l13[l4]; return 1;
    }
    return -1;
}

/*
 * Decode I-frame: DCs via DPCM with 3 predictors (Y,Cb,Cr), skip ACs via VLC.
 * If dc_values != NULL, store DC values. Returns count of DCs decoded.
 * Also stores DC summary stats in provided pointers if non-NULL.
 */
static int decode_iframe(const uint8_t *bitstream, int bs_size,
                         int *dc_values, int max_dcs, int *out_ac_count,
                         double *out_sum, double *out_abs_sum,
                         int *out_min, int *out_max,
                         int *out_zeros, int *out_small) {
    bitstream_t bs = { bitstream, bs_size, 0 };
    int pred[3] = {0, 0, 0};
    int count = 0, ac_total = 0;
    double sum = 0, abs_sum = 0;
    int mn = 999999, mx = -999999, zeros = 0, small = 0;

    for (int mb = 0; mb < max_dcs / 6 && count < max_dcs; mb++) {
        for (int blk = 0; blk < 6 && count < max_dcs; blk++) {
            int comp = blk < 4 ? 0 : (blk == 4 ? 1 : 2);
            int ok = 0;
            int diff = decode_dc_coeff(&bs, &ok);
            if (!ok) goto done;

            pred[comp] += diff;
            int v = pred[comp];
            if (dc_values) dc_values[count] = v;
            count++;
            sum += v; abs_sum += fabs(v);
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            if (v == 0) zeros++;
            if (abs(v) <= 5) small++;

            /* Skip AC coefficients */
            int idx = 1;
            while (idx < 64 && bs_bits_left(&bs) > 2) {
                int run, level;
                int ret = decode_ac(&bs, &run, &level);
                if (ret == 0) break;
                if (ret < 0) goto done;
                idx += run;
                if (idx >= 64) break;
                ac_total++;
                idx++;
            }
        }
    }
done:
    if (out_ac_count) *out_ac_count = ac_total;
    if (out_sum) *out_sum = sum;
    if (out_abs_sum) *out_abs_sum = abs_sum;
    if (out_min) *out_min = mn;
    if (out_max) *out_max = mx;
    if (out_zeros) *out_zeros = zeros;
    if (out_small) *out_small = small;
    return count;
}

/* Parse CUE file to get Track 2 bin filename */
static char *parse_cue_track2(const char *cue_path) {
    FILE *f = fopen(cue_path, "r");
    if (!f) return NULL;
    static char binfile[2048];
    char line[1024], current_file[1024] = {0};
    int in_track2 = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "FILE ", 5) == 0) {
            char *q1 = strchr(line, '"');
            if (q1) { char *q2 = strchr(q1+1, '"');
                if (q2) { int len=q2-q1-1; strncpy(current_file,q1+1,len); current_file[len]=0; } }
        }
        if (strstr(line, "TRACK 02")) in_track2 = 1;
        if (in_track2 && strstr(line, "INDEX 01")) {
            char *d = strrchr(cue_path, '/');
            if (d) { int dl=d-cue_path+1; strncpy(binfile,cue_path,dl); binfile[dl]=0; strcat(binfile,current_file); }
            else strcpy(binfile, current_file);
            fclose(f); return binfile;
        }
    }
    fclose(f); return NULL;
}

/* Lightweight per-packet record */
typedef struct {
    uint8_t header[HDR_SIZE];
    uint8_t valid;
    uint8_t type;
    uint8_t qs;
    uint8_t f1_sectors;   /* how many F1 sectors in this packet */
    int dc_count;
    int ac_count;
    double dc_sum, dc_abs_sum;
    int dc_min, dc_max;
    int dc_zeros, dc_small;
} pkt_rec_t;

/* For detailed analysis: store full DC arrays for first N packets per type */
#define DETAIL_PER_TYPE 5
typedef struct {
    int pkt_index;
    uint8_t type;
    int dc_count;
    int ac_count;
    int dc_vals[864];
    double dc_sum, dc_abs_sum;
    int dc_min, dc_max, dc_zeros, dc_small;
} detail_rec_t;

int main(int argc, char **argv) {
    const char *cue_path = "/home/wizzard/share/GitHub/playdia-roms/"
        "Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku - Uchuu-hen (Japan).cue";

    char *bin_path = parse_cue_track2(cue_path);
    if (!bin_path) { fprintf(stderr, "Failed to parse CUE for Track 2\n"); return 1; }
    printf("BIN file: %s\n\n", bin_path);

    FILE *f = fopen(bin_path, "rb");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", bin_path); return 1; }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    long num_sectors = file_size / SECTOR_SIZE;
    fseek(f, 0, SEEK_SET);
    printf("File size: %ld bytes, %ld sectors\n\n", file_size, num_sectors);

    uint8_t *disc = malloc(file_size);
    if (!disc) { perror("malloc disc"); return 1; }
    if ((long)fread(disc, 1, file_size, f) != file_size) {
        fprintf(stderr, "Short read\n"); free(disc); fclose(f); return 1;
    }
    fclose(f);

    /* Allocate records */
    pkt_rec_t *pkts = calloc(MAX_PKTS, sizeof(pkt_rec_t));
    if (!pkts) { perror("calloc"); free(disc); return 1; }

    /* Detail records: up to DETAIL_PER_TYPE per type */
    int detail_count_per_type[256] = {0};
    int total_detail = 0;
    int max_detail = 256 * DETAIL_PER_TYPE;
    detail_rec_t *details = calloc(max_detail, sizeof(detail_rec_t));

    uint8_t *pkt_buf = malloc(PKT_SIZE);
    int pkt_count = 0;
    int f1_count = 0;
    int payload_size = 0;

    for (long s = 0; s < num_sectors && pkt_count < MAX_PKTS; s++) {
        const uint8_t *sec = disc + s * SECTOR_SIZE;

        /* Validate CD sync */
        if (sec[0] != 0x00 || sec[1] != 0xFF || sec[11] != 0x00) continue;
        if (sec[15] != 2) continue;
        if (sec[18] & 0x20) continue; /* skip Form 2 */

        uint8_t marker = sec[24];

        if (marker == 0xF3) {
            f1_count = 0; payload_size = 0; continue;
        }

        if (marker == 0xF1) {
            if (payload_size + F1_PAYLOAD <= PKT_SIZE) {
                memcpy(pkt_buf + payload_size, sec + F1_OFFSET, F1_PAYLOAD);
                payload_size += F1_PAYLOAD;
                f1_count++;
            }
            continue;
        }

        if (marker == 0xF2) {
            if (f1_count > 0 && payload_size >= HDR_SIZE) {
                pkt_rec_t *p = &pkts[pkt_count];
                memcpy(p->header, pkt_buf, HDR_SIZE);
                p->qs = pkt_buf[3];
                p->type = pkt_buf[39];
                p->f1_sectors = f1_count;

                int ok1 = (pkt_buf[0]==0x00 && pkt_buf[1]==0x80 && pkt_buf[2]==0x04);
                int ok2 = (pkt_buf[36]==0x00 && pkt_buf[37]==0x80 && pkt_buf[38]==0x24);
                int okq = (memcmp(pkt_buf+4, pkt_buf+20, 16)==0);
                int oks = (p->qs >= 1 && p->qs <= 100);
                p->valid = ok1 && ok2 && okq && oks;

                /* Decide whether to store full DCs */
                int want_detail = p->valid && detail_count_per_type[p->type] < DETAIL_PER_TYPE
                                  && total_detail < max_detail;

                int bs_size = payload_size - HDR_SIZE;
                if (bs_size > 0) {
                    int *dc_buf = want_detail ? details[total_detail].dc_vals : NULL;
                    p->dc_count = decode_iframe(pkt_buf + HDR_SIZE, bs_size,
                        dc_buf, 864, &p->ac_count,
                        &p->dc_sum, &p->dc_abs_sum,
                        &p->dc_min, &p->dc_max,
                        &p->dc_zeros, &p->dc_small);

                    if (want_detail) {
                        detail_rec_t *d = &details[total_detail];
                        d->pkt_index = pkt_count;
                        d->type = p->type;
                        d->dc_count = p->dc_count;
                        d->ac_count = p->ac_count;
                        d->dc_sum = p->dc_sum;
                        d->dc_abs_sum = p->dc_abs_sum;
                        d->dc_min = p->dc_min;
                        d->dc_max = p->dc_max;
                        d->dc_zeros = p->dc_zeros;
                        d->dc_small = p->dc_small;
                        detail_count_per_type[p->type]++;
                        total_detail++;
                    }
                }
                pkt_count++;
            }
            f1_count = 0; payload_size = 0;
            continue;
        }
    }

    printf("========================================\n");
    printf("TOTAL PACKETS ASSEMBLED: %d\n", pkt_count);
    printf("========================================\n\n");

    /* ---- Valid/Invalid counts ---- */
    int valid_count = 0, invalid_count = 0;
    int valid_type_dist[256] = {0};
    int invalid_type_dist[256] = {0};

    for (int i = 0; i < pkt_count; i++) {
        if (pkts[i].valid) { valid_count++; valid_type_dist[pkts[i].type]++; }
        else { invalid_count++; invalid_type_dist[pkts[i].type]++; }
    }

    printf("VALID headers:   %d\n", valid_count);
    printf("INVALID headers: %d\n\n", invalid_count);

    /* Valid type distribution */
    printf("--- Valid-header packets: byte[39] distribution ---\n");
    for (int t = 0; t < 256; t++)
        if (valid_type_dist[t] > 0)
            printf("  type=0x%02X: %5d packets\n", t, valid_type_dist[t]);

    /* Invalid type distribution */
    printf("\n--- Invalid-header packets: byte[39] distribution ---\n");
    for (int t = 0; t < 256; t++)
        if (invalid_type_dist[t] > 0)
            printf("  type=0x%02X: %5d packets\n", t, invalid_type_dist[t]);

    /* Dump first 10 invalid headers */
    printf("\n--- First 10 invalid-header packets: 40-byte hex dump ---\n");
    int inv_dumped = 0;
    for (int i = 0; i < pkt_count && inv_dumped < 10; i++) {
        if (pkts[i].valid) continue;
        pkt_rec_t *p = &pkts[i];
        printf("  Pkt #%d (%d F1 sectors): ", i, p->f1_sectors);
        for (int j = 0; j < HDR_SIZE; j++) {
            printf("%02X ", p->header[j]);
            if (j == 19) printf("\n                          ");
        }
        printf(" [");
        if (!(p->header[0]==0x00 && p->header[1]==0x80 && p->header[2]==0x04)) printf("MAGIC1 ");
        if (!(p->header[36]==0x00 && p->header[37]==0x80 && p->header[38]==0x24)) printf("MAGIC2 ");
        if (memcmp(p->header+4, p->header+20, 16) != 0) printf("QTABLE ");
        if (p->qs < 1 || p->qs > 100) printf("QS=%d ", p->qs);
        printf("]\n");
        inv_dumped++;
    }

    /* ---- Section 4: Per-type decode analysis (valid only) ---- */
    printf("\n========================================\n");
    printf("I-FRAME DECODE ANALYSIS (valid-header, first packet per type)\n");
    printf("========================================\n\n");

    for (int t = 0; t < 256; t++) {
        if (valid_type_dist[t] == 0) continue;

        /* Find first valid packet of this type */
        pkt_rec_t *first = NULL;
        for (int i = 0; i < pkt_count; i++) {
            if (pkts[i].valid && pkts[i].type == t) { first = &pkts[i]; break; }
        }
        if (!first) continue;

        int ndcs = first->dc_count;
        double mean = ndcs > 0 ? first->dc_sum / ndcs : 0;
        double mean_abs = ndcs > 0 ? first->dc_abs_sum / ndcs : 0;
        int span = first->dc_max - first->dc_min;

        printf("type=0x%02X (%5d valid pkts)\n", t, valid_type_dist[t]);
        printf("  First pkt: %d/864 DCs, %d ACs\n", ndcs, first->ac_count);
        if (ndcs > 0) {
            printf("  DC mean=%.1f, |DC| mean=%.1f, range=[%d,%d] span=%d\n",
                   mean, mean_abs, first->dc_min, first->dc_max, span);
            printf("  zeros=%d, small(|v|<=5)=%d/%d\n",
                   first->dc_zeros, first->dc_small, ndcs);
            if (span > 50 && first->dc_small < ndcs * 7 / 10)
                printf("  => ABSOLUTE I-frame\n");
            else if (ndcs >= 864 && first->dc_small > ndcs * 7 / 10)
                printf("  => DELTA frame\n");
            else if (ndcs < 100)
                printf("  => Early decode failure (%d DCs)\n", ndcs);
            else
                printf("  => Ambiguous\n");
        }
        printf("\n");
    }

    /* ---- Section 5: Detailed type=0x06 ---- */
    printf("========================================\n");
    printf("DETAILED TYPE=0x06 ANALYSIS (DPCM pred=0)\n");
    printf("========================================\n\n");

    int t06_shown = 0;
    for (int d = 0; d < total_detail; d++) {
        if (details[d].type != 0x06) continue;
        detail_rec_t *r = &details[d];
        int ndcs = r->dc_count;
        double mean = ndcs > 0 ? r->dc_sum / ndcs : 0;
        double mean_abs = ndcs > 0 ? r->dc_abs_sum / ndcs : 0;
        int span = r->dc_max - r->dc_min;

        printf("  Pkt #%d: %d DCs, %d ACs, mean=%.1f, |mean|=%.1f, range=[%d,%d], small=%d\n",
               r->pkt_index, ndcs, r->ac_count, mean, mean_abs, r->dc_min, r->dc_max, r->dc_small);

        int show = ndcs < 24 ? ndcs : 24;
        printf("    First %d DCs: ", show);
        for (int i = 0; i < show; i++) printf("%d ", r->dc_vals[i]);
        printf("\n");

        if (span > 50 && r->dc_small < ndcs * 7 / 10)
            printf("    => ABSOLUTE I-frame (large DC values)\n");
        else if (ndcs >= 864 && r->dc_small > ndcs * 7 / 10)
            printf("    => DELTA (DCs near zero)\n");
        else
            printf("    => Ambiguous (span=%d, small=%d/%d)\n", span, r->dc_small, ndcs);
        printf("\n");
        t06_shown++;
    }
    printf("  Showed %d detail packets for type=0x06 (of %d total valid)\n\n",
           t06_shown, valid_type_dist[0x06]);

    /* Also show type=0x00 and type=0x07 details for comparison */
    for (int target = 0x00; target <= 0x07; target++) {
        if (target != 0x00 && target != 0x07) continue;
        printf("--- Detail for type=0x%02X ---\n", target);
        for (int d = 0; d < total_detail; d++) {
            if (details[d].type != target) continue;
            detail_rec_t *r = &details[d];
            int ndcs = r->dc_count;
            int show = ndcs < 24 ? ndcs : 24;
            printf("  Pkt #%d: %d DCs, %d ACs | first %d: ", r->pkt_index, ndcs, r->ac_count, show);
            for (int i = 0; i < show; i++) printf("%d ", r->dc_vals[i]);
            printf("\n");
        }
        printf("\n");
    }

    /* ---- Aggregate stats per type ---- */
    printf("========================================\n");
    printf("AGGREGATE STATS PER TYPE (all valid packets)\n");
    printf("========================================\n\n");

    for (int t = 0; t < 256; t++) {
        if (valid_type_dist[t] == 0) continue;

        int nfull = 0, npartial = 0, nfail = 0;
        double grand_abs = 0;
        int grand_min = 999999, grand_max = -999999;
        long grand_acs = 0;
        int cnt = 0;

        for (int i = 0; i < pkt_count; i++) {
            if (!pkts[i].valid || pkts[i].type != t) continue;
            int nd = pkts[i].dc_count;
            if (nd >= 864) nfull++;
            else if (nd >= 100) npartial++;
            else nfail++;

            if (nd > 0) {
                grand_abs += pkts[i].dc_abs_sum / nd;
                if (pkts[i].dc_min < grand_min) grand_min = pkts[i].dc_min;
                if (pkts[i].dc_max > grand_max) grand_max = pkts[i].dc_max;
                cnt++;
            }
            grand_acs += pkts[i].ac_count;
        }

        printf("type=0x%02X: %5d pkts | full=%d partial=%d fail=%d",
               t, valid_type_dist[t], nfull, npartial, nfail);
        if (cnt > 0)
            printf(" | avg|DC|=%.1f range=[%d,%d] avgAC=%ld",
                   grand_abs / cnt, grand_min, grand_max, grand_acs / cnt);
        printf("\n");
    }

    printf("\n========================================\n");
    printf("DONE\n");
    printf("========================================\n");

    free(details);
    free(pkts);
    free(pkt_buf);
    free(disc);
    return 0;
}
