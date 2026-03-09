/*
 * Playdia video - Block size probing
 * Strategy: DC VLC seems correct. Find how many bits between DCs.
 * Try: DC + skip K bits, repeat. Find K that gives stable DC values.
 * Also try JPEG-style AC (run/size pairs instead of MPEG-1 run/level)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <zip.h>

#define SECTOR_RAW 2352
#define MAX_FRAME  65536
#define OUT_DIR "/home/wizzard/share/GitHub/playdia-emu/tools/test_output/"

static void write_pgm(const char *p, const uint8_t *g, int w, int h) {
    FILE *f=fopen(p,"wb"); if(!f)return;
    fprintf(f,"P5\n%d %d\n255\n",w,h); fwrite(g,1,w*h,f); fclose(f);
    printf("  -> %s (%dx%d)\n",p,w,h);
}

typedef struct { const uint8_t *data; int len,pos,bit,total; } BR;
static void br_init(BR *b, const uint8_t *d, int l) { b->data=d;b->len=l;b->pos=0;b->bit=7;b->total=0; }
static int br_eof(BR *b) { return b->pos>=b->len; }
static int br_get1(BR *b) {
    if(b->pos>=b->len) return 0;
    int v=(b->data[b->pos]>>b->bit)&1;
    if(--b->bit<0){b->bit=7;b->pos++;}
    b->total++; return v;
}
static int br_get(BR *b, int n) { int v=0; for(int i=0;i<n;i++) v=(v<<1)|br_get1(b); return v; }
static void br_skip(BR *b, int n) { for(int i=0;i<n;i++) br_get1(b); }
static void br_save(BR *b, int *sp, int *sb) { *sp=b->pos; *sb=b->bit; }
static void br_restore(BR *b, int sp, int sb, int total) { b->pos=sp; b->bit=sb; b->total=total; }

static int mpeg1_dc_lum(BR *b) {
    int size;
    if (br_get1(b) == 0) { size = br_get1(b) ? 2 : 1; }
    else {
        if (br_get1(b) == 0) { size = br_get1(b) ? 3 : 0; }
        else {
            if (br_get1(b) == 0) size = 4;
            else if (br_get1(b) == 0) size = 5;
            else if (br_get1(b) == 0) size = 6;
            else size = br_get1(b) ? 8 : 7;
        }
    }
    if (size == 0) return 0;
    int val = br_get(b, size);
    if (val < (1 << (size-1))) val = val - (1 << size) + 1;
    return val;
}

static int mpeg1_dc_chr(BR *b) {
    int size;
    if (br_get1(b) == 0) { size = br_get1(b) ? 1 : 0; }
    else {
        if (br_get1(b) == 0) size = 2;
        else if (br_get1(b) == 0) size = 3;
        else if (br_get1(b) == 0) size = 4;
        else if (br_get1(b) == 0) size = 5;
        else if (br_get1(b) == 0) size = 6;
        else size = br_get1(b) ? 8 : 7;
    }
    if (size == 0) return 0;
    int val = br_get(b, size);
    if (val < (1 << (size-1))) val = val - (1 << size) + 1;
    return val;
}

static int assemble_frames(const uint8_t *disc, int tsec, int slba,
    uint8_t fr[][MAX_FRAME], int fs[], int mx) {
    int n=0,c=0; bool inf=false;
    for(int l=slba;l<tsec&&n<mx;l++){
        const uint8_t *s=disc+(long)l*SECTOR_RAW;
        if(s[0]!=0||s[1]!=0xFF||s[15]!=2||(s[18]&4)) continue;
        if(s[24]==0xF1){if(!inf){inf=true;c=0;}if(c+2047<MAX_FRAME){memcpy(fr[n]+c,s+25,2047);c+=2047;}}
        else if(s[24]==0xF2){if(inf&&c>0){fs[n]=c;n++;inf=false;c=0;}}
    } return n;
}

/*
 * Probe 1: DC + skip K fixed bits between blocks
 * Measure DC value stability (variance of differences)
 */
static void probe_fixed_skip(const uint8_t *bs, int bslen) {
    printf("\n=== Probe: DC + fixed skip ===\n");
    printf("Skip  | Blocks | DC range  | Avg|diff| | Assessment\n");
    printf("------+--------+-----------+-----------+-----------\n");

    for (int skip = 0; skip <= 400; skip += 5) {
        if (skip == 0) skip = 0; /* include 0 */
        BR br; br_init(&br, bs, bslen);
        int dc[4096], ndc = 0;
        int prev = 0;
        int errors = 0;

        while (ndc < 4096 && !br_eof(&br)) {
            int before = br.total;
            int diff = mpeg1_dc_lum(&br);
            int bits_used = br.total - before;

            /* Sanity check: DC size shouldn't be > 8, value shouldn't be huge */
            if (bits_used > 16 || abs(diff) > 255) { errors++; }

            prev += diff;
            dc[ndc++] = prev;

            if (skip > 0) br_skip(&br, skip);
        }

        if (ndc < 10) continue;

        /* Measure stability */
        int mn = dc[0], mx = dc[0];
        double sumdiff = 0;
        for (int i = 1; i < ndc && i < 500; i++) {
            if (dc[i] < mn) mn = dc[i];
            if (dc[i] > mx) mx = dc[i];
            sumdiff += abs(dc[i] - dc[i-1]);
        }
        int cnt = ndc < 500 ? ndc : 500;
        double avgdiff = sumdiff / (cnt - 1);
        int range = mx - mn;

        /* Good: range stays small (< 300), avg diff is small */
        const char *assess = "";
        if (range < 200 && avgdiff < 20) assess = "*** GOOD ***";
        else if (range < 400 && avgdiff < 40) assess = "* possible *";

        if (skip % 50 == 0 || assess[0] == '*')
            printf("%5d | %5d | [%4d,%4d] | %8.1f | %s err=%d\n",
                   skip, ndc, mn, mx, avgdiff, assess, errors);

        if (skip == 0) skip = 5 - 5; /* will become 5 next iteration */
    }
}

/*
 * Probe 2: Try JPEG-style AC decoding
 * In JPEG, AC uses a Huffman code that gives (run, size),
 * then 'size' additional bits for the value.
 * The standard JPEG luminance AC Huffman table:
 */
static const struct {
    int run, size;
    int code, codelen;
} jpeg_ac_lum[] = {
    /* EOB */  {0, 0, 0x0A, 4},   /* 1010 */
    /* ZRL */  {15, 0, 0x7F9, 11}, /* ... */
    /* Common codes - simplified standard table */
    {0, 1, 0x00, 2},   /* 00 */
    {0, 2, 0x01, 2},   /* 01 */
    {0, 3, 0x04, 3},   /* 100 */
    {0, 4, 0x0B, 4},   /* 1011 */
    {0, 5, 0x1A, 5},   /* 11010 */
    {0, 6, 0x78, 7},   /* 1111000 */
    {1, 1, 0x0C, 4},   /* 1100 */
    {1, 2, 0x1B, 5},   /* 11011 */
    {1, 3, 0x79, 7},   /* 1111001 */
    {2, 1, 0x1C, 5},   /* 11100 */
    {2, 2, 0xF8, 8},   /* 11111000 */
    {3, 1, 0x3A, 6},   /* 111010 */
    {4, 1, 0x3B, 6},   /* 111011 */
    {5, 1, 0x7A, 7},   /* 1111010 */
    {6, 1, 0x7B, 7},   /* 1111011 */
    {7, 1, 0xF9, 8},   /* 11111001 */
    {8, 1, 0x1F6, 9},  /* 111110110 */
    {9, 1, 0x1F7, 9},  /* 111110111 */
    {10,1, 0xFF4, 12},
    {11,1, 0xFF5, 12},
    {-1,-1,0,0}  /* sentinel */
};

/*
 * Probe 3: Measure actual bit consumption per block with MPEG-1 AC
 * but WITHOUT treating '10' as EOB
 */
static void probe_no_eob(const uint8_t *bs, int bslen) {
    printf("\n=== Probe: MPEG-1 AC VLC but 10=run0/level1 (no EOB) ===\n");

    BR br; br_init(&br, bs, bslen);
    int dc_pred = 0;

    for (int blk = 0; blk < 10 && !br_eof(&br); blk++) {
        int before = br.total;

        /* DC */
        int dc_diff = mpeg1_dc_lum(&br);
        dc_pred += dc_diff;
        int dc_bits = br.total - before;

        printf("Block %d: DC=%d (diff=%d, %d bits)\n", blk, dc_pred, dc_diff, dc_bits);

        /* Try to decode 63 AC coefficients
         * Treat ALL VLC codes as run/level (no EOB)
         * '10' → ??? and '11s' → run=0,level=±1
         */
        printf("  AC coeffs: ");
        int ac_count = 0;
        int idx = 1;
        while (idx < 64 && !br_eof(&br)) {
            int ac_before = br.total;
            int b1 = br_get1(&br);

            if (b1 == 1) {
                int b2 = br_get1(&br);
                if (b2 == 1) {
                    /* 11s → run=0, level=±1 */
                    int s = br_get1(&br);
                    int level = s ? -1 : 1;
                    printf("[%d]=%d ", idx, level);
                    idx++;
                } else {
                    /* 10 - in MPEG-1 this is EOB, but what if it's not?
                     * Maybe it means something else.
                     * Option A: run=0, level=0 (skip)
                     * Option B: some other coding
                     * For now, treat as "next coeff is zero, skip"
                     */
                    printf("[%d]=EOB? ", idx);
                    idx = 64; /* force end for now */
                }
            } else {
                /* 0... longer codes */
                int b2 = br_get1(&br);
                if (b2 == 1) {
                    /* 01s → run=1, level=±1 */
                    int s = br_get1(&br);
                    int level = s ? -1 : 1;
                    idx += 1;
                    if (idx < 64) {
                        printf("[%d]=%d ", idx, level);
                        idx++;
                    }
                } else {
                    /* 00... */
                    int b3 = br_get1(&br);
                    if (b3 == 1) {
                        int b4 = br_get1(&br);
                        int s = br_get1(&br);
                        if (b4 == 0) { /* 0010s → run=0, level=±2 */
                            printf("[%d]=%d ", idx, s ? -2 : 2);
                            idx++;
                        } else { /* 0011s → run=2, level=±1 */
                            idx += 2;
                            if (idx < 64) { printf("[%d]=%d ", idx, s ? -1 : 1); idx++; }
                        }
                    } else {
                        /* 000... skip rest for now */
                        printf("(long code at bit %d) ", br.total);
                        break;
                    }
                }
            }
            ac_count++;
        }
        printf("\n  Total: %d bits for block (%d AC decoded)\n", br.total - before, ac_count);
    }
}

/*
 * Probe 4: What if data is entirely sign-magnitude coded samples?
 * Code: size (unary or VLC) + value bits
 * Essentially the "DC VLC" applied to every sample
 * Try as DPCM (differences from previous sample)
 */
static void probe_dpcm_vlc(const uint8_t *bs, int bslen, const char *game, int fi) {
    printf("\n=== Probe: Full DPCM with DC VLC per pixel ===\n");

    BR br; br_init(&br, bs, bslen);

    /* Count samples */
    int nsamples = 0;
    int prev = 128;  /* start at mid-gray */
    int vals[32768];

    while (nsamples < 32768 && !br_eof(&br)) {
        int diff = mpeg1_dc_lum(&br);
        prev += diff;
        /* Clamp to 0-255 */
        if (prev < 0) prev = 0;
        if (prev > 255) prev = 255;
        vals[nsamples++] = prev;
    }

    printf("Decoded %d DPCM samples using %d bits (%.2f bits/sample)\n",
           nsamples, br.total, (double)br.total / nsamples);

    /* Render at various widths */
    int widths[] = {64, 128, 160, 192, 256};
    for (int wi = 0; wi < 5; wi++) {
        int W = widths[wi];
        int H = nsamples / W;
        if (H < 2) continue;
        uint8_t *img = calloc(W * H, 1);
        for (int i = 0; i < W * H; i++) img[i] = vals[i];
        char path[256];
        snprintf(path, sizeof(path), OUT_DIR "dpcm_%s_f%d_w%d.pgm", game, fi, W);
        write_pgm(path, img, W, H);
        free(img);
    }

    /* Also try: DPCM with prediction reset per line */
    for (int W = 128; W <= 128; W += 32) {
        br_init(&br, bs, bslen);
        int H = 192;  /* try various heights */
        uint8_t *img = calloc(W * H, 1);
        for (int y = 0; y < H && !br_eof(&br); y++) {
            prev = 128;  /* reset per line */
            for (int x = 0; x < W && !br_eof(&br); x++) {
                int diff = mpeg1_dc_lum(&br);
                prev += diff;
                if (prev < 0) prev = 0;
                if (prev > 255) prev = 255;
                img[y * W + x] = prev;
            }
        }
        char path[256];
        snprintf(path, sizeof(path), OUT_DIR "dpcm_lr_%s_f%d_w%d.pgm", game, fi, W);
        write_pgm(path, img, W, H);
        free(img);
    }
}

/*
 * Probe 5: What if the 16 qtable entries define bit-widths?
 * E.g., qtable[i] / something = number of bits for coefficient i
 * With values like 10,20,14,13,18,37,22,28,15,24,15,18,18,31,17,20
 * Divided by 8: 1.25, 2.5, 1.75, 1.6, 2.25, 4.6, 2.75, 3.5, ...
 * That doesn't make sense as bit counts.
 * Divided by 4: 2.5, 5, 3.5, 3.25, 4.5, 9.25, 5.5, 7, ...
 * Also weird. Skip this.
 */

/*
 * Probe 6: Look for patterns in bit distances between "10" patterns
 * If "10" really is EOB, the distances between EOBs should cluster
 * at certain values (block sizes)
 */
static void probe_eob_distances(const uint8_t *bs, int bslen) {
    printf("\n=== Probe: Distance between '10' patterns ===\n");

    /* Find all positions where bits are "10" */
    int positions[10000], npos = 0;
    for (int i = 0; i < bslen * 8 - 1 && npos < 10000; i++) {
        int b0 = (bs[i/8] >> (7 - (i%8))) & 1;
        int b1 = (bs[(i+1)/8] >> (7 - ((i+1)%8))) & 1;
        if (b0 == 1 && b1 == 0) {
            positions[npos++] = i;
        }
    }

    printf("Found %d '10' patterns in first %d bits\n", npos, bslen * 8);

    /* Histogram of distances */
    int hist[512] = {0};
    for (int i = 1; i < npos && i < 5000; i++) {
        int dist = positions[i] - positions[i-1];
        if (dist < 512) hist[dist]++;
    }

    printf("Top 20 most common distances:\n");
    int hist2[512]; memcpy(hist2, hist, sizeof(hist));
    for (int t = 0; t < 20; t++) {
        int maxv = 0, maxi = 0;
        for (int i = 1; i < 512; i++) {
            if (hist2[i] > maxv) { maxv = hist2[i]; maxi = i; }
        }
        if (maxv == 0) break;
        printf("  dist=%3d: %d times\n", maxi, maxv);
        hist2[maxi] = 0;
    }
}

/* LSB probe removed for now */

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <zip> [lba] [game]\n", argv[0]); return 1; }
    int slba = argc > 2 ? atoi(argv[2]) : 502;
    const char *game = argc > 3 ? argv[3] : "mari";

    int err; zip_t *z = zip_open(argv[1], ZIP_RDONLY, &err); if (!z) return 1;
    int bi=-1; zip_uint64_t bs2=0;
    for (int i=0; i<(int)zip_get_num_entries(z,0); i++) {
        zip_stat_t st; if(zip_stat_index(z,i,0,&st)==0 && st.size>bs2){bs2=st.size;bi=i;}}
    zip_stat_t st; zip_stat_index(z,bi,0,&st);
    zip_file_t *zf = zip_fopen_index(z,bi,0);
    uint8_t *disc = malloc(st.size);
    zip_int64_t rd=0;
    while(rd<(zip_int64_t)st.size){zip_int64_t r=zip_fread(zf,disc+rd,st.size-rd);if(r<=0)break;rd+=r;}
    zip_fclose(zf);
    int tsec = (int)(st.size/SECTOR_RAW);

    static uint8_t frames[16][MAX_FRAME]; int fsizes[16];
    int nf = assemble_frames(disc,tsec,slba,frames,fsizes,16);
    printf("Assembled %d frames\n", nf);

    /* Analyze frame 0 */
    int fi = 0;
    uint8_t *f = frames[fi];
    int fsize = fsizes[fi];
    printf("Frame %d: %d bytes, qscale=%d, type=%d\n", fi, fsize, f[3], f[39]);

    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;

    probe_fixed_skip(bs, bslen);
    probe_no_eob(bs, bslen);
    probe_eob_distances(bs, bslen);
    probe_dpcm_vlc(bs, bslen, game, fi);
    /* probe_lsb_dpcm(bs, bslen, game, fi); */

    free(disc); zip_close(z);
    return 0;
}
