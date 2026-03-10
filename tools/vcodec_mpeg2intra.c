/*
 * vcodec_mpeg2intra.c - Test MPEG-2 Intra AC VLC (Table B.15)
 *
 * MPEG-2 has TWO AC coefficient VLC tables:
 * - Table B.14 = same as MPEG-1 (EOB="10", already tested and ruled out)
 * - Table B.15 = different table for INTRA blocks (EOB="0110")
 *
 * Key differences from MPEG-1:
 * - EOB = "0110" (4 bits) vs "10" (2 bits)
 * - Most common coeff (0,1) = "10" (2 bits + sign)
 * - Different code assignments for other run/level pairs
 * - Escape = "000001" (6 bits) + 6-bit run + 12-bit level (signed)
 *
 * Also test with the first coefficient special case (MPEG-1 has this).
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

/* DC VLC table */
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

/* MPEG-2 Table B.15 - DCT coefficient table one (intra) */
/* Format: {code_bits, code_len, run, level} */
/* Sign bit is read separately after the code */
typedef struct { uint32_t code; int len; int run; int level; } VLCEntry;

static const VLCEntry mpeg2_b15[] = {
    /* EOB = special, handled separately */
    /* 2-bit codes */
    {0x2, 2, 0, 1},   /* 10 → (0,1) */
    /* 3-bit codes */
    {0x2, 3, 1, 1},   /* 010 → (1,1) */
    {0x6, 3, 0, 2},   /* 110 → (0,2) */
    /* 5-bit codes */
    {0x04, 5, 2, 1},  /* 00100 → (2,1) */
    {0x05, 5, 0, 3},  /* 00101 → (0,3) */
    {0x06, 5, 3, 1},  /* 00110 → (3,1) */
    {0x07, 5, 4, 1},  /* 00111 → (4,1) */
    /* 6-bit codes */
    {0x04, 6, 1, 2},  /* 000100 → (1,2) */
    {0x05, 6, 5, 1},  /* 000101 → (5,1) */
    {0x06, 6, 6, 1},  /* 000110 → (6,1) */
    {0x07, 6, 7, 1},  /* 000111 → (7,1) */
    /* 7-bit codes */
    {0x04, 7, 0, 4},  /* 0000100 → (0,4) */
    {0x05, 7, 2, 2},  /* 0000101 → (2,2) */
    {0x06, 7, 8, 1},  /* 0000110 → (8,1) */
    {0x07, 7, 9, 1},  /* 0000111 → (9,1) */
    /* 8-bit codes */
    {0x20, 8, 0, 5},   /* 00000100 0 → (0,5) -- wait, 8-bit with trailing s */
    /* Actually, the codes in Table B.15 include the sign bit position differently */
    /* Let me re-read the table more carefully */
    /* For 8-bit base codes: */
    {0x08, 8, 0, 5},  /* 0000 0100 0 → 9 bits with sign, base = 00000100 */
    {0x09, 8, 10, 1}, /* 0000 0100 1 */
    {0x0A, 8, 0, 6},  /* 0000 0101 0 */
    {0x0B, 8, 11, 1}, /* 0000 0101 1 */
    {0x0C, 8, 12, 1}, /* 0000 0110 0 */
    {0x0D, 8, 13, 1}, /* 0000 0110 1 */
    {0x0E, 8, 0, 7},  /* 0000 0111 0 */
    {0x0F, 8, 1, 3},  /* 0000 0111 1 */
    /* 12-bit codes */
    {0x010, 12, 3, 2},  /* 0000 0001 0000 */
    {0x011, 12, 14, 1}, /* 0000 0001 0001 */
    {0x012, 12, 0, 8},  /* 0000 0001 0010 */
    {0x013, 12, 15, 1}, /* 0000 0001 0011 */
    {0x014, 12, 0, 9},  /* 0000 0001 0100 */
    {0x015, 12, 0, 10}, /* 0000 0001 0101 */
    {0x016, 12, 16, 1}, /* 0000 0001 0110 */
    {0x017, 12, 5, 2},  /* 0000 0001 0111 */
    {0x018, 12, 4, 2},  /* 0000 0001 1000 */
    {0x019, 12, 0, 11}, /* 0000 0001 1001 */
    {0x01A, 12, 0, 12}, /* 0000 0001 1010 */
    {0x01B, 12, 0, 13}, /* 0000 0001 1011 */
    {0x01C, 12, 0, 14}, /* 0000 0001 1100 */
    {0x01D, 12, 0, 15}, /* 0000 0001 1101 */
    {0x01E, 12, 1, 4},  /* 0000 0001 1110 */
    {0x01F, 12, 2, 3},  /* 0000 0001 1111 */
    /* 13-bit codes */
    {0x010, 13, 17, 1},  /* 0000 0000 1000 0 */
    {0x011, 13, 18, 1},
    {0x012, 13, 0, 16},
    {0x013, 13, 0, 17},
    {0x014, 13, 6, 2},
    {0x015, 13, 19, 1},
    {0x016, 13, 20, 1},
    {0x017, 13, 0, 18},
    /* More 13-bit codes... */
    {0x018, 13, 21, 1},
    {0x019, 13, 7, 2},
    {0x01A, 13, 22, 1},
    {0x01B, 13, 0, 19},
    {0x01C, 13, 23, 1},
    {0x01D, 13, 24, 1},
    {0x01E, 13, 3, 3},
    {0x01F, 13, 0, 20},
};

#define N_B15 (sizeof(mpeg2_b15)/sizeof(mpeg2_b15[0]))

/* Decode one AC coefficient using MPEG-2 Table B.15 */
/* Returns bits consumed, or -1 for error, or -2 for EOB */
static int dec_ac_b15(const uint8_t *d, int bp, int *run, int *level, int tb) {
    /* Check EOB first: "0110" */
    if(bp+4 <= tb && get_bits(d,bp,4) == 0x6) {
        return -2; /* EOB */
    }

    /* Check escape: "000001" */
    if(bp+6 <= tb && get_bits(d,bp,6) == 0x01) {
        /* Escape: 6-bit run + 12-bit level (signed, 2's complement) */
        if(bp+6+6+12 > tb) return -1;
        *run = get_bits(d, bp+6, 6);
        int lev = get_bits(d, bp+12, 12);
        if(lev >= 2048) lev -= 4096; /* sign extend */
        *level = lev;
        return 6+6+12; /* 24 bits total */
    }

    /* Try all table entries */
    for(int i=0; i<(int)N_B15; i++) {
        int len = mpeg2_b15[i].len;
        if(bp+len+1 > tb) continue; /* +1 for sign bit */
        uint32_t bits = get_bits(d, bp, len);
        if(bits == mpeg2_b15[i].code) {
            *run = mpeg2_b15[i].run;
            *level = mpeg2_b15[i].level;
            /* Read sign bit */
            int sign = get_bit(d, bp+len);
            if(sign) *level = -(*level);
            return len + 1;
        }
    }
    return -1; /* no match */
}

/* MPEG-1 Table B.14 for comparison */
typedef struct { uint32_t code; int len; int run; int level; } VLC1Entry;
static const VLC1Entry mpeg1_b14[] = {
    /* EOB handled separately: "10" */
    {0x3, 2, 0, 1},   /* 1s → (0,1) -- NOTE: first coeff in block "1s" is special */
    {0x3, 3, 1, 1},   /* 011s */
    {0x4, 4, 0, 2},   /* 0100s */
    {0x5, 4, 2, 1},   /* 0101s */
    {0x2, 5, 0, 3},   /* 00100s */
    {0x3, 5, 3, 1},   /* 00101s */
    {0x6, 5, 4, 1},   /* 00110s */
    {0x7, 5, 1, 2},   /* 00111s */
    {0x4, 6, 5, 1},   /* 000100s */
    {0x5, 6, 6, 1},   /* 000101s */
    {0x6, 6, 7, 1},   /* 000110s */
    {0x7, 6, 0, 4},   /* 000111s */
    {0x4, 7, 2, 2},   /* 0000100s */
    {0x5, 7, 8, 1},   /* 0000101s */
    {0x6, 7, 9, 1},   /* 0000110s */
    {0x7, 7, 0, 5},   /* 0000111s */
    {0x20, 8, 0, 6},  /* 00100000s */  /* Actually this is wrong... let me just be more careful */
};

/* Actually let me just implement the B.14 escape + EOB and a few short codes */
static int dec_ac_b14(const uint8_t *d, int bp, int *run, int *level, int tb) {
    /* EOB: "10" */
    if(bp+2<=tb && get_bits(d,bp,2)==0x2) return -2;

    /* First coeff "1s" → (0,1) */
    if(bp+2<=tb && get_bit(d,bp)==1) {
        *run=0; *level=1;
        if(get_bit(d,bp+1)) *level=-1;
        return 2;
    }

    /* "011s" → (1,1) */
    if(bp+4<=tb && get_bits(d,bp,3)==0x3) {
        *run=1; *level=1;
        if(get_bit(d,bp+3)) *level=-1;
        return 4;
    }

    /* "0100s" → (0,2) */
    if(bp+5<=tb && get_bits(d,bp,4)==0x4) {
        *run=0; *level=2;
        if(get_bit(d,bp+4)) *level=-2;
        return 5;
    }

    /* "0101s" → (2,1) */
    if(bp+5<=tb && get_bits(d,bp,4)==0x5) {
        *run=2; *level=1;
        if(get_bit(d,bp+4)) *level=-1;
        return 5;
    }

    /* Escape "000001" + 6-bit run + 8-bit level */
    if(bp+20<=tb && get_bits(d,bp,6)==0x01) {
        *run = get_bits(d,bp+6,6);
        int lev = get_bits(d,bp+12,8);
        if(lev==0){
            /* Extended: next 8 bits unsigned */
            if(bp+26>tb) return -1;
            *level = get_bits(d,bp+20,8);
            return 26;
        } else if(lev>=128) {
            *level = lev - 256;
        } else {
            *level = lev;
        }
        return 20;
    }

    /* More codes... simplified for now */
    /* "00100s" → (0,3) */
    if(bp+6<=tb && get_bits(d,bp,5)==0x04) {
        *run=0; *level=3;
        if(get_bit(d,bp+5)) *level=-3;
        return 6;
    }
    /* "00101s" → (3,1) */
    if(bp+6<=tb && get_bits(d,bp,5)==0x05) {
        *run=3; *level=1;
        if(get_bit(d,bp+5)) *level=-1;
        return 6;
    }
    /* "00110s" → (4,1) */
    if(bp+6<=tb && get_bits(d,bp,5)==0x06) {
        *run=4; *level=1;
        if(get_bit(d,bp+5)) *level=-1;
        return 6;
    }
    /* "00111s" → (1,2) */
    if(bp+6<=tb && get_bits(d,bp,5)==0x07) {
        *run=1; *level=2;
        if(get_bit(d,bp+5)) *level=-2;
        return 6;
    }

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

int main() {
    const char *games[] = {
        "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Ie Naki Ko - Suzu no Sentaku (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku - Chikyuu-hen (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Aqua Adventure - Blue Lilty (Japan) (Track 2).bin",
        NULL
    };
    int test_lbas[] = {148, 500, 755, 1100};

    printf("=== MPEG-2 Table B.15 (Intra AC VLC) Test ===\n\n");

    for(int gi=0; games[gi]; gi++){
        const char *gn=strrchr(games[gi],'/');
        if(gn) gn++; else gn=games[gi];

        for(int li=0; li<4; li++){
            if(!load_frame(games[gi], test_lbas[li])) continue;
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
                    int used=dec_dc(fdata,bp,&dv,de*8);
                    if(used<0){ok=0;break;}
                    dc_pred[comp]+=dv;
                    bp+=used;
                }
            }
            if(!ok) continue;

            int ac_start=bp;
            int ac_bits=de*8-ac_start;

            /* Test MPEG-2 Table B.15 */
            {
                bp=ac_start;
                int blocks=0,nz=0,eobs=0,errors=0,escapes=0;
                int band_nz[64]={0};
                int rl_hist[64]={0}; /* run histogram */
                int lv_hist[256]={0}; /* level histogram (offset by 128) */

                for(int blk=0;blk<864&&bp<de*8;blk++){
                    int pos=0;
                    while(pos<64 && bp<de*8){
                        int run,level;
                        int used=dec_ac_b15(fdata,bp,&run,&level,de*8);
                        if(used==-2){eobs++;bp+=4;break;} /* EOB */
                        if(used<0){errors++;bp++;break;}
                        bp+=used;
                        /* Check if escape */
                        if(used==24) escapes++;
                        pos+=run;
                        if(pos>=63){break;}
                        pos++;
                        if(level!=0){nz++;if(pos<64)band_nz[pos]++;
                            if(run<64)rl_hist[run]++;
                            int lvi=level+128;if(lvi>=0&&lvi<256)lv_hist[lvi]++;
                        }
                    }
                    blocks++;
                }

                int consumed=bp-ac_start;
                printf("%-55s LBA~%4d QS=%2d t=%d:\n", gn, test_lbas[li], qs, ft);
                printf("  B.15: %d blks, %d/%d (%.1f%%), NZ=%d EOB=%d err=%d esc=%d\n",
                       blocks, consumed, ac_bits, 100.0*consumed/ac_bits,
                       nz, eobs, errors, escapes);
                printf("    Bands: ");
                for(int i=1;i<16;i++) printf("%d ",band_nz[i]);
                printf("\n    Runs: ");
                for(int i=0;i<20;i++) if(rl_hist[i]) printf("r%d=%d ",i,rl_hist[i]);
                printf("\n    Levels: ");
                for(int i=120;i<136;i++) if(lv_hist[i]) printf("l%d=%d ",i-128,lv_hist[i]);
                printf("\n");
            }

            /* Test MPEG-1 Table B.14 (partial implementation) for comparison */
            {
                bp=ac_start;
                int blocks=0,nz=0,eobs=0,errors=0;
                int band_nz[64]={0};

                for(int blk=0;blk<864&&bp<de*8;blk++){
                    int pos=0;
                    while(pos<64 && bp<de*8){
                        int run,level;
                        int used=dec_ac_b14(fdata,bp,&run,&level,de*8);
                        if(used==-2){eobs++;bp+=2;break;}
                        if(used<0){errors++;bp++;break;}
                        bp+=used;
                        pos+=run;
                        if(pos>=63)break;
                        pos++;
                        if(level!=0){nz++;if(pos<64)band_nz[pos]++;}
                    }
                    blocks++;
                }

                printf("  B.14: %d blks, %d/%d (%.1f%%), NZ=%d EOB=%d err=%d\n",
                       blocks, bp-ac_start, ac_bits,
                       100.0*(bp-ac_start)/ac_bits, nz, eobs, errors);
                printf("    Bands: ");
                for(int i=1;i<16;i++) printf("%d ",band_nz[i]);
                printf("\n");
            }
            printf("\n");
        }
    }

    /* Random data comparison */
    printf("=== Random data comparison ===\n");
    {
        srand(42);
        uint8_t rnd[16384];
        for(int i=0;i<16384;i++) rnd[i]=rand()&0xFF;
        int tb=92097;

        /* B.15 on random */
        int bp=0,blocks=0,nz=0,eobs=0,errors=0,escapes=0;
        int band_nz[64]={0};
        for(int blk=0;blk<864&&bp<tb;blk++){
            int pos=0;
            while(pos<64 && bp<tb){
                int run,level;
                int used=dec_ac_b15(rnd,bp,&run,&level,tb);
                if(used==-2){eobs++;bp+=4;break;}
                if(used<0){errors++;bp++;break;}
                bp+=used;
                if(used==24)escapes++;
                pos+=run; if(pos>=63)break; pos++;
                if(level!=0){nz++;if(pos<64)band_nz[pos]++;}
            }
            blocks++;
        }
        printf("RANDOM B.15: %d blks, %d/%d (%.1f%%), NZ=%d EOB=%d err=%d esc=%d\n",
               blocks, bp, tb, 100.0*bp/tb, nz, eobs, errors, escapes);
        printf("  Bands: ");
        for(int i=1;i<16;i++) printf("%d ",band_nz[i]);
        printf("\n");

        /* B.14 on random */
        bp=0;blocks=0;nz=0;eobs=0;errors=0;
        memset(band_nz,0,sizeof(band_nz));
        for(int blk=0;blk<864&&bp<tb;blk++){
            int pos=0;
            while(pos<64 && bp<tb){
                int run,level;
                int used=dec_ac_b14(rnd,bp,&run,&level,tb);
                if(used==-2){eobs++;bp+=2;break;}
                if(used<0){errors++;bp++;break;}
                bp+=used;
                pos+=run;if(pos>=63)break;pos++;
                if(level!=0){nz++;if(pos<64)band_nz[pos]++;}
            }
            blocks++;
        }
        printf("RANDOM B.14: %d blks, %d/%d (%.1f%%), NZ=%d EOB=%d err=%d\n",
               blocks, bp, tb, 100.0*bp/tb, nz, eobs, errors);
        printf("  Bands: ");
        for(int i=1;i<16;i++) printf("%d ",band_nz[i]);
        printf("\n");
    }

    printf("\nDone.\n");
    return 0;
}
