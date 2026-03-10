/*
 * vcodec_prefix.c - Systematic test of simple prefix-code structures for AC
 *
 * Test all possible 1-3 bit prefix assignments for (zero, nonzero_value, EOB).
 * For each assignment, decode all 864 blocks and check consumption percentage.
 * The correct scheme should consume ~100% for unpadded frames.
 *
 * Nonzero values are coded using DC VLC (size + magnitude).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

static int dec_vlc(const uint8_t *d, int bp, int *val, int tb) {
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

/* Prefix code structure */
typedef struct {
    const char *name;
    /* For each symbol type, the prefix code bits and length */
    uint32_t zero_code; int zero_len;    /* zero coefficient */
    uint32_t eob_code;  int eob_len;     /* end of block */
    uint32_t nz_code;   int nz_len;      /* nonzero (followed by DC VLC) */
} PrefixScheme;

static int test_scheme(const PrefixScheme *s, int ac_start, int ac_end,
                       int *out_blocks, int *out_nz, int *out_eobs, int *out_consumed)
{
    int bp = ac_start;
    int blocks=0, nz=0, eobs=0;

    for(int blk=0; blk<864 && bp<ac_end; blk++){
        int pos=0;
        while(pos<63 && bp<ac_end){
            /* Try to match each prefix */
            int matched=0;

            /* Check zero */
            if(bp+s->zero_len <= ac_end){
                uint32_t bits = get_bits(fdata, bp, s->zero_len);
                if(bits == s->zero_code){
                    bp += s->zero_len;
                    pos++;
                    matched=1;
                }
            }

            if(!matched && bp+s->eob_len <= ac_end){
                uint32_t bits = get_bits(fdata, bp, s->eob_len);
                if(bits == s->eob_code){
                    bp += s->eob_len;
                    eobs++;
                    matched=2;
                }
            }

            if(!matched && bp+s->nz_len <= ac_end){
                uint32_t bits = get_bits(fdata, bp, s->nz_len);
                if(bits == s->nz_code){
                    bp += s->nz_len;
                    /* Read value using DC VLC */
                    int val;
                    int used = dec_vlc(fdata, bp, &val, ac_end);
                    if(used < 0) return -1;
                    bp += used;
                    nz++;
                    pos++;
                    matched=3;
                }
            }

            if(!matched) return -1; /* no prefix matches */
            if(matched==2) break; /* EOB */
        }
        blocks++;
    }

    *out_blocks = blocks;
    *out_nz = nz;
    *out_eobs = eobs;
    *out_consumed = bp - ac_start;
    return 0;
}

int main() {
    const char *binfile = "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin";

    /* Define all possible 1-3 bit prefix schemes */
    /* Must be prefix-free (no code is prefix of another) */
    PrefixScheme schemes[] = {
        /* 1-bit zero, 2-bit EOB/NZ */
        {"0=Z 10=EOB 11=NZ",  0,1, 2,2, 3,2},
        {"0=Z 10=NZ  11=EOB", 0,1, 3,2, 2,2},
        {"1=Z 00=EOB 01=NZ",  1,1, 0,2, 1,2},
        {"1=Z 00=NZ  01=EOB", 1,1, 1,2, 0,2},
        /* 1-bit NZ, 2-bit Z/EOB */
        {"0=NZ 10=Z   11=EOB", 0,1, 3,2, 0,1},  /* Wait, NZ=0 len=1, but we need Z and EOB too */
        /* Actually let me be more systematic. Prefix-free codes on {Z, EOB, NZ}: */
        /* With code lengths (1,2,2): one symbol gets 1 bit, two get 2 bits starting with the other bit */
        /* Z=0, EOB=10, NZ=11 */
        /* Z=0, EOB=11, NZ=10 */
        /* Z=1, EOB=00, NZ=01 */
        /* Z=1, EOB=01, NZ=00 */
        /* EOB=0, Z=10, NZ=11 */
        {"0=EOB 10=Z  11=NZ",  2,2, 0,1, 3,2},
        /* EOB=0, Z=11, NZ=10 */
        {"0=EOB 11=Z  10=NZ",  3,2, 0,1, 2,2},
        /* EOB=1, Z=00, NZ=01 */
        {"1=EOB 00=Z  01=NZ",  0,2, 1,1, 1,2},
        /* EOB=1, Z=01, NZ=00 */
        {"1=EOB 01=Z  00=NZ",  1,2, 1,1, 0,2},
        /* NZ=0, Z=10, EOB=11 */
        {"0=NZ  10=Z  11=EOB", 2,2, 3,2, 0,1},
        /* NZ=0, Z=11, EOB=10 */
        {"0=NZ  11=Z  10=EOB", 3,2, 2,2, 0,1},
        /* NZ=1, Z=00, EOB=01 */
        {"1=NZ  00=Z  01=EOB", 0,2, 1,2, 1,1},
        /* NZ=1, Z=01, EOB=00 */
        {"1=NZ  01=Z  00=EOB", 1,2, 0,2, 1,1},

        /* (1,3,3) codes: one symbol 1 bit, two get 3 bits */
        /* Z=0, EOB=100, NZ=101 */
        {"0=Z 100=EOB 101=NZ", 0,1, 4,3, 5,3},
        /* Z=0, EOB=110, NZ=111 */
        {"0=Z 110=EOB 111=NZ", 0,1, 6,3, 7,3},
        /* Z=0, EOB=101, NZ=100 */
        {"0=Z 101=EOB 100=NZ", 0,1, 5,3, 4,3},
        /* Z=0, EOB=111, NZ=110 */
        {"0=Z 111=EOB 110=NZ", 0,1, 7,3, 6,3},
        /* Z=1, EOB=000, NZ=001 */
        {"1=Z 000=EOB 001=NZ", 1,1, 0,3, 1,3},
        /* Z=1, EOB=010, NZ=011 */
        {"1=Z 010=EOB 011=NZ", 1,1, 2,3, 3,3},
        /* Z=1, EOB=001, NZ=000 */
        {"1=Z 001=EOB 000=NZ", 1,1, 1,3, 0,3},
        /* Z=1, EOB=011, NZ=010 */
        {"1=Z 011=EOB 010=NZ", 1,1, 3,3, 2,3},

        /* (2,2,2) codes: all 2 bits — but can't have 3 symbols in 2 bits prefix-free! */
        /* Need at least 1-bit for one of them. Skip this. */

        /* (2,3,3) codes */
        /* Z=00, EOB=010, NZ=011 */
        {"00=Z 010=EOB 011=NZ", 0,2, 2,3, 3,3},
        /* Z=00, EOB=011, NZ=010 */
        {"00=Z 011=EOB 010=NZ", 0,2, 3,3, 2,3},
        /* Z=01, EOB=000, NZ=001 */
        {"01=Z 000=EOB 001=NZ", 1,2, 0,3, 1,3},
        /* Z=10, EOB=110, NZ=111 */
        {"10=Z 110=EOB 111=NZ", 2,2, 6,3, 7,3},
        /* Z=11, EOB=100, NZ=101 */
        {"11=Z 100=EOB 101=NZ", 3,2, 4,3, 5,3},

        /* EOB=00, Z=01, NZ=1 */
        {"01=Z 00=EOB 1=NZ",   1,2, 0,2, 1,1},

        /* Some with value=DC VLC directly (size=0 for zero, special for EOB) */
        /* Actually, what if there's NO separate zero/nz flag, just DC VLC where size=0=EOB? */
        /* Already tested. Let's also try: DC VLC where size=0=zero (skip), and EOB is special */

        {NULL,0,0,0,0,0,0}
    };

    printf("=== Systematic Prefix Code Testing ===\n\n");

    /* Test on multiple frames */
    int lbas[] = {148, 500, 755, 1100};
    const char *lba_names[] = {"148(logo)", "500(scene)", "755(scene)", "1100(scene)"};

    for(int li=0; li<4; li++){
        if(!load_frame(binfile, lbas[li])) continue;
        int qs=fdata[3], ft=fdata[39];
        int de=fdatalen; while(de>0&&fdata[de-1]==0xFF)de--;
        int pad=fdatalen-de;

        /* Decode DC */
        int bp=40*8;
        int dc_pred[3]={0,0,0};
        int ok=1;
        for(int mb=0;mb<144&&ok;mb++){
            for(int bl=0;bl<6&&ok;bl++){
                int comp=(bl<4)?0:(bl==4)?1:2;
                int dv;
                int used=dec_vlc(fdata,bp,&dv,de*8);
                if(used<0){ok=0;break;}
                dc_pred[comp]+=dv;
                bp+=used;
            }
        }
        if(!ok) continue;
        int ac_start=bp;
        int ac_bits=de*8-ac_start;

        printf("--- %s QS=%d type=%d pad=%d AC=%d (%.1f/blk) ---\n",
               lba_names[li], qs, ft, pad, ac_bits, (float)ac_bits/864);

        for(int si=0; schemes[si].name; si++){
            int blocks,nz,eobs,consumed;
            int ret = test_scheme(&schemes[si], ac_start, de*8, &blocks, &nz, &eobs, &consumed);
            float pct = 100.0 * consumed / ac_bits;
            /* Only show schemes that decode 864 blocks and consume 90-110% */
            if(ret==0 && blocks==864 && pct>85 && pct<115){
                printf("  *** %-28s: %d blk %5d/%5d (%.1f%%) NZ=%d EOB=%d avg=%.1f\n",
                       schemes[si].name, blocks, consumed, ac_bits, pct, nz, eobs,
                       (float)nz/864);
            }
            /* Also show schemes with 864 blocks regardless of consumption for debug */
            else if(ret==0 && blocks==864 && (pct>25 && pct<200)){
                /* Show only first time */
                if(li==0)
                    printf("  %-28s: %d blk %5d/%5d (%.1f%%) NZ=%d EOB=%d\n",
                           schemes[si].name, blocks, consumed, ac_bits, pct, nz, eobs);
            }
        }
        printf("\n");
    }

    /* Also test on padded frames from Ie Naki Ko (sparse) */
    printf("=== Sparse frame test ===\n");
    const char *ienaki = "/home/wizzard/share/GitHub/playdia-roms/Ie Naki Ko - Suzu no Sentaku (Japan) (Track 2).bin";
    /* Find most padded frame */
    {
        FILE *fp=fopen(ienaki,"rb");
        if(fp){
            int f1c=0, best_pad=0;
            uint8_t tmpf[16384], best[16384];
            for(int s=0;s<20000;s++){
                long off=(long)(148+s)*2352;
                uint8_t sec[2352]; fseek(fp,off,SEEK_SET);
                if(fread(sec,1,2352,fp)!=2352)break;
                if(sec[24]==0xF1){if(f1c<6)memcpy(tmpf+f1c*2047,sec+25,2047);f1c++;}
                else if(sec[24]==0xF2){
                    if(f1c==6&&tmpf[0]==0x00&&tmpf[1]==0x80&&tmpf[2]==0x04){
                        int fl=6*2047,de2=fl;while(de2>0&&tmpf[de2-1]==0xFF)de2--;
                        int p=fl-de2;
                        if(p>best_pad){best_pad=p;memcpy(best,tmpf,fl);}
                    }
                    f1c=0;
                } else f1c=0;
            }
            fclose(fp);

            if(best_pad>100){
                memcpy(fdata,best,6*2047);fdatalen=6*2047;
                int de=fdatalen;while(de>0&&fdata[de-1]==0xFF)de--;
                int qs=fdata[3],ft=fdata[39];

                int bp=40*8;
                int dc_pred[3]={0,0,0};
                int ok2=1;
                for(int mb=0;mb<144&&ok2;mb++)
                    for(int bl=0;bl<6&&ok2;bl++){
                        int comp=(bl<4)?0:(bl==4)?1:2;
                        int dv;int used=dec_vlc(fdata,bp,&dv,de*8);
                        if(used<0){ok2=0;break;}dc_pred[comp]+=dv;bp+=used;
                    }
                if(ok2){
                    int ac_start=bp,ac_bits=de*8-ac_start;
                    printf("Sparse: QS=%d type=%d pad=%d AC=%d (%.1f/blk)\n\n",
                           qs,ft,best_pad,ac_bits,(float)ac_bits/864);
                    for(int si=0;schemes[si].name;si++){
                        int blocks,nz,eobs,consumed;
                        int ret=test_scheme(&schemes[si],ac_start,de*8,&blocks,&nz,&eobs,&consumed);
                        float pct=100.0*consumed/ac_bits;
                        if(ret==0 && blocks==864 && pct>85 && pct<115){
                            printf("  *** %-28s: %d blk %5d/%5d (%.1f%%) NZ=%d EOB=%d avg=%.1f\n",
                                   schemes[si].name,blocks,consumed,ac_bits,pct,nz,eobs,(float)nz/864);
                        }
                    }
                }
            }
        }
    }

    /* Random data comparison for schemes that passed */
    printf("\n=== Random data comparison ===\n");
    {
        srand(42);
        uint8_t rnd[16384];
        for(int i=0;i<16384;i++) rnd[i]=rand()&0xFF;
        memcpy(fdata,rnd,16384);
        fdatalen=12282;
        int ac_start=0, ac_end=92097;

        for(int si=0; schemes[si].name; si++){
            int blocks,nz,eobs,consumed;
            int ret=test_scheme(&schemes[si],ac_start,ac_end,&blocks,&nz,&eobs,&consumed);
            float pct=100.0*consumed/ac_end;
            if(ret==0 && blocks==864){
                printf("  %-28s: RAND %d blk %5d/%5d (%.1f%%) NZ=%d EOB=%d\n",
                       schemes[si].name,blocks,consumed,ac_end,pct,nz,eobs);
            }
        }
    }

    printf("\nDone.\n");
    return 0;
}
