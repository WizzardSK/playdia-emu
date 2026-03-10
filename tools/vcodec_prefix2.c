/*
 * vcodec_prefix2.c - Extended prefix code testing
 *
 * Show ALL results (not just 85%+), and also test interleaved DC+AC
 * where DC and AC are per-block (not all DC first then all AC).
 *
 * Also test: run-level coding variants (MPEG-1 style but with different tables)
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

int main() {
    const char *binfile = "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin";

    printf("=== Extended Prefix + Interleaved Testing ===\n\n");

    int lbas[] = {148, 500};
    for(int li=0; li<2; li++){
        if(!load_frame(binfile, lbas[li])) continue;
        int de=fdatalen; while(de>0&&fdata[de-1]==0xFF)de--;
        int tb=de*8;
        int qs=fdata[3], ft=fdata[39], pad=fdatalen-de;

        printf("=== LBA~%d QS=%d type=%d pad=%d data=%d bytes ===\n", lbas[li], qs, ft, pad, de);

        /* TEST 1: Interleaved DC+AC per block, prefix codes for AC */
        /* Try: 0=zero, 10=EOB, 11=nonzero(DC VLC) */
        printf("\n--- Interleaved DC+AC, various prefix codes ---\n");
        struct {
            const char *name;
            uint32_t zc; int zl; uint32_t ec; int el; uint32_t nc; int nl;
        } pfx[] = {
            {"0=Z 10=EOB 11=NZ",   0,1, 2,2, 3,2},
            {"1=Z 00=EOB 01=NZ",   1,1, 0,2, 1,2},
            {"0=Z 10=NZ  11=EOB",  0,1, 3,2, 2,2},
            {"1=Z 01=NZ  00=EOB",  1,1, 0,2, 1,2},
            {"0=EOB 10=Z 11=NZ",   2,2, 0,1, 3,2},
            {"0=EOB 11=Z 10=NZ",   3,2, 0,1, 2,2},
            {"1=EOB 00=Z 01=NZ",   0,2, 1,1, 1,2},
            {"0=NZ  10=Z 11=EOB",  2,2, 3,2, 0,1},
            {"0=NZ  11=Z 10=EOB",  3,2, 2,2, 0,1},
            {"1=NZ  00=Z 01=EOB",  0,2, 1,2, 1,1},
            {NULL,0,0,0,0,0,0}
        };

        for(int pi=0; pfx[pi].name; pi++){
            int bp=40*8;
            int dc_pred[3]={0,0,0};
            int blocks=0, nz=0, eobs=0, errs=0;

            for(int mb=0;mb<144&&bp<tb;mb++){
                for(int bl=0;bl<6&&bp<tb;bl++){
                    int comp=(bl<4)?0:(bl==4)?1:2;
                    int dv;
                    int used=dec_vlc(fdata,bp,&dv,tb);
                    if(used<0){errs++;goto interl_next;}
                    dc_pred[comp]+=dv;
                    bp+=used;

                    /* AC with prefix codes */
                    int pos=0;
                    while(pos<63 && bp<tb){
                        int matched=0;
                        if(bp+pfx[pi].zl<=tb && get_bits(fdata,bp,pfx[pi].zl)==pfx[pi].zc){
                            bp+=pfx[pi].zl; pos++; matched=1;
                        }
                        if(!matched && bp+pfx[pi].el<=tb && get_bits(fdata,bp,pfx[pi].el)==pfx[pi].ec){
                            bp+=pfx[pi].el; eobs++; matched=2;
                        }
                        if(!matched && bp+pfx[pi].nl<=tb && get_bits(fdata,bp,pfx[pi].nl)==pfx[pi].nc){
                            bp+=pfx[pi].nl;
                            int val; used=dec_vlc(fdata,bp,&val,tb);
                            if(used<0){errs++;goto interl_next;}
                            bp+=used; nz++; pos++; matched=3;
                        }
                        if(!matched){errs++;goto interl_next;}
                        if(matched==2) break;
                    }
                    blocks++;
                }
            }
            interl_next:;
            float pct=100.0*(bp-40*8)/(tb-40*8);
            printf("  %-25s: %3d blk %5.1f%% NZ=%4d EOB=%3d err=%d\n",
                   pfx[pi].name, blocks, pct, nz, eobs, errs);
        }

        /* TEST 2: What if AC uses run-level coding? */
        /* run = fixed bits (4 or 6), level = DC VLC, then EOB = special run value */
        printf("\n--- Run-Level coding (run=fixed bits, level=DC VLC) ---\n");
        for(int rbits=3; rbits<=6; rbits++){
            int eob_run = (1<<rbits)-1; /* All 1s = EOB */
            int bp=40*8;
            int dc_pred[3]={0,0,0};
            int blocks=0, nz=0, eobs=0, errs=0;

            for(int mb=0;mb<144&&bp<tb;mb++){
                for(int bl=0;bl<6&&bp<tb;bl++){
                    int comp=(bl<4)?0:(bl==4)?1:2;
                    int dv;
                    int used=dec_vlc(fdata,bp,&dv,tb);
                    if(used<0){errs++;goto rl_next;}
                    dc_pred[comp]+=dv;
                    bp+=used;

                    int pos=0;
                    while(pos<63 && bp+rbits<=tb){
                        int run=get_bits(fdata,bp,rbits);
                        bp+=rbits;
                        if(run==eob_run){eobs++;break;}
                        pos+=run; /* skip zeros */
                        if(pos>=63) break;
                        int val;
                        used=dec_vlc(fdata,bp,&val,tb);
                        if(used<0){errs++;goto rl_next;}
                        bp+=used;
                        nz++;
                        pos++;
                    }
                    blocks++;
                }
            }
            rl_next:;
            float pct=100.0*(bp-40*8)/(tb-40*8);
            printf("  run=%dbit(EOB=%d): %3d blk %5.1f%% NZ=%4d EOB=%3d err=%d\n",
                   rbits, eob_run, blocks, pct, nz, eobs, errs);
        }

        /* TEST 3: Run-level with run=0 meaning EOB */
        printf("\n--- Run-Level (run=0 → EOB, else skip run-1 zeros then value) ---\n");
        for(int rbits=3; rbits<=6; rbits++){
            int bp=40*8;
            int dc_pred[3]={0,0,0};
            int blocks=0, nz=0, eobs=0, errs=0;

            for(int mb=0;mb<144&&bp<tb;mb++){
                for(int bl=0;bl<6&&bp<tb;bl++){
                    int comp=(bl<4)?0:(bl==4)?1:2;
                    int dv;
                    int used=dec_vlc(fdata,bp,&dv,tb);
                    if(used<0){errs++;goto rl2_next;}
                    dc_pred[comp]+=dv;
                    bp+=used;

                    int pos=0;
                    while(pos<63 && bp+rbits<=tb){
                        int run=get_bits(fdata,bp,rbits);
                        bp+=rbits;
                        if(run==0){eobs++;break;}
                        pos+=run-1; /* skip run-1 zeros */
                        if(pos>=63) break;
                        int val;
                        used=dec_vlc(fdata,bp,&val,tb);
                        if(used<0){errs++;goto rl2_next;}
                        bp+=used;
                        nz++;
                        pos++;
                    }
                    blocks++;
                }
            }
            rl2_next:;
            float pct=100.0*(bp-40*8)/(tb-40*8);
            printf("  run=%dbit(0=EOB): %3d blk %5.1f%% NZ=%4d EOB=%3d err=%d\n",
                   rbits, blocks, pct, nz, eobs, errs);
        }

        /* TEST 4: What if AC coefficients use a DIFFERENT VLC table? */
        /* Try: unary (1s followed by 0) for size, then magnitude bits */
        printf("\n--- Unary-coded AC (unary size + magnitude) ---\n");
        {
            int bp=40*8;
            int dc_pred[3]={0,0,0};
            int blocks=0, nz=0, eobs=0, errs=0;

            for(int mb=0;mb<144&&bp<tb;mb++){
                for(int bl=0;bl<6&&bp<tb;bl++){
                    int comp=(bl<4)?0:(bl==4)?1:2;
                    int dv;
                    int used=dec_vlc(fdata,bp,&dv,tb);
                    if(used<0){errs++;goto un_next;}
                    dc_pred[comp]+=dv;
                    bp+=used;

                    /* AC: unary for size, 0=EOB, 10=size1, 110=size2, etc */
                    int pos=0;
                    while(pos<63 && bp<tb){
                        if(get_bit(fdata,bp)==0){bp++;eobs++;break;} /* 0=EOB */
                        /* Count 1s for size */
                        int sz=0;
                        while(bp<tb && get_bit(fdata,bp)==1){sz++;bp++;}
                        if(bp<tb) bp++; /* skip terminating 0 */
                        if(bp+sz>tb){errs++;goto un_next;}
                        if(sz>0){
                            uint32_t mag=get_bits(fdata,bp,sz);
                            bp+=sz;
                            nz++;
                        }
                        pos++;
                    }
                    blocks++;
                }
            }
            un_next:;
            float pct=100.0*(bp-40*8)/(tb-40*8);
            printf("  unary-0EOB: %3d blk %5.1f%% NZ=%4d EOB=%3d err=%d\n",
                   blocks, pct, nz, eobs, errs);
        }

        /* TEST 5: Exp-Golomb order 0 for AC (like H.264) */
        printf("\n--- Exp-Golomb order 0 for AC (0=EOB) ---\n");
        {
            int bp=40*8;
            int dc_pred[3]={0,0,0};
            int blocks=0, nz=0, eobs=0, errs=0;

            for(int mb=0;mb<144&&bp<tb;mb++){
                for(int bl=0;bl<6&&bp<tb;bl++){
                    int comp=(bl<4)?0:(bl==4)?1:2;
                    int dv;
                    int used=dec_vlc(fdata,bp,&dv,tb);
                    if(used<0){errs++;goto eg_next;}
                    dc_pred[comp]+=dv;
                    bp+=used;

                    int pos=0;
                    while(pos<63 && bp<tb){
                        /* Exp-Golomb: count leading zeros, then 1, then N bits */
                        int lz=0;
                        while(bp<tb && get_bit(fdata,bp)==0){lz++;bp++;}
                        if(bp>=tb){errs++;goto eg_next;}
                        bp++; /* skip the 1 */
                        if(lz>10){errs++;goto eg_next;}
                        uint32_t code=0;
                        if(lz>0){
                            if(bp+lz>tb){errs++;goto eg_next;}
                            code=get_bits(fdata,bp,lz);
                            bp+=lz;
                        }
                        int val=(1<<lz)-1+code;
                        if(val==0){eobs++;break;}
                        /* signed: odd=positive, even=negative */
                        nz++;
                        pos++;
                    }
                    blocks++;
                }
            }
            eg_next:;
            float pct=100.0*(bp-40*8)/(tb-40*8);
            printf("  exp-golomb: %3d blk %5.1f%% NZ=%4d EOB=%3d err=%d\n",
                   blocks, pct, nz, eobs, errs);
        }

        /* TEST 6: Rice coding (quotient in unary + remainder in fixed bits) */
        printf("\n--- Rice coding for AC ---\n");
        for(int k=0; k<=3; k++){
            int bp=40*8;
            int dc_pred[3]={0,0,0};
            int blocks=0, nz=0, eobs=0, errs=0;

            for(int mb=0;mb<144&&bp<tb;mb++){
                for(int bl=0;bl<6&&bp<tb;bl++){
                    int comp=(bl<4)?0:(bl==4)?1:2;
                    int dv;
                    int used=dec_vlc(fdata,bp,&dv,tb);
                    if(used<0){errs++;goto rice_next;}
                    dc_pred[comp]+=dv;
                    bp+=used;

                    int pos=0;
                    while(pos<63 && bp<tb){
                        /* Quotient: count 0s, terminated by 1 */
                        int q=0;
                        while(bp<tb && get_bit(fdata,bp)==0){q++;bp++;}
                        if(bp>=tb){errs++;goto rice_next;}
                        bp++; /* skip 1 */
                        if(q>20){errs++;goto rice_next;}
                        int r=0;
                        if(k>0){
                            if(bp+k>tb){errs++;goto rice_next;}
                            r=get_bits(fdata,bp,k);
                            bp+=k;
                        }
                        int val=q*(1<<k)+r;
                        if(val==0){eobs++;break;}
                        nz++;
                        pos++;
                    }
                    blocks++;
                }
            }
            rice_next:;
            float pct=100.0*(bp-40*8)/(tb-40*8);
            printf("  rice k=%d: %3d blk %5.1f%% NZ=%4d EOB=%3d err=%d\n",
                   k, blocks, pct, nz, eobs, errs);
        }

        /* TEST 7: What if there's a macroblock-level skip? */
        /* 1 bit per MB: 0=skip (all blocks zero AC), 1=has AC data */
        printf("\n--- MB-level coded/not-coded flag + DC VLC AC ---\n");
        {
            int bp=40*8;
            int dc_pred[3]={0,0,0};
            int blocks=0, nz=0, eobs=0, coded_mbs=0, errs=0;

            for(int mb=0;mb<144&&bp<tb;mb++){
                for(int bl=0;bl<6&&bp<tb;bl++){
                    int comp=(bl<4)?0:(bl==4)?1:2;
                    int dv;
                    int used=dec_vlc(fdata,bp,&dv,tb);
                    if(used<0){errs++;goto mb_next;}
                    dc_pred[comp]+=dv;
                    bp+=used;
                }

                /* MB coded flag */
                if(bp>=tb){errs++;goto mb_next;}
                int coded=get_bit(fdata,bp);
                bp++;

                if(coded){
                    coded_mbs++;
                    for(int bl=0;bl<6&&bp<tb;bl++){
                        for(int pos=0;pos<63&&bp<tb;pos++){
                            int av;
                            int used=dec_vlc(fdata,bp,&av,tb);
                            if(used<0){errs++;goto mb_next;}
                            bp+=used;
                            if(av==0){eobs++;break;}
                            nz++;
                        }
                    }
                }
                blocks+=6;
            }
            mb_next:;
            float pct=100.0*(bp-40*8)/(tb-40*8);
            printf("  MB flag+DC-VLC-EOB: %3d blk %5.1f%% coded=%d NZ=%4d EOB=%3d err=%d\n",
                   blocks, pct, coded_mbs, nz, eobs, errs);
        }

        /* TEST 8: Block-level coded flag (1 bit per block) + DC VLC AC */
        printf("\n--- Block-level coded flag + DC VLC AC ---\n");
        {
            int bp=40*8;
            int dc_pred[3]={0,0,0};
            int blocks=0, nz=0, eobs=0, coded=0, errs=0;

            for(int mb=0;mb<144&&bp<tb;mb++){
                for(int bl=0;bl<6&&bp<tb;bl++){
                    int comp=(bl<4)?0:(bl==4)?1:2;
                    int dv;
                    int used=dec_vlc(fdata,bp,&dv,tb);
                    if(used<0){errs++;goto bl_next;}
                    dc_pred[comp]+=dv;
                    bp+=used;

                    /* Block coded flag */
                    if(bp>=tb){errs++;goto bl_next;}
                    int cf=get_bit(fdata,bp);
                    bp++;

                    if(cf){
                        coded++;
                        for(int pos=0;pos<63&&bp<tb;pos++){
                            int av;
                            used=dec_vlc(fdata,bp,&av,tb);
                            if(used<0){errs++;goto bl_next;}
                            bp+=used;
                            if(av==0){eobs++;break;}
                            nz++;
                        }
                    }
                    blocks++;
                }
            }
            bl_next:;
            float pct=100.0*(bp-40*8)/(tb-40*8);
            printf("  blk flag+DC-VLC-EOB: %3d blk %5.1f%% coded=%d NZ=%4d EOB=%3d err=%d\n",
                   blocks, pct, coded, nz, eobs, errs);
        }

        /* TEST 9: What if header byte 39 (frame type) affects structure? */
        /* And what about bytes 4-39 of the header? Let's examine them */
        printf("\n--- Header bytes 4-39 ---\n  ");
        for(int i=4; i<40; i++) printf("%02X ", fdata[i]);
        printf("\n");

        printf("\n");
    }

    /* TEST 10: Analyze bit patterns at AC boundary more carefully */
    printf("=== Bit pattern analysis at DC/AC boundary ===\n");
    if(load_frame(binfile, 148)){
        int de=fdatalen; while(de>0&&fdata[de-1]==0xFF)de--;
        int tb=de*8;
        int bp=40*8;
        int dc_pred[3]={0,0,0};
        for(int mb=0;mb<144;mb++)
            for(int bl=0;bl<6;bl++){
                int comp=(bl<4)?0:(bl==4)?1:2;
                int dv; int used=dec_vlc(fdata,bp,&dv,tb);
                if(used<0){printf("DC decode failed at mb=%d bl=%d\n",mb,bl);goto done;}
                dc_pred[comp]+=dv; bp+=used;
            }
        printf("AC starts at bit %d (byte %d.%d)\n", bp, bp/8, bp%8);
        printf("First 64 bits of AC: ");
        for(int i=0;i<64&&bp+i<tb;i++){
            printf("%d",get_bit(fdata,bp+i));
            if(i%8==7) printf(" ");
        }
        printf("\n");

        /* Byte at AC start */
        printf("First 16 bytes of AC: ");
        int start_byte=bp/8;
        for(int i=0;i<16;i++) printf("%02X ",fdata[start_byte+i]);
        printf("\n");

        /* What's the first 100 DC values? Check if they make sense as image data */
        bp=40*8;
        dc_pred[0]=dc_pred[1]=dc_pred[2]=0;
        printf("\nFirst 24 DC diffs (Y blocks): ");
        for(int i=0;i<24;i++){
            int dv; dec_vlc(fdata,bp,&dv,tb);
            printf("%d ", dv);
            bp+=dec_vlc(fdata,bp,&dv,tb); /* re-decode, get length */
            /* Actually need to re-decode properly */
        }
        printf("\n");

        /* Re-do properly */
        bp=40*8;
        printf("First 24 DC reconstructed (comp0): ");
        int dc0=0;
        for(int i=0;i<24;i++){
            int dv; int used=dec_vlc(fdata,bp,&dv,tb);
            bp+=used;
            dc0+=dv;
            printf("%d ", dc0);
        }
        printf("\n");
    }
    done:

    printf("\nDone.\n");
    return 0;
}
