/*
 * vcodec_image.c - Produce actual decoded images to visually verify AC coding
 *
 * Strategy:
 * 1. Find actual game content (skip Bandai logo at LBA 150)
 * 2. Decode DC-only image as baseline
 * 3. Try promising AC schemes and output PPM images for comparison
 * 4. Use padded frames where exact bit boundary is known
 * 5. Check if AC adds meaningful detail or noise
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

/* Standard 8x8 IDCT */
static void idct8x8(const int block[64], int out[64]) {
    double tmp[64];
    /* Row pass */
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
    /* Column pass */
    for(int x=0;x<8;x++){
        for(int y=0;y<8;y++){
            double sum=0;
            for(int v=0;v<8;v++){
                double cv=(v==0)?1.0/sqrt(2.0):1.0;
                sum+=cv*tmp[v*8+x]*cos((2*y+1)*v*M_PI/16.0);
            }
            out[y*8+x]=(int)round(sum*0.5)+128;
        }
    }
}

/* Zigzag scan order */
static const int zigzag[64]={
    0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
   12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
   35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
   58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

/* Quantization table from header */
static const int qtable[64]={
    10,20,14,13,18,37,22,28,
    15,24,15,18,18,31,17,20,
    10,20,14,13,18,37,22,28,  /* repeated in 8x8 pattern */
    15,24,15,18,18,31,17,20,
    10,20,14,13,18,37,22,28,
    15,24,15,18,18,31,17,20,
    10,20,14,13,18,37,22,28,
    15,24,15,18,18,31,17,20
};

/* Actually the qtable from header is 16 bytes = one row of 8x8 zigzag? */
/* Let me use it more carefully: header has 16 bytes at offset 4 */
static int qtable_from_header[64];

static void build_qtable(const uint8_t *hdr) {
    /* The 16-byte qtable might be: first 16 zigzag positions */
    /* Or it might be: one 4x4 subblock repeated */
    /* For now, just use the 16 values cycling */
    for(int i=0;i<64;i++){
        qtable_from_header[i] = hdr[4 + (i % 16)];
    }
}

static void dequantize(int block[64], int qs) {
    /* block[0] is DC, already in correct scale (×8) */
    /* AC coefficients: level = (2×|val|+1) × qs × qtable[i] / 32 */
    /* Actually for intra: level = val × qs × qtable[i] / 8 (MPEG-1 intra) */
    for(int i=1;i<64;i++){
        if(block[i]==0) continue;
        int q = qtable_from_header[zigzag[i]];
        /* MPEG-1 intra dequant: (2*level+sign)*qs*W/32, but simplified: */
        block[i] = block[i] * qs * q / 8;
    }
}

/* Clamp to 0-255 */
static int clamp(int v) { return v<0?0:v>255?255:v; }

/* BT.601 YCbCr to RGB */
static void ycbcr_to_rgb(int y, int cb, int cr, uint8_t *r, uint8_t *g, uint8_t *b) {
    int ri = y + (int)(1.402 * (cr - 128));
    int gi = y - (int)(0.344136 * (cb - 128)) - (int)(0.714136 * (cr - 128));
    int bi = y + (int)(1.772 * (cb - 128));
    *r = clamp(ri); *g = clamp(gi); *b = clamp(bi);
}

/* Write PPM image */
static void write_ppm(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, w*h*3, f);
    fclose(f);
    printf("Wrote %s\n", path);
}

/* Load frame from disc */
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
        else {f1c=0;}
    }
    fclose(fp); return -1;
}

/* DC-only decode: produce image using only DC coefficients */
static void decode_dc_only(const uint8_t *fdata, int data_bytes, uint8_t *rgb_out) {
    int total_bits = data_bytes * 8;
    int dc_pred[3] = {0, 0, 0}; /* Y, Cb, Cr predictors */
    int y_plane[HEIGHT][WIDTH], cb_plane[HEIGHT/2][WIDTH/2], cr_plane[HEIGHT/2][WIDTH/2];
    memset(y_plane, 128, sizeof(y_plane));
    memset(cb_plane, 128, sizeof(cb_plane));
    memset(cr_plane, 128, sizeof(cr_plane));

    int bp = 0;
    for(int mby=0; mby<MB_H; mby++){
        for(int mbx=0; mbx<MB_W; mbx++){
            for(int blk=0; blk<6; blk++){
                int dv;
                int c = dec_dc(fdata+40, bp, &dv, total_bits);
                if(c<0) return;
                bp += c;

                int comp = (blk<4) ? 0 : (blk==4) ? 1 : 2;
                dc_pred[comp] += dv;
                int dc_val = clamp(dc_pred[comp] * 8 + 128);

                if(blk < 4){
                    /* Y block: TL(0), TR(1), BL(2), BR(3) */
                    int bx = (blk & 1) * 8;
                    int by = (blk >> 1) * 8;
                    int px = mbx*16 + bx;
                    int py = mby*16 + by;
                    for(int y=0;y<8;y++)
                        for(int x=0;x<8;x++)
                            if(py+y<HEIGHT && px+x<WIDTH)
                                y_plane[py+y][px+x] = dc_val;
                } else if(blk == 4){
                    int px = mbx*8, py = mby*8;
                    for(int y=0;y<8;y++)
                        for(int x=0;x<8;x++)
                            if(py+y<HEIGHT/2 && px+x<WIDTH/2)
                                cb_plane[py+y][px+x] = dc_val;
                } else {
                    int px = mbx*8, py = mby*8;
                    for(int y=0;y<8;y++)
                        for(int x=0;x<8;x++)
                            if(py+y<HEIGHT/2 && px+x<WIDTH/2)
                                cr_plane[py+y][px+x] = dc_val;
                }
            }
        }
    }

    /* Convert to RGB */
    for(int y=0;y<HEIGHT;y++){
        for(int x=0;x<WIDTH;x++){
            int Y = y_plane[y][x];
            int Cb = cb_plane[y/2][x/2];
            int Cr = cr_plane[y/2][x/2];
            ycbcr_to_rgb(Y, Cb, Cr, &rgb_out[(y*WIDTH+x)*3],
                        &rgb_out[(y*WIDTH+x)*3+1], &rgb_out[(y*WIDTH+x)*3+2]);
        }
    }
}

/* Full decode with AC using flag+signmag scheme */
/* 0=zero, 1+N bits=nonzero (sign + magnitude) */
static int decode_full_flag_sm(const uint8_t *fdata, int data_bytes, int vbits, uint8_t *rgb_out) {
    int total_bits = data_bytes * 8;
    int dc_pred[3] = {0, 0, 0};
    int y_plane[HEIGHT][WIDTH], cb_plane[HEIGHT/2][WIDTH/2], cr_plane[HEIGHT/2][WIDTH/2];
    memset(y_plane, 0, sizeof(y_plane));
    memset(cb_plane, 0, sizeof(cb_plane));
    memset(cr_plane, 0, sizeof(cr_plane));

    int qs = fdata[3];
    build_qtable(fdata);

    /* First pass: decode all DC */
    int bp = 0;
    int dc_vals[NBLK];
    for(int b=0;b<NBLK;b++){
        int dv;
        int c=dec_dc(fdata+40,bp,&dv,total_bits);
        if(c<0) return -1;
        bp+=c;
        dc_vals[b]=dv;
    }
    int ac_start = bp;

    /* Second pass: decode AC with flag+signmag */
    int blk_idx = 0;
    for(int mby=0; mby<MB_H; mby++){
        for(int mbx=0; mbx<MB_W; mbx++){
            for(int blk=0; blk<6; blk++){
                int block[64];
                memset(block, 0, sizeof(block));

                int comp = (blk<4) ? 0 : (blk==4) ? 1 : 2;
                dc_pred[comp] += dc_vals[blk_idx];
                block[0] = dc_pred[comp] * 8;

                /* Decode AC */
                for(int pos=1;pos<64;pos++){
                    if(bp >= total_bits) break;
                    int flag = get_bit(fdata+40, bp); bp++;
                    if(flag == 0) continue; /* zero */
                    if(bp + vbits > total_bits) break;
                    int val;
                    if(vbits == 1){
                        val = get_bit(fdata+40, bp) ? -1 : 1;
                        bp += 1;
                    } else {
                        int sign = get_bit(fdata+40, bp); bp++;
                        int mag = get_bits(fdata+40, bp, vbits-1); bp += vbits-1;
                        val = mag + 1;
                        if(sign) val = -val;
                    }
                    block[zigzag[pos]] = val;
                }

                /* Dequantize AC */
                dequantize(block, qs);

                /* IDCT */
                int pixels[64];
                idct8x8(block, pixels);

                /* Store in plane */
                if(blk < 4){
                    int bx = (blk & 1) * 8;
                    int by = (blk >> 1) * 8;
                    int px = mbx*16 + bx;
                    int py = mby*16 + by;
                    for(int y=0;y<8;y++)
                        for(int x=0;x<8;x++)
                            if(py+y<HEIGHT && px+x<WIDTH)
                                y_plane[py+y][px+x] = clamp(pixels[y*8+x]);
                } else if(blk == 4){
                    int px = mbx*8, py = mby*8;
                    for(int y=0;y<8;y++)
                        for(int x=0;x<8;x++)
                            if(py+y<HEIGHT/2 && px+x<WIDTH/2)
                                cb_plane[py+y][px+x] = clamp(pixels[y*8+x]);
                } else {
                    int px = mbx*8, py = mby*8;
                    for(int y=0;y<8;y++)
                        for(int x=0;x<8;x++)
                            if(py+y<HEIGHT/2 && px+x<WIDTH/2)
                                cr_plane[py+y][px+x] = clamp(pixels[y*8+x]);
                }
                blk_idx++;
            }
        }
    }

    /* Convert to RGB */
    for(int y=0;y<HEIGHT;y++){
        for(int x=0;x<WIDTH;x++){
            int Y = y_plane[y][x];
            int Cb = cb_plane[y/2][x/2];
            int Cr = cr_plane[y/2][x/2];
            ycbcr_to_rgb(Y, Cb, Cr, &rgb_out[(y*WIDTH+x)*3],
                        &rgb_out[(y*WIDTH+x)*3+1], &rgb_out[(y*WIDTH+x)*3+2]);
        }
    }
    return bp - ac_start; /* AC bits consumed */
}

/* Full decode with DC VLC per-position (each AC coeff coded as DC-style VLC) */
static int decode_full_dcvlc(const uint8_t *fdata, int data_bytes, uint8_t *rgb_out) {
    int total_bits = data_bytes * 8;
    int dc_pred[3] = {0, 0, 0};
    int y_plane[HEIGHT][WIDTH], cb_plane[HEIGHT/2][WIDTH/2], cr_plane[HEIGHT/2][WIDTH/2];
    memset(y_plane, 0, sizeof(y_plane));
    memset(cb_plane, 0, sizeof(cb_plane));
    memset(cr_plane, 0, sizeof(cr_plane));

    int qs = fdata[3];
    build_qtable(fdata);

    int bp = 0;
    int dc_vals[NBLK];
    for(int b=0;b<NBLK;b++){
        int dv;
        int c=dec_dc(fdata+40,bp,&dv,total_bits);
        if(c<0) return -1;
        bp+=c;
        dc_vals[b]=dv;
    }
    int ac_start = bp;

    int blk_idx = 0;
    for(int mby=0; mby<MB_H; mby++){
        for(int mbx=0; mbx<MB_W; mbx++){
            for(int blk=0; blk<6; blk++){
                int block[64];
                memset(block, 0, sizeof(block));

                int comp = (blk<4) ? 0 : (blk==4) ? 1 : 2;
                dc_pred[comp] += dc_vals[blk_idx];
                block[0] = dc_pred[comp] * 8;

                for(int pos=1;pos<64;pos++){
                    int val;
                    int c = dec_dc(fdata+40, bp, &val, total_bits);
                    if(c<0) break;
                    bp += c;
                    block[zigzag[pos]] = val;
                }

                dequantize(block, qs);
                int pixels[64];
                idct8x8(block, pixels);

                if(blk < 4){
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
                blk_idx++;
            }
        }
    }

    for(int y=0;y<HEIGHT;y++)
        for(int x=0;x<WIDTH;x++){
            int Y=y_plane[y][x],Cb=cb_plane[y/2][x/2],Cr=cr_plane[y/2][x/2];
            ycbcr_to_rgb(Y,Cb,Cr,&rgb_out[(y*WIDTH+x)*3],
                        &rgb_out[(y*WIDTH+x)*3+1],&rgb_out[(y*WIDTH+x)*3+2]);
        }
    return bp - ac_start;
}

int main() {
    const char *binfile = "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin";
    const char *outdir = "/home/wizzard/share/GitHub/playdia-emu/output";

    /* Create output directory */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", outdir);
    system(cmd);

    /* First, scan all frames from multiple LBAs to find varied content */
    printf("=== Scanning for video frames ===\n");

    int test_lbas[] = {150, 277, 502, 757, 1112, 1872};
    int nlbas = 6;

    for(int li=0; li<nlbas; li++){
        int lba = test_lbas[li];
        /* Load first few frames from this LBA */
        for(int fi=0; fi<5; fi++){
            if(load_frame(binfile, lba, fi) != 0) break;
            if(fbuf[0]!=0x00 || fbuf[1]!=0x80 || fbuf[2]!=0x04) break;

            int qs = fbuf[3];
            int type = fbuf[39];
            int de = flen;
            while(de>0 && fbuf[de-1]==0xFF) de--;
            int pad = flen - de;

            /* Decode DC to verify */
            int tb = (de-40)*8;
            int bp=0;
            int ok=1;
            for(int b=0;b<NBLK;b++){
                int dv; int c=dec_dc(fbuf+40,bp,&dv,tb);
                if(c<0){ok=0;break;} bp+=c;
            }
            int dc_bits=bp, ac_bits=tb-dc_bits;

            printf("LBA %4d F%d: QS=%2d type=%d pad=%4d DC=%4d AC=%5d %.2f b/c %s\n",
                   lba, fi, qs, type, pad, dc_bits, ac_bits,
                   (double)ac_bits/(NBLK*63), ok?"OK":"FAIL");

            /* Output DC-only image for interesting frames */
            if(ok && fi==0 && type==0){
                char path[512];
                uint8_t rgb[WIDTH*HEIGHT*3];

                /* DC-only */
                decode_dc_only(fbuf, de-40, rgb);
                snprintf(path, sizeof(path), "%s/lba%d_f%d_dc.ppm", outdir, lba, fi);
                write_ppm(path, rgb, WIDTH, HEIGHT);

                /* Flag+SM(1) - just sign */
                int consumed = decode_full_flag_sm(fbuf, de, 1, rgb);
                snprintf(path, sizeof(path), "%s/lba%d_f%d_fsm1.ppm", outdir, lba, fi);
                write_ppm(path, rgb, WIDTH, HEIGHT);
                printf("  Flag+SM(1) consumed %d/%d AC bits (%.1f%%)\n",
                       consumed, ac_bits, 100.0*consumed/ac_bits);

                /* Flag+SM(2) - sign + 1bit magnitude */
                consumed = decode_full_flag_sm(fbuf, de, 2, rgb);
                snprintf(path, sizeof(path), "%s/lba%d_f%d_fsm2.ppm", outdir, lba, fi);
                write_ppm(path, rgb, WIDTH, HEIGHT);
                printf("  Flag+SM(2) consumed %d/%d AC bits (%.1f%%)\n",
                       consumed, ac_bits, 100.0*consumed/ac_bits);

                /* DC VLC per position */
                consumed = decode_full_dcvlc(fbuf, de, rgb);
                snprintf(path, sizeof(path), "%s/lba%d_f%d_dcvlc.ppm", outdir, lba, fi);
                write_ppm(path, rgb, WIDTH, HEIGHT);
                printf("  DCVLC consumed %d/%d AC bits (%.1f%%)\n",
                       consumed, ac_bits, 100.0*consumed/ac_bits);
            }
        }
        printf("\n");
    }

    /* Now also check unique frame data: compare first 16 AC bytes across different frames */
    printf("=== First 16 AC bytes (hex) per LBA ===\n");
    for(int li=0; li<nlbas; li++){
        int lba = test_lbas[li];
        if(load_frame(binfile, lba, 0) != 0) continue;
        if(fbuf[0]!=0x00 || fbuf[1]!=0x80 || fbuf[2]!=0x04) continue;

        int de=flen; while(de>0&&fbuf[de-1]==0xFF)de--;
        int tb=(de-40)*8;
        int bp=0;
        for(int b=0;b<NBLK;b++){int dv;int c=dec_dc(fbuf+40,bp,&dv,tb);if(c<0)break;bp+=c;}

        int ac_byte = bp/8;
        printf("LBA %4d: ", lba);
        for(int i=0;i<16;i++) printf("%02X ", fbuf[40+ac_byte+i]);
        printf("\n");
    }

    printf("\nDone. Check output images in %s/\n", outdir);
    return 0;
}
