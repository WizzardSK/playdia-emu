/*
 * vcodec_rl8_image.c - Produce images using 8-bit run-level coding variants
 *
 * Tests the most promising AC coding hypothesis: 8-bit fixed-width entries
 * with 3-bit run + 5-bit level, or 4-bit run + 4-bit level, with EOB.
 *
 * Multiple variants:
 * A) 3run + 5level (sign-magnitude, level=0=EOB)
 * B) 4run + 4level (sign-magnitude, level=0=EOB)
 * C) 3run + 1sign + 4magnitude (no separate EOB, run=7 mag=0 = EOB)
 * D) DC VLC for run, DC VLC for level (variable-width)
 * E) 8-bit entries but with run being the DELTA position (not zeros to skip)
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

typedef struct {
    int y_plane[HEIGHT][WIDTH];
    int cb_plane[HEIGHT/2][WIDTH/2];
    int cr_plane[HEIGHT/2][WIDTH/2];
} YCbCr;

static void planes_to_rgb(YCbCr *p, uint8_t *rgb) {
    for(int y=0;y<HEIGHT;y++)
        for(int x=0;x<WIDTH;x++){
            int Y=p->y_plane[y][x], Cb=p->cb_plane[y/2][x/2], Cr=p->cr_plane[y/2][x/2];
            rgb[(y*WIDTH+x)*3]=clamp(Y+(int)(1.402*(Cr-128)));
            rgb[(y*WIDTH+x)*3+1]=clamp(Y-(int)(0.344136*(Cb-128))-(int)(0.714136*(Cr-128)));
            rgb[(y*WIDTH+x)*3+2]=clamp(Y+(int)(1.772*(Cb-128)));
        }
}

static void place_block(YCbCr *p, int bidx, int pixels[64]) {
    int mb = bidx / 6;
    int blk = bidx % 6;
    int mby = mb / MB_W, mbx = mb % MB_W;

    if(blk < 4){
        int bx=(blk&1)*8, by=(blk>>1)*8;
        int px=mbx*16+bx, py=mby*16+by;
        for(int y=0;y<8;y++)for(int x=0;x<8;x++)
            if(py+y<HEIGHT&&px+x<WIDTH)
                p->y_plane[py+y][px+x]=clamp(pixels[y*8+x]);
    } else if(blk==4){
        int px=mbx*8,py=mby*8;
        for(int y=0;y<8;y++)for(int x=0;x<8;x++)
            if(py+y<HEIGHT/2&&px+x<WIDTH/2)
                p->cb_plane[py+y][px+x]=clamp(pixels[y*8+x]);
    } else {
        int px=mbx*8,py=mby*8;
        for(int y=0;y<8;y++)for(int x=0;x<8;x++)
            if(py+y<HEIGHT/2&&px+x<WIDTH/2)
                p->cr_plane[py+y][px+x]=clamp(pixels[y*8+x]);
    }
}

/* Dequantize AC coefficient at zigzag position zpos */
static int dequant(int val, int zpos, int qs, const uint8_t *qt) {
    if(val == 0) return 0;
    /* Use qtable value for this position (cycling 16 entries) */
    int q = qt[(zpos-1) % 16]; /* zpos-1 because zpos 0 is DC */
    /* MPEG-1 intra dequant: val * qs * W / 8 */
    return val * qs * q / 8;
}

int main() {
    const char *binfile = "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin";
    const char *outdir = "/home/wizzard/share/GitHub/playdia-emu/output";

    /* Load F00 from LBA 502 (actual game content, QS=8) */
    if(load_frame(binfile, 502, 0)!=0){printf("Failed\n");return 1;}

    int qs=fbuf[3];
    printf("Frame: QS=%d, type=%d\n", qs, fbuf[39]);

    int de=flen; while(de>0&&fbuf[de-1]==0xFF)de--;
    int total_bits=(de-40)*8;

    /* Decode all DC */
    int dc_diffs[NBLK];
    int bp=0;
    for(int b=0;b<NBLK;b++){
        int dv;int c=dec_dc(fbuf+40,bp,&dv,total_bits);
        if(c<0){printf("DC fail at %d\n",b);return 1;}
        bp+=c;
        dc_diffs[b]=dv;
    }
    int dc_bits=bp, ac_bits=total_bits-dc_bits;
    printf("DC=%d bits, AC=%d bits\n", dc_bits, ac_bits);

    uint8_t rgb[WIDTH*HEIGHT*3];
    YCbCr planes;
    int dc_pred[3];
    int ac_bp;

    /* === Variant A: 3-bit run + 5-bit level (sign-mag), level=0=EOB === */
    printf("\n--- Variant A: 3run+5level (SM), level=0=EOB ---\n");
    memset(&planes, 0, sizeof(planes));
    dc_pred[0]=dc_pred[1]=dc_pred[2]=0;
    ac_bp = dc_bits;
    {
        int total_consumed=0, total_nz=0, eobs=0;
        for(int b=0;b<NBLK;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dc_pred[comp]+=dc_diffs[b];

            int block[64];
            memset(block,0,sizeof(block));
            block[0]=dc_pred[comp]*8;

            int pos=0;
            while(pos<63 && ac_bp+8<=total_bits){
                int entry=get_bits(fbuf+40,ac_bp,8); ac_bp+=8;
                int run=(entry>>5)&7;
                int level_raw=entry&0x1F;
                if(level_raw==0){eobs++;break;} /* EOB */
                /* sign-magnitude: bit4=sign, bits0-3=magnitude */
                int sign=(level_raw>>4)&1;
                int mag=level_raw&0xF;
                if(mag==0) mag=16; /* level_raw=0x10 → mag=0? no, handled above */
                int level=sign?-mag:mag;
                pos+=run;
                if(pos>=63) break;
                block[zigzag[pos+1]]=dequant(level, pos+1, qs, fbuf+4);
                total_nz++;
                pos++;
            }

            int pixels[64];
            idct8x8(block, pixels);
            place_block(&planes, b, pixels);
        }
        printf("AC consumed: %d/%d (%.1f%%), EOBs=%d, NZ=%d\n",
               ac_bp-dc_bits, ac_bits, 100.0*(ac_bp-dc_bits)/ac_bits, eobs, total_nz);
        planes_to_rgb(&planes, rgb);
        char path[512]; snprintf(path,sizeof(path),"%s/lba502_f0_rl35_eob.ppm",outdir);
        write_ppm(path, rgb, WIDTH, HEIGHT);
    }

    /* === Variant B: 4-bit run + 4-bit level (sign-mag), level=0=EOB === */
    printf("\n--- Variant B: 4run+4level (SM), level=0=EOB ---\n");
    memset(&planes, 0, sizeof(planes));
    dc_pred[0]=dc_pred[1]=dc_pred[2]=0;
    ac_bp = dc_bits;
    {
        int total_consumed=0, total_nz=0, eobs=0;
        for(int b=0;b<NBLK;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dc_pred[comp]+=dc_diffs[b];

            int block[64];
            memset(block,0,sizeof(block));
            block[0]=dc_pred[comp]*8;

            int pos=0;
            while(pos<63 && ac_bp+8<=total_bits){
                int entry=get_bits(fbuf+40,ac_bp,8); ac_bp+=8;
                int run=(entry>>4)&0xF;
                int level_raw=entry&0xF;
                if(level_raw==0){eobs++;break;} /* EOB */
                int sign=(level_raw>>3)&1;
                int mag=level_raw&7;
                if(mag==0) mag=8;
                int level=sign?-mag:mag;
                pos+=run;
                if(pos>=63) break;
                block[zigzag[pos+1]]=dequant(level, pos+1, qs, fbuf+4);
                total_nz++;
                pos++;
            }

            int pixels[64];
            idct8x8(block, pixels);
            place_block(&planes, b, pixels);
        }
        printf("AC consumed: %d/%d (%.1f%%), EOBs=%d, NZ=%d\n",
               ac_bp-dc_bits, ac_bits, 100.0*(ac_bp-dc_bits)/ac_bits, eobs, total_nz);
        planes_to_rgb(&planes, rgb);
        char path[512]; snprintf(path,sizeof(path),"%s/lba502_f0_rl44_eob.ppm",outdir);
        write_ppm(path, rgb, WIDTH, HEIGHT);
    }

    /* === Variant C: 3run + 5level (two's complement), level=0=EOB === */
    printf("\n--- Variant C: 3run+5level (2C), level=0=EOB ---\n");
    memset(&planes, 0, sizeof(planes));
    dc_pred[0]=dc_pred[1]=dc_pred[2]=0;
    ac_bp = dc_bits;
    {
        int total_nz=0, eobs=0;
        for(int b=0;b<NBLK;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dc_pred[comp]+=dc_diffs[b];

            int block[64];
            memset(block,0,sizeof(block));
            block[0]=dc_pred[comp]*8;

            int pos=0;
            while(pos<63 && ac_bp+8<=total_bits){
                int entry=get_bits(fbuf+40,ac_bp,8); ac_bp+=8;
                int run=(entry>>5)&7;
                int level_raw=entry&0x1F;
                if(level_raw==0){eobs++;break;}
                /* Two's complement 5-bit */
                int level=(level_raw>=16)?(level_raw-32):level_raw;
                pos+=run;
                if(pos>=63) break;
                block[zigzag[pos+1]]=dequant(level, pos+1, qs, fbuf+4);
                total_nz++;
                pos++;
            }

            int pixels[64];
            idct8x8(block, pixels);
            place_block(&planes, b, pixels);
        }
        printf("AC consumed: %d/%d (%.1f%%), EOBs=%d, NZ=%d\n",
               ac_bp-dc_bits, ac_bits, 100.0*(ac_bp-dc_bits)/ac_bits, eobs, total_nz);
        planes_to_rgb(&planes, rgb);
        char path[512]; snprintf(path,sizeof(path),"%s/lba502_f0_rl35_2c.ppm",outdir);
        write_ppm(path, rgb, WIDTH, HEIGHT);
    }

    /* === Variant D: MPEG-1 Table B.14 VLC run-level (proper implementation) === */
    /* Simplified: just the most common entries */
    printf("\n--- Variant D: MPEG-1 B.14 VLC (simplified 20 entries + escape) ---\n");
    memset(&planes, 0, sizeof(planes));
    dc_pred[0]=dc_pred[1]=dc_pred[2]=0;
    ac_bp = dc_bits;
    {
        /* Table B.14 entries (code, length, run, level) */
        /* EOB = "10" */
        /* Escape = "000001" + 6-bit run + 8-bit level (signed) */
        typedef struct { uint32_t code; int len; int run; int level; } vlc_entry;
        vlc_entry table[] = {
            /* code, len, run, level (sign added separately) */
            {0x02, 2, -1, 0},   /* EOB: 10 */
            {0x03, 2,  0, 1},   /* 11s -> (0,1) with sign */
            {0x03, 3,  1, 1},   /* 011s */
            {0x04, 4,  0, 2},   /* 0100s */
            {0x05, 4,  2, 1},   /* 0101s */
            {0x06, 5,  0, 3},   /* 00110s */
            {0x0E, 6,  3, 1},   /* 001110s */
            {0x0F, 6,  4, 1},   /* 001111s */
            {0x04, 6,  0, 4},   /* 000100s */
            {0x05, 6,  5, 1},   /* 000101s */
            {0x06, 6,  1, 2},   /* 000110s */
            {0x07, 6,  6, 1},   /* 000111s */
            {0x04, 7,  0, 5},   /* 0000100s */
            {0x05, 7,  7, 1},   /* 0000101s */
            {0x06, 7,  2, 2},   /* 0000110s */
            {0x07, 7,  8, 1},   /* 0000111s */
            {0x01, 6, -2, 0},   /* ESCAPE: 000001 */
        };
        int ntable = 17;

        int total_nz=0, eobs=0, escapes=0, errors=0;
        for(int b=0;b<NBLK;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dc_pred[comp]+=dc_diffs[b];

            int block[64];
            memset(block,0,sizeof(block));
            block[0]=dc_pred[comp]*8;

            int pos=0;
            while(pos<63 && ac_bp<total_bits){
                int found=0;
                for(int t=0;t<ntable;t++){
                    if(ac_bp+table[t].len > total_bits) continue;
                    uint32_t bits=get_bits(fbuf+40,ac_bp,table[t].len);
                    if(bits==table[t].code){
                        if(table[t].run==-1){ /* EOB */
                            ac_bp+=table[t].len;
                            eobs++;
                            found=2;
                            break;
                        }
                        if(table[t].run==-2){ /* ESCAPE */
                            ac_bp+=table[t].len;
                            if(ac_bp+14>total_bits){errors++;found=2;break;}
                            int run=get_bits(fbuf+40,ac_bp,6); ac_bp+=6;
                            int lev=get_bits(fbuf+40,ac_bp,8); ac_bp+=8;
                            if(lev>=128) lev-=256; /* signed */
                            pos+=run;
                            if(pos>=63){found=2;break;}
                            if(lev!=0){
                                block[zigzag[pos+1]]=dequant(lev, pos+1, qs, fbuf+4);
                                total_nz++;
                            }
                            pos++;
                            escapes++;
                            found=1;
                            break;
                        }
                        /* Normal entry */
                        ac_bp+=table[t].len;
                        /* Read sign bit */
                        if(ac_bp>=total_bits){errors++;found=2;break;}
                        int sign=get_bit(fbuf+40,ac_bp); ac_bp++;
                        int level=table[t].level;
                        if(sign) level=-level;
                        pos+=table[t].run;
                        if(pos>=63){found=2;break;}
                        block[zigzag[pos+1]]=dequant(level, pos+1, qs, fbuf+4);
                        total_nz++;
                        pos++;
                        found=1;
                        break;
                    }
                }
                if(found==2) break; /* EOB or error */
                if(!found){
                    /* No match - skip bit and continue */
                    ac_bp++; errors++;
                    if(errors>1000) break;
                }
            }

            int pixels[64];
            idct8x8(block, pixels);
            place_block(&planes, b, pixels);
        }
        printf("AC consumed: %d/%d (%.1f%%), EOBs=%d, NZ=%d, ESC=%d, err=%d\n",
               ac_bp-dc_bits, ac_bits, 100.0*(ac_bp-dc_bits)/ac_bits,
               eobs, total_nz, escapes, errors);
        planes_to_rgb(&planes, rgb);
        char path[512]; snprintf(path,sizeof(path),"%s/lba502_f0_mpeg1vlc.ppm",outdir);
        write_ppm(path, rgb, WIDTH, HEIGHT);
    }

    /* Also produce DC-only for comparison */
    printf("\n--- DC-only baseline ---\n");
    memset(&planes, 0, sizeof(planes));
    dc_pred[0]=dc_pred[1]=dc_pred[2]=0;
    for(int b=0;b<NBLK;b++){
        int comp=(b%6<4)?0:(b%6==4)?1:2;
        dc_pred[comp]+=dc_diffs[b];
        int block[64];
        memset(block,0,sizeof(block));
        block[0]=dc_pred[comp]*8;
        int pixels[64];
        idct8x8(block, pixels);
        place_block(&planes, b, pixels);
    }
    planes_to_rgb(&planes, rgb);
    {
        char path[512]; snprintf(path,sizeof(path),"%s/lba502_f0_dc_baseline.ppm",outdir);
        write_ppm(path, rgb, WIDTH, HEIGHT);
    }

    /* Repeat for LBA 757 (different scene) */
    printf("\n=== LBA 757 (different scene) ===\n");
    if(load_frame(binfile, 757, 0)==0){
        qs=fbuf[3];
        printf("QS=%d, type=%d\n", qs, fbuf[39]);
        de=flen; while(de>0&&fbuf[de-1]==0xFF)de--;
        total_bits=(de-40)*8;
        bp=0;
        for(int b=0;b<NBLK;b++){int dv;bp+=dec_dc(fbuf+40,bp,&dv,total_bits);dc_diffs[b]=dv;}
        dc_bits=bp; ac_bits=total_bits-dc_bits;

        /* DC only */
        memset(&planes, 0, sizeof(planes));
        dc_pred[0]=dc_pred[1]=dc_pred[2]=0;
        for(int b=0;b<NBLK;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dc_pred[comp]+=dc_diffs[b];
            int block[64]; memset(block,0,sizeof(block));
            block[0]=dc_pred[comp]*8;
            int pixels[64]; idct8x8(block,pixels);
            place_block(&planes,b,pixels);
        }
        planes_to_rgb(&planes, rgb);
        char path[512]; snprintf(path,sizeof(path),"%s/lba757_f0_dc_baseline.ppm",outdir);
        write_ppm(path, rgb, WIDTH, HEIGHT);

        /* RL 3+5 EOB */
        memset(&planes, 0, sizeof(planes));
        dc_pred[0]=dc_pred[1]=dc_pred[2]=0;
        ac_bp = dc_bits;
        int eobs=0, nz=0;
        for(int b=0;b<NBLK;b++){
            int comp=(b%6<4)?0:(b%6==4)?1:2;
            dc_pred[comp]+=dc_diffs[b];
            int block[64]; memset(block,0,sizeof(block));
            block[0]=dc_pred[comp]*8;
            int pos=0;
            while(pos<63 && ac_bp+8<=total_bits){
                int entry=get_bits(fbuf+40,ac_bp,8); ac_bp+=8;
                int run=(entry>>5)&7;
                int level_raw=entry&0x1F;
                if(level_raw==0){eobs++;break;}
                int level=(level_raw>=16)?(level_raw-32):level_raw;
                pos+=run;
                if(pos>=63) break;
                block[zigzag[pos+1]]=dequant(level,pos+1,qs,fbuf+4);
                nz++; pos++;
            }
            int pixels[64]; idct8x8(block,pixels);
            place_block(&planes,b,pixels);
        }
        printf("RL35: consumed %d/%d (%.1f%%), EOBs=%d, NZ=%d\n",
               ac_bp-dc_bits, ac_bits, 100.0*(ac_bp-dc_bits)/ac_bits, eobs, nz);
        planes_to_rgb(&planes, rgb);
        snprintf(path,sizeof(path),"%s/lba757_f0_rl35_2c.ppm",outdir);
        write_ppm(path, rgb, WIDTH, HEIGHT);
    }

    printf("\nDone.\n");
    return 0;
}
