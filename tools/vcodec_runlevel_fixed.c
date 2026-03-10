/*
 * vcodec_runlevel_fixed.c - Test fixed-width run-level entries
 *
 * KEY TEST: For the correct entry size, sum(run) + num_entries should
 * consistently equal ~63 per block (covering all AC positions).
 * Wrong entry sizes will give random/inconsistent sums.
 *
 * Also: check if levels are small (±1,±2) and positions show frequency decay.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define NBLK 864

static int get_bit(const uint8_t *d, int bp) { return (d[bp>>3]>>(7-(bp&7)))&1; }
static uint32_t get_bits(const uint8_t *d, int bp, int n) {
    uint32_t v=0; for(int i=0;i<n;i++) v=(v<<1)|get_bit(d,bp+i); return v;
}

static const struct{int len;uint32_t code;} dcv[]={
    {3,0x4},{2,0x0},{2,0x1},{3,0x5},{3,0x6},
    {4,0xE},{5,0x1E},{6,0x3E},{7,0x7E},
    {8,0xFE},{9,0x1FE},{10,0x3FE}};

static int dec_dc(const uint8_t *d, int bp, int *val, int tb) {
    for(int i=0;i<12;i++){
        if(bp+dcv[i].len>tb) continue;
        uint32_t b=get_bits(d,bp,dcv[i].len);
        if(b==dcv[i].code){
            int sz=i,c=dcv[i].len;
            if(sz==0){*val=0;}
            else{if(bp+c+sz>tb)return-1;uint32_t r=get_bits(d,bp+c,sz);c+=sz;
                *val=(r<(1u<<(sz-1)))?(int)r-(1<<sz)+1:(int)r;}
            return c;}}
    return -1;
}

static uint8_t fbuf[16384];
static int flen;

static int load_frame(const char *binfile, int start_lba, int target) {
    FILE *fp=fopen(binfile,"rb"); if(!fp)return-1;
    int fc=0,f1c=0; flen=0;
    for(int s=0;s<3000;s++){
        long off=(long)(start_lba+s)*2352;
        uint8_t sec[2352]; fseek(fp,off,SEEK_SET);
        if(fread(sec,1,2352,fp)!=2352)break;
        uint8_t t=sec[24];
        if(t==0xF1){if(fc==target&&f1c<6)memcpy(fbuf+f1c*2047,sec+25,2047);f1c++;}
        else if(t==0xF2){if(fc==target&&f1c==6){flen=6*2047;fclose(fp);return 0;}fc++;f1c=0;}
        else if(t==0xF3||t==0x1C){f1c=0;}
        else{f1c=0;}
    }
    fclose(fp); return -1;
}

/* Test fixed-width run-level entries with no EOB (read until end of block budget) */
/* entry_bits = run_bits + level_bits */
/* Returns: number of blocks where sum(run)+entries reaches exactly 63 */
static void test_fixed_rl(const uint8_t *ac, int abits, int run_bits, int level_bits,
                          const char *label, int verbose) {
    int entry_bits = run_bits + level_bits;
    int bp = 0;
    int exact63 = 0, over63 = 0, under63 = 0;
    int total_nz = 0;
    int level_hist[256] = {0}; /* histogram of |level| values */
    int bands[8] = {0};

    for(int b = 0; b < NBLK; b++) {
        int pos = 0;
        int entries = 0;
        int block_ok = 1;

        while(pos < 63 && bp + entry_bits <= abits) {
            int run = get_bits(ac, bp, run_bits);
            bp += run_bits;

            int raw_level = get_bits(ac, bp, level_bits);
            bp += level_bits;

            /* Signed level: MSB is sign, rest is magnitude */
            int level;
            if(level_bits > 1) {
                int sign = raw_level >> (level_bits - 1);
                int mag = raw_level & ((1 << (level_bits-1)) - 1);
                level = sign ? -mag : mag;
                if(mag == 0 && sign == 0) level = 0;
            } else {
                level = raw_level; /* 1-bit: 0 or 1 */
            }

            pos += run;
            if(pos >= 63) { over63++; block_ok = 0; break; }

            if(level != 0) {
                total_nz++;
                int band = pos / 8;
                if(band < 8) bands[band]++;
                int alev = abs(level);
                if(alev < 256) level_hist[alev]++;
            }
            pos++; /* position of the coded coefficient */
            entries++;

            /* Check for EOB-like condition: run reaches 63 */
            /* Some codecs use run=special or level=0 as EOB */
        }

        if(pos == 63) exact63++;
        else if(pos < 63 && block_ok) under63++;
    }

    double pct = 100.0 * bp / abits;
    printf("%-25s %5.1f%%  exact63=%3d over=%3d under=%3d  NZ=%5d  ",
           label, pct, exact63, over63, under63, total_nz);
    printf("bands:%d %d %d %d %d %d %d %d  ",
           bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
    printf("lev: 0=%d 1=%d 2=%d 3=%d 4=%d 5+=%d\n",
           level_hist[0], level_hist[1], level_hist[2], level_hist[3], level_hist[4],
           level_hist[5]+level_hist[6]+level_hist[7]+level_hist[8]+level_hist[9]+
           level_hist[10]+level_hist[11]+level_hist[12]+level_hist[13]+level_hist[14]+level_hist[15]);

    if(verbose && exact63 > 100) {
        printf("  ** %d blocks (%.0f%%) hit exactly 63 positions! **\n",
               exact63, 100.0*exact63/NBLK);
    }
}

/* Test with EOB: level=0 means EOB (skip remaining positions) */
static void test_fixed_rl_eob(const uint8_t *ac, int abits, int run_bits, int level_bits,
                               const char *label) {
    int entry_bits = run_bits + level_bits;
    int bp = 0;
    int blocks_ok = 0, blocks_eob = 0;
    int total_nz = 0;
    int bands[8] = {0};
    int entries_per_block[NBLK];

    for(int b = 0; b < NBLK; b++) {
        int pos = 0;
        int entries = 0;
        int eob = 0;

        while(pos < 63 && bp + entry_bits <= abits) {
            int run = get_bits(ac, bp, run_bits);
            bp += run_bits;

            int raw_level = get_bits(ac, bp, level_bits);
            bp += level_bits;

            /* Check EOB: level=0 (or some special value) */
            if(raw_level == 0) { eob = 1; break; }

            int sign = raw_level >> (level_bits - 1);
            int mag = raw_level & ((1 << (level_bits-1)) - 1);
            if(mag == 0) mag = (1 << (level_bits-1)); /* handle sign-magnitude edge */
            int level = sign ? -(int)mag : (int)mag;

            pos += run;
            if(pos >= 63) break;

            total_nz++;
            int band = pos / 8;
            if(band < 8) bands[band]++;
            pos++;
            entries++;
        }

        entries_per_block[b] = entries;
        if(eob) blocks_eob++;
        blocks_ok++;
    }

    double pct = 100.0 * bp / abits;
    /* Average entries per block */
    double avg_entries = 0;
    for(int b = 0; b < NBLK; b++) avg_entries += entries_per_block[b];
    avg_entries /= NBLK;

    printf("%-25s %5.1f%%  EOBs=%3d  NZ=%5d  avg_entries=%.1f  ",
           label, pct, blocks_eob, total_nz, avg_entries);
    printf("bands:%d %d %d %d %d %d %d %d\n",
           bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
}

/* PS1 MDEC style: 16-bit words, [run(6):level(10)] signed, level=0x200=EOB */
static void test_mdec_style(const uint8_t *ac, int abits, const char *label, int eob_val) {
    int bp = 0;
    int blocks_ok = 0, blocks_eob = 0;
    int total_nz = 0;
    int bands[8] = {0};
    double avg_entries = 0;

    for(int b = 0; b < NBLK; b++) {
        int pos = 0;
        int entries = 0;

        while(pos < 63 && bp + 16 <= abits) {
            int word = get_bits(ac, bp, 16);
            bp += 16;

            int run = (word >> 10) & 0x3F;
            int level_raw = word & 0x3FF;

            /* Check for EOB */
            if(level_raw == eob_val || word == 0) {
                blocks_eob++;
                break;
            }

            /* Signed 10-bit level */
            int level;
            if(level_raw >= 512)
                level = level_raw - 1024;
            else
                level = level_raw;

            pos += run;
            if(pos >= 63) break;

            total_nz++;
            int band = pos / 8;
            if(band < 8) bands[band]++;
            pos++;
            entries++;
        }
        avg_entries += entries;
        blocks_ok++;
    }

    avg_entries /= NBLK;
    double pct = 100.0 * bp / abits;
    printf("%-25s %5.1f%%  EOBs=%3d  NZ=%5d  avg=%.1f  ",
           label, pct, blocks_eob, total_nz, avg_entries);
    printf("bands:%d %d %d %d %d %d %d %d\n",
           bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
}

/* Test INTERLEAVED DC+AC (not DC-first) with MDEC-style 16-bit words */
static void test_interleaved_mdec(const uint8_t *fdata, int total_bits, int eob_val) {
    int bp = 0;
    int blocks_ok = 0, blocks_eob = 0;
    int total_nz = 0;
    int bands[8] = {0};
    int dc_pred[3] = {0,0,0};
    double avg_entries = 0;

    for(int b = 0; b < NBLK && bp < total_bits; b++) {
        /* First: decode DC with VLC */
        int dv;
        int c = dec_dc(fdata+40, bp, &dv, total_bits);
        if(c < 0) {
            printf("INTRL MDEC: DC fail at block %d, bit %d\n", b, bp);
            break;
        }
        bp += c;

        /* Then: decode AC with 16-bit run-level pairs */
        int pos = 0;
        int entries = 0;

        while(pos < 63 && bp + 16 <= total_bits) {
            int word = get_bits(fdata+40, bp, 16);
            bp += 16;

            int run = (word >> 10) & 0x3F;
            int level_raw = word & 0x3FF;

            if(level_raw == eob_val || word == 0) {
                blocks_eob++;
                break;
            }

            int level = (level_raw >= 512) ? level_raw - 1024 : level_raw;
            pos += run;
            if(pos >= 63) break;

            total_nz++;
            int band = pos / 8;
            if(band < 8) bands[band]++;
            pos++;
            entries++;
        }
        avg_entries += entries;
        blocks_ok++;
    }

    avg_entries /= (blocks_ok > 0 ? blocks_ok : 1);
    printf("INTRL_MDEC(eob=%d):       %5.1f%%  blk=%d  EOBs=%3d  NZ=%5d  avg=%.1f  ",
           eob_val, 100.0*bp/total_bits, blocks_ok, blocks_eob, total_nz, avg_entries);
    printf("bands:%d %d %d %d %d %d %d %d\n",
           bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
}

int main() {
    const char *binfile = "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin";

    /* Test on two frames: F03 (padded, QS=8) and F00 (QS=8, minimal padding) */
    int test_frames[] = {3, 0};
    int test_lbas[] = {502, 502};

    for(int tf = 0; tf < 2; tf++) {
        if(load_frame(binfile, test_lbas[tf], test_frames[tf]) != 0) continue;
        printf("=== Frame F%02d (QS=%d, type=%d) ===\n", test_frames[tf], fbuf[3], fbuf[39]);

        int de = flen; while(de > 0 && fbuf[de-1] == 0xFF) de--;
        int total_bits = (de-40)*8;

        int bp = 0;
        for(int b = 0; b < NBLK; b++) {
            int dv; int c = dec_dc(fbuf+40, bp, &dv, total_bits);
            if(c < 0) { printf("DC fail\n"); break; }
            bp += c;
        }
        int dc_bits = bp, ac_bits = total_bits - dc_bits;
        printf("DC=%d, AC=%d bits (%.2f bits/coeff, %.1f bits/block)\n\n",
               dc_bits, ac_bits, (double)ac_bits/(NBLK*63), (double)ac_bits/NBLK);

        int ac_bytes = (ac_bits+7)/8+1;
        uint8_t *ac_data = calloc(ac_bytes, 1);
        for(int i = 0; i < ac_bits; i++)
            if(get_bit(fbuf+40, dc_bits+i))
                ac_data[i>>3] |= (1 << (7-(i&7)));

        /* Test various fixed-width run-level entries WITHOUT EOB */
        printf("--- Fixed-width run-level (no EOB, read until 63 positions) ---\n");
        test_fixed_rl(ac_data, ac_bits, 3, 3, "RL(3+3=6bit)", 1);
        test_fixed_rl(ac_data, ac_bits, 3, 5, "RL(3+5=8bit)", 1);
        test_fixed_rl(ac_data, ac_bits, 4, 4, "RL(4+4=8bit)", 1);
        test_fixed_rl(ac_data, ac_bits, 5, 3, "RL(5+3=8bit)", 1);
        test_fixed_rl(ac_data, ac_bits, 3, 7, "RL(3+7=10bit)", 1);
        test_fixed_rl(ac_data, ac_bits, 4, 6, "RL(4+6=10bit)", 1);
        test_fixed_rl(ac_data, ac_bits, 5, 5, "RL(5+5=10bit)", 1);
        test_fixed_rl(ac_data, ac_bits, 6, 4, "RL(6+4=10bit)", 1);
        test_fixed_rl(ac_data, ac_bits, 4, 8, "RL(4+8=12bit)", 1);
        test_fixed_rl(ac_data, ac_bits, 5, 7, "RL(5+7=12bit)", 1);
        test_fixed_rl(ac_data, ac_bits, 6, 6, "RL(6+6=12bit)", 1);
        test_fixed_rl(ac_data, ac_bits, 6, 10, "RL(6+10=16bit)", 1);

        /* Test with EOB (level=0 means EOB) */
        printf("\n--- Fixed-width run-level WITH EOB (level=0) ---\n");
        test_fixed_rl_eob(ac_data, ac_bits, 3, 5, "RL_EOB(3+5=8bit)");
        test_fixed_rl_eob(ac_data, ac_bits, 4, 4, "RL_EOB(4+4=8bit)");
        test_fixed_rl_eob(ac_data, ac_bits, 4, 6, "RL_EOB(4+6=10bit)");
        test_fixed_rl_eob(ac_data, ac_bits, 5, 5, "RL_EOB(5+5=10bit)");
        test_fixed_rl_eob(ac_data, ac_bits, 6, 6, "RL_EOB(6+6=12bit)");
        test_fixed_rl_eob(ac_data, ac_bits, 6, 10, "RL_EOB(6+10=16bit)");

        /* PS1 MDEC style 16-bit on AC-only data */
        printf("\n--- MDEC-style 16-bit on AC-only data ---\n");
        test_mdec_style(ac_data, ac_bits, "MDEC(eob=0x200)", 0x200);
        test_mdec_style(ac_data, ac_bits, "MDEC(eob=0x000)", 0x000);
        test_mdec_style(ac_data, ac_bits, "MDEC(eob=0x3FE)", 0x3FE);
        test_mdec_style(ac_data, ac_bits, "MDEC(eob=0x3FF)", 0x3FF);

        /* Also test RANDOM data for comparison */
        uint8_t *rnd = malloc(ac_bytes);
        srand(42+tf);
        for(int i = 0; i < ac_bytes; i++) rnd[i] = rand() & 0xFF;

        printf("\n--- RANDOM DATA comparison ---\n");
        test_fixed_rl(rnd, ac_bits, 4, 4, "RAND RL(4+4)", 0);
        test_fixed_rl(rnd, ac_bits, 6, 6, "RAND RL(6+6)", 0);
        test_fixed_rl_eob(rnd, ac_bits, 4, 4, "RAND RL_EOB(4+4)");
        test_fixed_rl_eob(rnd, ac_bits, 6, 6, "RAND RL_EOB(6+6)");

        free(ac_data);
        free(rnd);

        /* Also test INTERLEAVED DC+AC (not DC-first) */
        printf("\n--- INTERLEAVED DC+AC with MDEC-style ---\n");
        test_interleaved_mdec(fbuf, total_bits, 0x200);
        test_interleaved_mdec(fbuf, total_bits, 0x000);

        printf("\n\n");
    }

    /* Test on second game too */
    printf("=== Dragon Ball Z, first I-frame ===\n");
    const char *dbz = "/home/wizzard/share/GitHub/playdia-roms/Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku - Chikyuu-hen (Japan) (Track 2).bin";

    for(int fi = 0; fi < 10; fi++) {
        if(load_frame(dbz, 150, fi) != 0) break;
        if(fbuf[0]!=0x00||fbuf[1]!=0x80||fbuf[2]!=0x04) break;
        if(fbuf[39] != 0) continue; /* Skip P-frames */

        printf("F%d: QS=%d, type=%d\n", fi, fbuf[3], fbuf[39]);
        int de=flen; while(de>0&&fbuf[de-1]==0xFF)de--;
        int total_bits=(de-40)*8;
        int bp=0;
        for(int b=0;b<NBLK;b++){int dv;bp+=dec_dc(fbuf+40,bp,&dv,total_bits);}
        int ac_bits=total_bits-bp;
        printf("AC=%d bits (%.1f bits/block)\n", ac_bits, (double)ac_bits/NBLK);

        int ac_bytes=(ac_bits+7)/8+1;
        uint8_t *ac_data=calloc(ac_bytes,1);
        for(int i=0;i<ac_bits;i++)
            if(get_bit(fbuf+40,bp+i)) ac_data[i>>3]|=(1<<(7-(i&7)));

        test_fixed_rl(ac_data, ac_bits, 4, 4, "RL(4+4=8bit)", 1);
        test_fixed_rl(ac_data, ac_bits, 6, 6, "RL(6+6=12bit)", 1);
        test_fixed_rl_eob(ac_data, ac_bits, 4, 4, "RL_EOB(4+4)");
        test_fixed_rl_eob(ac_data, ac_bits, 6, 6, "RL_EOB(6+6)");
        test_mdec_style(ac_data, ac_bits, "MDEC(eob=0x200)", 0x200);

        free(ac_data);
        break; /* Just first I-frame */
    }

    printf("\nDone.\n");
    return 0;
}
