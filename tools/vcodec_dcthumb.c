/*
 * vcodec_dcthumb.c - Extract DC-only thumbnails with various block skip strategies
 *
 * The idea: decode ONLY the first VLC per block (DC), then skip ahead
 * by various amounts to test different block sizes and structures.
 * A correct structure should produce a recognizable low-res image.
 *
 * Tests:
 * A) Pure VLC stream: every Nth VLC value is a DC (no bit flags, no EOB)
 * B) Per-AC bit flag: DC VLC + 63 (flag + optional VLC) per block
 * C) EOB model: DC VLC + VLCs until value=0
 * D) Fixed bit count per block (try various)
 * E) Different block counts (not 432)
 * F) Test at LBAs from different scenes
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <zip.h>

#define SECTOR_RAW 2352
#define MAX_FRAME  65536
#define OUT_DIR "/home/wizzard/share/GitHub/playdia-emu/tools/test_output/"

static void write_pgm(const char *p, const uint8_t *g, int w, int h) {
    FILE *f=fopen(p,"wb"); if(!f)return;
    fprintf(f,"P5\n%d %d\n255\n",w,h); fwrite(g,1,w*h,f); fclose(f);
    printf("  -> %s (%dx%d)\n",p,w,h);
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

/* Upscale a small image by factor */
static void upscale(const uint8_t *src, int sw, int sh, uint8_t *dst, int factor) {
    int dw = sw * factor;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++)
            for (int dy = 0; dy < factor; dy++)
                for (int dx = 0; dx < factor; dx++)
                    dst[(y*factor+dy)*dw + x*factor+dx] = src[y*sw+x];
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    int slba = argc > 2 ? atoi(argv[2]) : 502;
    const char *tag = argc > 3 ? argv[3] : "m";

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
    if (nf < 1) { printf("No frames found\n"); return 1; }

    /* Use frame 0 (intra frame) */
    uint8_t *f = frames[0];
    int fsize = fsizes[0];
    int qscale = f[3];
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;
    printf("Frame 0: qscale=%d, type=%d, bslen=%d\n\n", qscale, f[39], bslen);

    /* ====== TEST A: Every Nth VLC is a DC ====== */
    printf("=== TEST A: Every Nth VLC is DC (flat stream) ===\n");
    {
        BR br; br_init(&br, bs, bslen);
        int vals[25000]; int nv=0;
        while(nv<25000 && !br_eof(&br)) {
            int old=br.total; vals[nv]=vlc_coeff(&br);
            if(br.total==old) break; nv++;
        }
        printf("Total VLC values: %d\n", nv);

        /* Try N = 48, 49, 64, 65 (common block sizes + DC) */
        int strides[] = {17, 33, 49, 64, 65};
        for (int si = 0; si < 5; si++) {
            int stride = strides[si];
            int nblocks = nv / stride;
            /* Try multiple widths */
            int widths[] = {8, 16, 18, 20, 24, 32};
            for (int wi = 0; wi < 6; wi++) {
                int w = widths[wi];
                int h = nblocks / w;
                if (h < 4 || w*h > nblocks) continue;

                uint8_t *img = malloc(w*h);
                int dc = 0;
                for (int i = 0; i < w*h; i++) {
                    dc += vals[i*stride];
                    img[i] = clamp8(dc * qscale / 2 + 128);
                }
                char path[256];
                snprintf(path,sizeof(path),OUT_DIR "dctA_%s_s%d_w%d.pgm",tag,stride,w);

                /* Upscale for visibility */
                int factor = 8;
                uint8_t *big = malloc(w*factor*h*factor);
                upscale(img, w, h, big, factor);
                write_pgm(path, big, w*factor, h*factor);
                free(img); free(big);
            }
        }
    }

    /* ====== TEST B: Per-AC bit flag, DC-only extraction ====== */
    printf("\n=== TEST B: Per-AC bit flag DC extraction ===\n");
    /* 4:2:0 MB-interleaved: 4Y+Cb+Cr per macroblock */
    {
        BR br; br_init(&br, bs, bslen);
        int mbw=8, mbh=9;
        uint8_t dc_img_y[16*18]; /* 16 blocks wide, 18 high */
        int dc_y=0, dc_cb=0, dc_cr=0;
        int bidx = 0;

        for (int mby=0; mby<mbh && !br_eof(&br); mby++) {
            for (int mbx=0; mbx<mbw && !br_eof(&br); mbx++) {
                for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                    dc_y += vlc_coeff(&br);
                    int bx = mbx*2 + (yb&1);
                    int by = mby*2 + (yb>>1);
                    if (by < 18 && bx < 16)
                        dc_img_y[by*16+bx] = clamp8(dc_y * qscale + 128);
                    /* Skip 63 AC positions */
                    for (int i=1; i<64 && !br_eof(&br); i++)
                        if (br_get1(&br)) vlc_coeff(&br);
                }
                dc_cb += vlc_coeff(&br);
                for(int i=1;i<64 && !br_eof(&br);i++) if(br_get1(&br)) vlc_coeff(&br);
                dc_cr += vlc_coeff(&br);
                for(int i=1;i<64 && !br_eof(&br);i++) if(br_get1(&br)) vlc_coeff(&br);
            }
        }
        printf("  Bits: %d/%d (%.1f%%)\n", br.total, bslen*8, 100.0*br.total/(bslen*8));

        uint8_t *big = malloc(16*8*18*8);
        upscale(dc_img_y, 16, 18, big, 8);
        char path[256];
        snprintf(path,sizeof(path),OUT_DIR "dctB_acflag_%s.pgm",tag);
        write_pgm(path, big, 128, 144);
        free(big);
    }

    /* ====== TEST C: EOB model DC extraction ====== */
    printf("\n=== TEST C: EOB (value=0 ends block) DC extraction ===\n");
    {
        BR br; br_init(&br, bs, bslen);
        int mbw=8, mbh=9;
        uint8_t dc_img[16*18];
        int dc_y=0;
        int total_ac = 0;

        for (int mby=0; mby<mbh && !br_eof(&br); mby++) {
            for (int mbx=0; mbx<mbw && !br_eof(&br); mbx++) {
                for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                    dc_y += vlc_coeff(&br);
                    int bx = mbx*2 + (yb&1);
                    int by = mby*2 + (yb>>1);
                    if (by<18 && bx<16)
                        dc_img[by*16+bx] = clamp8(dc_y * qscale + 128);
                    /* Read AC until value=0 (EOB) */
                    int ac=0;
                    while(ac<63 && !br_eof(&br)){
                        int v=vlc_coeff(&br);
                        if(v==0) break;
                        ac++; total_ac++;
                    }
                }
                /* Skip Cb, Cr same way */
                for(int c=0;c<2 && !br_eof(&br);c++){
                    vlc_coeff(&br);
                    int ac=0;
                    while(ac<63 && !br_eof(&br)){
                        int v=vlc_coeff(&br); if(v==0)break; ac++; total_ac++;
                    }
                }
            }
        }
        printf("  Bits: %d/%d (%.1f%%), avg AC/block=%.1f\n",
               br.total, bslen*8, 100.0*br.total/(bslen*8), (double)total_ac/432);

        uint8_t *big = malloc(128*144);
        upscale(dc_img, 16, 18, big, 8);
        char path[256];
        snprintf(path,sizeof(path),OUT_DIR "dctC_eob_%s.pgm",tag);
        write_pgm(path, big, 128, 144);
        free(big);
    }

    /* ====== TEST D: 1-bit skip flag per block + DC only ====== */
    printf("\n=== TEST D: 1-bit skip flag per block ===\n");
    {
        BR br; br_init(&br, bs, bslen);
        uint8_t dc_img[16*18];
        memset(dc_img, 128, sizeof(dc_img));
        int dc_y=0;
        int coded=0, skipped=0;

        for (int mby=0; mby<9 && !br_eof(&br); mby++) {
            for (int mbx=0; mbx<8 && !br_eof(&br); mbx++) {
                for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                    int flag = br_get1(&br);
                    int bx = mbx*2 + (yb&1);
                    int by = mby*2 + (yb>>1);
                    if (flag) {
                        dc_y += vlc_coeff(&br);
                        coded++;
                    } else {
                        skipped++;
                    }
                    if (by<18 && bx<16)
                        dc_img[by*16+bx] = clamp8(dc_y * qscale + 128);
                }
                for(int c=0;c<2 && !br_eof(&br);c++){
                    int flag=br_get1(&br);
                    if(flag) vlc_coeff(&br);
                }
            }
        }
        printf("  Coded: %d, Skipped: %d, Bits: %d/%d\n",
               coded, skipped, br.total, bslen*8);

        uint8_t *big = malloc(128*144);
        upscale(dc_img, 16, 18, big, 8);
        char path[256];
        snprintf(path,sizeof(path),OUT_DIR "dctD_skip_%s.pgm",tag);
        write_pgm(path, big, 128, 144);
        free(big);
    }

    /* ====== TEST E: 1-bit skip + per-AC flag combined ====== */
    printf("\n=== TEST E: 1-bit block skip + per-AC bit flag ===\n");
    {
        BR br; br_init(&br, bs, bslen);
        uint8_t dc_img[16*18];
        memset(dc_img, 128, sizeof(dc_img));
        int dc_y=0;
        int coded=0, skipped=0;

        for (int mby=0; mby<9 && !br_eof(&br); mby++) {
            for (int mbx=0; mbx<8 && !br_eof(&br); mbx++) {
                for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                    int flag = br_get1(&br);
                    int bx = mbx*2 + (yb&1);
                    int by = mby*2 + (yb>>1);
                    if (flag) {
                        dc_y += vlc_coeff(&br);
                        for (int i=1;i<64 && !br_eof(&br);i++)
                            if(br_get1(&br)) vlc_coeff(&br);
                        coded++;
                    } else {
                        skipped++;
                    }
                    if (by<18 && bx<16)
                        dc_img[by*16+bx] = clamp8(dc_y * qscale + 128);
                }
                for(int c=0;c<2 && !br_eof(&br);c++){
                    int flag=br_get1(&br);
                    if(flag){
                        vlc_coeff(&br);
                        for(int i=1;i<64 && !br_eof(&br);i++)
                            if(br_get1(&br)) vlc_coeff(&br);
                    }
                }
            }
        }
        printf("  Coded: %d, Skipped: %d, Bits: %d/%d (%.1f%%)\n",
               coded, skipped, br.total, bslen*8, 100.0*br.total/(bslen*8));

        uint8_t *big = malloc(128*144);
        upscale(dc_img, 16, 18, big, 8);
        char path[256];
        snprintf(path,sizeof(path),OUT_DIR "dctE_skipac_%s.pgm",tag);
        write_pgm(path, big, 128, 144);
        free(big);
    }

    /* ====== TEST F: Plane-sequential with per-AC bit flag ====== */
    printf("\n=== TEST F: Plane-sequential (all Y first) per-AC flag ===\n");
    {
        BR br; br_init(&br, bs, bslen);
        uint8_t dc_img[16*18];
        int dc=0;

        for (int by=0; by<18 && !br_eof(&br); by++) {
            for (int bx=0; bx<16 && !br_eof(&br); bx++) {
                dc += vlc_coeff(&br);
                dc_img[by*16+bx] = clamp8(dc * qscale + 128);
                for (int i=1;i<64 && !br_eof(&br);i++)
                    if(br_get1(&br)) vlc_coeff(&br);
            }
        }
        printf("  Y bits: %d/%d (%.1f%%)\n", br.total, bslen*8, 100.0*br.total/(bslen*8));

        uint8_t *big = malloc(128*144);
        upscale(dc_img, 16, 18, big, 8);
        char path[256];
        snprintf(path,sizeof(path),OUT_DIR "dctF_plseq_%s.pgm",tag);
        write_pgm(path, big, 128, 144);
        free(big);
    }

    /* ====== TEST G: DC + EOB, at different scenes ====== */
    printf("\n=== TEST G: Try multiple scenes with EOB model ===\n");
    {
        /* Re-assemble at different LBAs */
        int scene_lbas[] = {277, 502, 757, 1112, 1352, 1592, 1872};
        for (int si = 0; si < 7; si++) {
            int lba = scene_lbas[si];
            static uint8_t sf[4][MAX_FRAME]; int sfs[4];
            int snf = assemble_frames(disc,tsec,lba,sf,sfs,4);
            if (snf < 1) continue;

            BR br; br_init(&br, sf[0]+40, sfs[0]-40);
            uint8_t dc_img[16*18];
            int dc=0;

            for (int mby=0; mby<9 && !br_eof(&br); mby++) {
                for (int mbx=0; mbx<8 && !br_eof(&br); mbx++) {
                    for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                        dc += vlc_coeff(&br);
                        int bx = mbx*2 + (yb&1);
                        int by = mby*2 + (yb>>1);
                        if (by<18 && bx<16)
                            dc_img[by*16+bx] = clamp8(dc * qscale + 128);
                        int ac=0;
                        while(ac<63 && !br_eof(&br)){
                            int v=vlc_coeff(&br); if(v==0)break; ac++;
                        }
                    }
                    for(int c=0;c<2 && !br_eof(&br);c++){
                        vlc_coeff(&br);
                        int ac=0;
                        while(ac<63 && !br_eof(&br)){
                            int v=vlc_coeff(&br); if(v==0)break; ac++;
                        }
                    }
                }
            }
            printf("  LBA %d: bits %d/%d (%.1f%%)\n",
                   lba, br.total, (sfs[0]-40)*8, 100.0*br.total/((sfs[0]-40)*8));

            uint8_t *big = malloc(128*144);
            upscale(dc_img, 16, 18, big, 8);
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "dctG_scene_%s_lba%d.pgm",tag,lba);
            write_pgm(path, big, 128, 144);
            free(big);
        }
    }

    /* ====== TEST H: What if resolution is different? Try 176x132 (QCIF-ish) ====== */
    printf("\n=== TEST H: Different resolutions with per-AC flag ===\n");
    {
        /* 176x144 = 22x18 blocks = 396 blocks = 11x9 MBs (4:2:0 = 6*99 = 594 blocks) */
        /* 160x120 = 20x15 blocks = 300 blocks = 10x7.5 (not integer) */
        /* 128x96 = 16x12 blocks = 192 Y blocks = 8x6 MBs = 8*6*(4+2)=288 blocks */
        /* 96x72 = 12x9 blocks = 108 Y blocks = 6x4.5 (not integer) */

        struct { int mbw, mbh; const char *name; } resolutions[] = {
            {8, 9, "128x144"},
            {11, 9, "176x144"},
            {8, 6, "128x96"},
            {10, 8, "160x128"},
            {16, 9, "256x144"},
            {8, 12, "128x192"},
        };

        for (int ri = 0; ri < 6; ri++) {
            int mbw = resolutions[ri].mbw;
            int mbh = resolutions[ri].mbh;
            int ybw = mbw*2, ybh = mbh*2;
            int total_blocks = mbw*mbh*6;

            BR br; br_init(&br, bs, bslen);
            uint8_t *dc_img = calloc(ybw*ybh, 1);
            int dc_y=0;
            int ok = 1;

            for (int mby=0; mby<mbh && !br_eof(&br) && ok; mby++) {
                for (int mbx=0; mbx<mbw && !br_eof(&br) && ok; mbx++) {
                    for (int yb=0; yb<4 && !br_eof(&br); yb++) {
                        dc_y += vlc_coeff(&br);
                        int bx = mbx*2 + (yb&1);
                        int by = mby*2 + (yb>>1);
                        if (by<ybh && bx<ybw)
                            dc_img[by*ybw+bx] = clamp8(dc_y * qscale + 128);
                        for (int i=1;i<64 && !br_eof(&br);i++)
                            if(br_get1(&br)) vlc_coeff(&br);
                    }
                    /* Cb, Cr */
                    for(int c=0;c<2 && !br_eof(&br);c++){
                        vlc_coeff(&br);
                        for(int i=1;i<64 && !br_eof(&br);i++)
                            if(br_get1(&br)) vlc_coeff(&br);
                    }
                    if (br.total > bslen*8) { ok=0; break; }
                }
            }
            printf("  %s (%d MBs, %d blocks): bits %d/%d (%.1f%%) %s\n",
                   resolutions[ri].name, mbw*mbh, total_blocks,
                   br.total, bslen*8, 100.0*br.total/(bslen*8),
                   ok ? "OK" : "OVERFLOW");

            int scale = 8;
            uint8_t *big = malloc(ybw*scale*ybh*scale);
            upscale(dc_img, ybw, ybh, big, scale);
            char path[256];
            snprintf(path,sizeof(path),OUT_DIR "dctH_%s_%s.pgm",resolutions[ri].name,tag);
            write_pgm(path, big, ybw*scale, ybh*scale);
            free(big); free(dc_img);
        }
    }

    free(disc); zip_close(z);
    return 0;
}
