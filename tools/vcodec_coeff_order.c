/*
 * vcodec_coeff_order.c - Test coefficient-first orderings and fixed-width AC
 *
 * KEY INSIGHT: per-AC flag model gives UNIFORM distribution across all
 * frequencies — this proves the model is WRONG. The bits between DCs
 * are being misinterpreted.
 *
 * New approaches:
 * 1. Progressive/spectral ordering: all DCs, then all AC[1], then all AC[2], etc.
 * 2. Fixed-width AC coefficients (3-bit or 4-bit signed)
 * 3. Band-based: DC for all blocks, then low-freq AC, then high-freq AC
 * 4. All DC first + bulk AC data with a different VLC
 * 5. Macroblock CBP (which blocks have AC) + VLC AC coefficients
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

static void write_ppm(const char *p, const uint8_t *rgb, int w, int h) {
    FILE *f=fopen(p,"wb"); if(!f)return;
    fprintf(f,"P6\n%d %d\n255\n",w,h); fwrite(rgb,1,w*h*3,f); fclose(f);
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
static int br_sget(BR *b, int n) {
    /* Read n-bit signed (two's complement) */
    int v = br_get(b, n);
    if (v >= (1 << (n-1))) v -= (1 << n);
    return v;
}

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

static const int zz8[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

static void idct8x8(double block[64], double out[64]) {
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

static int clamp8(int v) { return v<0?0:v>255?255:v; }

static void render_blocks(double blocks[][64], int imgW, int imgH, const char *name) {
    double *planeY = calloc(imgW*imgH, sizeof(double));
    double *planeCb = calloc((imgW/2)*(imgH/2), sizeof(double));
    double *planeCr = calloc((imgW/2)*(imgH/2), sizeof(double));

    int blk = 0;
    for (int mby = 0; mby < 9; mby++) {
        for (int mbx = 0; mbx < 8; mbx++) {
            for (int yb = 0; yb < 4; yb++, blk++) {
                double spatial[64]; idct8x8(blocks[blk], spatial);
                int bx = mbx*2+(yb&1), by = mby*2+(yb>>1);
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    planeY[(by*8+y)*imgW+bx*8+x] = spatial[y*8+x]+128.0;
            }
            for (int c=0;c<2;c++,blk++) {
                double spatial[64]; idct8x8(blocks[blk], spatial);
                double *plane=(c==0)?planeCb:planeCr;
                for (int y=0;y<8;y++) for(int x=0;x<8;x++)
                    plane[(mby*8+y)*(imgW/2)+mbx*8+x] = spatial[y*8+x];
            }
        }
    }

    uint8_t *rgb = malloc(imgW*imgH*3);
    double pmin=1e9, pmax=-1e9;
    for(int i=0;i<imgW*imgH;i++){if(planeY[i]<pmin)pmin=planeY[i];if(planeY[i]>pmax)pmax=planeY[i];}
    for (int y=0;y<imgH;y++) for(int x=0;x<imgW;x++) {
        double yv=planeY[y*imgW+x], cb=planeCb[(y/2)*(imgW/2)+x/2], cr=planeCr[(y/2)*(imgW/2)+x/2];
        rgb[(y*imgW+x)*3+0]=clamp8((int)round(yv+1.402*cr));
        rgb[(y*imgW+x)*3+1]=clamp8((int)round(yv-0.344*cb-0.714*cr));
        rgb[(y*imgW+x)*3+2]=clamp8((int)round(yv+1.772*cb));
    }
    char path[256];
    snprintf(path,sizeof(path),OUT_DIR "co_%s.ppm",name);
    write_ppm(path, rgb, imgW, imgH);
    printf("  %s: Y[%.0f, %.0f]\n", name, pmin, pmax);
    free(rgb); free(planeY); free(planeCb); free(planeCr);
}

/* Model 1: Progressive - all DC first, then per-coefficient VLC pass */
static void test_progressive(const uint8_t *f, int fsize, int imgW, int imgH) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double blocks[432][64];
    memset(blocks, 0, sizeof(blocks));

    /* Pass 0: All 432 DC coefficients (DPCM) */
    double dc_y=0, dc_cb=0, dc_cr=0;
    int blk = 0;
    for (int mby = 0; mby < 9; mby++)
        for (int mbx = 0; mbx < 8; mbx++) {
            for (int yb = 0; yb < 4; yb++, blk++) {
                dc_y += vlc_coeff(&br);
                blocks[blk][0] = dc_y;
            }
            dc_cb += vlc_coeff(&br); blocks[blk][0] = dc_cb; blk++;
            dc_cr += vlc_coeff(&br); blocks[blk][0] = dc_cr; blk++;
        }
    int dc_bits = br.total;
    printf("  Progressive DC: %d bits (%.1f%%)\n", dc_bits, 100.0*dc_bits/(bslen*8));

    /* Pass 1+: For each AC position, read VLC for all 432 blocks */
    int ac_pos_done = 0;
    for (int ac = 1; ac < 64 && !br_eof(&br); ac++) {
        for (int b = 0; b < 432 && !br_eof(&br); b++)
            blocks[b][zz8[ac]] = vlc_coeff(&br);
        ac_pos_done = ac;
    }
    printf("  Progressive: %d AC positions done, bits %d/%d (%.1f%%)\n",
        ac_pos_done, br.total, bslen*8, 100.0*br.total/(bslen*8));

    render_blocks(blocks, imgW, imgH, "prog_vlc");
}

/* Model 2: Progressive with per-block flag per AC position */
static void test_progressive_flag(const uint8_t *f, int fsize, int imgW, int imgH) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double blocks[432][64];
    memset(blocks, 0, sizeof(blocks));

    /* All DC first */
    double dc_y=0, dc_cb=0, dc_cr=0;
    int blk = 0;
    for (int mby = 0; mby < 9; mby++)
        for (int mbx = 0; mbx < 8; mbx++) {
            for (int yb = 0; yb < 4; yb++, blk++) {
                dc_y += vlc_coeff(&br);
                blocks[blk][0] = dc_y;
            }
            dc_cb += vlc_coeff(&br); blocks[blk][0] = dc_cb; blk++;
            dc_cr += vlc_coeff(&br); blocks[blk][0] = dc_cr; blk++;
        }

    /* For each AC position: 1-bit flag per block, then VLC for flagged blocks */
    for (int ac = 1; ac < 64 && !br_eof(&br); ac++) {
        for (int b = 0; b < 432 && !br_eof(&br); b++) {
            if (br_get1(&br))
                blocks[b][zz8[ac]] = vlc_coeff(&br);
        }
    }
    printf("  ProgFlag: bits %d/%d (%.1f%%)\n", br.total, bslen*8, 100.0*br.total/(bslen*8));
    render_blocks(blocks, imgW, imgH, "prog_flag");
}

/* Model 3: Fixed-width AC coefficients */
static void test_fixed_width(const uint8_t *f, int fsize, int imgW, int imgH, int ac_bits) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double blocks[432][64];
    memset(blocks, 0, sizeof(blocks));

    double dc_y=0, dc_cb=0, dc_cr=0;
    int blk = 0;
    for (int mby = 0; mby < 9 && !br_eof(&br); mby++)
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++, blk++) {
                dc_y += vlc_coeff(&br);
                blocks[blk][0] = dc_y;
                for (int i = 1; i < 64 && !br_eof(&br); i++)
                    blocks[blk][zz8[i]] = br_sget(&br, ac_bits);
            }
            for (int c=0;c<2&&!br_eof(&br);c++,blk++) {
                double *dc = (c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                blocks[blk][0] = *dc;
                for (int i=1;i<64&&!br_eof(&br);i++)
                    blocks[blk][zz8[i]] = br_sget(&br, ac_bits);
            }
        }

    char name[64]; snprintf(name,sizeof(name),"fixed%d", ac_bits);
    printf("  %s: %d/%d blocks, bits %d/%d (%.1f%%)\n",
        name, blk, 432, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render_blocks(blocks, imgW, imgH, name);
}

/* Model 4: Macroblock CBP (6-bit pattern) + VLC AC data for coded blocks */
static void test_mbcbp(const uint8_t *f, int fsize, int imgW, int imgH) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double blocks[432][64];
    memset(blocks, 0, sizeof(blocks));

    double dc_y=0, dc_cb=0, dc_cr=0;
    int coded_ac = 0, skipped_ac = 0;
    int blk = 0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            /* Read 6-bit CBP: which of 4Y+Cb+Cr blocks have AC data */
            int cbp = br_get(&br, 6);

            for (int yb = 0; yb < 4 && !br_eof(&br); yb++, blk++) {
                dc_y += vlc_coeff(&br);
                blocks[blk][0] = dc_y;
                if (cbp & (1 << (5-yb))) { /* Y blocks: bits 5,4,3,2 */
                    for (int i=1;i<64&&!br_eof(&br);i++)
                        blocks[blk][zz8[i]] = vlc_coeff(&br);
                    coded_ac++;
                } else {
                    skipped_ac++;
                }
            }
            for (int c=0;c<2&&!br_eof(&br);c++,blk++) {
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                blocks[blk][0] = *dc;
                if (cbp & (1 << (1-c))) { /* Cb=bit1, Cr=bit0 */
                    for (int i=1;i<64&&!br_eof(&br);i++)
                        blocks[blk][zz8[i]] = vlc_coeff(&br);
                    coded_ac++;
                } else {
                    skipped_ac++;
                }
            }
        }
    }

    printf("  mbcbp: coded=%d skipped=%d bits %d/%d (%.1f%%)\n",
        coded_ac, skipped_ac, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render_blocks(blocks, imgW, imgH, "mbcbp");
}

/* Model 5: VLC skip count + VLC CBP per macroblock (like MPEG-1 P-frame) */
static void test_mb_header(const uint8_t *f, int fsize, int imgW, int imgH) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double blocks[432][64];
    memset(blocks, 0, sizeof(blocks));

    double dc_y=0, dc_cb=0, dc_cr=0;
    int blk = 0;

    for (int mby = 0; mby < 9 && !br_eof(&br); mby++) {
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            /* DC for all 6 blocks first, then AC with EOB */
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++, blk++) {
                dc_y += vlc_coeff(&br);
                blocks[blk][0] = dc_y;
            }
            dc_cb += vlc_coeff(&br); blocks[blk][0] = dc_cb; blk++;
            dc_cr += vlc_coeff(&br); blocks[blk][0] = dc_cr; blk++;

            /* AC for all 6 blocks with EOB */
            for (int b = blk-6; b < blk && !br_eof(&br); b++) {
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    int v = vlc_coeff(&br);
                    if (v == 0) break; /* EOB */
                    blocks[b][zz8[i]] = v;
                }
            }
        }
    }

    printf("  mb_dc_ac: bits %d/%d (%.1f%%)\n", br.total, bslen*8, 100.0*br.total/(bslen*8));
    render_blocks(blocks, imgW, imgH, "mb_dc_ac");
}

/* Model 6: DC + AC with EOB for all blocks, then per-AC flag enhancement */
static void test_eob_enhance(const uint8_t *f, int fsize, int imgW, int imgH) {
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    BR br; br_init(&br, bs, bslen);

    double blocks[432][64];
    memset(blocks, 0, sizeof(blocks));

    /* First pass: DC + AC with EOB */
    double dc_y=0, dc_cb=0, dc_cr=0;
    int blk = 0;
    int total_acs = 0;
    for (int mby = 0; mby < 9 && !br_eof(&br); mby++)
        for (int mbx = 0; mbx < 8 && !br_eof(&br); mbx++) {
            for (int yb = 0; yb < 4 && !br_eof(&br); yb++, blk++) {
                dc_y += vlc_coeff(&br);
                blocks[blk][0] = dc_y;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    int v = vlc_coeff(&br);
                    if (v == 0) break;
                    blocks[blk][zz8[i]] = v;
                    total_acs++;
                }
            }
            for (int c=0;c<2&&!br_eof(&br);c++,blk++) {
                double *dc=(c==0)?&dc_cb:&dc_cr;
                *dc += vlc_coeff(&br);
                blocks[blk][0] = *dc;
                for (int i=1;i<64&&!br_eof(&br);i++) {
                    int v = vlc_coeff(&br);
                    if (v == 0) break;
                    blocks[blk][zz8[i]] = v;
                    total_acs++;
                }
            }
        }
    int pass1 = br.total;
    printf("  eob+ref: pass1 %d bits (%.1f%%), %d ACs\n", pass1, 100.0*pass1/(bslen*8), total_acs);

    /* Pass 2: remaining bits as additional spectral data
     * For each block, for positions that were zero-terminated,
     * continue reading VLC values from where EOB stopped */
    /* Actually, let's try: remaining bits are a REFINEMENT pass.
     * For each block, for each non-zero position, read 1 bit to add to LSB */
    int refined = 0;
    for (int b = 0; b < 432 && !br_eof(&br); b++) {
        for (int i = 1; i < 64 && !br_eof(&br); i++) {
            if (blocks[b][zz8[i]] != 0) {
                int bit = br_get1(&br);
                if (bit) {
                    /* Refine: add 1 to magnitude */
                    if (blocks[b][zz8[i]] > 0) blocks[b][zz8[i]] += 0.5;
                    else blocks[b][zz8[i]] -= 0.5;
                    refined++;
                }
            }
        }
    }
    printf("  eob+ref: pass2 refined %d, total %d/%d (%.1f%%)\n",
        refined, br.total, bslen*8, 100.0*br.total/(bslen*8));
    render_blocks(blocks, imgW, imgH, "eob_refine");
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    int slba = argc > 2 ? atoi(argv[2]) : 5232;

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

    static uint8_t frames[4][MAX_FRAME]; int fsizes[4];
    int nf = assemble_frames(disc,tsec,slba,frames,fsizes,4);
    if (nf < 1) return 1;

    int imgW = 128, imgH = 144;
    printf("LBA %d: qs=%d type=%d\n", slba, frames[0][3], frames[0][39]);

    printf("\n--- Progressive VLC (all DCs, then all AC[1], all AC[2], ...) ---\n");
    test_progressive(frames[0],fsizes[0],imgW,imgH);

    printf("\n--- Progressive with per-block flag ---\n");
    test_progressive_flag(frames[0],fsizes[0],imgW,imgH);

    printf("\n--- Fixed-width AC ---\n");
    test_fixed_width(frames[0],fsizes[0],imgW,imgH, 3);
    test_fixed_width(frames[0],fsizes[0],imgW,imgH, 4);

    printf("\n--- MB CBP (6-bit per MB) + VLC AC ---\n");
    test_mbcbp(frames[0],fsizes[0],imgW,imgH);

    printf("\n--- MB: all 6 DCs then all 6 AC+EOB ---\n");
    test_mb_header(frames[0],fsizes[0],imgW,imgH);

    printf("\n--- EOB + refinement pass ---\n");
    test_eob_enhance(frames[0],fsizes[0],imgW,imgH);

    free(disc); zip_close(z);
    return 0;
}
