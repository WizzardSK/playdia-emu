/*
 * vcodec_multigame.c - Cross-game AC bitstream analysis
 * Uses full ROM collection to validate codec properties
 *
 * Key tests:
 * 1. First AC bits pattern across games/frames
 * 2. Reversed unary run (1s→0) WITHOUT EOB + various level codings
 * 3. Simple 2-state coding: 0=zero-coeff, 1=non-zero-coeff+value
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static uint8_t fbuf[16384];
static int flen;

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

static int load_frame(const char *binfile, int start_lba, int target) {
    FILE *fp=fopen(binfile,"rb"); if(!fp)return-1;
    int fc=0,f1c=0; flen=0;
    for(int s=0;s<500;s++){
        long off=(long)(start_lba+s)*2352;
        uint8_t sec[2352]; fseek(fp,off,SEEK_SET);
        if(fread(sec,1,2352,fp)!=2352)break;
        uint8_t t=sec[24];
        if(t==0xF1){if(fc==target&&f1c<6)memcpy(fbuf+f1c*2047,sec+25,2047);f1c++;}
        else if(t==0xF2){if(fc==target&&f1c==6){flen=6*2047;fclose(fp);return 0;}fc++;f1c=0;}
        else if(t==0xF3){f1c=0;}
    }
    fclose(fp); return -1;
}

/* Test: reversed unary run + N-bit sign-magnitude level, NO EOB */
/* Block ends when 63 positions consumed. Run past end = padding. */
static void test_runary_signmag(const uint8_t *ac, int abits, int nblk, int lbits, const char *lbl) {
    int bp=0, tnz=0, bands[8]={0}, bok=0;
    for(int b=0;b<nblk&&bp<abits;b++){
        int pos=0,bnz=0;
        while(pos<63&&bp<abits){
            /* Reversed unary: count 1-bits → run length */
            int run=0;
            while(bp<abits&&get_bit(ac,bp)==1){run++;bp++;if(run>63)break;}
            if(bp>=abits)break;
            bp++; /* skip terminating 0 */
            pos+=run;
            if(pos>=63)break;
            /* Non-zero: read sign-magnitude */
            if(bp+lbits>abits)break;
            /* int val = get_bits(ac,bp,lbits); */
            bp+=lbits;
            bnz++;
            int band=pos/8; if(band<8)bands[band]++;
            pos++;
        }
        tnz+=bnz; bok++;
    }
    double pct=100.0*bp/abits;
    printf("  RU+SM(%d) %s: %.1f%%, blk=%d, NZ=%d, bands: %d %d %d %d %d %d %d %d\n",
           lbits,lbl,pct,bok,tnz,bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
}

/* Test: reversed unary run + DC VLC level, NO EOB */
static void test_runary_dcvlc(const uint8_t *ac, int abits, int nblk, const char *lbl) {
    int bp=0, tnz=0, bands[8]={0}, bok=0, errs=0;
    for(int b=0;b<nblk&&bp<abits;b++){
        int pos=0,bnz=0;
        while(pos<63&&bp<abits){
            int run=0;
            while(bp<abits&&get_bit(ac,bp)==1){run++;bp++;if(run>63)break;}
            if(bp>=abits||run>63)break;
            bp++;
            pos+=run;
            if(pos>=63)break;
            int val;
            int c=dec_dc(ac,bp,&val,abits);
            if(c<0){errs++;break;}
            bp+=c;
            bnz++;
            int band=pos/8; if(band<8)bands[band]++;
            pos++;
        }
        tnz+=bnz; bok++;
    }
    double pct=100.0*bp/abits;
    printf("  RU+VLC %s: %.1f%%, blk=%d, NZ=%d, err=%d, bands: %d %d %d %d %d %d %d %d\n",
           lbl,pct,bok,tnz,errs,bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
}

/* Test: 0=zero, 1+sign(1)+unary_magnitude → non-zero */
/* Magnitude: count 0s until 1 = magnitude exponent, then read that many bits */
static void test_flag_expgolomb(const uint8_t *ac, int abits, int nblk, const char *lbl) {
    int bp=0, tnz=0, bands[8]={0}, bok=0;
    for(int b=0;b<nblk&&bp<abits;b++){
        int bnz=0;
        for(int pos=0;pos<63&&bp<abits;pos++){
            int flag=get_bit(ac,bp); bp++;
            if(flag==0) continue; /* zero coefficient */
            /* Non-zero: read signed exp-golomb value */
            int leading=0;
            while(bp<abits&&get_bit(ac,bp)==0){leading++;bp++;if(leading>16)break;}
            if(bp>=abits||leading>16)break;
            bp++; /* skip 1 */
            if(bp+leading>abits)break;
            bp+=leading; /* suffix bits */
            /* sign bit */
            if(bp>=abits)break;
            bp++; /* sign */
            bnz++;
            int band=pos/8; if(band<8)bands[band]++;
        }
        tnz+=bnz; bok++;
    }
    double pct=100.0*bp/abits;
    printf("  Flag+SEG %s: %.1f%%, blk=%d, NZ=%d, bands: %d %d %d %d %d %d %d %d\n",
           lbl,pct,bok,tnz,bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
}

/* Test: per-coefficient signed exp-golomb (no flag, no run) */
/* 0→0, then unsigned EG gives magnitude, sign bit gives sign */
static void test_per_coeff_seg(const uint8_t *ac, int abits, int nblk, const char *lbl) {
    int bp=0, tnz=0, bands[8]={0}, bok=0;
    for(int b=0;b<nblk&&bp<abits;b++){
        int bnz=0;
        for(int pos=0;pos<63&&bp<abits;pos++){
            /* Unsigned exp-golomb */
            int leading=0;
            while(bp<abits&&get_bit(ac,bp)==0){leading++;bp++;if(leading>16)break;}
            if(bp>=abits||leading>16)break;
            bp++; /* skip 1 */
            if(bp+leading>abits)break;
            uint32_t suffix=0;
            if(leading>0) suffix=get_bits(ac,bp,leading);
            bp+=leading;
            int mag=(1<<leading)-1+suffix;
            if(mag==0) continue; /* zero */
            /* sign bit */
            if(bp>=abits)break;
            bp++;
            bnz++;
            int band=pos/8; if(band<8)bands[band]++;
        }
        tnz+=bnz; bok++;
    }
    double pct=100.0*bp/abits;
    printf("  PerCoeff-SEG %s: %.1f%%, blk=%d, NZ=%d, bands: %d %d %d %d %d %d %d %d\n",
           lbl,pct,bok,tnz,bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
}

int main() {
    const char *romdir = "/home/wizzard/share/GitHub/playdia-roms";
    int nblk = 864;
    
    /* Find Track 2 .bin files */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "find '%s' -name '*Track 2*' -name '*.bin' | sort | head -20", romdir);
    FILE *pp = popen(cmd, "r");
    if (!pp) return 1;
    
    char files[20][512];
    int nfiles = 0;
    while (nfiles < 20 && fgets(files[nfiles], sizeof(files[0]), pp)) {
        files[nfiles][strcspn(files[nfiles], "\n")] = 0;
        nfiles++;
    }
    pclose(pp);
    
    printf("Found %d Track 2 files\n\n", nfiles);
    
    /* Analyze first frame from each game */
    printf("=== First-frame AC properties across games ===\n");
    printf("%-50s QS  T  pad   DC    AC   b/c  1st8ac\n", "Game");
    
    /* Store AC data for multi-game tests */
    int good_count = 0;
    
    for (int g = 0; g < nfiles; g++) {
        /* Try several starting LBAs */
        int lbas[] = {150, 200, 250, 300, 350, 400, 450, 500, 502};
        int found = 0;
        
        for (int li = 0; li < 9 && !found; li++) {
            if (load_frame(files[g], lbas[li], 0) == 0) {
                if (fbuf[0]==0x00 && fbuf[1]==0x80 && fbuf[2]==0x04) {
                    int qs = fbuf[3];
                    int type = fbuf[39];
                    int de = flen;
                    while(de>0 && fbuf[de-1]==0xFF) de--;
                    int pad = flen - de;
                    int tb = (de-40)*8;
                    int bp = 0, ok = 1;
                    for(int b=0;b<nblk;b++){
                        int dv; int c=dec_dc(fbuf+40,bp,&dv,tb);
                        if(c<0){ok=0;break;} bp+=c;
                    }
                    if(ok){
                        int dc=bp, ac=tb-dc;
                        /* First 8 AC bits */
                        char first8[9]={0};
                        for(int i=0;i<8&&i<ac;i++) first8[i]='0'+get_bit(fbuf+40,dc+i);
                        
                        /* Extract short game name */
                        const char *name = strrchr(files[g], '/');
                        name = name ? name+1 : files[g];
                        char short_name[50];
                        strncpy(short_name, name, 49); short_name[49]=0;
                        char *trk = strstr(short_name, " (Track");
                        if(trk) *trk = 0;
                        
                        printf("%-50s %2d  %d %4d %4d %5d %.2f %s (LBA %d)\n",
                               short_name, qs, type, pad, dc, ac,
                               (double)ac/(nblk*63), first8, lbas[li]);
                        good_count++;
                        found = 1;
                    }
                }
            }
        }
    }
    
    printf("\nSuccessfully analyzed %d games\n", good_count);
    
    /* Now do detailed AC coding tests on a few games */
    printf("\n=== AC coding tests across games ===\n");
    
    const char *test_files[] = {
        "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku - Chikyuu-hen (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Ultraman Powered - Kaiju Gekimetsu Sakusen (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Hello Kitty - Yume no Kuni Daiboken (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Gamera - The Time Adventure (Japan) (Track 2).bin",
    };
    int test_lbas[] = {502, 502, 502, 502, 502};
    int ntest = 5;
    
    for (int g = 0; g < ntest; g++) {
        /* Try to find first valid frame */
        int start_lba = test_lbas[g];
        int found_lba = -1;
        for (int try_lba = 150; try_lba <= 600; try_lba += 50) {
            if (load_frame(test_files[g], try_lba, 0) == 0) {
                if (fbuf[0]==0x00 && fbuf[1]==0x80 && fbuf[2]==0x04) {
                    found_lba = try_lba;
                    break;
                }
            }
        }
        if (found_lba < 0) continue;
        
        load_frame(test_files[g], found_lba, 0);
        int qs = fbuf[3];
        int de = flen; while(de>0&&fbuf[de-1]==0xFF)de--;
        int tb = (de-40)*8;
        int bp = 0;
        for(int b=0;b<nblk;b++){int dv;bp+=dec_dc(fbuf+40,bp,&dv,tb);}
        int dc=bp, ac=tb-dc;
        
        int aclen=(ac+7)/8+1;
        uint8_t *acd=calloc(aclen,1);
        for(int i=0;i<ac;i++) if(get_bit(fbuf+40,dc+i)) acd[i>>3]|=(1<<(7-(i&7)));
        
        uint8_t *rnd=malloc(aclen);
        srand(42+g); for(int i=0;i<aclen;i++) rnd[i]=rand()&0xFF;
        
        const char *name = strrchr(test_files[g], '/');
        name = name ? name+1 : test_files[g];
        printf("\n--- %s (QS=%d, AC=%d) ---\n", name, qs, ac);
        
        test_runary_signmag(acd, ac, nblk, 2, "REAL");
        test_runary_signmag(rnd, ac, nblk, 2, "RAND");
        test_runary_signmag(acd, ac, nblk, 3, "REAL");
        test_runary_signmag(rnd, ac, nblk, 3, "RAND");
        test_runary_dcvlc(acd, ac, nblk, "REAL");
        test_runary_dcvlc(rnd, ac, nblk, "RAND");
        test_flag_expgolomb(acd, ac, nblk, "REAL");
        test_flag_expgolomb(rnd, ac, nblk, "RAND");
        test_per_coeff_seg(acd, ac, nblk, "REAL");
        test_per_coeff_seg(rnd, ac, nblk, "RAND");
        
        free(acd); free(rnd);
    }
    
    printf("\nDone.\n");
    return 0;
}
