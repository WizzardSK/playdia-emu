/*
 * vcodec_lsb_boundary.c - Test LSB-first bit reading + padded frame boundary analysis
 *
 * Two key unexplored directions:
 * 1. LSB-first bit reading (many hardware codecs use this)
 * 2. Analyze exact byte boundary of padded frames
 * 3. Fixed 96-bit block budget internal structure analysis
 * 4. Bitmask + values hypothesis
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static uint8_t frame[16384];
static int framelen;

/* MSB-first (current) */
static int get_bit_msb(const uint8_t *d, int bp) { return (d[bp>>3]>>(7-(bp&7)))&1; }
static uint32_t get_bits_msb(const uint8_t *d, int bp, int n) {
    uint32_t v=0; for(int i=0;i<n;i++) v=(v<<1)|get_bit_msb(d,bp+i); return v;
}

/* LSB-first */
static int get_bit_lsb(const uint8_t *d, int bp) { return (d[bp>>3]>>(bp&7))&1; }
static uint32_t get_bits_lsb(const uint8_t *d, int bp, int n) {
    uint32_t v=0; for(int i=0;i<n;i++) v|=((uint32_t)get_bit_lsb(d,bp+i))<<i; return v;
}

/* DC VLC table - MSB-first */
static const struct{int len;uint32_t code;} dcv_msb[]={
    {3,0x4},{2,0x0},{2,0x1},{3,0x5},{3,0x6},
    {4,0xE},{5,0x1E},{6,0x3E},{7,0x7E},
    {8,0xFE},{9,0x1FE},{10,0x3FE}};

/* DC VLC table - LSB-first (same codes but read in reverse) */
static const struct{int len;uint32_t code;} dcv_lsb[]={
    {3,0x1},{2,0x0},{2,0x2},{3,0x5},{3,0x3},
    {4,0x7},{5,0x0F},{6,0x1F},{7,0x3F},
    {8,0x7F},{9,0xFF},{10,0x1FF}};

static int dec_dc_msb(const uint8_t *d, int bp, int *val, int tb) {
    for(int i=0;i<12;i++){
        if(bp+dcv_msb[i].len>tb) continue;
        uint32_t b=get_bits_msb(d,bp,dcv_msb[i].len);
        if(b==dcv_msb[i].code){
            int sz=i,c=dcv_msb[i].len;
            if(sz==0){*val=0;}
            else{if(bp+c+sz>tb)return-1;uint32_t r=get_bits_msb(d,bp+c,sz);c+=sz;
                *val=(r<(1u<<(sz-1)))?(int)r-(1<<sz)+1:(int)r;}
            return c;}}
    return -1;
}

static int dec_dc_lsb(const uint8_t *d, int bp, int *val, int tb) {
    for(int i=0;i<12;i++){
        if(bp+dcv_lsb[i].len>tb) continue;
        uint32_t b=get_bits_lsb(d,bp,dcv_lsb[i].len);
        if(b==dcv_lsb[i].code){
            int sz=i,c=dcv_lsb[i].len;
            if(sz==0){*val=0;}
            else{if(bp+c+sz>tb)return-1;uint32_t r=get_bits_lsb(d,bp+c,sz);c+=sz;
                *val=(r>=(1u<<(sz-1)))?(int)r:(int)r-(1<<sz)+1;}
            return c;}}
    return -1;
}

/* Byte-reversed MSB reading (bytes read MSB-first but byte order reversed within 16-bit words) */
static int get_bit_swap16(const uint8_t *d, int bp) {
    int bytepos = bp >> 3;
    int bitpos = bp & 7;
    /* Swap byte pairs */
    int swapped = bytepos ^ 1;
    return (d[swapped] >> (7 - bitpos)) & 1;
}
static uint32_t get_bits_swap16(const uint8_t *d, int bp, int n) {
    uint32_t v=0; for(int i=0;i<n;i++) v=(v<<1)|get_bit_swap16(d,bp+i); return v;
}

static int dec_dc_swap16(const uint8_t *d, int bp, int *val, int tb) {
    for(int i=0;i<12;i++){
        if(bp+dcv_msb[i].len>tb) continue;
        uint32_t b=get_bits_swap16(d,bp,dcv_msb[i].len);
        if(b==dcv_msb[i].code){
            int sz=i,c=dcv_msb[i].len;
            if(sz==0){*val=0;}
            else{if(bp+c+sz>tb)return-1;uint32_t r=get_bits_swap16(d,bp+c,sz);c+=sz;
                *val=(r<(1u<<(sz-1)))?(int)r-(1<<sz)+1:(int)r;}
            return c;}}
    return -1;
}

static int load_frame(const char *binfile, int target_lba) {
    FILE *fp=fopen(binfile,"rb"); if(!fp)return 0;
    int f1c=0;
    uint8_t tmpframe[16384];
    for(int s=0;s<2000;s++){
        long off=(long)(target_lba+s)*2352;
        uint8_t sec[2352]; fseek(fp,off,SEEK_SET);
        if(fread(sec,1,2352,fp)!=2352)break;
        uint8_t t=sec[24];
        if(t==0xF1){
            if(f1c<6) memcpy(tmpframe+f1c*2047,sec+25,2047);
            f1c++;
        } else if(t==0xF2){
            if(f1c==6 && tmpframe[0]==0x00 && tmpframe[1]==0x80 && tmpframe[2]==0x04){
                framelen=6*2047;
                memcpy(frame,tmpframe,framelen);
                fclose(fp);
                return 1;
            }
            f1c=0;
        } else if(t==0xF3) { f1c=0; }
    }
    fclose(fp);
    return 0;
}

int main() {
    const char *binfile = "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin";

    /* ===== TEST 1: LSB-first vs MSB-first DC decode ===== */
    printf("===== TEST 1: Bit reading order comparison =====\n\n");

    /* Load frame at LBA 502 (content frame, not logo) */
    if(!load_frame(binfile, 500)) { printf("Can't load frame\n"); return 1; }
    int qs = frame[3];
    int ftype = frame[39];
    int pad=framelen; while(pad>0&&frame[pad-1]==0xFF)pad--;
    int dataend = pad;
    int padding = framelen - dataend;
    printf("Frame: QS=%d type=%d datalen=%d padding=%d\n", qs, ftype, dataend, padding);

    int tb = dataend * 8;

    /* MSB-first DC decode */
    {
        int bp = 40*8; /* after header */
        int dc_pred[3]={0,0,0};
        int ok=1, blocks=0;
        int dc_vals[864];
        for(int mb=0;mb<144&&ok;mb++){
            for(int bl=0;bl<6&&ok;bl++){
                int comp=(bl<4)?0:(bl==4)?1:2;
                int dv;
                int used=dec_dc_msb(frame,bp,&dv,tb);
                if(used<0){ok=0;break;}
                dc_pred[comp]+=dv;
                dc_vals[blocks]=dc_pred[comp];
                bp+=used;
                blocks++;
            }
        }
        printf("MSB-first: decoded %d DC blocks, used %d bits (%.1f bits/dc)\n",
               blocks, bp-40*8, (float)(bp-40*8)/blocks);
        printf("  DC range: ");
        int mn=9999,mx=-9999;
        for(int i=0;i<blocks;i++){if(dc_vals[i]<mn)mn=dc_vals[i];if(dc_vals[i]>mx)mx=dc_vals[i];}
        printf("[%d, %d]\n", mn, mx);
        printf("  AC bits remaining: %d (%.1f bits/block)\n", tb-bp, (float)(tb-bp)/864);
    }

    /* LSB-first DC decode */
    {
        int bp = 40*8;
        int dc_pred[3]={0,0,0};
        int ok=1, blocks=0;
        int dc_vals[864];
        for(int mb=0;mb<144&&ok;mb++){
            for(int bl=0;bl<6&&ok;bl++){
                int comp=(bl<4)?0:(bl==4)?1:2;
                int dv;
                int used=dec_dc_lsb(frame,bp,&dv,tb);
                if(used<0){ok=0;break;}
                dc_pred[comp]+=dv;
                dc_vals[blocks]=dc_pred[comp];
                bp+=used;
                blocks++;
            }
        }
        if(ok){
            printf("LSB-first: decoded %d DC blocks, used %d bits\n", blocks, bp-40*8);
            int mn=9999,mx=-9999;
            for(int i=0;i<blocks;i++){if(dc_vals[i]<mn)mn=dc_vals[i];if(dc_vals[i]>mx)mx=dc_vals[i];}
            printf("  DC range: [%d, %d]\n", mn, mx);
        } else {
            printf("LSB-first: FAILED at block %d\n", blocks);
        }
    }

    /* Byte-swapped MSB */
    {
        int bp = 40*8;
        int dc_pred[3]={0,0,0};
        int ok=1, blocks=0;
        for(int mb=0;mb<144&&ok;mb++){
            for(int bl=0;bl<6&&ok;bl++){
                int comp=(bl<4)?0:(bl==4)?1:2;
                int dv;
                int used=dec_dc_swap16(frame,bp,&dv,tb);
                if(used<0){ok=0;break;}
                dc_pred[comp]+=dv;
                bp+=used;
                blocks++;
            }
        }
        if(ok)
            printf("Swap16-MSB: decoded %d DC blocks, used %d bits\n", blocks, bp-40*8);
        else
            printf("Swap16-MSB: FAILED at block %d\n", blocks);
    }

    /* ===== TEST 2: Padded frame boundary analysis ===== */
    printf("\n===== TEST 2: Padded frame boundary analysis =====\n\n");

    /* Find all padded frames */
    int lba_starts[] = {148, 277, 500, 755, 1110, 1870};
    for(int li=0; li<6; li++){
        if(!load_frame(binfile, lba_starts[li])) continue;
        int qs2 = frame[3];
        int ft2 = frame[39];
        int de=framelen; while(de>0&&frame[de-1]==0xFF)de--;
        int pad2=framelen-de;
        if(pad2 < 10) continue; /* Only analyze well-padded frames */

        printf("LBA ~%d: QS=%d type=%d dataend=%d pad=%d\n", lba_starts[li], qs2, ft2, de, pad2);

        /* Decode DC to find AC start */
        int bp = 40*8;
        int dc_pred[3]={0,0,0};
        int ok=1;
        for(int mb=0;mb<144&&ok;mb++){
            for(int bl=0;bl<6&&ok;bl++){
                int comp=(bl<4)?0:(bl==4)?1:2;
                int dv;
                int used=dec_dc_msb(frame,bp,&dv,de*8);
                if(used<0){ok=0;break;}
                dc_pred[comp]+=dv;
                bp+=used;
            }
        }
        if(!ok){ printf("  DC decode failed\n"); continue; }

        int ac_start = bp;
        int ac_bits = de*8 - ac_start;
        int dc_bits = ac_start - 40*8;
        printf("  DC: %d bits, AC: %d bits (%.2f bits/block, %.3f bits/coeff)\n",
               dc_bits, ac_bits, (float)ac_bits/864, (float)ac_bits/864/63);

        /* Last 32 bytes of real data */
        printf("  Last 32 bytes: ");
        for(int i=de-32;i<de;i++) printf("%02X ",frame[i]);
        printf("\n");

        /* Last 8 bytes in binary */
        printf("  Last 64 bits: ");
        for(int i=(de-8)*8; i<de*8; i++) printf("%d",get_bit_msb(frame,i));
        printf("\n");

        /* Check AC bit counts modulo various numbers */
        printf("  AC bits mod:  96=%d  48=%d  12=%d  8=%d  6=%d  64=%d\n",
               ac_bits%96, ac_bits%48, ac_bits%12, ac_bits%8, ac_bits%6, ac_bits%64);

        /* Check if AC bits / 864 is close to integer */
        printf("  AC/864 = %.4f, AC/144 = %.4f\n",
               (float)ac_bits/864, (float)ac_bits/144);
    }

    /* ===== TEST 3: Multiple games padded frame survey ===== */
    printf("\n===== TEST 3: Multi-game padded frame survey =====\n\n");

    const char *games[] = {
        "/home/wizzard/share/GitHub/playdia-roms/Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku - Chikyuu-hen (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Ultraman - Hikou no Himitsu (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Hello Kitty - Yume no Kuni Daibouken (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Bishojo Senshi Sailor Moon S - Quiz Taiketsu! Sailor Power Kesshuu (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Ie Naki Ko - Suzu no Sentaku (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Aqua Adventure - Blue Lilty (Japan) (Track 2).bin",
        NULL
    };

    for(int gi=0; games[gi]; gi++){
        FILE *fp=fopen(games[gi],"rb");
        if(!fp) continue;
        const char *gn = strrchr(games[gi],'/');
        if(gn) gn++; else gn=games[gi];
        printf("--- %s ---\n", gn);

        int found=0;
        int f1c=0;
        uint8_t tmpf[16384];
        for(int s=0;s<8000&&found<20;s++){
            long off=(long)(148+s)*2352;
            uint8_t sec[2352]; fseek(fp,off,SEEK_SET);
            if(fread(sec,1,2352,fp)!=2352)break;
            uint8_t t=sec[24];
            if(t==0xF1){
                if(f1c<6) memcpy(tmpf+f1c*2047,sec+25,2047);
                f1c++;
            } else if(t==0xF2){
                if(f1c==6 && tmpf[0]==0x00 && tmpf[1]==0x80 && tmpf[2]==0x04){
                    int fl=6*2047;
                    int de=fl; while(de>0&&tmpf[de-1]==0xFF)de--;
                    int pad3=fl-de;
                    if(pad3>=50){
                        int qs3=tmpf[3], ft3=tmpf[39];
                        /* Decode DC */
                        int bp=40*8;
                        int dc_pred[3]={0,0,0};
                        int ok=1;
                        for(int mb=0;mb<144&&ok;mb++){
                            for(int bl=0;bl<6&&ok;bl++){
                                int comp=(bl<4)?0:(bl==4)?1:2;
                                int dv;
                                /* Need to use the tmpf frame */
                                memcpy(frame,tmpf,fl);
                                int used=dec_dc_msb(frame,bp,&dv,de*8);
                                if(used<0){ok=0;break;}
                                dc_pred[comp]+=dv;
                                bp+=used;
                            }
                        }
                        if(ok){
                            int ac_bits=de*8-bp;
                            printf("  QS=%2d type=%d pad=%4d AC=%5d bits  %.2f/blk  mod96=%d mod48=%d mod8=%d\n",
                                   qs3,ft3,pad3,ac_bits,(float)ac_bits/864,ac_bits%96,ac_bits%48,ac_bits%8);
                        }
                        found++;
                    }
                }
                f1c=0;
            } else { f1c=0; }
        }
        if(found==0) printf("  (no well-padded frames found)\n");
        fclose(fp);
    }

    /* ===== TEST 4: Fixed 96-bit block structure analysis ===== */
    printf("\n===== TEST 4: 96-bit block structure (padded frame) =====\n\n");

    /* Load the most-padded frame */
    if(!load_frame(binfile, 500)){printf("Can't load\n");return 1;}

    /* Find the padded frame (F03 at LBA 502+18) - scan a range */
    /* Try loading from around where we expect the padded frames */
    int best_pad=0, best_lba=0;
    for(int try_lba=148; try_lba<200; try_lba++){
        if(!load_frame(binfile, try_lba)) continue;
        int de=framelen; while(de>0&&frame[de-1]==0xFF)de--;
        int p=framelen-de;
        if(p>best_pad){best_pad=p;best_lba=try_lba;}
    }
    printf("Best padded frame starts near LBA %d (pad=%d)\n", best_lba, best_pad);

    if(best_pad > 100 && load_frame(binfile, best_lba)){
        int de=framelen; while(de>0&&frame[de-1]==0xFF)de--;
        int qs2=frame[3];

        /* Decode DC */
        int bp=40*8;
        int dc_pred[3]={0,0,0};
        int dc_vals[864];
        for(int mb=0;mb<144;mb++){
            for(int bl=0;bl<6;bl++){
                int comp=(bl<4)?0:(bl==4)?1:2;
                int dv;
                int used=dec_dc_msb(frame,bp,&dv,de*8);
                if(used<0){printf("DC fail at block %d\n",mb*6+bl);return 1;}
                dc_pred[comp]+=dv;
                dc_vals[mb*6+bl]=dc_pred[comp];
                bp+=used;
            }
        }
        int ac_start=bp;
        int ac_bits=de*8-ac_start;
        printf("AC starts at bit %d, %d AC bits for 864 blocks\n", ac_start, ac_bits);

        /* Analyze as 96-bit blocks */
        printf("\nFirst 10 blocks as 96-bit chunks (12 bytes each):\n");
        for(int b=0;b<10&&(ac_start+b*96+96<=de*8);b++){
            int bstart = ac_start + b*96;
            printf("Block %d (DC=%4d): ", b, dc_vals[b]);
            for(int i=0;i<12;i++){
                uint8_t byte=0;
                for(int j=0;j<8;j++) byte=(byte<<1)|get_bit_msb(frame,bstart+i*8+j);
                printf("%02X ", byte);
            }
            /* Count nonzero bits in each 96-bit chunk */
            int ones=0;
            for(int i=0;i<96;i++) ones+=get_bit_msb(frame,bstart+i);
            printf(" [1s=%d]\n", ones);
        }

        /* Analyze 96-bit block statistics */
        printf("\n96-bit block statistics:\n");
        int nblocks = ac_bits / 96;
        int leftover = ac_bits % 96;
        printf("  %d full 96-bit blocks, %d leftover bits\n", nblocks, leftover);

        /* Distribution of 1-bit counts per 96-bit block */
        int hist[97]={0};
        for(int b=0;b<nblocks;b++){
            int bstart=ac_start+b*96;
            int ones=0;
            for(int i=0;i<96;i++) ones+=get_bit_msb(frame,bstart+i);
            hist[ones]++;
        }
        printf("  1-count distribution (96-bit blocks):\n  ");
        for(int i=0;i<97;i++) if(hist[i]) printf("%d:%d ", i, hist[i]);
        printf("\n");

        /* Check first byte of each 96-bit block */
        printf("\n  First byte histogram:\n");
        int fbhist[256]={0};
        for(int b=0;b<nblocks;b++){
            int bstart=ac_start+b*96;
            uint8_t byte=0;
            for(int j=0;j<8;j++) byte=(byte<<1)|get_bit_msb(frame,bstart+j);
            fbhist[byte]++;
        }
        printf("  Top 20: ");
        for(int t=0;t<20;t++){
            int mx=0,mi=0;
            for(int i=0;i<256;i++) if(fbhist[i]>mx){mx=fbhist[i];mi=i;}
            if(mx==0)break;
            printf("%02X=%d ", mi, mx);
            fbhist[mi]=0;
        }
        printf("\n");

        /* Check if first N bits of each block form a meaningful pattern */
        printf("\n  First 4 bits of each 96-bit block (distribution):\n");
        int f4hist[16]={0};
        for(int b=0;b<nblocks;b++){
            int bstart=ac_start+b*96;
            int v=0;
            for(int i=0;i<4;i++) v=(v<<1)|get_bit_msb(frame,bstart+i);
            f4hist[v]++;
        }
        for(int i=0;i<16;i++) printf("  %d%d%d%d: %d\n", (i>>3)&1,(i>>2)&1,(i>>1)&1,i&1, f4hist[i]);
    }

    /* ===== TEST 5: AC per-coefficient DC VLC with LSB-first ===== */
    printf("\n===== TEST 5: Per-coefficient DC VLC (LSB-first) =====\n\n");
    if(load_frame(binfile, 500)){
        int de=framelen; while(de>0&&frame[de-1]==0xFF)de--;
        int bp=40*8;
        int dc_pred[3]={0,0,0};
        for(int mb=0;mb<144;mb++){
            for(int bl=0;bl<6;bl++){
                int comp=(bl<4)?0:(bl==4)?1:2;
                int dv;
                int used=dec_dc_msb(frame,bp,&dv,de*8);
                if(used<0){printf("DC fail\n");return 1;}
                dc_pred[comp]+=dv;
                bp+=used;
            }
        }
        int ac_start=bp;
        int ac_end=de*8;

        /* Try DC VLC for AC values, LSB-first */
        int blocks_done=0, vals_done=0;
        int total_nz=0;
        int band_nz[64]={0};
        int bp_lsb = ac_start;
        int ok=1;
        for(int blk=0;blk<864&&ok;blk++){
            for(int pos=0;pos<63&&ok;pos++){
                if(bp_lsb>=ac_end){ok=0;break;}
                /* Read DC VLC code LSB-first */
                int dv;
                int used=dec_dc_lsb(frame,bp_lsb,&dv,ac_end);
                if(used<0){ok=0;break;}
                bp_lsb+=used;
                vals_done++;
                if(dv!=0){total_nz++;band_nz[pos]++;}
            }
            if(ok) blocks_done++;
        }
        printf("LSB DC-VLC per AC coeff: %d blocks, %d vals, consumed %d/%d bits (%.1f%%)\n",
               blocks_done, vals_done, bp_lsb-ac_start, ac_end-ac_start,
               100.0*(bp_lsb-ac_start)/(ac_end-ac_start));
        printf("  NZ=%d, bands[0..9]: ", total_nz);
        for(int i=0;i<10;i++) printf("%d ",band_nz[i]);
        printf("\n");

        /* Also try MSB DC-VLC per AC coeff for comparison */
        blocks_done=0; vals_done=0; total_nz=0;
        memset(band_nz,0,sizeof(band_nz));
        int bp_msb = ac_start;
        ok=1;
        for(int blk=0;blk<864&&ok;blk++){
            for(int pos=0;pos<63&&ok;pos++){
                if(bp_msb>=ac_end){ok=0;break;}
                int dv;
                int used=dec_dc_msb(frame,bp_msb,&dv,ac_end);
                if(used<0){ok=0;break;}
                bp_msb+=used;
                vals_done++;
                if(dv!=0){total_nz++;band_nz[pos]++;}
            }
            if(ok) blocks_done++;
        }
        printf("MSB DC-VLC per AC coeff: %d blocks, %d vals, consumed %d/%d bits (%.1f%%)\n",
               blocks_done, vals_done, bp_msb-ac_start, ac_end-ac_start,
               100.0*(bp_msb-ac_start)/(ac_end-ac_start));
        printf("  NZ=%d, bands[0..9]: ", total_nz);
        for(int i=0;i<10;i++) printf("%d ",band_nz[i]);
        printf("\n");
    }

    /* ===== TEST 6: Bitmask hypothesis ===== */
    printf("\n===== TEST 6: Bitmask + values hypothesis =====\n\n");
    /* If 96 bits/block: could be N-bit bitmask + values for marked positions */
    /* With 63 positions and 96 bits:
     * Option A: 63-bit mask + 33 bits for values (avg 5-6 NZ coeffs × 5-6 bits each)
     * Option B: shorter mask for first N positions + values
     * Option C: count + positions + values
     */
    if(load_frame(binfile, 500)){
        int de=framelen; while(de>0&&frame[de-1]==0xFF)de--;
        int bp=40*8;
        int dc_pred[3]={0,0,0};
        for(int mb=0;mb<144;mb++){
            for(int bl=0;bl<6;bl++){
                int comp=(bl<4)?0:(bl==4)?1:2;
                int dv;
                int used=dec_dc_msb(frame,bp,&dv,de*8);
                if(used<0){printf("DC fail\n");return 1;}
                dc_pred[comp]+=dv;
                bp+=used;
            }
        }
        int ac_start=bp;

        /* Test: read 63-bit bitmask per block, check statistics */
        printf("63-bit bitmask test (first 20 blocks):\n");
        int total_set=0;
        for(int b=0;b<20;b++){
            int bstart=ac_start+b*96;
            int nset=0;
            for(int i=0;i<63;i++) nset+=get_bit_msb(frame,bstart+i);
            total_set+=nset;
            int remaining=96-63; /* 33 bits for values */
            printf("  Block %d: %d NZ positions, %d value bits (%.1f bits/NZ)\n",
                   b, nset, remaining, nset>0?(float)remaining/nset:0);
        }
        printf("  Avg NZ per block: %.1f\n", total_set/20.0);

        /* Does NZ count look reasonable? For QS=40, expect few NZ */
        /* If avg ~5.5 NZ with 33 value bits, that's ~6 bits per value → 2^6=64 max */

        /* Test: 32-bit bitmask (first 32 AC positions only) + values */
        printf("\n32-bit bitmask test (first 20 blocks):\n");
        total_set=0;
        for(int b=0;b<20;b++){
            int bstart=ac_start+b*96;
            int nset=0;
            for(int i=0;i<32;i++) nset+=get_bit_msb(frame,bstart+i);
            total_set+=nset;
            int remaining=96-32; /* 64 bits for values */
            printf("  Block %d: %d NZ (of 32), %d value bits (%.1f bits/NZ)\n",
                   b, nset, remaining, nset>0?(float)remaining/nset:0);
        }
        printf("  Avg NZ per block: %.1f\n", total_set/20.0);
    }

    /* ===== TEST 7: Try reading AC with byte-reversed bit order ===== */
    printf("\n===== TEST 7: Byte-reversed AC analysis =====\n\n");
    if(load_frame(binfile, 500)){
        int de=framelen; while(de>0&&frame[de-1]==0xFF)de--;
        /* Decode DC normally (MSB) */
        int bp=40*8;
        int dc_pred[3]={0,0,0};
        for(int mb=0;mb<144;mb++){
            for(int bl=0;bl<6;bl++){
                int comp=(bl<4)?0:(bl==4)?1:2;
                int dv;
                int used=dec_dc_msb(frame,bp,&dv,de*8);
                if(used<0){printf("DC fail\n");return 1;}
                dc_pred[comp]+=dv;
                bp+=used;
            }
        }
        int ac_start=bp;

        /* Create byte-reversed version of AC data */
        printf("AC starts at bit %d (byte %d, bit offset %d)\n", ac_start, ac_start/8, ac_start%8);

        /* Bit-reverse each byte of AC data */
        uint8_t rev_frame[16384];
        memcpy(rev_frame, frame, framelen);
        int start_byte = ac_start / 8;
        for(int i=start_byte; i<de; i++){
            uint8_t b = frame[i];
            uint8_t r = 0;
            for(int j=0;j<8;j++) r|=((b>>j)&1)<<(7-j);
            rev_frame[i] = r;
        }

        /* Run-length analysis on byte-reversed AC data */
        int rl_hist_0[32]={0}, rl_hist_1[32]={0};
        int max_run_0=0, max_run_1=0;
        int run=0, curbit=get_bit_msb(rev_frame, ac_start);
        for(int i=ac_start+1; i<de*8; i++){
            int b=get_bit_msb(rev_frame,i);
            if(b==curbit) run++;
            else{
                if(run<32){
                    if(curbit) rl_hist_1[run]++; else rl_hist_0[run]++;
                }
                if(curbit && run>max_run_1) max_run_1=run;
                if(!curbit && run>max_run_0) max_run_0=run;
                run=0; curbit=b;
            }
        }
        printf("Byte-reversed run-length: max_0=%d max_1=%d\n", max_run_0, max_run_1);
        printf("  0-runs: "); for(int i=0;i<20;i++) if(rl_hist_0[i]) printf("r%d=%d ",i,rl_hist_0[i]); printf("\n");
        printf("  1-runs: "); for(int i=0;i<20;i++) if(rl_hist_1[i]) printf("r%d=%d ",i,rl_hist_1[i]); printf("\n");
    }

    printf("\nDone.\n");
    return 0;
}
