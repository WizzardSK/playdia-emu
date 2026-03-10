/*
 * vcodec_bandwise.c - Test band-by-band AC organization
 *
 * HYPOTHESIS: AC data is organized as:
 *   [all 864 blocks' AC[1]] [all 864 blocks' AC[2]] ... [all 864 blocks' AC[63]]
 * instead of:
 *   [block0's 63 AC values] [block1's 63 AC values] ...
 *
 * If band-by-band, reading with Flag+SM per-value in band order should
 * produce meaningful images (not noise), while block-by-block would produce noise.
 *
 * Tests multiple value codings:
 * - Flag+SM(1): 0=zero, 1+sign = ±1
 * - Flag+SM(2): 0=zero, 1+sign+mag = ±1..±2
 * - Flag+DCVLC: 0=zero, 1+DCVLC = variable size value
 * - Per-value DCVLC (no flag): each value coded directly as DCVLC
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

static const int zigzag[64]={
    0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
   12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
   35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
   58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

static int clamp(int v) { return v<0?0:v>255?255:v; }

static void idct8x8(const int block[64], int out[64]) {
    double tmp[64];
    for(int y=0;y<8;y++)
        for(int x=0;x<8;x++){
            double sum=0;
            for(int u=0;u<8;u++){
                double cu=(u==0)?1.0/sqrt(2.0):1.0;
                sum+=cu*block[y*8+u]*cos((2*x+1)*u*M_PI/16.0);
            }
            tmp[y*8+x]=sum*0.5;
        }
    for(int x=0;x<8;x++)
        for(int y=0;y<8;y++){
            double sum=0;
            for(int v=0;v<8;v++){
                double cv=(v==0)?1.0/sqrt(2.0):1.0;
                sum+=cv*tmp[v*8+x]*cos((2*y+1)*v*M_PI/16.0);
            }
            out[y*8+x]=(int)round(sum*0.5)+128;
        }
}

static void write_ppm(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f=fopen(path,"wb");
    fprintf(f,"P6\n%d %d\n255\n",w,h);
    fwrite(rgb,1,w*h*3,f);
    fclose(f);
    printf("  Wrote %s\n", path);
}

static int dequant_ac(int val, int zz_pos, int qs, const uint8_t *qt) {
    if(val == 0 || zz_pos == 0) return 0;
    int q = qt[(zz_pos-1) % 16];
    return val * qs * q / 8;
}

static void render_blocks(int blocks[][64], int dc_diffs[], int qs, const uint8_t *qt,
                          uint8_t *rgb) {
    int y_plane[HEIGHT][WIDTH], cb_plane[HEIGHT/2][WIDTH/2], cr_plane[HEIGHT/2][WIDTH/2];
    memset(y_plane,0,sizeof(y_plane));
    memset(cb_plane,0,sizeof(cb_plane));
    memset(cr_plane,0,sizeof(cr_plane));

    int dc_pred[3]={0,0,0};
    for(int bidx=0;bidx<NBLK;bidx++){
        int comp=(bidx%6<4)?0:(bidx%6==4)?1:2;
        dc_pred[comp]+=dc_diffs[bidx];
        blocks[bidx][0]=dc_pred[comp]*8;

        /* Dequantize AC */
        for(int pos=1;pos<64;pos++){
            int zz_pos = -1;
            /* Find which zigzag position maps to this spatial position */
            /* Actually, blocks[bidx][pos] is already in spatial position */
            /* We need to know the zigzag index for dequant */
        }

        /* Actually, let's dequant in the place where we set the values */
        int pixels[64];
        idct8x8(blocks[bidx], pixels);

        int mb=bidx/6, blk=bidx%6;
        int mby=mb/MB_W, mbx=mb%MB_W;
        if(blk<4){
            int bx=(blk&1)*8, by=(blk>>1)*8;
            int px=mbx*16+bx, py=mby*16+by;
            for(int y=0;y<8;y++)for(int x=0;x<8;x++)
                if(py+y<HEIGHT&&px+x<WIDTH)
                    y_plane[py+y][px+x]=clamp(pixels[y*8+x]);
        } else if(blk==4){
            int px=mbx*8,py=mby*8;
            for(int y=0;y<8;y++)for(int x=0;x<8;x++)
                if(py+y<HEIGHT/2&&px+x<WIDTH/2)
                    cb_plane[py+y][px+x]=clamp(pixels[y*8+x]);
        } else {
            int px=mbx*8,py=mby*8;
            for(int y=0;y<8;y++)for(int x=0;x<8;x++)
                if(py+y<HEIGHT/2&&px+x<WIDTH/2)
                    cr_plane[py+y][px+x]=clamp(pixels[y*8+x]);
        }
    }

    for(int y=0;y<HEIGHT;y++)
        for(int x=0;x<WIDTH;x++){
            int Y=y_plane[y][x],Cb=cb_plane[y/2][x/2],Cr=cr_plane[y/2][x/2];
            rgb[(y*WIDTH+x)*3]=clamp(Y+(int)(1.402*(Cr-128)));
            rgb[(y*WIDTH+x)*3+1]=clamp(Y-(int)(0.344136*(Cb-128))-(int)(0.714136*(Cr-128)));
            rgb[(y*WIDTH+x)*3+2]=clamp(Y+(int)(1.772*(Cb-128)));
        }
}

int main() {
    const char *binfile = "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin";
    const char *outdir = "/home/wizzard/share/GitHub/playdia-emu/output";

    if(load_frame(binfile, 502, 0)!=0){printf("Failed\n");return 1;}

    int qs=fbuf[3];
    printf("Frame: QS=%d, type=%d\n", qs, fbuf[39]);

    int de=flen; while(de>0&&fbuf[de-1]==0xFF)de--;
    int total_bits=(de-40)*8;

    int dc_diffs[NBLK];
    int bp=0;
    for(int b=0;b<NBLK;b++){
        int dv;int c=dec_dc(fbuf+40,bp,&dv,total_bits);
        if(c<0){printf("DC fail at %d\n",b);return 1;}
        bp+=c;
        dc_diffs[b]=dv;
    }
    int dc_bits=bp, ac_bits=total_bits-dc_bits;
    printf("DC=%d bits, AC=%d bits\n\n", dc_bits, ac_bits);

    uint8_t rgb[WIDTH*HEIGHT*3];

    /* Allocate block data: blocks[block_idx][spatial_pos] */
    int (*blocks)[64] = calloc(NBLK, sizeof(int[64]));

    /* === Test 1: BAND-WISE Flag+SM(2): 0=zero, 1+2bits=±1..±2 === */
    printf("--- BAND-WISE Flag+SM(2) ---\n");
    memset(blocks, 0, NBLK * sizeof(int[64]));
    {
        int acbp = dc_bits;
        int bands_coded = 0;
        int total_nz = 0;

        for(int band = 1; band < 64 && acbp < total_bits; band++) {
            int band_nz = 0;
            for(int b = 0; b < NBLK && acbp < total_bits; b++) {
                int flag = get_bit(fbuf+40, acbp); acbp++;
                if(flag) {
                    if(acbp + 2 > total_bits) break;
                    int sign = get_bit(fbuf+40, acbp); acbp++;
                    int mag = get_bit(fbuf+40, acbp) + 1; acbp++;
                    int val = sign ? -mag : mag;
                    blocks[b][zigzag[band]] = dequant_ac(val, band, qs, fbuf+4);
                    band_nz++;
                }
            }
            total_nz += band_nz;
            bands_coded = band;
        }
        printf("Bands coded: %d, NZ=%d, AC used: %d/%d (%.1f%%)\n",
               bands_coded, total_nz, acbp-dc_bits, ac_bits,
               100.0*(acbp-dc_bits)/ac_bits);

        render_blocks(blocks, dc_diffs, qs, fbuf+4, rgb);
        char path[512]; snprintf(path,sizeof(path),"%s/lba502_f0_bandwise_fsm2.ppm",outdir);
        write_ppm(path, rgb, WIDTH, HEIGHT);
    }

    /* === Test 2: BAND-WISE DC VLC per value === */
    printf("\n--- BAND-WISE DC VLC per value ---\n");
    memset(blocks, 0, NBLK * sizeof(int[64]));
    {
        int acbp = dc_bits;
        int bands_coded = 0;
        int total_nz = 0;

        for(int band = 1; band < 64 && acbp < total_bits; band++) {
            int band_nz = 0;
            for(int b = 0; b < NBLK; b++) {
                int val;
                int c = dec_dc(fbuf+40, acbp, &val, total_bits);
                if(c < 0) goto done_vlc;
                acbp += c;
                if(val != 0) {
                    blocks[b][zigzag[band]] = dequant_ac(val, band, qs, fbuf+4);
                    band_nz++;
                }
            }
            total_nz += band_nz;
            bands_coded = band;
        }
        done_vlc:
        printf("Bands coded: %d, NZ=%d, AC used: %d/%d (%.1f%%)\n",
               bands_coded, total_nz, acbp-dc_bits, ac_bits,
               100.0*(acbp-dc_bits)/ac_bits);

        render_blocks(blocks, dc_diffs, qs, fbuf+4, rgb);
        char path[512]; snprintf(path,sizeof(path),"%s/lba502_f0_bandwise_dcvlc.ppm",outdir);
        write_ppm(path, rgb, WIDTH, HEIGHT);
    }

    /* === Test 3: BAND-WISE Flag + DC VLC value === */
    printf("\n--- BAND-WISE Flag + DC VLC value ---\n");
    memset(blocks, 0, NBLK * sizeof(int[64]));
    {
        int acbp = dc_bits;
        int bands_coded = 0;
        int total_nz = 0;

        for(int band = 1; band < 64 && acbp < total_bits; band++) {
            int band_nz = 0;
            for(int b = 0; b < NBLK && acbp < total_bits; b++) {
                int flag = get_bit(fbuf+40, acbp); acbp++;
                if(flag) {
                    int val;
                    int c = dec_dc(fbuf+40, acbp, &val, total_bits);
                    if(c < 0) goto done_fvlc;
                    acbp += c;
                    if(val != 0) {
                        blocks[b][zigzag[band]] = dequant_ac(val, band, qs, fbuf+4);
                        band_nz++;
                    }
                }
            }
            total_nz += band_nz;
            bands_coded = band;
        }
        done_fvlc:
        printf("Bands coded: %d, NZ=%d, AC used: %d/%d (%.1f%%)\n",
               bands_coded, total_nz, acbp-dc_bits, ac_bits,
               100.0*(acbp-dc_bits)/ac_bits);

        render_blocks(blocks, dc_diffs, qs, fbuf+4, rgb);
        char path[512]; snprintf(path,sizeof(path),"%s/lba502_f0_bandwise_fvlc.ppm",outdir);
        write_ppm(path, rgb, WIDTH, HEIGHT);
    }

    /* === Test 4: BLOCK-WISE Flag+SM(2) for comparison === */
    printf("\n--- BLOCK-WISE Flag+SM(2) (for comparison) ---\n");
    memset(blocks, 0, NBLK * sizeof(int[64]));
    {
        int acbp = dc_bits;
        int total_nz = 0;

        for(int b = 0; b < NBLK && acbp < total_bits; b++) {
            for(int pos = 1; pos < 64 && acbp < total_bits; pos++) {
                int flag = get_bit(fbuf+40, acbp); acbp++;
                if(flag) {
                    if(acbp + 2 > total_bits) break;
                    int sign = get_bit(fbuf+40, acbp); acbp++;
                    int mag = get_bit(fbuf+40, acbp) + 1; acbp++;
                    int val = sign ? -mag : mag;
                    blocks[b][zigzag[pos]] = dequant_ac(val, pos, qs, fbuf+4);
                    total_nz++;
                }
            }
        }
        printf("NZ=%d, AC used: %d/%d (%.1f%%)\n",
               total_nz, acbp-dc_bits, ac_bits,
               100.0*(acbp-dc_bits)/ac_bits);

        render_blocks(blocks, dc_diffs, qs, fbuf+4, rgb);
        char path[512]; snprintf(path,sizeof(path),"%s/lba502_f0_blockwise_fsm2.ppm",outdir);
        write_ppm(path, rgb, WIDTH, HEIGHT);
    }

    /* === Test 5: BAND-WISE with DC VLC but SEPARATE DPCM per band === */
    printf("\n--- BAND-WISE DC VLC with DPCM per band ---\n");
    memset(blocks, 0, NBLK * sizeof(int[64]));
    {
        int acbp = dc_bits;
        int bands_coded = 0;
        int total_nz = 0;

        for(int band = 1; band < 64 && acbp < total_bits; band++) {
            int pred = 0;
            int band_nz = 0;
            for(int b = 0; b < NBLK; b++) {
                int dv;
                int c = dec_dc(fbuf+40, acbp, &dv, total_bits);
                if(c < 0) goto done_dpcm;
                acbp += c;
                pred += dv;
                if(pred != 0) {
                    blocks[b][zigzag[band]] = dequant_ac(pred, band, qs, fbuf+4);
                    band_nz++;
                }
            }
            total_nz += band_nz;
            bands_coded = band;
        }
        done_dpcm:
        printf("Bands coded: %d, NZ=%d, AC used: %d/%d (%.1f%%)\n",
               bands_coded, total_nz, acbp-dc_bits, ac_bits,
               100.0*(acbp-dc_bits)/ac_bits);

        render_blocks(blocks, dc_diffs, qs, fbuf+4, rgb);
        char path[512]; snprintf(path,sizeof(path),"%s/lba502_f0_bandwise_dpcm.ppm",outdir);
        write_ppm(path, rgb, WIDTH, HEIGHT);
    }

    free(blocks);
    printf("\nDone. Check images in %s/\n", outdir);
    return 0;
}
