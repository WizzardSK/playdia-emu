/*
 * Playdia video - MPEG-1 intra block coding adapted for 4×4 blocks
 * Uses MPEG-1 DC luminance table + MPEG-1 AC coefficient VLC (Table B.14)
 * Also tries: Exp-Golomb coding, and raw VLC analysis
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <zip.h>

#define SECTOR_RAW 2352
#define MAX_FRAME  65536
#define PI 3.14159265358979323846

static void write_pgm(const char *p, const uint8_t *g, int w, int h) {
    FILE *f=fopen(p,"wb"); if(!f)return;
    fprintf(f,"P5\n%d %d\n255\n",w,h); fwrite(g,1,w*h,f); fclose(f);
    printf("  -> %s (%dx%d)\n",p,w,h);
}

typedef struct { const uint8_t *data; int len,pos,bit,total; } BR;
static void br_init(BR *b, const uint8_t *d, int l) { b->data=d;b->len=l;b->pos=0;b->bit=7;b->total=0; }
static int br_get1(BR *b) {
    if(b->pos>=b->len) return -1;
    int v=(b->data[b->pos]>>b->bit)&1;
    if(--b->bit<0){b->bit=7;b->pos++;}
    b->total++; return v;
}
static int br_get(BR *b, int n) { int v=0; for(int i=0;i<n;i++){int x=br_get1(b);if(x<0)return 0;v=(v<<1)|x;} return v; }
static bool br_eof(BR *b) { return b->pos>=b->len; }

// Save/restore bitreader state
static void br_save(BR *b, BR *s) { *s = *b; }
static void br_restore(BR *b, BR *s) { *b = *s; }

// === MPEG-1 DC luminance decode ===
static int m1_dc_size_lum(BR *b) {
    // Table B.12: 0→100, 1→00, 2→01, 3→101, 4→110, 5→1110, 6→11110, 7→111110, 8→1111110
    int b1 = br_get1(b); if(b1<0) return -1;
    if (b1 == 0) {
        int b2 = br_get1(b); if(b2<0) return -1;
        return b2 == 0 ? 1 : 2;
    }
    int b2 = br_get1(b); if(b2<0) return -1;
    if (b2 == 0) {
        int b3 = br_get1(b); if(b3<0) return -1;
        return b3 == 0 ? 0 : 3;
    }
    int b3 = br_get1(b); if(b3<0) return -1;
    if (b3 == 0) return 4;
    int b4 = br_get1(b); if(b4<0) return -1;
    if (b4 == 0) return 5;
    int b5 = br_get1(b); if(b5<0) return -1;
    if (b5 == 0) return 6;
    int b6 = br_get1(b); if(b6<0) return -1;
    if (b6 == 0) return 7;
    int b7 = br_get1(b); if(b7<0) return -1;
    if (b7 == 0) return 8;
    return -1;
}

static int m1_dc_diff(BR *b, int size) {
    if (size == 0) return 0;
    int val = br_get(b, size);
    if (!(val & (1 << (size-1)))) val -= (1 << size) - 1;
    return val;
}

// === MPEG-1 AC coefficient VLC (Table B.14) ===
// Returns: 0=decoded (run,level), 1=EOB, -1=error
// Table entries: (code, nbits, run, level)
typedef struct { int run, level; } RL;

// The full MPEG-1 DCT coefficient table zero
// For first coefficient: `1s` = (0,1), for others: `10` = EOB, `11s` = (0,1)
static int m1_ac_decode(BR *b, int *run, int *level, bool is_first) {
    int b1 = br_get1(b); if(b1<0) return -1;

    if (is_first && b1 == 1) {
        int s = br_get1(b); if(s<0) return -1;
        *run = 0; *level = s ? -1 : 1; return 0;
    }

    if (!is_first && b1 == 1) {
        int b2 = br_get1(b); if(b2<0) return -1;
        if (b2 == 0) return 1; // EOB = 10
        int s = br_get1(b); if(s<0) return -1;
        *run = 0; *level = s ? -1 : 1; return 0; // 11s = (0,1)
    }

    // b1 = 0
    int b2 = br_get1(b); if(b2<0) return -1;
    if (b2 == 1) {
        int b3 = br_get1(b); if(b3<0) return -1;
        if (b3 == 1) { // 011s
            int s = br_get1(b); if(s<0) return -1;
            *run = 1; *level = s ? -1 : 1; return 0;
        }
        // 010x
        int b4 = br_get1(b); if(b4<0) return -1;
        int s = br_get1(b); if(s<0) return -1;
        if (b4 == 0) { *run = 0; *level = s ? -2 : 2; return 0; } // 0100s
        else { *run = 2; *level = s ? -1 : 1; return 0; }          // 0101s
    }

    // 00...
    int b3 = br_get1(b); if(b3<0) return -1;
    if (b3 == 1) {
        int b4 = br_get1(b); if(b4<0) return -1;
        int b5 = br_get1(b); if(b5<0) return -1;
        int s = br_get1(b); if(s<0) return -1;
        if (b4 == 0 && b5 == 0) { *run = 4; *level = s ? -1 : 1; return 0; } // 00100s
        if (b4 == 0 && b5 == 1) { *run = 1; *level = s ? -2 : 2; return 0; } // 00101s
        if (b4 == 1 && b5 == 0) { *run = 0; *level = s ? -3 : 3; return 0; } // 00110s
        if (b4 == 1 && b5 == 1) { *run = 3; *level = s ? -1 : 1; return 0; } // 00111s
    }

    // 000...
    int b4 = br_get1(b); if(b4<0) return -1;
    if (b4 == 1) {
        int b5 = br_get1(b); if(b5<0) return -1;
        int b6 = br_get1(b); if(b6<0) return -1;
        int s = br_get1(b); if(s<0) return -1;
        if (b5 == 0 && b6 == 0) { *run = 6; *level = s ? -1 : 1; return 0; } // 000100s
        if (b5 == 0 && b6 == 1) { *run = 7; *level = s ? -1 : 1; return 0; } // 000101s
        if (b5 == 1 && b6 == 0) { *run = 5; *level = s ? -1 : 1; return 0; } // 000110s
        if (b5 == 1 && b6 == 1) { *run = 0; *level = s ? -4 : 4; return 0; } // 000111s
    }

    // 0000...
    int b5 = br_get1(b); if(b5<0) return -1;
    if (b5 == 1) {
        int b6 = br_get1(b); if(b6<0) return -1;
        int b7 = br_get1(b); if(b7<0) return -1;
        int s = br_get1(b); if(s<0) return -1;
        if (b6 == 0 && b7 == 0) { *run = 8; *level = s ? -1 : 1; return 0; }  // 0000100s
        if (b6 == 0 && b7 == 1) { *run = 9; *level = s ? -1 : 1; return 0; }  // 0000101s
        if (b6 == 1 && b7 == 0) { *run = 0; *level = s ? -5 : 5; return 0; }  // 0000110s
        if (b6 == 1 && b7 == 1) { *run = 2; *level = s ? -2 : 2; return 0; }  // 0000111s
    }

    // 00000...
    int b6 = br_get1(b); if(b6<0) return -1;
    if (b6 == 1) {
        // ESCAPE = 000001
        *run = br_get(b, 6);
        int lv = br_get(b, 8);
        if (lv == 0) lv = br_get(b, 8);
        else if (lv == 128) lv = br_get(b, 8) - 256;
        else if (lv > 128) lv -= 256;
        *level = lv;
        return 0;
    }

    // 000000... - longer codes
    int b7 = br_get1(b); if(b7<0) return -1;
    if (b7 == 1) {
        // 0000001xx...
        int b8 = br_get1(b); if(b8<0) return -1;
        int b9 = br_get1(b); if(b9<0) return -1;
        int s = br_get1(b); if(s<0) return -1;
        if (b8 == 0 && b9 == 0) { *run = 10; *level = s ? -1 : 1; return 0; }
        if (b8 == 0 && b9 == 1) { *run = 11; *level = s ? -1 : 1; return 0; }
        if (b8 == 1 && b9 == 0) { *run = 0; *level = s ? -6 : 6; return 0; }
        if (b8 == 1 && b9 == 1) { *run = 1; *level = s ? -3 : 3; return 0; }
    }

    // 0000000... more codes
    int b8 = br_get1(b); if(b8<0) return -1;
    if (b8 == 1) {
        int b9 = br_get1(b); if(b9<0) return -1;
        int b10 = br_get1(b); if(b10<0) return -1;
        int s = br_get1(b); if(s<0) return -1;
        if (b9 == 0 && b10 == 0) { *run = 12; *level = s ? -1 : 1; return 0; }
        if (b9 == 0 && b10 == 1) { *run = 13; *level = s ? -1 : 1; return 0; }
        if (b9 == 1 && b10 == 0) { *run = 0; *level = s ? -7 : 7; return 0; }
        if (b9 == 1 && b10 == 1) { *run = 3; *level = s ? -2 : 2; return 0; }
    }

    // Even longer codes - give up
    return -1;
}

static const int zigzag4[16] = { 0,1,4,8, 5,2,3,6, 9,12,13,10, 7,11,14,15 };

static void idct4x4(const double c[16], int out[16]) {
    for(int y=0;y<4;y++) for(int x=0;x<4;x++) {
        double s=0;
        for(int v=0;v<4;v++){double cv=v==0?0.5:sqrt(0.5);
        for(int u=0;u<4;u++){double cu=u==0?0.5:sqrt(0.5);
        s+=cu*cv*c[v*4+u]*cos((2*x+1)*u*PI/8.0)*cos((2*y+1)*v*PI/8.0);}}
        out[y*4+x]=(int)round(s);
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

int main(int argc, char **argv) {
    if(argc<2){fprintf(stderr,"Usage: %s <zip> [lba]\n",argv[0]);return 1;}
    int slba = argc>2 ? atoi(argv[2]) : 502;

    int err; zip_t *z=zip_open(argv[1],ZIP_RDONLY,&err);
    if(!z){fprintf(stderr,"zip fail\n");return 1;}
    int bi=-1;zip_uint64_t bs2=0;
    for(int i=0;i<(int)zip_get_num_entries(z,0);i++){
        zip_stat_t st;if(zip_stat_index(z,i,0,&st)==0&&st.size>bs2){bs2=st.size;bi=i;}}
    zip_stat_t st;zip_stat_index(z,bi,0,&st);
    zip_file_t *zf=zip_fopen_index(z,bi,0);
    uint8_t *disc=malloc(st.size);
    zip_int64_t rd=0;
    while(rd<(zip_int64_t)st.size){zip_int64_t r=zip_fread(zf,disc+rd,st.size-rd);if(r<=0)break;rd+=r;}
    zip_fclose(zf);
    int tsec=(int)(st.size/SECTOR_RAW);

    static uint8_t frames[8][MAX_FRAME]; int fsizes[8];
    int nf=assemble_frames(disc,tsec,slba,frames,fsizes,8);
    printf("Assembled %d frames\n",nf);

    for (int fi = 0; fi < 2 && fi < nf; fi++) {
        uint8_t *f = frames[fi];
        int fsize = fsizes[fi];
        uint8_t qtab[16]; memcpy(qtab,f+4,16);
        int qscale = f[3];

        printf("\n=== FRAME %d: %d bytes, qscale=%d, type=%02X ===\n",
               fi, fsize, qscale, f[39]);

        const uint8_t *bs = f + 40;
        int bslen = fsize - 40;
        int W = 128, H = 144;
        int bw = W/4, bh = H/4;

        // === MPEG-1 INTRA: DC + AC ===
        printf("\n--- MPEG-1 Intra 4x4 ---\n");
        {
            BR br; br_init(&br, bs, bslen);
            uint8_t *img = calloc(W*H, 1);
            int prev_dc = 128 * 4; // MPEG-1 intra DC starts at 128*8 for 8x8, so 128*4 for 4×4?
            // Actually, MPEG-1 uses: DC = prev_DC + diff, where DC represents block average * 8
            // For 4×4 blocks maybe DC * 4?
            int blocks = 0;

            for (int by = 0; by < bh && !br_eof(&br); by++) {
                for (int bx = 0; bx < bw && !br_eof(&br); bx++) {
                    int dcsize = m1_dc_size_lum(&br);
                    if (dcsize < 0) { printf("DC fail at block %d\n", blocks); goto done_m1; }
                    int diff = m1_dc_diff(&br, dcsize);
                    prev_dc += diff;

                    double coeff[16] = {0};
                    coeff[0] = prev_dc; // DC

                    // AC using MPEG-1 table
                    int k = 1;
                    bool first_ac = true;
                    while (k < 16) {
                        int run, level;
                        int ret = m1_ac_decode(&br, &run, &level, first_ac);
                        first_ac = false;
                        if (ret < 0) { goto done_m1; }
                        if (ret == 1) break; // EOB
                        k += run;
                        if (k >= 16) break;
                        coeff[zigzag4[k]] = level * qtab[k] * qscale;
                        k++;
                    }

                    // DC dequant (different from AC)
                    coeff[0] = prev_dc * 2; // MPEG-1 uses dc * 8 for 8x8 → dc * 2 for 4×4?

                    int pixels[16];
                    idct4x4(coeff, pixels);

                    for (int dy = 0; dy < 4; dy++)
                        for (int dx = 0; dx < 4; dx++) {
                            int px = bx*4+dx, py = by*4+dy;
                            if (px<W && py<H) {
                                int v = pixels[dy*4+dx];
                                if(v<0)v=0;if(v>255)v=255;
                                img[py*W+px] = v;
                            }
                        }
                    blocks++;
                }
            }
            done_m1:
            printf("MPEG1: %d blocks, %d bits\n", blocks, br.total);
            char path[256];
            snprintf(path,sizeof(path),"/home/wizzard/share/GitHub/pd_m1i_f%d.pgm",fi);
            write_pgm(path, img, W, H);
            free(img);
        }

        // === MPEG-1 DC only (for DC image) ===
        printf("\n--- MPEG-1 DC only ---\n");
        {
            BR br; br_init(&br, bs, bslen);
            int prev_dc = 0;
            int dc_vals[2048], ndc = 0;

            // Just decode DC, skip everything else as AC using MPEG-1 table
            for (int i = 0; i < 2048 && !br_eof(&br); i++) {
                int dcsize = m1_dc_size_lum(&br);
                if (dcsize < 0) break;
                int diff = m1_dc_diff(&br, dcsize);
                prev_dc += diff;
                dc_vals[ndc++] = prev_dc;

                // Try to skip AC using MPEG-1 AC table
                int k = 1;
                bool first_ac = true;
                bool ac_ok = true;
                while (k < 16 && ac_ok) {
                    int run, level;
                    int ret = m1_ac_decode(&br, &run, &level, first_ac);
                    first_ac = false;
                    if (ret < 0) { ac_ok = false; break; }
                    if (ret == 1) break;
                    k += run + 1;
                }
                if (!ac_ok) { printf("AC fail at block %d\n", i); break; }
            }

            printf("DC-only: %d blocks, %d bits\n", ndc, br.total);
            if (ndc >= 32) {
                printf("First 32 DC: ");
                for (int i = 0; i < 32; i++) printf("%d ", dc_vals[i]);
                printf("\n");
                int mn=99999,mx=-99999;
                for(int i=0;i<ndc;i++){if(dc_vals[i]<mn)mn=dc_vals[i];if(dc_vals[i]>mx)mx=dc_vals[i];}
                printf("DC range: %d to %d (over %d blocks)\n", mn, mx, ndc);
            }

            // Generate DC-only image at 32×36 scaled to 256×288
            if (ndc >= bw*bh) {
                uint8_t scaled[256*288];
                for (int y = 0; y < 288; y++)
                    for (int x = 0; x < 256; x++) {
                        int v = dc_vals[(y/8)*bw + (x/8)] + 128;
                        if (v<0)v=0; if(v>255)v=255;
                        scaled[y*256+x] = v;
                    }
                char path[256];
                snprintf(path,sizeof(path),"/home/wizzard/share/GitHub/pd_m1dc_f%d.pgm",fi);
                write_pgm(path, scaled, 256, 288);
            }
        }

        // === Exp-Golomb decode ===
        printf("\n--- Signed Exp-Golomb ---\n");
        {
            BR br; br_init(&br, bs, bslen);
            int vals[2048], nv = 0;

            for (int i = 0; i < 2048 && !br_eof(&br); i++) {
                // Count leading zeros
                int lz = 0;
                while (!br_eof(&br)) {
                    int bit = br_get1(&br);
                    if (bit < 0) goto done_eg;
                    if (bit == 1) break;
                    lz++;
                    if (lz > 20) goto done_eg; // safety
                }
                int suffix = lz > 0 ? br_get(&br, lz) : 0;
                int code_num = (1 << lz) - 1 + suffix;
                // Signed: even code_num → positive, odd → negative
                int se = (code_num & 1) ? -((code_num+1)/2) : (code_num/2);
                vals[nv++] = se;
            }
            done_eg:
            printf("Exp-Golomb: %d values in %d bits (avg %.1f bits/val)\n",
                   nv, br.total, (double)br.total/nv);
            if (nv >= 32) {
                printf("First 32 values: ");
                for (int i = 0; i < 32; i++) printf("%d ", vals[i]);
                printf("\n");
            }

            // Try interpreting every 16th value as DC (16 values per 4×4 block)
            if (nv >= bw*bh*16) {
                uint8_t scaled[256*288];
                for (int y = 0; y < 288; y++)
                    for (int x = 0; x < 256; x++) {
                        int bi2 = (y/8)*bw + (x/8);
                        int v = vals[bi2 * 16] + 128; // first value of each block
                        if(v<0)v=0;if(v>255)v=255;
                        scaled[y*256+x] = v;
                    }
                write_pgm("/home/wizzard/share/GitHub/pd_eg_dc16_f1.pgm", scaled, 256, 288);
            }

            // Try just sequential values as pixel rows
            if (nv >= W*4) {
                uint8_t *img = calloc(W*H, 1);
                for (int i = 0; i < W*H && i < nv; i++) {
                    int v = vals[i] + 128;
                    if(v<0)v=0;if(v>255)v=255;
                    img[i] = v;
                }
                write_pgm("/home/wizzard/share/GitHub/pd_eg_raw_f1.pgm", img, W, H);
                free(img);
            }
        }
    }

    free(disc); zip_close(z);
    return 0;
}
