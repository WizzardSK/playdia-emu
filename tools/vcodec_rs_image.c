/*
 * vcodec_rs_image.c - Test (run, size) AC coding candidates and generate images
 *
 * Candidates: r2+s4 and r3+s4 both hit 100% consumption on multiple frames
 * Test: sparse frame, random data, distribution analysis, image generation
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

/* JPEG zigzag order */
static const int zigzag[64] = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
    12,19,26,33,40,48,41,34,27,20,13,6,7,14,
    21,28,35,42,49,56,57,50,43,36,29,22,15,
    23,30,37,44,51,58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};

/* Standard JPEG quantization matrix (scaled) */
static const uint8_t jpeg_qt[64] = {
    16,11,10,16,24,40,51,61,12,12,14,19,26,58,60,55,
    14,13,16,24,40,57,69,56,14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77,24,35,55,64,81,104,113,92,
    49,64,78,87,103,121,120,101,72,92,95,98,112,100,103,99
};

/* Decode frame with (run_bits, size_bits) scheme and output block data */
static int decode_rs(int rbits, int sbits, int dc_start,
                     int block_coeff[864][64], int tb)
{
    int bp = dc_start;
    int dc_pred[3]={0,0,0};

    for(int mb=0; mb<144 && bp<tb; mb++){
        for(int bl=0; bl<6 && bp<tb; bl++){
            int blk_idx = mb*6+bl;
            int comp = (bl<4)?0:(bl==4)?1:2;

            /* DC */
            int dv;
            int used = dec_vlc(fdata, bp, &dv, tb);
            if(used<0) return blk_idx;
            dc_pred[comp] += dv;
            bp += used;

            memset(block_coeff[blk_idx], 0, sizeof(int)*64);
            block_coeff[blk_idx][0] = dc_pred[comp];

            /* AC with (run, size) */
            int pos = 1;
            while(pos < 64 && bp+rbits+sbits <= tb){
                int run = get_bits(fdata, bp, rbits);
                int sz  = get_bits(fdata, bp+rbits, sbits);
                bp += rbits + sbits;
                if(run==0 && sz==0) break; /* EOB */
                pos += run;
                if(pos >= 64) break;
                if(sz > 0){
                    if(bp+sz > tb) return blk_idx;
                    uint32_t mag = get_bits(fdata, bp, sz);
                    bp += sz;
                    int val = (mag < (1u<<(sz-1))) ? (int)mag-(1<<sz)+1 : (int)mag;
                    block_coeff[blk_idx][pos] = val;
                }
                pos++;
            }
        }
    }
    return 864;
}

/* Write PPM image */
static void write_ppm(const char *fname, uint8_t *rgb, int w, int h) {
    FILE *fp = fopen(fname, "wb");
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, w*h*3, fp);
    fclose(fp);
}

static void idct8x8(int coeff[64], int out[64]) {
    double tmp[64];
    /* Row IDCT */
    for(int y=0;y<8;y++){
        for(int x=0;x<8;x++){
            double sum=0;
            for(int u=0;u<8;u++){
                double cu = (u==0) ? 1.0/sqrt(2.0) : 1.0;
                sum += cu * coeff[y*8+u] * cos((2*x+1)*u*M_PI/16.0);
            }
            tmp[y*8+x] = sum * 0.5;
        }
    }
    /* Column IDCT */
    for(int x=0;x<8;x++){
        for(int y=0;y<8;y++){
            double sum=0;
            for(int v=0;v<8;v++){
                double cv = (v==0) ? 1.0/sqrt(2.0) : 1.0;
                sum += cv * tmp[v*8+x] * cos((2*y+1)*v*M_PI/16.0);
            }
            out[y*8+x] = (int)(sum * 0.5 + 128.5); /* +128 for DC level shift */
        }
    }
}

static int clamp(int v) { return v<0?0:v>255?255:v; }

int main() {
    const char *binfile = "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin";

    printf("=== Run-Size Candidate Validation ===\n\n");

    /* Test on multiple frames including sparse */
    struct { const char *file; int lba; const char *name; } tests[] = {
        {binfile, 148, "Mari148"},
        {binfile, 500, "Mari500"},
        {binfile, 755, "Mari755"},
        {binfile, 1100, "Mari1100"},
        {"/home/wizzard/share/GitHub/playdia-roms/Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku - Chikyuu-Hen (Japan) (Track 2).bin", 500, "DBZ500"},
        {NULL, 0, NULL}
    };

    int rbits_list[] = {2, 3};

    for(int ri=0; ri<2; ri++){
        int rbits=rbits_list[ri], sbits=4;
        printf("=== Testing r%d+s%d ===\n", rbits, sbits);

        for(int ti=0; tests[ti].file; ti++){
            if(!load_frame(tests[ti].file, tests[ti].lba)) continue;
            int de=fdatalen; while(de>0&&fdata[de-1]==0xFF)de--;
            int tb=de*8, pad=fdatalen-de;

            int (*bc)[64] = calloc(864, sizeof(int[64]));
            int decoded = decode_rs(rbits, sbits, 40*8, bc, tb);

            /* Statistics */
            int total_nz=0, total_eob_pos=0;
            int run_hist[64]={0}, sz_hist[16]={0};
            for(int b=0;b<decoded;b++){
                int last_nz=0;
                for(int p=1;p<64;p++){
                    if(bc[b][p]!=0){total_nz++;last_nz=p;}
                }
                total_eob_pos+=last_nz;
            }

            printf("  %s: %d blk pad=%d NZ=%d avg_nz=%.1f avg_last=%.1f\n",
                   tests[ti].name, decoded, pad, total_nz,
                   (float)total_nz/decoded, (float)total_eob_pos/decoded);

            /* Value distribution of AC coefficients */
            int val_hist[21]={0}; /* -10..0..+10 */
            int large=0;
            for(int b=0;b<decoded;b++){
                for(int p=1;p<64;p++){
                    int v=bc[b][p];
                    if(v==0) continue;
                    if(v>=-10 && v<=10) val_hist[v+10]++;
                    else large++;
                }
            }
            if(ti==0){ /* Only print once */
                printf("  Val dist [-10..+10]: ");
                for(int i=0;i<21;i++) if(val_hist[i]) printf("%d=%d ", i-10, val_hist[i]);
                printf("large=%d\n", large);
            }

            free(bc);
        }
        printf("\n");
    }

    /* Now test on sparse frame from Ie Naki Ko */
    printf("=== Sparse frame test ===\n");
    {
        const char *ienaki = "/home/wizzard/share/GitHub/playdia-roms/Ie Naki Ko - Suzu no Sentaku (Japan) (Track 2).bin";
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
                        if(fl-de2>best_pad){best_pad=fl-de2;memcpy(best,tmpf,fl);}
                    }
                    f1c=0;
                } else f1c=0;
            }
            fclose(fp);

            if(best_pad>100){
                memcpy(fdata,best,6*2047); fdatalen=6*2047;
                int de=fdatalen;while(de>0&&fdata[de-1]==0xFF)de--;
                int tb=de*8;
                printf("Sparse frame: pad=%d data=%d bytes (%.1f bits/blk)\n",
                       best_pad, de, (float)(tb-40*8)/864);

                for(int rbits=2; rbits<=3; rbits++){
                    int (*bc)[64] = calloc(864, sizeof(int[64]));
                    int decoded = decode_rs(rbits, 4, 40*8, bc, tb);
                    int total_nz=0;
                    for(int b=0;b<decoded;b++)
                        for(int p=1;p<64;p++)
                            if(bc[b][p]!=0) total_nz++;
                    printf("  r%d+s4: %d blk NZ=%d avg=%.1f\n",
                           rbits, decoded, total_nz, (float)total_nz/decoded);
                    free(bc);
                }
            }
        }
    }

    /* Random data comparison */
    printf("\n=== Random data comparison ===\n");
    {
        srand(42);
        for(int i=0;i<16384;i++) fdata[i]=rand()&0xFF;
        fdatalen=12282;
        int tb=12282*8;
        for(int rbits=2; rbits<=3; rbits++){
            int (*bc)[64] = calloc(864, sizeof(int[64]));
            int decoded = decode_rs(rbits, 4, 40*8, bc, tb);
            int total_nz=0;
            for(int b=0;b<decoded;b++)
                for(int p=1;p<64;p++)
                    if(bc[b][p]!=0) total_nz++;
            printf("  RANDOM r%d+s4: %d blk NZ=%d avg=%.1f\n",
                   rbits, decoded, total_nz, (float)total_nz/decoded);
            free(bc);
        }
    }

    /* Generate images */
    printf("\n=== Generating images ===\n");
    if(load_frame(binfile, 500)){
        int de=fdatalen; while(de>0&&fdata[de-1]==0xFF)de--;
        int tb=de*8;
        uint8_t qt[16];
        memcpy(qt, fdata+4, 16);
        int qs = fdata[3];

        for(int rbits=2; rbits<=3; rbits++){
            int (*bc)[64] = calloc(864, sizeof(int[64]));
            int decoded = decode_rs(rbits, 4, 40*8, bc, tb);

            /* Build image: 256x144 */
            int Y[144][256]={{0}}, Cb[72][128]={{0}}, Cr[72][128]={{0}};

            for(int mb=0; mb<144; mb++){
                int mx = mb%16, my = mb/16;
                for(int bl=0; bl<6 && mb*6+bl<decoded; bl++){
                    int idx = mb*6+bl;
                    /* Dequantize and IDCT */
                    int dq[64], px[64];
                    /* Just do simple dequant with qtable */
                    for(int i=0;i<64;i++){
                        int qi = (i<16) ? qt[i] : qt[i%16]; /* extend qtable */
                        dq[zigzag[i]] = bc[idx][i] * qi * qs;
                    }
                    dq[0] = bc[idx][0] * 8; /* DC: simple scaling */

                    idct8x8(dq, px);

                    if(bl<4){
                        int bx = mx*16 + (bl&1)*8;
                        int by = my*16 + (bl>>1)*8;
                        for(int y=0;y<8;y++)
                            for(int x=0;x<8;x++)
                                if(by+y<144 && bx+x<256)
                                    Y[by+y][bx+x] = clamp(px[y*8+x]);
                    } else if(bl==4){
                        int bx = mx*8, by = my*8;
                        for(int y=0;y<8;y++)
                            for(int x=0;x<8;x++)
                                if(by+y<72 && bx+x<128)
                                    Cb[by+y][bx+x] = clamp(px[y*8+x]);
                    } else {
                        int bx = mx*8, by = my*8;
                        for(int y=0;y<8;y++)
                            for(int x=0;x<8;x++)
                                if(by+y<72 && bx+x<128)
                                    Cr[by+y][bx+x] = clamp(px[y*8+x]);
                    }
                }
            }

            /* YCbCr→RGB */
            uint8_t rgb[144*256*3];
            for(int y=0;y<144;y++){
                for(int x=0;x<256;x++){
                    int yv = Y[y][x];
                    int cb = Cb[y/2][x/2] - 128;
                    int cr = Cr[y/2][x/2] - 128;
                    rgb[(y*256+x)*3+0] = clamp(yv + 1.402*cr);
                    rgb[(y*256+x)*3+1] = clamp(yv - 0.344*cb - 0.714*cr);
                    rgb[(y*256+x)*3+2] = clamp(yv + 1.772*cb);
                }
            }

            char fname[256];
            snprintf(fname,sizeof(fname),"output/lba502_r%ds4_dq.ppm", rbits);
            write_ppm(fname, rgb, 256, 144);
            printf("  Wrote %s\n", fname);

            /* Also try DC-only for comparison (no dequant on AC, just DC) */
            memset(Y, 0, sizeof(Y));
            memset(Cb, 0, sizeof(Cb));
            memset(Cr, 0, sizeof(Cr));
            for(int mb=0; mb<144; mb++){
                int mx=mb%16, my=mb/16;
                for(int bl=0; bl<6 && mb*6+bl<decoded; bl++){
                    int idx=mb*6+bl;
                    int dc_val = bc[idx][0] * 8 + 1024; /* Shift to unsigned range */
                    int px_val = clamp(dc_val >> 3); /* Scale down */
                    if(bl<4){
                        int bx=mx*16+(bl&1)*8, by=my*16+(bl>>1)*8;
                        for(int y=0;y<8;y++)
                            for(int x=0;x<8;x++)
                                if(by+y<144&&bx+x<256) Y[by+y][bx+x]=px_val;
                    } else if(bl==4){
                        int bx=mx*8, by=my*8;
                        for(int y=0;y<8;y++)
                            for(int x=0;x<8;x++)
                                if(by+y<72&&bx+x<128) Cb[by+y][bx+x]=px_val;
                    } else {
                        int bx=mx*8, by=my*8;
                        for(int y=0;y<8;y++)
                            for(int x=0;x<8;x++)
                                if(by+y<72&&bx+x<128) Cr[by+y][bx+x]=px_val;
                    }
                }
            }
            for(int y=0;y<144;y++)
                for(int x=0;x<256;x++){
                    int yv=Y[y][x],cb=Cb[y/2][x/2]-128,cr=Cr[y/2][x/2]-128;
                    rgb[(y*256+x)*3+0]=clamp(yv+1.402*cr);
                    rgb[(y*256+x)*3+1]=clamp(yv-0.344*cb-0.714*cr);
                    rgb[(y*256+x)*3+2]=clamp(yv+1.772*cb);
                }
            snprintf(fname,sizeof(fname),"output/lba502_r%ds4_dconly.ppm", rbits);
            write_ppm(fname, rgb, 256, 144);
            printf("  Wrote %s (DC-only for comparison)\n", fname);

            /* Generate image WITHOUT dequant (raw coefficients as IDCT input) */
            memset(Y, 0, sizeof(Y));
            memset(Cb, 0, sizeof(Cb));
            memset(Cr, 0, sizeof(Cr));
            for(int mb=0; mb<144; mb++){
                int mx=mb%16, my=mb/16;
                for(int bl=0; bl<6 && mb*6+bl<decoded; bl++){
                    int idx=mb*6+bl;
                    int raw[64], px[64];
                    for(int i=0;i<64;i++) raw[zigzag[i]] = bc[idx][i];
                    raw[0] = bc[idx][0] * 8;
                    idct8x8(raw, px);
                    if(bl<4){
                        int bx=mx*16+(bl&1)*8, by=my*16+(bl>>1)*8;
                        for(int y=0;y<8;y++)
                            for(int x=0;x<8;x++)
                                if(by+y<144&&bx+x<256) Y[by+y][bx+x]=clamp(px[y*8+x]);
                    } else if(bl==4){
                        int bx=mx*8, by=my*8;
                        for(int y=0;y<8;y++)
                            for(int x=0;x<8;x++)
                                if(by+y<72&&bx+x<128) Cb[by+y][bx+x]=clamp(px[y*8+x]);
                    } else {
                        int bx=mx*8, by=my*8;
                        for(int y=0;y<8;y++)
                            for(int x=0;x<8;x++)
                                if(by+y<72&&bx+x<128) Cr[by+y][bx+x]=clamp(px[y*8+x]);
                    }
                }
            }
            for(int y=0;y<144;y++)
                for(int x=0;x<256;x++){
                    int yv=Y[y][x],cb=Cb[y/2][x/2]-128,cr=Cr[y/2][x/2]-128;
                    rgb[(y*256+x)*3+0]=clamp(yv+1.402*cr);
                    rgb[(y*256+x)*3+1]=clamp(yv-0.344*cb-0.714*cr);
                    rgb[(y*256+x)*3+2]=clamp(yv+1.772*cb);
                }
            snprintf(fname,sizeof(fname),"output/lba502_r%ds4_raw.ppm", rbits);
            write_ppm(fname, rgb, 256, 144);
            printf("  Wrote %s (raw, no dequant)\n", fname);

            free(bc);
        }
    }

    printf("\nDone.\n");
    return 0;
}
