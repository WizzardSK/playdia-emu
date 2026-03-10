/*
 * vcodec_bytepat.c - Byte-level pattern analysis of AC data
 *
 * Instead of guessing coding schemes, analyze the raw data for structural
 * patterns: common byte sequences, byte alignment, autocorrelation, etc.
 *
 * Key question: is there any detectable structure at the byte level?
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
    printf("=== Byte-Level Pattern Analysis ===\n\n");

    if(!load_frame(binfile, 148)) { printf("Failed to load\n"); return 1; }
    int de=fdatalen; while(de>0&&fdata[de-1]==0xFF)de--;
    int tb=de*8;

    /* Decode all DC to find AC start position */
    int bp=40*8;
    int dc_pred[3]={0,0,0};
    for(int mb=0;mb<144;mb++)
        for(int bl=0;bl<6;bl++){
            int comp=(bl<4)?0:(bl==4)?1:2;
            int dv; int used=dec_vlc(fdata,bp,&dv,tb);
            if(used<0){printf("DC fail at mb=%d bl=%d\n",mb,bl);return 1;}
            dc_pred[comp]+=dv; bp+=used;
        }

    int ac_start_bit = bp;
    int ac_start_byte = bp / 8;
    int ac_bit_offset = bp % 8;
    int ac_bytes = de - ac_start_byte;
    printf("DC decoded: %d bits (%d bytes + %d bits)\n", bp-40*8, (bp-40*8)/8, (bp-40*8)%8);
    printf("AC starts at bit %d (byte %d + %d bits)\n", ac_start_bit, ac_start_byte, ac_bit_offset);
    printf("AC data: %d bytes (~%d bits)\n\n", ac_bytes, ac_bytes*8);

    /* Check DC end bit alignment across multiple frames */
    printf("=== DC end bit alignment across frames ===\n");
    int test_lbas[] = {148, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1100, 1200};
    for(int li=0; li<12; li++){
        if(!load_frame(binfile, test_lbas[li])) continue;
        int de2=fdatalen; while(de2>0&&fdata[de2-1]==0xFF)de2--;
        int tb2=de2*8;
        int bp2=40*8;
        int dc2[3]={0,0,0};
        int ok=1;
        for(int mb=0;mb<144&&ok;mb++)
            for(int bl=0;bl<6&&ok;bl++){
                int comp=(bl<4)?0:(bl==4)?1:2;
                int dv; int used=dec_vlc(fdata,bp2,&dv,tb2);
                if(used<0){ok=0;break;}
                dc2[comp]+=dv; bp2+=used;
            }
        if(!ok) continue;
        int pad=fdatalen-de2;
        printf("  LBA~%4d: DC=%4d bits (byte %d.%d) AC=%5d bits type=%d pad=%d\n",
               test_lbas[li], bp2-40*8, (bp2-40*8)/8, bp2%8, de2*8-bp2, fdata[39], pad);
    }

    /* Check across games */
    printf("\n=== DC end alignment across games ===\n");
    const char *games[] = {
        "/home/wizzard/share/GitHub/playdia-roms/Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku - Chikyuu-Hen (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Ie Naki Ko - Suzu no Sentaku (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Ultraman - Chou Toushi Gekiden (Japan) (Track 2).bin",
    };
    const char *gn[] = {"DBZ", "Ie Naki", "Ultraman"};
    for(int gi=0;gi<3;gi++){
        for(int lba=148; lba<=600; lba+=150){
            if(!load_frame(games[gi], lba)) continue;
            int de2=fdatalen; while(de2>0&&fdata[de2-1]==0xFF)de2--;
            int tb2=de2*8;
            int bp2=40*8;
            int dc2[3]={0,0,0}; int ok=1;
            for(int mb=0;mb<144&&ok;mb++)
                for(int bl=0;bl<6&&ok;bl++){
                    int comp=(bl<4)?0:(bl==4)?1:2;
                    int dv; int used=dec_vlc(fdata,bp2,&dv,tb2);
                    if(used<0){ok=0;break;} dc2[comp]+=dv; bp2+=used;
                }
            if(!ok) continue;
            printf("  %-10s LBA~%d: DC=%4d bits (%d.%d) AC=%5d type=%d\n",
                   gn[gi], lba, bp2-40*8, (bp2-40*8)/8, bp2%8, de2*8-bp2, fdata[39]);
        }
    }

    /* Reload the first frame for further analysis */
    load_frame(binfile, 148);
    de=fdatalen; while(de>0&&fdata[de-1]==0xFF)de--;
    tb=de*8;
    bp=40*8;
    dc_pred[0]=dc_pred[1]=dc_pred[2]=0;
    for(int mb=0;mb<144;mb++)
        for(int bl=0;bl<6;bl++){
            int comp=(bl<4)?0:(bl==4)?1:2;
            int dv; dec_vlc(fdata,bp,&dv,tb); bp+=dec_vlc(fdata,bp,&dv,tb);
            dc_pred[comp]+=dv;
        }
    /* re-decode properly */
    bp=40*8;
    dc_pred[0]=dc_pred[1]=dc_pred[2]=0;
    for(int mb=0;mb<144;mb++)
        for(int bl=0;bl<6;bl++){
            int comp=(bl<4)?0:(bl==4)?1:2;
            int dv; int used=dec_vlc(fdata,bp,&dv,tb);
            dc_pred[comp]+=dv; bp+=used;
        }

    ac_start_bit = bp;
    ac_start_byte = bp / 8;
    ac_bytes = de - ac_start_byte;

    /* Bigram analysis (consecutive byte pairs) */
    printf("\n=== Byte bigram analysis (top 20) ===\n");
    {
        int bigram[256][256];
        memset(bigram, 0, sizeof(bigram));
        for(int i=ac_start_byte; i<de-1; i++)
            bigram[fdata[i]][fdata[i+1]]++;

        /* Find top bigrams */
        for(int top=0;top<20;top++){
            int best=0, bi=0, bj=0;
            for(int i=0;i<256;i++)
                for(int j=0;j<256;j++)
                    if(bigram[i][j]>best){best=bigram[i][j];bi=i;bj=j;}
            if(best==0) break;
            printf("  %02X %02X: %d (%.2f%%)\n", bi, bj, best, 100.0*best/ac_bytes);
            bigram[bi][bj]=0;
        }
    }

    /* Autocorrelation at byte level */
    printf("\n=== Byte-level autocorrelation ===\n");
    {
        double mean=0;
        for(int i=ac_start_byte;i<de;i++) mean+=fdata[i];
        mean/=ac_bytes;
        double var=0;
        for(int i=ac_start_byte;i<de;i++) var+=(fdata[i]-mean)*(fdata[i]-mean);
        var/=ac_bytes;

        for(int lag=1;lag<=20;lag++){
            double sum=0;
            int n=0;
            for(int i=ac_start_byte;i<de-lag;i++){
                sum+=(fdata[i]-mean)*(fdata[i+lag]-mean);
                n++;
            }
            double ac=(n>0)?sum/(n*var):0;
            printf("  lag %2d: %.4f %s\n", lag, ac, (ac>0.1||ac<-0.1)?"***":"");
        }
    }

    /* Nibble distribution (high vs low) */
    printf("\n=== Nibble distribution in AC data ===\n");
    {
        int hi[16]={0}, lo[16]={0};
        for(int i=ac_start_byte;i<de;i++){
            hi[fdata[i]>>4]++;
            lo[fdata[i]&0xF]++;
        }
        printf("  High nibble: ");
        for(int i=0;i<16;i++) printf("%X=%d ",i,hi[i]);
        printf("\n  Low nibble:  ");
        for(int i=0;i<16;i++) printf("%X=%d ",i,lo[i]);
        printf("\n");
    }

    /* Bit transition analysis (0→1 vs 1→0) */
    printf("\n=== Bit transition rates ===\n");
    {
        int t01=0, t10=0, t00=0, t11=0;
        for(int i=ac_start_bit;i<tb-1;i++){
            int a=get_bit(fdata,i), b=get_bit(fdata,i+1);
            if(a==0&&b==0) t00++;
            else if(a==0&&b==1) t01++;
            else if(a==1&&b==0) t10++;
            else t11++;
        }
        int total=t00+t01+t10+t11;
        printf("  0→0: %d (%.1f%%)  0→1: %d (%.1f%%)\n",
               t00, 100.0*t00/total, t01, 100.0*t01/total);
        printf("  1→0: %d (%.1f%%)  1→1: %d (%.1f%%)\n",
               t10, 100.0*t10/total, t11, 100.0*t11/total);
        printf("  P(0|prev=0)=%.3f P(1|prev=0)=%.3f\n",
               (double)t00/(t00+t01), (double)t01/(t00+t01));
        printf("  P(0|prev=1)=%.3f P(1|prev=1)=%.3f\n",
               (double)t10/(t10+t11), (double)t11/(t10+t11));
    }

    /* Try to find the VLC table by looking at what bit patterns START blocks */
    /* If DC coding ends at a known position, then the next bits are the START
       of AC for block 0. What does that look like? */
    printf("\n=== First bits of each AC block (interleaved decode) ===\n");
    {
        /* Interleaved decode: DC then AC per block */
        bp=40*8;
        dc_pred[0]=dc_pred[1]=dc_pred[2]=0;
        printf("First 20 blocks, first 8 AC bits:\n");
        for(int mb=0;mb<144&&bp<tb;mb++){
            for(int bl=0;bl<6&&bp<tb;bl++){
                int comp=(bl<4)?0:(bl==4)?1:2;
                int dv; int used=dec_vlc(fdata,bp,&dv,tb);
                if(used<0) goto done;
                dc_pred[comp]+=dv; bp+=used;

                /* Record first 8 bits of AC area */
                int blk_idx=mb*6+bl;
                if(blk_idx<20){
                    printf("  blk%3d (DC=%4d): ", blk_idx, dc_pred[comp]);
                    for(int i=0;i<16&&bp+i<tb;i++){
                        printf("%d",get_bit(fdata,bp+i));
                        if(i==7) printf(" ");
                    }
                    printf(" (%02X %02X)", bp+8<=tb?(int)get_bits(fdata,bp,8):0,
                           bp+16<=tb?(int)get_bits(fdata,bp+8,8):0);
                    printf("\n");
                }

                /* Skip AC (we don't know how) - for interleaved view we can't skip */
                /* So this only works for block 0 then we'd need to know block size */
                if(blk_idx>=20) goto done;

                /* Try DC VLC EOB to skip AC */
                for(int pos=0;pos<63&&bp<tb;pos++){
                    int av; used=dec_vlc(fdata,bp,&av,tb);
                    if(used<0) goto done;
                    bp+=used;
                    if(av==0) break;
                }
            }
        }
    }
    done:

    /* Look at AC data right after all DC values */
    printf("\n=== AC data after all DCs (first 200 bits) ===\n");
    load_frame(binfile, 148);
    de=fdatalen; while(de>0&&fdata[de-1]==0xFF)de--;
    tb=de*8;
    bp=40*8;
    dc_pred[0]=dc_pred[1]=dc_pred[2]=0;
    for(int mb=0;mb<144;mb++)
        for(int bl=0;bl<6;bl++){
            int comp=(bl<4)?0:(bl==4)?1:2;
            int dv; int used=dec_vlc(fdata,bp,&dv,tb);
            dc_pred[comp]+=dv; bp+=used;
        }
    printf("AC starts at bit %d:\n", bp);
    for(int i=0;i<200&&bp+i<tb;i++){
        printf("%d",get_bit(fdata,bp+i));
        if(i%8==7) printf(" ");
        if(i%64==63) printf("\n");
    }
    printf("\n");

    /* Look for the DC VLC size=0 pattern (code 100) in AC data */
    /* If DC VLC is used for AC, we'd expect to see 100 (EOB) frequently */
    printf("\n=== Frequency of 3-bit pattern '100' (DC VLC size=0) in AC ===\n");
    {
        int count=0, total=0;
        for(int i=bp;i+3<=tb;i++){
            total++;
            if(get_bits(fdata,i,3)==0x4) count++;
        }
        printf("  100 appears %d/%d times (%.1f%%, expected %.1f%% if random)\n",
               count, total, 100.0*count/total, 100.0/8);

        /* Count all 3-bit patterns */
        printf("  All 3-bit patterns: ");
        int pat3[8]={0};
        for(int i=bp;i+3<=tb;i++) pat3[get_bits(fdata,i,3)]++;
        for(int i=0;i<8;i++) printf("%d%d%d=%d ", (i>>2)&1,(i>>1)&1,i&1, pat3[i]);
        printf("\n");

        /* 2-bit patterns */
        int pat2[4]={0};
        for(int i=bp;i+2<=tb;i++) pat2[get_bits(fdata,i,2)]++;
        printf("  2-bit patterns: ");
        for(int i=0;i<4;i++) printf("%d%d=%d ", (i>>1)&1,i&1, pat2[i]);
        printf("\n");
    }

    /* What if the AC data is structured per-macroblock rather than per-block? */
    /* i.e., all 6 blocks' AC data for MB0, then all 6 for MB1, etc. */
    /* Try to detect if there's a pattern every ~N bits that could be MB boundary */
    printf("\n=== Bit autocorrelation (looking for periodicity) ===\n");
    {
        int ac_len = tb - bp;
        double mean=0;
        for(int i=0;i<ac_len;i++) mean+=get_bit(fdata,bp+i);
        mean/=ac_len;

        /* Check specific interesting periods */
        int periods[] = {96, 108, 109, 128, 144, 192, 216, 432, 648, 864};
        for(int pi=0;pi<10;pi++){
            int p=periods[pi];
            if(p>=ac_len/2) continue;
            double sum=0;
            int n=ac_len-p;
            for(int i=0;i<n;i++){
                double a=get_bit(fdata,bp+i)-mean;
                double b=get_bit(fdata,bp+i+p)-mean;
                sum+=a*b;
            }
            double var2=mean*(1-mean);
            double ac2=sum/(n*var2);
            printf("  period %4d: autocorr=%.6f %s\n", p, ac2,
                   (ac2>0.01||ac2<-0.01)?"*":"");
        }
    }

    printf("\nDone.\n");
    return 0;
}
