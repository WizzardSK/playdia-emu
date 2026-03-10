/*
 * vcodec_runsize.c - Test JPEG-style (run, size) AC coding
 *
 * In JPEG, AC coefficients are coded as:
 *   Huffman(RRRRSSSS) + magnitude_bits
 * where RRRR = run of zeros (0-15), SSSS = magnitude size (0-10)
 * (0,0) = EOB, (15,0) = ZRL (16 zeros)
 *
 * Test: what if Playdia uses fixed-length (run, size) coding?
 * Also test: byte alignment after DC, various (run_bits, size_bits) combos
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

/* Decode DC for all blocks, return bit position after last DC */
static int decode_all_dc(int tb) {
    int bp=40*8;
    int dc_pred[3]={0,0,0};
    for(int mb=0;mb<144;mb++)
        for(int bl=0;bl<6;bl++){
            int comp=(bl<4)?0:(bl==4)?1:2;
            int dv; int used=dec_vlc(fdata,bp,&dv,tb);
            if(used<0) return -1;
            dc_pred[comp]+=dv; bp+=used;
        }
    return bp;
}

int main() {
    const char *binfile = "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin";
    printf("=== Run-Size AC Coding Tests ===\n\n");

    int lbas[] = {148, 500};
    for(int li=0; li<2; li++){
        if(!load_frame(binfile, lbas[li])) continue;
        int de=fdatalen; while(de>0&&fdata[de-1]==0xFF)de--;
        int tb=de*8, pad=fdatalen-de;
        int qs=fdata[3], ft=fdata[39];

        int dc_end = decode_all_dc(tb);
        if(dc_end<0) continue;
        int ac_bits = tb - dc_end;

        printf("=== LBA~%d QS=%d type=%d pad=%d DC_end=bit %d AC=%d bits (%.1f/blk) ===\n",
               lbas[li], qs, ft, pad, dc_end, ac_bits, (float)ac_bits/864);

        /* Check if qtables differ between bytes 4-19 and 20-35 */
        int qt_same = (memcmp(fdata+4, fdata+20, 16)==0);
        printf("Qtables same: %s\n", qt_same ? "YES" : "NO");
        if(!qt_same){
            printf("  QT1: "); for(int i=4;i<20;i++) printf("%02X ",fdata[i]); printf("\n");
            printf("  QT2: "); for(int i=20;i<36;i++) printf("%02X ",fdata[i]); printf("\n");
        }

        /* TEST 1: Fixed (run_bits, size_bits) + magnitude */
        printf("\n--- Fixed (run_bits, size_bits) + magnitude ---\n");
        for(int rbits=2; rbits<=6; rbits++){
            for(int sbits=2; sbits<=4; sbits++){
                int bp=dc_end;
                int blocks=0, nz=0, eobs=0, errs=0;

                for(int blk=0;blk<864&&bp<tb;blk++){
                    int pos=0;
                    while(pos<63 && bp+rbits+sbits<=tb){
                        int run=get_bits(fdata,bp,rbits);
                        int sz=get_bits(fdata,bp+rbits,sbits);
                        bp+=rbits+sbits;
                        if(run==0 && sz==0){eobs++;break;} /* EOB */
                        pos+=run;
                        if(pos>=63) break;
                        if(sz>0){
                            if(bp+sz>tb){errs++;goto rs_next;}
                            uint32_t mag=get_bits(fdata,bp,sz);
                            bp+=sz;
                            nz++;
                        }
                        pos++;
                    }
                    blocks++;
                }
                rs_next:;
                float pct=100.0*(bp-dc_end)/ac_bits;
                if(blocks>=800) /* Only show promising results */
                    printf("  r%d+s%d: %3d blk %5.1f%% NZ=%4d EOB=%3d err=%d\n",
                           rbits, sbits, blocks, pct, nz, eobs, errs);
            }
        }

        /* TEST 2: Same but with byte alignment after DC */
        int aligned_start = (dc_end + 7) & ~7; /* round up to byte */
        int aligned_ac = tb - aligned_start;
        printf("\n--- Byte-aligned AC start (bit %d → %d) ---\n", dc_end, aligned_start);
        for(int rbits=2; rbits<=6; rbits++){
            for(int sbits=2; sbits<=4; sbits++){
                int bp=aligned_start;
                int blocks=0, nz=0, eobs=0, errs=0;

                for(int blk=0;blk<864&&bp<tb;blk++){
                    int pos=0;
                    while(pos<63 && bp+rbits+sbits<=tb){
                        int run=get_bits(fdata,bp,rbits);
                        int sz=get_bits(fdata,bp+rbits,sbits);
                        bp+=rbits+sbits;
                        if(run==0 && sz==0){eobs++;break;}
                        pos+=run;
                        if(pos>=63) break;
                        if(sz>0){
                            if(bp+sz>tb){errs++;goto rs2_next;}
                            bp+=sz; nz++;
                        }
                        pos++;
                    }
                    blocks++;
                }
                rs2_next:;
                float pct=100.0*(bp-aligned_start)/aligned_ac;
                if(blocks>=800)
                    printf("  r%d+s%d: %3d blk %5.1f%% NZ=%4d EOB=%3d err=%d\n",
                           rbits, sbits, blocks, pct, nz, eobs, errs);
            }
        }

        /* TEST 3: Run-size with (15,0)=ZRL (skip 16 zeros), like JPEG */
        printf("\n--- JPEG-style (r4+s4) with ZRL=(15,0) ---\n");
        {
            int bp=dc_end;
            int blocks=0, nz=0, eobs=0, zrls=0, errs=0;
            int run_hist[16]={0}, sz_hist[16]={0};

            for(int blk=0;blk<864&&bp+8<=tb;blk++){
                int pos=0;
                while(pos<63 && bp+8<=tb){
                    int run=get_bits(fdata,bp,4);
                    int sz=get_bits(fdata,bp+4,4);
                    bp+=8;
                    if(run==0 && sz==0){eobs++;break;}
                    if(run==15 && sz==0){pos+=16;zrls++;continue;}
                    run_hist[run]++;
                    sz_hist[sz]++;
                    pos+=run;
                    if(pos>=63) break;
                    if(sz>0){
                        if(bp+sz>tb){errs++;goto j_next;}
                        bp+=sz; nz++;
                    }
                    pos++;
                }
                blocks++;
            }
            j_next:;
            float pct=100.0*(bp-dc_end)/ac_bits;
            printf("  %3d blk %5.1f%% NZ=%4d EOB=%3d ZRL=%d err=%d\n",
                   blocks, pct, nz, eobs, zrls, errs);
            printf("  Run dist: ");
            for(int i=0;i<16;i++) if(run_hist[i]) printf("r%d=%d ",i,run_hist[i]);
            printf("\n  Size dist: ");
            for(int i=0;i<16;i++) if(sz_hist[i]) printf("s%d=%d ",i,sz_hist[i]);
            printf("\n");
        }

        /* TEST 4: What if run and size use DC VLC too? */
        printf("\n--- VLC-run + VLC-size for AC ---\n");
        {
            int bp=dc_end;
            int blocks=0, nz=0, eobs=0, errs=0;

            for(int blk=0;blk<864&&bp<tb;blk++){
                int pos=0;
                while(pos<63 && bp<tb){
                    int run;
                    int used=dec_vlc(fdata,bp,&run,tb);
                    if(used<0){errs++;goto v_next;}
                    bp+=used;
                    if(run<0){errs++;goto v_next;}
                    if(run==0){
                        /* Could be EOB or zero-run nonzero */
                        int val;
                        used=dec_vlc(fdata,bp,&val,tb);
                        if(used<0){errs++;goto v_next;}
                        bp+=used;
                        if(val==0){eobs++;break;} /* (0,0)=EOB */
                        nz++; pos++;
                    } else {
                        pos+=run;
                        if(pos>=63) break;
                        int val;
                        used=dec_vlc(fdata,bp,&val,tb);
                        if(used<0){errs++;goto v_next;}
                        bp+=used;
                        nz++; pos++;
                    }
                }
                blocks++;
            }
            v_next:;
            float pct=100.0*(bp-dc_end)/ac_bits;
            printf("  %3d blk %5.1f%% NZ=%4d EOB=%3d err=%d\n",
                   blocks, pct, nz, eobs, errs);
        }

        /* TEST 5: Interleaved per-macroblock (DC for 6 blocks, then AC for 6 blocks) */
        printf("\n--- Per-MB interleaved: 6×DC then 6×AC(DC-VLC-EOB) ---\n");
        {
            int bp=40*8;
            int dc_pred[3]={0,0,0};
            int blocks=0, nz=0, eobs=0, errs=0;

            for(int mb=0;mb<144&&bp<tb;mb++){
                /* DC for 6 blocks */
                for(int bl=0;bl<6&&bp<tb;bl++){
                    int comp=(bl<4)?0:(bl==4)?1:2;
                    int dv; int used=dec_vlc(fdata,bp,&dv,tb);
                    if(used<0){errs++;goto mb2_next;}
                    dc_pred[comp]+=dv; bp+=used;
                }
                /* AC for 6 blocks */
                for(int bl=0;bl<6&&bp<tb;bl++){
                    for(int pos=0;pos<63&&bp<tb;pos++){
                        int av; int used=dec_vlc(fdata,bp,&av,tb);
                        if(used<0){errs++;goto mb2_next;}
                        bp+=used;
                        if(av==0){eobs++;break;}
                        nz++;
                    }
                    blocks++;
                }
            }
            mb2_next:;
            float pct=100.0*(bp-40*8)/(tb-40*8);
            printf("  %3d blk %5.1f%% NZ=%4d EOB=%3d err=%d\n",
                   blocks, pct, nz, eobs, errs);
        }

        /* TEST 6: What if it's NOT 6 blocks per MB but 4 (Y only for some reason)? */
        /* Or 3 blocks (Y,Cb,Cr at same resolution)? */
        printf("\n--- Different block counts per MB ---\n");
        for(int bpm=1; bpm<=8; bpm++){
            int nmb = 864 / bpm; /* approximate */
            if(864 % bpm != 0 && bpm != 5 && bpm != 7) continue; /* skip non-divisors except test */
            int bp=40*8;
            int blocks=0, nz=0, eobs=0, errs=0;

            for(int mb=0;mb<nmb&&bp<tb;mb++){
                for(int bl=0;bl<bpm&&bp<tb;bl++){
                    int dv; int used=dec_vlc(fdata,bp,&dv,tb);
                    if(used<0){errs++;goto bpm_next;}
                    bp+=used;
                    /* AC with DC VLC EOB */
                    for(int pos=0;pos<63&&bp<tb;pos++){
                        int av; used=dec_vlc(fdata,bp,&av,tb);
                        if(used<0){errs++;goto bpm_next;}
                        bp+=used;
                        if(av==0){eobs++;break;}
                        nz++;
                    }
                    blocks++;
                }
            }
            bpm_next:;
            float pct=100.0*(bp-40*8)/(tb-40*8);
            printf("  %d blk/MB (%d MBs): %3d blk %5.1f%% NZ=%4d EOB=%3d err=%d\n",
                   bpm, nmb, blocks, pct, nz, eobs, errs);
        }

        /* TEST 7: What if the entire thing uses Exp-Golomb for everything (DC too)? */
        printf("\n--- Full Exp-Golomb (DC+AC, 0=EOB) ---\n");
        {
            int bp=40*8;
            int blocks=0, nz=0, eobs=0, errs=0;

            for(int blk=0;blk<864&&bp<tb;blk++){
                /* DC as exp-golomb */
                int lz=0;
                while(bp<tb && get_bit(fdata,bp)==0){lz++;bp++;}
                if(bp>=tb||lz>15){errs++;break;}
                bp++;
                if(lz>0){if(bp+lz>tb){errs++;break;} bp+=lz;}

                /* AC as exp-golomb, 0=EOB */
                for(int pos=0;pos<63&&bp<tb;pos++){
                    lz=0;
                    while(bp<tb && get_bit(fdata,bp)==0){lz++;bp++;}
                    if(bp>=tb||lz>15){errs++;goto eg_next;}
                    bp++;
                    uint32_t code=0;
                    if(lz>0){
                        if(bp+lz>tb){errs++;goto eg_next;}
                        code=get_bits(fdata,bp,lz);
                        bp+=lz;
                    }
                    int val=(1<<lz)-1+code;
                    if(val==0){eobs++;break;}
                    nz++;
                }
                blocks++;
            }
            eg_next:;
            float pct=100.0*(bp-40*8)/(tb-40*8);
            printf("  %3d blk %5.1f%% NZ=%4d EOB=%3d err=%d\n",
                   blocks, pct, nz, eobs, errs);
        }

        /* TEST 8: Fixed-length AC coefficients (all 8-bit, all 6-bit, etc.) */
        printf("\n--- Fixed-length AC coefficients ---\n");
        for(int abits=4; abits<=8; abits++){
            int bp=dc_end;
            int blocks=0, nz=0;
            int expected_per_block=63;

            for(int blk=0;blk<864&&bp+abits<=tb;blk++){
                for(int pos=0;pos<expected_per_block&&bp+abits<=tb;pos++){
                    int v=get_bits(fdata,bp,abits);
                    bp+=abits;
                    if(v!=0) nz++;
                }
                blocks++;
            }
            float pct=100.0*(bp-dc_end)/ac_bits;
            printf("  %dbit×63: %3d blk %5.1f%% NZ=%4d (%.1f%%nz)\n",
                   abits, blocks, pct, nz, 100.0*nz/(blocks*63));
        }

        /* What if it's 15 AC coefficients (4x4 blocks)? */
        printf("\n--- Fixed-length 4×4 blocks (15 AC) ---\n");
        for(int abits=4; abits<=8; abits++){
            int bp=dc_end;
            int blocks=0, nz=0;
            for(int blk=0;blk<864&&bp+abits<=tb;blk++){
                for(int pos=0;pos<15&&bp+abits<=tb;pos++){
                    int v=get_bits(fdata,bp,abits);
                    bp+=abits;
                    if(v!=0) nz++;
                }
                blocks++;
            }
            float pct=100.0*(bp-dc_end)/ac_bits;
            printf("  %dbit×15: %3d blk %5.1f%% NZ=%4d (%.1f%%nz)\n",
                   abits, blocks, pct, nz, 100.0*nz/(blocks*15));
        }

        printf("\n");
    }

    /* Cross-game qtable comparison */
    printf("=== Cross-game qtable comparison ===\n");
    const char *games[] = {
        "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku - Chikyuu-Hen (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Ie Naki Ko - Suzu no Sentaku (Japan) (Track 2).bin",
    };
    const char *gnames[] = {"Mari-nee", "DBZ", "Ie Naki Ko"};
    for(int gi=0; gi<3; gi++){
        if(!load_frame(games[gi], 148)) continue;
        printf("%s LBA~148: QS=%d QT1=", gnames[gi], fdata[3]);
        for(int i=4;i<20;i++) printf("%02X",fdata[i]);
        printf(" QT2=");
        for(int i=20;i<36;i++) printf("%02X",fdata[i]);
        int same=(memcmp(fdata+4,fdata+20,16)==0);
        printf(" %s\n", same?"SAME":"DIFF");

        if(!load_frame(games[gi], 500)) continue;
        printf("%s LBA~500: QS=%d QT1=", gnames[gi], fdata[3]);
        for(int i=4;i<20;i++) printf("%02X",fdata[i]);
        printf(" QT2=");
        for(int i=20;i<36;i++) printf("%02X",fdata[i]);
        same=(memcmp(fdata+4,fdata+20,16)==0);
        printf(" %s\n", same?"SAME":"DIFF");
    }

    printf("\nDone.\n");
    return 0;
}
