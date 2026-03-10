/*
 * vcodec_countpos.c - Test "count + position + value" AC coding
 *
 * Hypothesis: each block's AC data starts with a count of nonzero coefficients,
 * followed by (position, value) pairs.
 *
 * Key observation: dense frame = 106.6 bits/block, sparse = 62.3 bits/block.
 * A count + position scheme would naturally scale:
 *   count_bits + NZ × (pos_bits + val_bits) = total
 *   For 5+NZ×7 (pos=6, val=1): dense 5+14.5×7=106.5, sparse 5+8.2×7=62.4 ← MATCHES BOTH!
 *   For 5+NZ×9 (pos=6, val≈3): dense 5+11.3×9=106.7, sparse 5+6.4×9=62.6 ← ALSO MATCHES!
 *
 * Also test:
 * - count + position only (all values ±1, sign encoded)
 * - count with DC VLC for values
 * - various count bit widths (4,5,6)
 * - zero count optimization (0 = no nonzero coefficients)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static uint8_t fdata[16384];
static int fdatalen;

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

static int load_frame(const char *binfile, int target_lba) {
    FILE *fp=fopen(binfile,"rb"); if(!fp)return 0;
    int f1c=0;
    uint8_t tmpf[16384];
    for(int s=0;s<2000;s++){
        long off=(long)(target_lba+s)*2352;
        uint8_t sec[2352]; fseek(fp,off,SEEK_SET);
        if(fread(sec,1,2352,fp)!=2352)break;
        uint8_t t=sec[24];
        if(t==0xF1){
            if(f1c<6) memcpy(tmpf+f1c*2047,sec+25,2047);
            f1c++;
        } else if(t==0xF2){
            if(f1c==6 && tmpf[0]==0x00 && tmpf[1]==0x80 && tmpf[2]==0x04){
                fdatalen=6*2047;
                memcpy(fdata,tmpf,fdatalen);
                fclose(fp);
                return 1;
            }
            f1c=0;
        } else if(t==0xF3) f1c=0;
    }
    fclose(fp);
    return 0;
}

/* Decode DC, return AC start bit position */
static int decode_dc_all(int *dc_vals) {
    int de=fdatalen; while(de>0&&fdata[de-1]==0xFF)de--;
    int tb=de*8;
    int bp=40*8;
    int dc_pred[3]={0,0,0};
    for(int mb=0;mb<144;mb++){
        for(int bl=0;bl<6;bl++){
            int comp=(bl<4)?0:(bl==4)?1:2;
            int dv;
            int used=dec_dc(fdata,bp,&dv,tb);
            if(used<0) return -1;
            dc_pred[comp]+=dv;
            dc_vals[mb*6+bl]=dc_pred[comp];
            bp+=used;
        }
    }
    return bp;
}

int main() {
    const char *games[] = {
        "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Ie Naki Ko - Suzu no Sentaku (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku - Chikyuu-hen (Japan) (Track 2).bin",
        NULL
    };
    int test_lbas[] = {148, 500, 755, 1100};

    printf("=== Count + Position + Value hypothesis testing ===\n\n");

    for(int gi=0; games[gi]; gi++){
        const char *gn = strrchr(games[gi],'/');
        if(gn) gn++; else gn=games[gi];

        for(int li=0; li<4; li++){
            if(!load_frame(games[gi], test_lbas[li])) continue;
            int qs=fdata[3], ft=fdata[39];
            int de=fdatalen; while(de>0&&fdata[de-1]==0xFF)de--;
            int pad=fdatalen-de;

            int dc_vals[864];
            int ac_start = decode_dc_all(dc_vals);
            if(ac_start<0) continue;
            int ac_bits = de*8 - ac_start;

            printf("--- %s LBA~%d QS=%d type=%d pad=%d AC=%d (%.1f/blk) ---\n",
                   gn, test_lbas[li], qs, ft, pad, ac_bits, (float)ac_bits/864);

            /* Test various count+pos+val combinations */
            struct {
                const char *name;
                int count_bits;
                int pos_bits;
                int val_bits; /* 0 = use DC VLC, >0 = fixed bits, -1 = sign only (val=±1) */
            } schemes[] = {
                {"5+6+1(sign)", 5, 6, -1},
                {"5+6+2(sm)", 5, 6, 2},
                {"5+6+3", 5, 6, 3},
                {"4+6+1", 4, 6, -1},
                {"4+6+2", 4, 6, 2},
                {"4+6+VLC", 4, 6, 0},
                {"5+6+VLC", 5, 6, 0},
                {"6+6+1", 6, 6, -1},
                {"3+6+2", 3, 6, 2},
                {"5+5+2", 5, 5, 2},
                {"5+4+2", 5, 4, 2},
                {"5+4+3", 5, 4, 3},
                {"4+4+3", 4, 4, 3},
                {"unary+6+1", 0, 6, -1}, /* 0 = unary count */
                {"unary+6+VLC", 0, 6, 0},
                {NULL,0,0,0}
            };

            for(int si=0; schemes[si].name; si++){
                int cb=schemes[si].count_bits;
                int pb=schemes[si].pos_bits;
                int vb=schemes[si].val_bits;

                int bp = ac_start;
                int blocks=0, total_nz=0;
                int count_hist[64]={0};
                int pos_hist[64]={0};
                int val_hist[256]={0};
                int ok=1;

                for(int blk=0; blk<864 && bp<de*8 && ok; blk++){
                    int count;
                    if(cb > 0){
                        if(bp+cb > de*8){ok=0;break;}
                        count = get_bits(fdata, bp, cb);
                        bp += cb;
                    } else {
                        /* Unary: count 0-bits until 1-bit */
                        count=0;
                        while(bp<de*8 && get_bit(fdata,bp)==0){count++;bp++;}
                        if(bp<de*8) bp++; /* consume the 1 */
                        else {ok=0;break;}
                    }

                    if(count>63){ok=0;break;}
                    count_hist[count]++;

                    for(int i=0; i<count && bp<de*8 && ok; i++){
                        /* Position */
                        if(bp+pb > de*8){ok=0;break;}
                        int pos = get_bits(fdata, bp, pb);
                        bp += pb;
                        if(pos>=63){ok=0;break;}
                        pos_hist[pos]++;

                        /* Value */
                        if(vb < 0){
                            /* Sign only: ±1 */
                            if(bp>=de*8){ok=0;break;}
                            bp++; /* sign bit */
                        } else if(vb == 0){
                            /* DC VLC */
                            int val;
                            int used = dec_dc(fdata, bp, &val, de*8);
                            if(used<0){ok=0;break;}
                            bp += used;
                        } else {
                            /* Fixed bits */
                            if(bp+vb > de*8){ok=0;break;}
                            int val = get_bits(fdata, bp, vb);
                            bp += vb;
                            if(val<256) val_hist[val]++;
                        }
                        total_nz++;
                    }
                    if(ok) blocks++;
                }

                int consumed = bp - ac_start;
                float pct = 100.0 * consumed / ac_bits;

                /* Only show schemes that decode 864 blocks and consume reasonable amount */
                if(blocks >= 800){
                    printf("  %-16s: %d blks, %d/%d (%.1f%%), NZ=%d (%.1f/blk)",
                           schemes[si].name, blocks, consumed, ac_bits, pct,
                           total_nz, (float)total_nz/blocks);

                    /* Show count distribution */
                    printf("  cnt:");
                    for(int i=0;i<20;i++) if(count_hist[i]) printf("%d=%d ",i,count_hist[i]);

                    /* Check if position distribution is uniform or decaying */
                    int pos_lo=0, pos_hi=0;
                    for(int i=0;i<10;i++) pos_lo+=pos_hist[i];
                    for(int i=53;i<63;i++) pos_hi+=pos_hist[i];
                    if(total_nz>100) printf("  posLo/Hi=%d/%d", pos_lo, pos_hi);

                    printf("\n");
                }
            }
            printf("\n");
        }
    }

    /* ===== Detailed test of most promising scheme ===== */
    printf("\n=== Detailed analysis of promising schemes ===\n\n");

    /* Test each scheme on the padded frame where we know exact bit boundary */
    if(load_frame("/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin", 500)){
        int dc_vals[864];
        int ac_start = decode_dc_all(dc_vals);
        int de=fdatalen; while(de>0&&fdata[de-1]==0xFF)de--;
        int ac_bits = de*8 - ac_start;
        printf("Dense frame: AC=%d bits (%.1f/blk)\n\n", ac_bits, (float)ac_bits/864);

        /* Dump first 20 blocks with 5+6+VLC */
        printf("5+6+VLC scheme, first 20 blocks:\n");
        int bp = ac_start;
        for(int blk=0; blk<20 && bp<de*8; blk++){
            int count = get_bits(fdata, bp, 5); bp+=5;
            printf("  Block %2d (DC=%4d): count=%d", blk, dc_vals[blk], count);
            if(count>20){printf(" [SUSPICIOUS]\n");continue;}
            for(int i=0;i<count&&bp<de*8;i++){
                int pos = get_bits(fdata, bp, 6); bp+=6;
                int val;
                int used = dec_dc(fdata, bp, &val, de*8);
                if(used<0){printf(" ERR");break;}
                bp+=used;
                printf(" [%d]=%d", pos, val);
            }
            printf("\n");
        }

        /* Also try 4+6+VLC */
        printf("\n4+6+VLC scheme, first 20 blocks:\n");
        bp = ac_start;
        for(int blk=0; blk<20 && bp<de*8; blk++){
            int count = get_bits(fdata, bp, 4); bp+=4;
            printf("  Block %2d (DC=%4d): count=%d", blk, dc_vals[blk], count);
            if(count>15){printf(" [MAX]\n");continue;}
            for(int i=0;i<count&&bp<de*8;i++){
                int pos = get_bits(fdata, bp, 6); bp+=6;
                int val;
                int used = dec_dc(fdata, bp, &val, de*8);
                if(used<0){printf(" ERR");break;}
                bp+=used;
                printf(" [%d]=%d", pos, val);
            }
            printf("\n");
        }

        /* Try: unary count + 6-bit pos + DC VLC val */
        printf("\nunary+6+VLC scheme, first 20 blocks:\n");
        bp = ac_start;
        for(int blk=0; blk<20 && bp<de*8; blk++){
            int count=0;
            while(bp<de*8 && get_bit(fdata,bp)==1){count++;bp++;}
            if(bp<de*8) bp++; /* 0 terminator */
            printf("  Block %2d (DC=%4d): count=%d", blk, dc_vals[blk], count);
            if(count>20){printf(" [TOO HIGH]\n");continue;}
            for(int i=0;i<count&&bp<de*8;i++){
                int pos = get_bits(fdata, bp, 6); bp+=6;
                int val;
                int used = dec_dc(fdata, bp, &val, de*8);
                if(used<0){printf(" ERR");break;}
                bp+=used;
                printf(" [%d]=%d", pos, val);
            }
            printf("\n");
        }

        /* Try: DC VLC count + 6-bit pos + DC VLC val */
        printf("\nDCVLC_count+6pos+DCVLC_val, first 20 blocks:\n");
        bp = ac_start;
        for(int blk=0; blk<20 && bp<de*8; blk++){
            int count;
            int used = dec_dc(fdata, bp, &count, de*8);
            if(used<0){printf("  Block %d: count decode error\n",blk);break;}
            bp+=used;
            printf("  Block %2d (DC=%4d): count=%d", blk, dc_vals[blk], count);
            if(count<0||count>63){printf(" [INVALID]\n");continue;}
            for(int i=0;i<count&&bp<de*8;i++){
                int pos = get_bits(fdata, bp, 6); bp+=6;
                int val;
                used = dec_dc(fdata, bp, &val, de*8);
                if(used<0){printf(" ERR");break;}
                bp+=used;
                printf(" [%d]=%d", pos, val);
            }
            printf("\n");
        }
    }

    /* ===== Random data comparison for the best scheme ===== */
    printf("\n=== Random data comparison ===\n");
    /* Generate random data and test 5+6+VLC scheme */
    {
        srand(12345);
        uint8_t rnd[16384];
        for(int i=0;i<16384;i++) rnd[i]=rand()&0xFF;
        int bp=0, tb=92097;
        int blocks=0, total_nz=0;
        int count_hist[64]={0};
        for(int blk=0;blk<864&&bp<tb;blk++){
            int count=get_bits(rnd,bp,5);bp+=5;
            if(count>63)count=63;
            count_hist[count]++;
            for(int i=0;i<count&&bp<tb;i++){
                int pos=get_bits(rnd,bp,6);bp+=6;
                int val;
                int used=dec_dc(rnd,bp,&val,tb);
                if(used<0){bp++;continue;}
                bp+=used;
                total_nz++;
            }
            blocks++;
        }
        printf("RANDOM 5+6+VLC: %d blks, %d/%d (%.1f%%), NZ=%d (%.1f/blk)\n",
               blocks, bp, tb, 100.0*bp/tb, total_nz, (float)total_nz/blocks);
        printf("  cnt: ");
        for(int i=0;i<32;i++) if(count_hist[i]) printf("%d=%d ",i,count_hist[i]);
        printf("\n");

        /* 4+6+VLC on random */
        bp=0; blocks=0; total_nz=0;
        memset(count_hist,0,sizeof(count_hist));
        for(int blk=0;blk<864&&bp<tb;blk++){
            int count=get_bits(rnd,bp,4);bp+=4;
            count_hist[count]++;
            for(int i=0;i<count&&bp<tb;i++){
                int pos=get_bits(rnd,bp,6);bp+=6;
                int val;
                int used=dec_dc(rnd,bp,&val,tb);
                if(used<0){bp++;continue;}
                bp+=used;
                total_nz++;
            }
            blocks++;
        }
        printf("RANDOM 4+6+VLC: %d blks, %d/%d (%.1f%%), NZ=%d (%.1f/blk)\n",
               blocks, bp, tb, 100.0*bp/tb, total_nz, (float)total_nz/blocks);
        printf("  cnt: ");
        for(int i=0;i<16;i++) if(count_hist[i]) printf("%d=%d ",i,count_hist[i]);
        printf("\n");
    }

    printf("\nDone.\n");
    return 0;
}
