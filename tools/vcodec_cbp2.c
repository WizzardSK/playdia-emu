/*
 * vcodec_cbp2.c - Test Coded Block Pattern + interleaved DC/AC hypotheses
 *
 * Key insight: DC-VLC-EOB consumes only 34% of AC bits. What if:
 * 1. There's a CBP indicating which blocks have AC data
 * 2. DC and AC are interleaved (not separate)
 * 3. There's a block-skip mechanism
 *
 * Also test: what if data has 16 coefficients per block (matching 16-entry qtable)?
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

int main() {
    const char *binfile = "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin";

    printf("=== CBP + Interleaved DC/AC Tests ===\n\n");

    int lbas[] = {148, 500, 755};
    for(int li=0; li<3; li++){
        if(!load_frame(binfile, lbas[li])) continue;
        int qs=fdata[3], ft=fdata[39];
        int de=fdatalen; while(de>0&&fdata[de-1]==0xFF)de--;
        int tb=de*8;

        printf("=== LBA~%d QS=%d type=%d data=%d bytes ===\n", lbas[li], qs, ft, de);

        /* Test: interleaved DC+AC with DC VLC, size=0 = block has NO AC (all-zero block) */
        /* Idea: DC size=0 (value=0) means this block has no AC and DC unchanged */
        /* DC size>0: DC diff followed by AC coefficients */
        printf("\n--- Interleaved: DC VLC, if DC diff=0 then skip AC ---\n");
        {
            int bp=40*8;
            int dc_pred[3]={0,0,0};
            int blocks=0,ac_blocks=0,nz=0,eobs=0;
            int band_nz[64]={0};

            for(int mb=0;mb<144&&bp<tb;mb++){
                for(int bl=0;bl<6&&bp<tb;bl++){
                    int comp=(bl<4)?0:(bl==4)?1:2;
                    int dv;
                    int used=dec_dc(fdata,bp,&dv,tb);
                    if(used<0) goto t1_done;
                    dc_pred[comp]+=dv;
                    bp+=used;

                    if(dv!=0){
                        /* Block has content - decode AC */
                        ac_blocks++;
                        for(int pos=1;pos<64&&bp<tb;pos++){
                            int av;
                            used=dec_dc(fdata,bp,&av,tb);
                            if(used<0) goto t1_done;
                            bp+=used;
                            if(av==0){eobs++;break;}
                            nz++;
                            band_nz[pos]++;
                        }
                    }
                    blocks++;
                }
            }
            t1_done:
            printf("  %d blocks (%d with AC), %d/%d bits (%.1f%%)\n",
                   blocks, ac_blocks, bp-40*8, tb-40*8,
                   100.0*(bp-40*8)/(tb-40*8));
            printf("  NZ=%d EOB=%d\n", nz, eobs);
            printf("  Bands: ");
            for(int i=1;i<11;i++) printf("%d ",band_nz[i]);
            printf("\n");
        }

        /* Test: interleaved DC+AC, ALL blocks have AC, coded as DC VLC values */
        /* DC VLC for DC diff, then DC VLC per AC position, size=0=EOB */
        printf("\n--- Interleaved: all blocks, DC VLC AC with EOB ---\n");
        {
            int bp=40*8;
            int dc_pred[3]={0,0,0};
            int blocks=0,nz=0,eobs=0;
            int band_nz[64]={0};

            for(int mb=0;mb<144&&bp<tb;mb++){
                for(int bl=0;bl<6&&bp<tb;bl++){
                    int comp=(bl<4)?0:(bl==4)?1:2;
                    int dv;
                    int used=dec_dc(fdata,bp,&dv,tb);
                    if(used<0) goto t2_done;
                    dc_pred[comp]+=dv;
                    bp+=used;

                    /* AC with DC VLC, size=0=EOB */
                    for(int pos=1;pos<64&&bp<tb;pos++){
                        int av;
                        used=dec_dc(fdata,bp,&av,tb);
                        if(used<0) goto t2_done;
                        bp+=used;
                        if(av==0){eobs++;break;}
                        nz++;
                        band_nz[pos]++;
                    }
                    blocks++;
                }
            }
            t2_done:
            printf("  %d blocks, %d/%d bits (%.1f%%)\n",
                   blocks, bp-40*8, tb-40*8,
                   100.0*(bp-40*8)/(tb-40*8));
            printf("  NZ=%d EOB=%d avg=%.1f\n", nz, eobs, blocks>0?(float)nz/blocks:0);
            printf("  Bands: ");
            for(int i=1;i<11;i++) printf("%d ",band_nz[i]);
            printf("\n");
        }

        /* Test: 16 coefficients per block (1 DC + 15 AC) */
        printf("\n--- 16 coefficients per block (4×4 DCT hypothesis) ---\n");
        {
            int bp=40*8;
            int dc_pred[3]={0,0,0};
            int blocks=0,nz=0;
            int band_nz[16]={0};

            /* Same macroblock structure but 16 coeffs instead of 64 */
            for(int mb=0;mb<144&&bp<tb;mb++){
                for(int bl=0;bl<6&&bp<tb;bl++){
                    int comp=(bl<4)?0:(bl==4)?1:2;
                    int dv;
                    int used=dec_dc(fdata,bp,&dv,tb);
                    if(used<0) goto t3_done;
                    dc_pred[comp]+=dv;
                    bp+=used;

                    /* 15 AC coefficients */
                    for(int pos=1;pos<16&&bp<tb;pos++){
                        int av;
                        used=dec_dc(fdata,bp,&av,tb);
                        if(used<0) goto t3_done;
                        bp+=used;
                        if(av!=0){nz++;band_nz[pos]++;}
                    }
                    blocks++;
                }
            }
            t3_done:
            printf("  %d blocks, %d/%d bits (%.1f%%)\n",
                   blocks, bp-40*8, tb-40*8,
                   100.0*(bp-40*8)/(tb-40*8));
            printf("  NZ=%d avg=%.1f\n", nz, blocks>0?(float)nz/blocks:0);
            printf("  Bands: ");
            for(int i=1;i<16;i++) printf("%d ",band_nz[i]);
            printf("\n");
        }

        /* Test: 16 coefficients per block WITH EOB */
        printf("\n--- 16 coeff + EOB (size=0) ---\n");
        {
            int bp=40*8;
            int dc_pred[3]={0,0,0};
            int blocks=0,nz=0,eobs=0;

            for(int mb=0;mb<144&&bp<tb;mb++){
                for(int bl=0;bl<6&&bp<tb;bl++){
                    int comp=(bl<4)?0:(bl==4)?1:2;
                    int dv;
                    int used=dec_dc(fdata,bp,&dv,tb);
                    if(used<0) goto t4_done;
                    dc_pred[comp]+=dv;
                    bp+=used;

                    for(int pos=1;pos<16&&bp<tb;pos++){
                        int av;
                        used=dec_dc(fdata,bp,&av,tb);
                        if(used<0) goto t4_done;
                        bp+=used;
                        if(av==0){eobs++;break;}
                        nz++;
                    }
                    blocks++;
                }
            }
            t4_done:
            printf("  %d blocks, %d/%d bits (%.1f%%)\n",
                   blocks, bp-40*8, tb-40*8,
                   100.0*(bp-40*8)/(tb-40*8));
            printf("  NZ=%d EOB=%d avg=%.1f\n", nz, eobs, blocks>0?(float)nz/blocks:0);
        }

        /* Test: 32 coefficients per block */
        printf("\n--- 32 coeff per block ---\n");
        {
            int bp=40*8;
            int dc_pred[3]={0,0,0};
            int blocks=0,nz=0;

            for(int mb=0;mb<144&&bp<tb;mb++){
                for(int bl=0;bl<6&&bp<tb;bl++){
                    int comp=(bl<4)?0:(bl==4)?1:2;
                    int dv;
                    int used=dec_dc(fdata,bp,&dv,tb);
                    if(used<0) goto t5_done;
                    dc_pred[comp]+=dv;
                    bp+=used;

                    for(int pos=1;pos<32&&bp<tb;pos++){
                        int av;
                        used=dec_dc(fdata,bp,&av,tb);
                        if(used<0) goto t5_done;
                        bp+=used;
                        if(av!=0) nz++;
                    }
                    blocks++;
                }
            }
            t5_done:
            printf("  %d blocks, %d/%d bits (%.1f%%)\n",
                   blocks, bp-40*8, tb-40*8,
                   100.0*(bp-40*8)/(tb-40*8));
            printf("  NZ=%d avg=%.1f\n", nz, blocks>0?(float)nz/blocks:0);
        }

        /* Test: what if macroblock is 8x8 (not 16x16)? → 256x144/8 = 32x18 = 576 MBs */
        /* Each MB: 1Y + Cb + Cr in 4:4:4? Or just 1Y? */
        printf("\n--- 576 blocks (8×8 single-component) ---\n");
        {
            int bp=40*8;
            int dc_pred=0;
            int blocks=0,nz=0;

            /* Just decode sequential DC VLC values */
            for(int blk=0;blk<576&&bp<tb;blk++){
                int dv;
                int used=dec_dc(fdata,bp,&dv,tb);
                if(used<0) break;
                dc_pred+=dv;
                bp+=used;

                /* 63 AC */
                for(int pos=0;pos<63&&bp<tb;pos++){
                    int av;
                    used=dec_dc(fdata,bp,&av,tb);
                    if(used<0) goto t6_done;
                    bp+=used;
                    if(av!=0) nz++;
                }
                blocks++;
            }
            t6_done:
            printf("  %d blocks, %d/%d bits (%.1f%%)\n",
                   blocks, bp-40*8, tb-40*8,
                   100.0*(bp-40*8)/(tb-40*8));
        }

        /* KEY TEST: What if the entire bitstream is just DC VLC values? */
        /* Count how many values we can decode until we hit the end */
        printf("\n--- Pure DC VLC value count ---\n");
        {
            int bp=40*8;
            int vals=0, zeros=0, sz_hist[12]={0};
            while(bp<tb){
                int av;
                int used=dec_dc(fdata,bp,&av,tb);
                if(used<0){bp++;continue;}
                bp+=used;
                vals++;
                if(av==0) zeros++;
                /* Track size */
                for(int i=0;i<12;i++){
                    if(bp-used+dcv[i].len<=tb){
                        uint32_t b=get_bits(fdata,bp-used,dcv[i].len);
                        if(b==dcv[i].code){sz_hist[i]++;break;}
                    }
                }
            }
            printf("  Total values: %d (zeros=%d = %.1f%%)\n",
                   vals, zeros, 100.0*zeros/vals);
            printf("  Size dist: ");
            for(int i=0;i<12;i++) if(sz_hist[i]) printf("s%d=%d ",i,sz_hist[i]);
            printf("\n");
            printf("  vals/864 = %.2f, vals/16 = %.1f, vals%%864 = %d\n",
                   (float)vals/864, (float)vals/16, vals%864);
            printf("  vals%%144 = %d, vals%%432 = %d\n", vals%144, vals%432);
        }

        printf("\n");
    }

    printf("Done.\n");
    return 0;
}
