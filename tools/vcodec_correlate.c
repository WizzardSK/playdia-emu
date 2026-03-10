/*
 * vcodec_correlate.c - Correlation between DC complexity and AC content
 *
 * KEY INSIGHT: For the correct AC scheme, blocks at object edges (large DC
 * differences) should have more nonzero AC coefficients. Wrong schemes
 * show no correlation.
 *
 * Also tests: "first K zigzag positions only" hypothesis (K=16 matching qtable)
 * Also tests: fixed 96 bits/block with various internal structures
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define NBLK 864
#define WIDTH 256
#define HEIGHT 144
#define MB_W 16
#define MB_H 9

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

static double correlation(const int *x, const int *y, int n) {
    double sx=0,sy=0,sxx=0,syy=0,sxy=0;
    for(int i=0;i<n;i++){
        sx+=x[i]; sy+=y[i];
        sxx+=x[i]*x[i]; syy+=y[i]*y[i];
        sxy+=x[i]*y[i];
    }
    double mx=sx/n, my=sy/n;
    double vx=sxx/n-mx*mx, vy=syy/n-my*my;
    if(vx<1e-10||vy<1e-10) return 0;
    return (sxy/n-mx*my)/sqrt(vx*vy);
}

/* Zigzag scan */
static const int zigzag[64]={
    0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
   12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
   35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
   58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

/* Decode Flag+SM(vbits) per block, return NZ per block */
static int decode_flag_sm(const uint8_t *ac, int abits, int vbits, int *nz_per_block) {
    int bp=0;
    for(int b=0;b<NBLK;b++){
        int nz=0;
        for(int pos=0;pos<63;pos++){
            if(bp>=abits) return -1;
            int flag=get_bit(ac,bp); bp++;
            if(flag){
                if(bp+vbits>abits) return -1;
                bp+=vbits;
                nz++;
            }
        }
        nz_per_block[b]=nz;
    }
    return bp;
}

/* Decode first K positions only, fixed width per position */
static int decode_first_k(const uint8_t *ac, int abits, int K, int bits_per_pos, int *nz_per_block) {
    int bp=0;
    for(int b=0;b<NBLK;b++){
        int nz=0;
        for(int pos=0;pos<K;pos++){
            if(bp+bits_per_pos>abits) return -1;
            int val;
            if(bits_per_pos<=8){
                val=get_bits(ac,bp,bits_per_pos);
                /* Interpret as signed */
                if(val >= (1<<(bits_per_pos-1)))
                    val -= (1<<bits_per_pos);
            } else val=0;
            bp+=bits_per_pos;
            if(val!=0) nz++;
        }
        nz_per_block[b]=nz;
    }
    return bp;
}

/* Decode with DC VLC per position (first K positions) */
static int decode_dcvlc_k(const uint8_t *ac, int abits, int K, int *nz_per_block) {
    int bp=0;
    for(int b=0;b<NBLK;b++){
        int nz=0;
        for(int pos=0;pos<K;pos++){
            int val;
            int c=dec_dc(ac,bp,&val,abits);
            if(c<0) return -1;
            bp+=c;
            if(val!=0) nz++;
        }
        nz_per_block[b]=nz;
    }
    return bp;
}

/* IDCT for a single block (simplified - just compute energy) */
static double block_ac_energy(const int *ac_vals, int n) {
    double e=0;
    for(int i=0;i<n;i++) e+=ac_vals[i]*ac_vals[i];
    return e;
}

int main() {
    const char *binfile = "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin";

    /* Load padded frame F03 */
    if(load_frame(binfile, 502, 3)!=0){printf("Failed\n");return 1;}
    printf("F03: QS=%d, type=%d\n", fbuf[3], fbuf[39]);

    int de=flen; while(de>0&&fbuf[de-1]==0xFF)de--;
    int total_bits=(de-40)*8;

    /* Decode DC for all blocks */
    int dc_diffs[NBLK], dc_vals[NBLK];
    int dc_pred[3]={0,0,0};
    int bp=0;
    for(int b=0;b<NBLK;b++){
        int dv;
        int c=dec_dc(fbuf+40,bp,&dv,total_bits);
        if(c<0){printf("DC fail at %d\n",b);return 1;}
        bp+=c;
        dc_diffs[b]=dv;
        int comp=(b%6<4)?0:(b%6==4)?1:2;
        dc_pred[comp]+=dv;
        dc_vals[b]=dc_pred[comp];
    }
    int dc_bits=bp, ac_bits=total_bits-dc_bits;
    printf("DC: %d bits, AC: %d bits\n\n", dc_bits, ac_bits);

    /* DC complexity metric: |dc_diff| for each block */
    int dc_complex[NBLK];
    for(int b=0;b<NBLK;b++) dc_complex[b]=abs(dc_diffs[b]);

    /* Extract AC data */
    int ac_bytes=(ac_bits+7)/8+1;
    uint8_t *ac_data=calloc(ac_bytes,1);
    for(int i=0;i<ac_bits;i++)
        if(get_bit(fbuf+40,dc_bits+i)) ac_data[i>>3]|=(1<<(7-(i&7)));

    /* Random data */
    uint8_t *rnd=malloc(ac_bytes);
    srand(42); for(int i=0;i<ac_bytes;i++) rnd[i]=rand()&0xFF;

    int nz_real[NBLK], nz_rand[NBLK];

    /* === Test 1: Flag+SM correlation === */
    printf("=== Correlation: NZ_per_block vs DC_complexity ===\n");
    printf("%-30s  REAL_corr  RAND_corr  bits%%\n", "Scheme");

    for(int vb=1;vb<=4;vb++){
        int cr=decode_flag_sm(ac_data,ac_bits,vb,nz_real);
        int rr=decode_flag_sm(rnd,ac_bits,vb,nz_rand);
        double corr_real=correlation(dc_complex,nz_real,NBLK);
        double corr_rand=correlation(dc_complex,nz_rand,NBLK);
        printf("Flag+SM(%d):                    %+.4f    %+.4f    %.1f%%\n",
               vb, corr_real, corr_rand, cr>0?100.0*cr/ac_bits:0);
    }

    /* === Test 2: First K positions, fixed width === */
    printf("\n=== First K positions, fixed width ===\n");
    printf("%-30s  bits  bits%%  REAL_corr  RAND_corr  avg_NZ\n", "Scheme");

    for(int K=8;K<=32;K+=4){
        for(int bpp=2;bpp<=6;bpp+=2){
            int cr=decode_first_k(ac_data,ac_bits,K,bpp,nz_real);
            if(cr<0) continue;
            int rr=decode_first_k(rnd,ac_bits,K,bpp,nz_rand);
            double corr_real=correlation(dc_complex,nz_real,NBLK);
            double corr_rand=rr>0?correlation(dc_complex,nz_rand,NBLK):0;
            double avg_nz=0; for(int b=0;b<NBLK;b++) avg_nz+=nz_real[b]; avg_nz/=NBLK;
            printf("First%d×%dbit:                   %5d %5.1f%%  %+.4f    %+.4f    %.1f\n",
                   K,bpp, cr, 100.0*cr/ac_bits, corr_real, corr_rand, avg_nz);
        }
    }

    /* === Test 3: First K positions with DC VLC per position === */
    printf("\n=== First K positions with DC VLC ===\n");
    for(int K=8;K<=32;K+=4){
        int cr=decode_dcvlc_k(ac_data,ac_bits,K,nz_real);
        if(cr<0){printf("First%d×DCVLC: FAILED\n", K); continue;}
        int rr=decode_dcvlc_k(rnd,ac_bits,K,nz_rand);
        double corr_real=correlation(dc_complex,nz_real,NBLK);
        double corr_rand=rr>0?correlation(dc_complex,nz_rand,NBLK):0;
        double avg_nz=0; for(int b=0;b<NBLK;b++) avg_nz+=nz_real[b]; avg_nz/=NBLK;
        printf("First%d×DCVLC:                  %5d %5.1f%%  %+.4f    %+.4f    %.1f\n",
               K, cr, 100.0*cr/ac_bits, corr_real, corr_rand, avg_nz);
    }

    /* === Test 4: Flag+SM but only first K positions === */
    printf("\n=== Flag+SM on first K positions only ===\n");
    for(int K=8;K<=24;K+=4){
        for(int vb=1;vb<=4;vb++){
            int bp2=0;
            int ok=1;
            for(int b=0;b<NBLK&&ok;b++){
                int nz=0;
                for(int pos=0;pos<K;pos++){
                    if(bp2>=ac_bits){ok=0;break;}
                    int flag=get_bit(ac_data,bp2); bp2++;
                    if(flag){
                        if(bp2+vb>ac_bits){ok=0;break;}
                        bp2+=vb;
                        nz++;
                    }
                }
                nz_real[b]=nz;
            }
            if(!ok) continue;
            double corr_real=correlation(dc_complex,nz_real,NBLK);
            double avg_nz=0; for(int b=0;b<NBLK;b++) avg_nz+=nz_real[b]; avg_nz/=NBLK;
            printf("Flag+SM(%d)×K=%d:               %5d %5.1f%%  %+.4f    avg_NZ=%.1f\n",
                   vb, K, bp2, 100.0*bp2/ac_bits, corr_real, avg_nz);
        }
    }

    /* === Test 5: Produce images with "first 16 pos × 6-bit fixed" === */
    printf("\n=== Image test: First 16 positions × 6-bit signed ===\n");
    {
        /* Reload the non-padded frame from LBA 502 for better image */
        if(load_frame(binfile, 502, 0)==0){
            int fde=flen; while(fde>0&&fbuf[fde-1]==0xFF)fde--;
            int ftb=(fde-40)*8;
            int qs=fbuf[3];
            int fbp=0;
            int fdc_vals[NBLK];
            int fdc_pred[3]={0,0,0};
            for(int b=0;b<NBLK;b++){
                int dv;int c=dec_dc(fbuf+40,fbp,&dv,ftb);fbp+=c;
                fdc_vals[b]=dv;
            }
            int fac_start=fbp;

            printf("F00: QS=%d, AC starts at bit %d\n", qs, fac_start);

            /* Decode and produce image */
            uint8_t rgb[WIDTH*HEIGHT*3];
            memset(rgb,128,sizeof(rgb));

            int y_plane[HEIGHT][WIDTH], cb_plane[HEIGHT/2][WIDTH/2], cr_plane[HEIGHT/2][WIDTH/2];
            memset(y_plane,0,sizeof(y_plane));
            memset(cb_plane,0,sizeof(cb_plane));
            memset(cr_plane,0,sizeof(cr_plane));

            /* Qtable: first 16 bytes from header */
            int qt[16];
            for(int i=0;i<16;i++) qt[i]=fbuf[4+i];

            int acbp=fac_start;
            int bidx=0;
            fdc_pred[0]=fdc_pred[1]=fdc_pred[2]=0;

            for(int mby=0;mby<MB_H;mby++){
                for(int mbx=0;mbx<MB_W;mbx++){
                    for(int blk=0;blk<6;blk++){
                        int comp=(blk<4)?0:(blk==4)?1:2;
                        fdc_pred[comp]+=fdc_vals[bidx];
                        int dc_val=fdc_pred[comp];

                        /* Read 16 AC coefficients, 6 bits each (signed) */
                        int block[64];
                        memset(block,0,sizeof(block));
                        block[0]=dc_val*8;

                        for(int pos=1;pos<=16&&pos<64;pos++){
                            if(acbp+6>ftb) break;
                            int raw=get_bits(fbuf+40,acbp,6); acbp+=6;
                            int val=(raw>=32)?(raw-64):raw; /* 6-bit signed */
                            /* Dequantize: val * qs * qt[pos-1] / 8 */
                            if(val!=0 && pos-1<16){
                                block[zigzag[pos]]=val*qs*qt[pos-1]/8;
                            }
                        }

                        /* Simple DC+AC to pixels (no IDCT, just DC for now to check) */
                        /* Actually do IDCT */
                        double tmp[64], pixels[64];
                        /* Row IDCT */
                        for(int y=0;y<8;y++){
                            for(int x=0;x<8;x++){
                                double sum=0;
                                for(int u=0;u<8;u++){
                                    double cu=(u==0)?1.0/sqrt(2.0):1.0;
                                    sum+=cu*block[y*8+u]*cos((2*x+1)*u*M_PI/16.0);
                                }
                                tmp[y*8+x]=sum*0.5;
                            }
                        }
                        /* Column IDCT */
                        for(int x=0;x<8;x++){
                            for(int y=0;y<8;y++){
                                double sum=0;
                                for(int v=0;v<8;v++){
                                    double cv=(v==0)?1.0/sqrt(2.0):1.0;
                                    sum+=cv*tmp[v*8+x]*cos((2*y+1)*v*M_PI/16.0);
                                }
                                int pval=(int)round(sum*0.5)+128;
                                if(pval<0)pval=0; if(pval>255)pval=255;
                                pixels[y*8+x]=pval;
                            }
                        }

                        /* Store pixels */
                        if(blk<4){
                            int bx=(blk&1)*8, by=(blk>>1)*8;
                            int px=mbx*16+bx, py=mby*16+by;
                            for(int y=0;y<8;y++)for(int x=0;x<8;x++)
                                if(py+y<HEIGHT&&px+x<WIDTH)
                                    y_plane[py+y][px+x]=(int)pixels[y*8+x];
                        } else if(blk==4){
                            int px=mbx*8,py=mby*8;
                            for(int y=0;y<8;y++)for(int x=0;x<8;x++)
                                if(py+y<HEIGHT/2&&px+x<WIDTH/2)
                                    cb_plane[py+y][px+x]=(int)pixels[y*8+x];
                        } else {
                            int px=mbx*8,py=mby*8;
                            for(int y=0;y<8;y++)for(int x=0;x<8;x++)
                                if(py+y<HEIGHT/2&&px+x<WIDTH/2)
                                    cr_plane[py+y][px+x]=(int)pixels[y*8+x];
                        }
                        bidx++;
                    }
                }
            }

            /* YCbCr to RGB */
            for(int y=0;y<HEIGHT;y++){
                for(int x=0;x<WIDTH;x++){
                    int Y=y_plane[y][x], Cb=cb_plane[y/2][x/2], Cr=cr_plane[y/2][x/2];
                    int r=Y+(int)(1.402*(Cr-128));
                    int g=Y-(int)(0.344136*(Cb-128))-(int)(0.714136*(Cr-128));
                    int b2=Y+(int)(1.772*(Cb-128));
                    if(r<0)r=0;if(r>255)r=255;
                    if(g<0)g=0;if(g>255)g=255;
                    if(b2<0)b2=0;if(b2>255)b2=255;
                    rgb[(y*WIDTH+x)*3]=r;
                    rgb[(y*WIDTH+x)*3+1]=g;
                    rgb[(y*WIDTH+x)*3+2]=b2;
                }
            }

            FILE *f=fopen("/home/wizzard/share/GitHub/playdia-emu/output/lba502_f0_first16x6.ppm","wb");
            fprintf(f,"P6\n%d %d\n255\n",WIDTH,HEIGHT);
            fwrite(rgb,1,WIDTH*HEIGHT*3,f);
            fclose(f);
            printf("Wrote first16x6 image. AC bits used: %d/%d (%.1f%%)\n",
                   acbp-fac_start, ftb-fac_start, 100.0*(acbp-fac_start)/(ftb-fac_start));
        }
    }

    /* Also test on F00 from different LBA (757 = different scene) */
    printf("\n=== Correlation test on LBA 757 (different scene) ===\n");
    if(load_frame(binfile, 757, 0)==0){
        int fde2=flen; while(fde2>0&&fbuf[fde2-1]==0xFF)fde2--;
        int ftb2=(fde2-40)*8;
        int bp2=0;
        int dc_diffs2[NBLK];
        for(int b=0;b<NBLK;b++){
            int dv;int c=dec_dc(fbuf+40,bp2,&dv,ftb2);if(c<0)break;bp2+=c;
            dc_diffs2[b]=dv;
        }
        int ac2=ftb2-bp2;
        int dc_complex2[NBLK];
        for(int b=0;b<NBLK;b++) dc_complex2[b]=abs(dc_diffs2[b]);

        uint8_t *ac2d=calloc((ac2+7)/8+1,1);
        for(int i=0;i<ac2;i++)
            if(get_bit(fbuf+40,bp2+i)) ac2d[i>>3]|=(1<<(7-(i&7)));

        for(int vb=1;vb<=3;vb++){
            decode_flag_sm(ac2d,ac2,vb,nz_real);
            double corr=correlation(dc_complex2,nz_real,NBLK);
            printf("Flag+SM(%d): corr=%+.4f\n", vb, corr);
        }
        for(int K=12;K<=20;K+=4){
            int c3=decode_dcvlc_k(ac2d,ac2,K,nz_real);
            if(c3>0){
                double corr=correlation(dc_complex2,nz_real,NBLK);
                printf("First%d×DCVLC: corr=%+.4f (%.1f%%)\n", K, corr, 100.0*c3/ac2);
            }
        }
        free(ac2d);
    }

    free(ac_data);
    free(rnd);
    printf("\nDone.\n");
    return 0;
}
