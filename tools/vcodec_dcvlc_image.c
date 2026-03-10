/*
 * vcodec_dcvlc_image.c - Generate images using DC VLC per-coefficient with EOB
 *
 * Test hypothesis: AC coefficients coded using same DC VLC table, size=0 = EOB
 * Try with proper dequantization and multiple scan orders.
 * Compare: DC-only baseline vs DC+AC with different scan orders
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

static int dec_dc_vlc(const uint8_t *d, int bp, int *val, int tb) {
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

/* Zigzag scan order (standard JPEG/MPEG) */
static const int zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10,
    17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* Row-major scan */
static const int rowscan[64] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
    32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,
    48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
};

/* Column-major scan */
static int colscan[64];

/* Alternate zigzag (MPEG-2 alternate) */
static const int altzigzag[64] = {
    0, 8, 16, 24, 1, 9, 2, 10,
    17, 25, 32, 40, 48, 56, 33, 41,
    18, 26, 3, 11, 4, 12, 19, 27,
    34, 42, 49, 57, 50, 58, 35, 43,
    20, 28, 5, 13, 6, 14, 21, 29,
    36, 44, 51, 59, 52, 60, 37, 45,
    22, 30, 7, 15, 23, 31, 38, 46,
    53, 61, 54, 62, 39, 47, 55, 63
};

/* 16 qtable values - need to map to 64 positions */
static const uint8_t qtable_raw[16] = {
    0x0A, 0x14, 0x0E, 0x0D, 0x12, 0x25, 0x16, 0x1C,
    0x0F, 0x18, 0x0F, 0x12, 0x12, 0x1F, 0x11, 0x14
};

/* Build 8×8 qtable from 16 values (interpretation: 4×4 upsampled) */
static int qtable_8x8[64];

static void build_qtable(int qs) {
    /* Interpretation 1: 16 values map to zigzag positions 0..15, rest = last value */
    /* Interpretation 2: 4×4 → 8×8 by nearest-neighbor upscale */
    /* Interpretation 3: 16 values for 16 frequency bands */
    /* Try interp 2 first: 4×4 upsampled to 8×8 */
    for(int r=0;r<8;r++){
        for(int c=0;c<8;c++){
            int qr = r/2, qc = c/2;
            int qval = qtable_raw[qr*4+qc];
            qtable_8x8[r*8+c] = qval * qs;
        }
    }
}

/* Simple IDCT (reference, slow) */
static void idct8x8(int block[64], double out[64]) {
    double temp[64];
    static const double pi8 = M_PI/8.0;
    /* rows */
    for(int r=0;r<8;r++){
        for(int x=0;x<8;x++){
            double sum=0;
            for(int u=0;u<8;u++){
                double cu = (u==0) ? 1.0/sqrt(2.0) : 1.0;
                sum += cu * block[r*8+u] * cos((2*x+1)*u*pi8/2.0);
            }
            temp[r*8+x] = sum * 0.5;
        }
    }
    /* cols */
    for(int c=0;c<8;c++){
        for(int y=0;y<8;y++){
            double sum=0;
            for(int v=0;v<8;v++){
                double cv = (v==0) ? 1.0/sqrt(2.0) : 1.0;
                sum += cv * temp[v*8+c] * cos((2*y+1)*v*pi8/2.0);
            }
            out[y*8+c] = sum * 0.5;
        }
    }
}

static int clamp(int v) { return v<0?0:v>255?255:v; }

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

/* Test a specific AC coding scheme + scan order, produce image */
static int test_scheme(const char *name, const int *scan, int use_eob, int use_dequant,
                        int16_t yplane[144][256], int16_t cbplane[72][128], int16_t crplane[72][128])
{
    int de=fdatalen; while(de>0&&fdata[de-1]==0xFF)de--;
    int tb=de*8;
    int qs=fdata[3];
    if(use_dequant) build_qtable(qs);

    int bp=40*8;
    int dc_pred[3]={0,0,0};
    int total_nz=0, total_eob=0;
    int band_nz[64]={0};

    for(int mb=0;mb<144;mb++){
        int mbx=(mb%16), mby=(mb/16);

        for(int bl=0;bl<6;bl++){
            int comp=(bl<4)?0:(bl==4)?1:2;
            /* DC */
            int dv;
            int used=dec_dc_vlc(fdata,bp,&dv,tb);
            if(used<0) return -1;
            dc_pred[comp]+=dv;
            bp+=used;

            int block[64]={0};
            block[0] = dc_pred[comp] * 8; /* DC scaling */

            /* AC */
            if(use_eob){
                /* DC VLC per coefficient, size=0 = EOB */
                for(int pos=1;pos<64&&bp<tb;pos++){
                    int av;
                    used=dec_dc_vlc(fdata,bp,&av,tb);
                    if(used<0) break;
                    bp+=used;
                    if(av==0){total_eob++;break;} /* EOB */
                    int zz_pos = scan[pos];
                    if(use_dequant && zz_pos>0)
                        block[zz_pos] = av * qtable_8x8[zz_pos];
                    else
                        block[zz_pos] = av;
                    total_nz++;
                    band_nz[pos]++;
                }
            } else {
                /* DC VLC per coefficient, all 63 values (no EOB) */
                for(int pos=1;pos<64&&bp<tb;pos++){
                    int av;
                    used=dec_dc_vlc(fdata,bp,&av,tb);
                    if(used<0) break;
                    bp+=used;
                    int zz_pos = scan[pos];
                    if(use_dequant && zz_pos>0)
                        block[zz_pos] = av * qtable_8x8[zz_pos];
                    else
                        block[zz_pos] = av;
                    if(av!=0){total_nz++;band_nz[pos]++;}
                }
            }

            /* IDCT */
            double out[64];
            idct8x8(block, out);

            /* Store to planes */
            if(comp==0){
                /* Luma */
                int bx = (bl&1)*8, by = (bl>>1)*8;
                int px = mbx*16+bx, py = mby*16+by;
                for(int r=0;r<8;r++)
                    for(int c=0;c<8;c++)
                        if(py+r<144 && px+c<256)
                            yplane[py+r][px+c] = (int16_t)(out[r*8+c]+128);
            } else if(comp==1){
                for(int r=0;r<8;r++)
                    for(int c=0;c<8;c++)
                        if(mby*8+r<72 && mbx*8+c<128)
                            cbplane[mby*8+r][mbx*8+c] = (int16_t)(out[r*8+c]+128);
            } else {
                for(int r=0;r<8;r++)
                    for(int c=0;c<8;c++)
                        if(mby*8+r<72 && mbx*8+c<128)
                            crplane[mby*8+r][mbx*8+c] = (int16_t)(out[r*8+c]+128);
            }
        }
    }

    int ac_bits = bp - 40*8;
    int total_dc_bits = 0; /* approximate */
    printf("  %-30s: consumed=%d/%d (%.1f%%) NZ=%d EOB=%d\n",
           name, bp-40*8, tb-40*8, 100.0*(bp-40*8)/(tb-40*8),
           total_nz, total_eob);
    printf("    Bands[0..9]: ");
    for(int i=1;i<11;i++) printf("%d ",band_nz[i]);
    printf("\n");
    return 0;
}

static void write_ppm(const char *fn, int16_t yp[144][256], int16_t cbp[72][128], int16_t crp[72][128]) {
    FILE *fp=fopen(fn,"wb");
    if(!fp){printf("Can't write %s\n",fn);return;}
    fprintf(fp,"P6\n256 144\n255\n");
    for(int y=0;y<144;y++){
        for(int x=0;x<256;x++){
            int Y=yp[y][x];
            int Cb=cbp[y/2][x/2];
            int Cr=crp[y/2][x/2];
            int R=Y+1.402*(Cr-128);
            int G=Y-0.344136*(Cb-128)-0.714136*(Cr-128);
            int B=Y+1.772*(Cb-128);
            uint8_t rgb[3]={(uint8_t)clamp(R),(uint8_t)clamp(G),(uint8_t)clamp(B)};
            fwrite(rgb,1,3,fp);
        }
    }
    fclose(fp);
}

int main() {
    const char *binfile = "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin";

    /* Init column scan */
    for(int c=0;c<8;c++) for(int r=0;r<8;r++) colscan[c*8+r]=r*8+c;

    /* Test on LBA 502 (content frame) and LBA 757 */
    int lbas[] = {500, 755};
    const char *lba_names[] = {"502", "757"};

    for(int li=0;li<2;li++){
        if(!load_frame(binfile, lbas[li])){printf("Can't load LBA %s\n",lba_names[li]);continue;}
        int qs=fdata[3], ft=fdata[39];
        printf("\n=== LBA %s (QS=%d type=%d) ===\n", lba_names[li], qs, ft);

        static int16_t yp[144][256], cbp[72][128], crp[72][128];
        char fn[256];

        /* 1. DC-only baseline */
        {
            memset(yp,0,sizeof(yp));memset(cbp,0,sizeof(cbp));memset(crp,0,sizeof(crp));
            int de=fdatalen; while(de>0&&fdata[de-1]==0xFF)de--;
            int bp=40*8, dc_pred[3]={0,0,0};
            for(int mb=0;mb<144;mb++){
                int mbx=mb%16, mby=mb/16;
                for(int bl=0;bl<6;bl++){
                    int comp=(bl<4)?0:(bl==4)?1:2;
                    int dv; int used=dec_dc_vlc(fdata,bp,&dv,de*8);
                    if(used<0)break; bp+=used; dc_pred[comp]+=dv;
                    int dc=dc_pred[comp]*8;
                    if(comp==0){
                        int bx=(bl&1)*8,by=(bl>>1)*8;
                        for(int r=0;r<8;r++)for(int c=0;c<8;c++)
                            if(mby*16+by+r<144&&mbx*16+bx+c<256)
                                yp[mby*16+by+r][mbx*16+bx+c]=(int16_t)(dc/8.0+128);
                    } else if(comp==1){
                        for(int r=0;r<8;r++)for(int c=0;c<8;c++)
                            if(mby*8+r<72&&mbx*8+c<128)
                                cbp[mby*8+r][mbx*8+c]=(int16_t)(dc/8.0+128);
                    } else {
                        for(int r=0;r<8;r++)for(int c=0;c<8;c++)
                            if(mby*8+r<72&&mbx*8+c<128)
                                crp[mby*8+r][mbx*8+c]=(int16_t)(dc/8.0+128);
                    }
                }
            }
            sprintf(fn,"/home/wizzard/share/GitHub/playdia-emu/output/lba%s_dconly.ppm",lba_names[li]);
            write_ppm(fn,yp,cbp,crp);
            printf("  DC-only: %s\n", fn);
        }

        /* 2. DC VLC + EOB, zigzag, no dequant */
        {
            memset(yp,0,sizeof(yp));memset(cbp,0,sizeof(cbp));memset(crp,0,sizeof(crp));
            test_scheme("DCVLC+EOB zigzag nodq", zigzag, 1, 0, yp, cbp, crp);
            sprintf(fn,"/home/wizzard/share/GitHub/playdia-emu/output/lba%s_dcvlc_eob_zz.ppm",lba_names[li]);
            write_ppm(fn,yp,cbp,crp);
        }

        /* 3. DC VLC + EOB, zigzag, with dequant */
        {
            memset(yp,0,sizeof(yp));memset(cbp,0,sizeof(cbp));memset(crp,0,sizeof(crp));
            test_scheme("DCVLC+EOB zigzag dq", zigzag, 1, 1, yp, cbp, crp);
            sprintf(fn,"/home/wizzard/share/GitHub/playdia-emu/output/lba%s_dcvlc_eob_zz_dq.ppm",lba_names[li]);
            write_ppm(fn,yp,cbp,crp);
        }

        /* 4. DC VLC + EOB, alt zigzag */
        {
            memset(yp,0,sizeof(yp));memset(cbp,0,sizeof(cbp));memset(crp,0,sizeof(crp));
            test_scheme("DCVLC+EOB altzz nodq", altzigzag, 1, 0, yp, cbp, crp);
            sprintf(fn,"/home/wizzard/share/GitHub/playdia-emu/output/lba%s_dcvlc_eob_azz.ppm",lba_names[li]);
            write_ppm(fn,yp,cbp,crp);
        }

        /* 5. DC VLC + EOB, row scan */
        {
            memset(yp,0,sizeof(yp));memset(cbp,0,sizeof(cbp));memset(crp,0,sizeof(crp));
            test_scheme("DCVLC+EOB row nodq", rowscan, 1, 0, yp, cbp, crp);
            sprintf(fn,"/home/wizzard/share/GitHub/playdia-emu/output/lba%s_dcvlc_eob_row.ppm",lba_names[li]);
            write_ppm(fn,yp,cbp,crp);
        }

        /* 6. DC VLC + EOB, column scan */
        {
            memset(yp,0,sizeof(yp));memset(cbp,0,sizeof(cbp));memset(crp,0,sizeof(crp));
            test_scheme("DCVLC+EOB col nodq", colscan, 1, 0, yp, cbp, crp);
            sprintf(fn,"/home/wizzard/share/GitHub/playdia-emu/output/lba%s_dcvlc_eob_col.ppm",lba_names[li]);
            write_ppm(fn,yp,cbp,crp);
        }

        /* 7. DC VLC NO EOB (all 63 coeffs), zigzag, no dequant */
        {
            memset(yp,0,sizeof(yp));memset(cbp,0,sizeof(cbp));memset(crp,0,sizeof(crp));
            test_scheme("DCVLC all63 zigzag", zigzag, 0, 0, yp, cbp, crp);
            sprintf(fn,"/home/wizzard/share/GitHub/playdia-emu/output/lba%s_dcvlc_all63_zz.ppm",lba_names[li]);
            write_ppm(fn,yp,cbp,crp);
        }

        /* 8. DC VLC + EOB, zigzag, dequant with just qtable (no QS multiply) */
        {
            memset(yp,0,sizeof(yp));memset(cbp,0,sizeof(cbp));memset(crp,0,sizeof(crp));
            /* Override qtable to use raw values without QS */
            int saved_qs = fdata[3];
            fdata[3] = 1; /* QS=1 so qtable * QS = qtable */
            test_scheme("DCVLC+EOB zz rawq", zigzag, 1, 1, yp, cbp, crp);
            fdata[3] = saved_qs;
            sprintf(fn,"/home/wizzard/share/GitHub/playdia-emu/output/lba%s_dcvlc_eob_rawq.ppm",lba_names[li]);
            write_ppm(fn,yp,cbp,crp);
        }
    }

    /* Also test on Ie Naki Ko frames */
    printf("\n=== Ie Naki Ko frames ===\n");
    const char *ienaki = "/home/wizzard/share/GitHub/playdia-roms/Ie Naki Ko - Suzu no Sentaku (Japan) (Track 2).bin";
    if(load_frame(ienaki, 148)){
        int qs=fdata[3], ft=fdata[39];
        printf("Ie Naki Ko LBA~148: QS=%d type=%d\n", qs, ft);
        static int16_t yp[144][256], cbp[72][128], crp[72][128];

        memset(yp,0,sizeof(yp));memset(cbp,0,sizeof(cbp));memset(crp,0,sizeof(crp));
        test_scheme("DCVLC+EOB zigzag nodq", zigzag, 1, 0, yp, cbp, crp);
        write_ppm("/home/wizzard/share/GitHub/playdia-emu/output/ienaki_dcvlc_eob_zz.ppm",yp,cbp,crp);

        /* DC only for comparison */
        memset(yp,0,sizeof(yp));memset(cbp,0,sizeof(cbp));memset(crp,0,sizeof(crp));
        {
            int de=fdatalen; while(de>0&&fdata[de-1]==0xFF)de--;
            int bp=40*8, dc_pred[3]={0,0,0};
            for(int mb=0;mb<144;mb++){
                int mbx=mb%16, mby=mb/16;
                for(int bl=0;bl<6;bl++){
                    int comp=(bl<4)?0:(bl==4)?1:2;
                    int dv; int used=dec_dc_vlc(fdata,bp,&dv,de*8);
                    if(used<0)break; bp+=used; dc_pred[comp]+=dv;
                    int dc=dc_pred[comp]*8;
                    if(comp==0){
                        int bx=(bl&1)*8,by=(bl>>1)*8;
                        for(int r=0;r<8;r++)for(int c=0;c<8;c++)
                            if(mby*16+by+r<144&&mbx*16+bx+c<256)
                                yp[mby*16+by+r][mbx*16+bx+c]=(int16_t)(dc/8.0+128);
                    } else if(comp==1){
                        for(int r=0;r<8;r++)for(int c=0;c<8;c++)
                            if(mby*8+r<72&&mbx*8+c<128)
                                cbp[mby*8+r][mbx*8+c]=(int16_t)(dc/8.0+128);
                    } else {
                        for(int r=0;r<8;r++)for(int c=0;c<8;c++)
                            if(mby*8+r<72&&mbx*8+c<128)
                                crp[mby*8+r][mbx*8+c]=(int16_t)(dc/8.0+128);
                    }
                }
            }
        }
        write_ppm("/home/wizzard/share/GitHub/playdia-emu/output/ienaki_dconly.ppm",yp,cbp,crp);
    }

    /* Quick consumption comparison across multiple frames */
    printf("\n=== DC-VLC+EOB consumption across frame types ===\n");
    const char *games[] = {
        "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku - Chikyuu-hen (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Ie Naki Ko - Suzu no Sentaku (Japan) (Track 2).bin",
        NULL
    };
    int test_lbas[] = {148, 277, 500, 755, 1100};
    for(int gi=0;games[gi];gi++){
        const char *gn=strrchr(games[gi],'/'); if(gn)gn++;else gn=games[gi];
        for(int li=0;li<5;li++){
            if(!load_frame(games[gi],test_lbas[li])) continue;
            int qs=fdata[3], ft=fdata[39];
            int de=fdatalen; while(de>0&&fdata[de-1]==0xFF)de--;
            int bp=40*8;
            int dc_pred[3]={0,0,0};
            int ok=1;
            for(int mb=0;mb<144&&ok;mb++)
                for(int bl=0;bl<6&&ok;bl++){
                    int comp=(bl<4)?0:(bl==4)?1:2;
                    int dv;int used=dec_dc_vlc(fdata,bp,&dv,de*8);
                    if(used<0){ok=0;break;}dc_pred[comp]+=dv;bp+=used;
                }
            if(!ok) continue;
            int ac_start=bp;
            int ac_end=de*8;
            /* Decode with EOB */
            int nz=0,eobs=0;
            for(int blk=0;blk<864&&bp<ac_end;blk++){
                for(int pos=0;pos<63;pos++){
                    if(bp>=ac_end)goto next;
                    int av;int used=dec_dc_vlc(fdata,bp,&av,ac_end);
                    if(used<0){bp++;continue;}
                    bp+=used;
                    if(av==0){eobs++;break;}
                    nz++;
                }
            }
            next:
            printf("  %-40s LBA~%4d QS=%2d t=%d: %5d/%5d (%.1f%%) NZ=%d EOB=%d avg=%.1f\n",
                   gn, test_lbas[li], qs, ft,
                   bp-ac_start, ac_end-ac_start,
                   100.0*(bp-ac_start)/(ac_end-ac_start),
                   nz, eobs, eobs>0?(float)nz/eobs:0);
        }
    }

    printf("\nDone.\n");
    return 0;
}
