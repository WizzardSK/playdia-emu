/*
 * Test: per-coefficient 1-bit flag for AC
 * Structure: DC VLC + for each AC pos (1..63): 1-bit flag, if 1: VLC value
 * Also test: bitmap + VLC values
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <zip.h>
#include <math.h>

#define SECTOR_RAW 2352
#define MAX_FRAME  65536
#define PI 3.14159265358979323846
#define OUT_DIR "/home/wizzard/share/GitHub/playdia-emu/tools/test_output/"

static void write_pgm(const char *p, const uint8_t *g, int w, int h) {
    FILE *f=fopen(p,"wb"); if(!f)return;
    fprintf(f,"P5\n%d %d\n255\n",w,h); fwrite(g,1,w*h,f); fclose(f);
    printf("  -> %s (%dx%d)\n",p,w,h);
}
static void write_ppm(const char *p, const uint8_t *rgb, int w, int h) {
    FILE *f=fopen(p,"wb"); if(!f)return;
    fprintf(f,"P6\n%d %d\n255\n",w,h); fwrite(rgb,1,w*h*3,f); fclose(f);
    printf("  -> %s (%dx%d RGB)\n",p,w,h);
}

typedef struct { const uint8_t *data; int len,pos,bit,total; } BR;
static void br_init(BR *b, const uint8_t *d, int l) { b->data=d;b->len=l;b->pos=0;b->bit=7;b->total=0; }
static int br_eof(BR *b) { return b->pos>=b->len; }
static int br_get1(BR *b) {
    if(b->pos>=b->len) return 0;
    int v=(b->data[b->pos]>>b->bit)&1;
    if(--b->bit<0){b->bit=7;b->pos++;}
    b->total++; return v;
}
static int br_get(BR *b, int n) { int v=0; for(int i=0;i<n;i++) v=(v<<1)|br_get1(b); return v; }

static int vlc_coeff(BR *b) {
    int size;
    if (br_get1(b) == 0) { size = br_get1(b) ? 2 : 1; }
    else {
        if (br_get1(b) == 0) { size = br_get1(b) ? 3 : 0; }
        else {
            if (br_get1(b) == 0) size = 4;
            else if (br_get1(b) == 0) size = 5;
            else if (br_get1(b) == 0) size = 6;
            else size = br_get1(b) ? 8 : 7;
        }
    }
    if (size == 0) return 0;
    int val = br_get(b, size);
    if (val < (1 << (size-1))) val = val - (1 << size) + 1;
    return val;
}

static const int zigzag8[64] = {
     0, 1, 8,16, 9, 2, 3,10,
    17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,
    27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,
    29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};

static void idct8x8(int block[64], double out[64]) {
    double tmp[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) {
            double sum = 0;
            for (int k = 0; k < 8; k++) {
                double ck = (k == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += ck * block[i*8+k] * cos((2*j+1)*k*PI/16.0);
            }
            tmp[i*8+j] = sum * 0.5;
        }
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) {
            double sum = 0;
            for (int k = 0; k < 8; k++) {
                double ck = (k == 0) ? 1.0/sqrt(2.0) : 1.0;
                sum += ck * tmp[k*8+j] * cos((2*i+1)*k*PI/16.0);
            }
            out[i*8+j] = sum * 0.5;
        }
}

static int assemble_frames(const uint8_t *disc, int tsec, int slba,
    uint8_t fr[][MAX_FRAME], int fs[], int mx) {
    int n=0,c=0; bool inf=false;
    for(int l=slba;l<tsec&&n<mx;l++){
        const uint8_t *s=disc+(long)l*SECTOR_RAW;
        if(s[0]!=0||s[1]!=0xFF||s[15]!=2||(s[18]&4)) continue;
        if(s[24]==0xF1){if(!inf){inf=true;c=0;}if(c+2047<MAX_FRAME){memcpy(fr[n]+c,s+25,2047);c+=2047;}}
        else if(s[24]==0xF2){if(inf&&c>0){fs[n]=c;n++;inf=false;c=0;}}
    } return n;
}

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    int slba = argc > 2 ? atoi(argv[2]) : 502;
    const char *game = argc > 3 ? argv[3] : "mari";

    int err; zip_t *z = zip_open(argv[1], ZIP_RDONLY, &err); if (!z) return 1;
    int bi2=-1; zip_uint64_t bs2=0;
    for (int i=0; i<(int)zip_get_num_entries(z,0); i++) {
        zip_stat_t st; if(zip_stat_index(z,i,0,&st)==0 && st.size>bs2){bs2=st.size;bi2=i;}}
    zip_stat_t st; zip_stat_index(z,bi2,0,&st);
    zip_file_t *zf = zip_fopen_index(z,bi2,0);
    uint8_t *disc = malloc(st.size);
    zip_int64_t rd=0;
    while(rd<(zip_int64_t)st.size){zip_int64_t r=zip_fread(zf,disc+rd,st.size-rd);if(r<=0)break;rd+=r;}
    zip_fclose(zf);
    int tsec = (int)(st.size/SECTOR_RAW);

    static uint8_t frames[16][MAX_FRAME]; int fsizes[16];
    int nf = assemble_frames(disc,tsec,slba,frames,fsizes,16);

    int mbw = 8, mbh = 9, imgW = 128, imgH = 144;

    for (int fi = 0; fi < 2 && fi < nf; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        int qscale = f[3];
        uint8_t qt[16]; memcpy(qt, f+4, 16);
        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;

        printf("\n=== Frame %d: qscale=%d, type=%d ===\n", fi, qscale, f[39]);

        int qm[64];
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 8; j++)
                qm[i*8+j] = qt[(i/2)*4 + (j/2)];

        /* ===== Test: Per-AC-coefficient 1-bit flag, MB-interleaved ===== */
        printf("--- Per-AC bit flag, MB-interleaved ---\n");
        BR br; br_init(&br, bs, bslen);
        
        int *planeY = calloc(imgW * imgH, sizeof(int));
        int *planeCb = calloc(imgW/2 * imgH/2, sizeof(int));
        int *planeCr = calloc(imgW/2 * imgH/2, sizeof(int));
        int dc_y = 0, dc_cb = 0, dc_cr = 0;
        int total_ac_coded = 0, total_ac_zero = 0;
        int complete_blocks = 0;

        for (int mby = 0; mby < mbh && !br_eof(&br); mby++) {
            for (int mbx = 0; mbx < mbw && !br_eof(&br); mbx++) {
                double yspat[4][64];
                for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                    int block[64] = {0};
                    dc_y += vlc_coeff(&br);
                    block[0] = dc_y * 8;
                    
                    for (int i = 1; i < 64 && !br_eof(&br); i++) {
                        int flag = br_get1(&br);
                        if (flag) {
                            block[zigzag8[i]] = vlc_coeff(&br) * qm[zigzag8[i]];
                            total_ac_coded++;
                        } else {
                            total_ac_zero++;
                        }
                    }
                    complete_blocks++;
                    idct8x8(block, yspat[yb]);
                }
                int offsets[4][2] = {{0,0},{8,0},{0,8},{8,8}};
                for (int yb = 0; yb < 4; yb++)
                    for (int dy = 0; dy < 8; dy++)
                        for (int dx = 0; dx < 8; dx++) {
                            int px=mbx*16+offsets[yb][0]+dx;
                            int py=mby*16+offsets[yb][1]+dy;
                            if(px<imgW&&py<imgH)
                                planeY[py*imgW+px]=(int)round(yspat[yb][dy*8+dx]);
                        }
                /* Cb */
                {
                    int block[64]={0}; double spatial[64];
                    dc_cb += vlc_coeff(&br);
                    block[0] = dc_cb * 8;
                    for(int i=1;i<64&&!br_eof(&br);i++){
                        int flag=br_get1(&br);
                        if(flag) { block[zigzag8[i]]=vlc_coeff(&br)*qm[zigzag8[i]]; total_ac_coded++; }
                        else total_ac_zero++;
                    }
                    complete_blocks++;
                    idct8x8(block,spatial);
                    for(int dy=0;dy<8;dy++) for(int dx=0;dx<8;dx++){
                        int px=mbx*8+dx,py=mby*8+dy;
                        if(px<imgW/2&&py<imgH/2) planeCb[py*(imgW/2)+px]=(int)round(spatial[dy*8+dx]);
                    }
                }
                /* Cr */
                {
                    int block[64]={0}; double spatial[64];
                    dc_cr += vlc_coeff(&br);
                    block[0] = dc_cr * 8;
                    for(int i=1;i<64&&!br_eof(&br);i++){
                        int flag=br_get1(&br);
                        if(flag) { block[zigzag8[i]]=vlc_coeff(&br)*qm[zigzag8[i]]; total_ac_coded++; }
                        else total_ac_zero++;
                    }
                    complete_blocks++;
                    idct8x8(block,spatial);
                    for(int dy=0;dy<8;dy++) for(int dx=0;dx<8;dx++){
                        int px=mbx*8+dx,py=mby*8+dy;
                        if(px<imgW/2&&py<imgH/2) planeCr[py*(imgW/2)+px]=(int)round(spatial[dy*8+dx]);
                    }
                }
            }
        }
        printf("Blocks: %d/432, AC coded: %d, AC zero: %d, coded%%: %.1f%%, bits=%d/%d (%.1f%%)\n",
               complete_blocks, total_ac_coded, total_ac_zero,
               100.0*total_ac_coded/(total_ac_coded+total_ac_zero),
               br.total, bslen*8, 100.0*br.total/(bslen*8));

        uint8_t *imgout = calloc(imgW * imgH, 1);
        for (int i = 0; i < imgW * imgH; i++)
            imgout[i] = clamp8(planeY[i] + 128);
        char path[256];
        snprintf(path, sizeof(path), OUT_DIR "acbit_y_%s_f%d.pgm", game, fi);
        write_pgm(path, imgout, imgW, imgH);

        uint8_t *rgb = calloc(imgW * imgH * 3, 1);
        for (int y = 0; y < imgH; y++) {
            for (int x = 0; x < imgW; x++) {
                int yv = planeY[y*imgW+x] + 128;
                int cb = planeCb[(y/2)*(imgW/2)+(x/2)];
                int cr = planeCr[(y/2)*(imgW/2)+(x/2)];
                int r = clamp8(yv + (int)(1.402 * cr));
                int g = clamp8(yv - (int)(0.344 * cb) - (int)(0.714 * cr));
                int b2 = clamp8(yv + (int)(1.772 * cb));
                rgb[(y*imgW+x)*3+0] = r;
                rgb[(y*imgW+x)*3+1] = g;
                rgb[(y*imgW+x)*3+2] = b2;
            }
        }
        snprintf(path, sizeof(path), OUT_DIR "acbit_rgb_%s_f%d.ppm", game, fi);
        write_ppm(path, rgb, imgW, imgH);

        free(planeY); free(planeCb); free(planeCr); free(imgout); free(rgb);

        /* ===== Also test: run-VLC + level-VLC (separate VLCs) with EOB ===== */
        printf("--- Run-VLC + Level-VLC with EOB ---\n");
        br_init(&br, bs, bslen);
        
        planeY = calloc(imgW * imgH, sizeof(int));
        planeCb = calloc(imgW/2 * imgH/2, sizeof(int));
        planeCr = calloc(imgW/2 * imgH/2, sizeof(int));
        dc_y = 0; dc_cb = 0; dc_cr = 0;
        complete_blocks = 0;
        int total_rl_pairs = 0;

        for (int mby = 0; mby < mbh && !br_eof(&br); mby++) {
            for (int mbx = 0; mbx < mbw && !br_eof(&br); mbx++) {
                double yspat[4][64];
                for (int yb = 0; yb < 4 && !br_eof(&br); yb++) {
                    int block[64] = {0};
                    dc_y += vlc_coeff(&br);
                    block[0] = dc_y * 8;
                    
                    int pos = 1;
                    while (pos < 64 && !br_eof(&br)) {
                        int run = vlc_coeff(&br);
                        if (run < 0) run = -run; /* absolute value for run */
                        /* Check: if first "run" decodes as 0, maybe it's EOB */
                        /* Actually, let's use: run_val comes from VLC */
                        /* If the VLC returns 0, interpret as EOB */
                        /* No wait, let me try: read VLC for run (skip zeros), 
                           then VLC for level. If level==0, EOB */
                        /* Hmm, but both use same VLC. Let me try: 
                           the first VLC is the level at current position.
                           If 0, it means "skip next N positions" where N comes from... */
                        
                        /* OK let me simplify. Classic approach:
                           Read VLC1 = run (number of zeros before this coeff)
                           Read VLC2 = level (coefficient value)
                           If both 0, EOB */
                        int level = vlc_coeff(&br);
                        if (run == 0 && level == 0) break; /* EOB */
                        pos += run;
                        if (pos < 64) {
                            block[zigzag8[pos]] = level * qm[zigzag8[pos]];
                        }
                        pos++;
                        total_rl_pairs++;
                    }
                    complete_blocks++;
                    idct8x8(block, yspat[yb]);
                }
                int offsets[4][2] = {{0,0},{8,0},{0,8},{8,8}};
                for (int yb = 0; yb < 4; yb++)
                    for (int dy = 0; dy < 8; dy++)
                        for (int dx = 0; dx < 8; dx++) {
                            int px=mbx*16+offsets[yb][0]+dx;
                            int py=mby*16+offsets[yb][1]+dy;
                            if(px<imgW&&py<imgH)
                                planeY[py*imgW+px]=(int)round(yspat[yb][dy*8+dx]);
                        }
                /* Cb */
                {
                    int block[64]={0}; double spatial[64];
                    dc_cb += vlc_coeff(&br);
                    block[0] = dc_cb * 8;
                    int pos = 1;
                    while (pos < 64 && !br_eof(&br)) {
                        int run = vlc_coeff(&br); if(run<0)run=-run;
                        int level = vlc_coeff(&br);
                        if (run == 0 && level == 0) break;
                        pos += run;
                        if (pos < 64) block[zigzag8[pos]] = level * qm[zigzag8[pos]];
                        pos++;
                        total_rl_pairs++;
                    }
                    complete_blocks++;
                    idct8x8(block,spatial);
                    for(int dy=0;dy<8;dy++) for(int dx=0;dx<8;dx++){
                        int px=mbx*8+dx,py=mby*8+dy;
                        if(px<imgW/2&&py<imgH/2) planeCb[py*(imgW/2)+px]=(int)round(spatial[dy*8+dx]);
                    }
                }
                /* Cr */
                {
                    int block[64]={0}; double spatial[64];
                    dc_cr += vlc_coeff(&br);
                    block[0] = dc_cr * 8;
                    int pos = 1;
                    while (pos < 64 && !br_eof(&br)) {
                        int run = vlc_coeff(&br); if(run<0)run=-run;
                        int level = vlc_coeff(&br);
                        if (run == 0 && level == 0) break;
                        pos += run;
                        if (pos < 64) block[zigzag8[pos]] = level * qm[zigzag8[pos]];
                        pos++;
                        total_rl_pairs++;
                    }
                    complete_blocks++;
                    idct8x8(block,spatial);
                    for(int dy=0;dy<8;dy++) for(int dx=0;dx<8;dx++){
                        int px=mbx*8+dx,py=mby*8+dy;
                        if(px<imgW/2&&py<imgH/2) planeCr[py*(imgW/2)+px]=(int)round(spatial[dy*8+dx]);
                    }
                }
            }
        }
        printf("Blocks: %d/432, R/L pairs: %d, bits=%d/%d (%.1f%%)\n",
               complete_blocks, total_rl_pairs, br.total, bslen*8,
               100.0*br.total/(bslen*8));

        imgout = calloc(imgW * imgH, 1);
        for (int i = 0; i < imgW * imgH; i++)
            imgout[i] = clamp8(planeY[i] + 128);
        snprintf(path, sizeof(path), OUT_DIR "runlvl_y_%s_f%d.pgm", game, fi);
        write_pgm(path, imgout, imgW, imgH);

        rgb = calloc(imgW * imgH * 3, 1);
        for (int y = 0; y < imgH; y++) {
            for (int x = 0; x < imgW; x++) {
                int yv = planeY[y*imgW+x] + 128;
                int cb2 = planeCb[(y/2)*(imgW/2)+(x/2)];
                int cr2 = planeCr[(y/2)*(imgW/2)+(x/2)];
                int r = clamp8(yv + (int)(1.402 * cr2));
                int g = clamp8(yv - (int)(0.344 * cb2) - (int)(0.714 * cr2));
                int b2 = clamp8(yv + (int)(1.772 * cb2));
                rgb[(y*imgW+x)*3+0] = r;
                rgb[(y*imgW+x)*3+1] = g;
                rgb[(y*imgW+x)*3+2] = b2;
            }
        }
        snprintf(path, sizeof(path), OUT_DIR "runlvl_rgb_%s_f%d.ppm", game, fi);
        write_ppm(path, rgb, imgW, imgH);

        free(planeY); free(planeCb); free(planeCr); free(imgout); free(rgb);
    }

    free(disc); zip_close(z);
    return 0;
}
