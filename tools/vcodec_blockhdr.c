/*
 * vcodec_blockhdr.c - Block header hypothesis: each block starts with a count
 * of nonzero coefficients, followed by position+value pairs.
 *
 * This tests the hypothesis that the AC bitstream has per-block structure:
 *   [count_bits: N] [pos_bits + val_bits] × N
 *
 * Key test: compare real data vs random data consumption.
 * If real ≈ 100% and random >> 100%, the scheme is promising.
 *
 * Also tests: decoded image quality with this scheme.
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

/* Test block header scheme: count_bits + N × (pos_bits + val_bits) */
static int test_blockhdr(const uint8_t *ac, int abits, int cb, int pb, int vb,
                         int *out_nz, int *out_counts, int *out_bands) {
    int bp=0, nz=0;
    if(out_bands) memset(out_bands,0,8*sizeof(int));
    if(out_counts) memset(out_counts,0,32*sizeof(int));
    for(int b=0;b<NBLK;b++){
        if(bp+cb>abits) return -1;
        int cnt=get_bits(ac,bp,cb); bp+=cb;
        if(out_counts && cnt<32) out_counts[cnt]++;
        int entry_bits=pb+vb;
        if(bp+cnt*entry_bits>abits) return -1;
        for(int i=0;i<cnt;i++){
            int pos=get_bits(ac,bp,pb); bp+=pb;
            /*int val=*/get_bits(ac,bp,vb); bp+=vb;
            nz++;
            if(out_bands && pos<63){int band=pos/8;if(band<8)out_bands[band]++;}
        }
    }
    if(out_nz) *out_nz=nz;
    return bp;
}

/* Test block header with DC VLC for values instead of fixed-width */
static int test_blockhdr_vlc(const uint8_t *ac, int abits, int cb, int pb,
                              int *out_nz, int *out_bands) {
    int bp=0, nz=0;
    if(out_bands) memset(out_bands,0,8*sizeof(int));
    for(int b=0;b<NBLK;b++){
        if(bp+cb>abits) return -1;
        int cnt=get_bits(ac,bp,cb); bp+=cb;
        for(int i=0;i<cnt;i++){
            if(bp+pb>abits) return -1;
            int pos=get_bits(ac,bp,pb); bp+=pb;
            int val;
            int c=dec_dc(ac,bp,&val,abits);
            if(c<0) return -1;
            bp+=c;
            nz++;
            if(out_bands && pos<63){int band=pos/8;if(band<8)out_bands[band]++;}
        }
    }
    if(out_nz) *out_nz=nz;
    return bp;
}

/* Test block header where count uses VLC instead of fixed bits */
static int test_blockhdr_vlccnt(const uint8_t *ac, int abits, int pb, int vb,
                                int *out_nz, int *out_bands) {
    int bp=0, nz=0;
    if(out_bands) memset(out_bands,0,8*sizeof(int));
    for(int b=0;b<NBLK;b++){
        /* Read count as DC VLC */
        int cnt;
        int c=dec_dc(ac,bp,&cnt,abits);
        if(c<0) return -1;
        bp+=c;
        if(cnt<0) cnt=-cnt; /* absolute value */
        if(cnt>63) return -1;
        int entry_bits=pb+vb;
        if(bp+cnt*entry_bits>abits) return -1;
        for(int i=0;i<cnt;i++){
            int pos=get_bits(ac,bp,pb); bp+=pb;
            bp+=vb;
            nz++;
            if(out_bands && pos<63){int band=pos/8;if(band<8)out_bands[band]++;}
        }
    }
    if(out_nz) *out_nz=nz;
    return bp;
}

/* Test: fixed bits per block (all blocks same size) */
static void test_fixed_block_size(const uint8_t *ac, int abits) {
    /* Total AC bits / 864 blocks */
    int bpb = abits / NBLK;
    int rem = abits % NBLK;
    printf("\nFixed block size: %d bits/block (%d remainder)\n", bpb, rem);

    /* Check if blocks are aligned to byte boundaries */
    printf("Byte alignment: %s (%.1f bytes/block)\n",
           (bpb % 8 == 0) ? "YES" : "no", bpb/8.0);

    /* Check common sizes */
    int sizes[] = {96, 104, 112, 120, 128};
    for(int s=0;s<5;s++){
        int total = sizes[s]*NBLK;
        printf("  %d bits/block × 864 = %d (diff from AC: %+d)\n",
               sizes[s], total, total-abits);
    }
}

int main() {
    const char *binfile = "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin";

    /* Load padded frame (F03 from LBA 502, 4th frame = index 3) */
    printf("=== Loading padded frame F03 (LBA 502, index 3) ===\n");
    if(load_frame(binfile, 502, 3)!=0){printf("Failed to load F03\n");return 1;}
    printf("QS=%d, type=%d\n", fbuf[3], fbuf[39]);

    int de=flen; while(de>0&&fbuf[de-1]==0xFF)de--;
    int pad=flen-de;
    int total_bits=(de-40)*8;
    printf("Data: %d bytes, padding: %d bytes\n", de-40, pad);

    int bp=0;
    for(int b=0;b<NBLK;b++){int dv;int c=dec_dc(fbuf+40,bp,&dv,total_bits);if(c<0){printf("DC fail at %d\n",b);return 1;}bp+=c;}
    int dc_bits=bp, ac_bits=total_bits-dc_bits;
    printf("DC: %d bits, AC: %d bits (%.2f bits/coeff)\n\n", dc_bits, ac_bits, (double)ac_bits/(NBLK*63));

    /* Extract AC data */
    int ac_bytes=(ac_bits+7)/8+1;
    uint8_t *ac_data=calloc(ac_bytes,1);
    for(int i=0;i<ac_bits;i++)
        if(get_bit(fbuf+40,dc_bits+i)) ac_data[i>>3]|=(1<<(7-(i&7)));

    /* Random control */
    uint8_t *rnd=malloc(ac_bytes);
    srand(42); for(int i=0;i<ac_bytes;i++) rnd[i]=rand()&0xFF;

    /* Fixed block size analysis */
    test_fixed_block_size(ac_data, ac_bits);

    /* Test all BlockHdr combinations on padded frame */
    printf("\n=== BlockHdr(cb+pb+vb) on padded frame F03 (AC=%d bits) ===\n", ac_bits);
    printf("%-20s  REAL%%  RAND%%   diff  REAL_NZ  RAND_NZ  REAL bands              RAND bands\n", "Scheme");

    for(int cb=3;cb<=6;cb++){
        for(int pb=5;pb<=6;pb++){
            for(int vb=1;vb<=6;vb++){
                int rnz, rrnd_nz, rbands[8], rnd_bands[8];
                int rc = test_blockhdr(ac_data, ac_bits, cb, pb, vb, &rnz, NULL, rbands);
                int rrc = test_blockhdr(rnd, ac_bits, cb, pb, vb, &rrnd_nz, NULL, rnd_bands);
                if(rc>0 && rrc>0){
                    double rpct=100.0*rc/ac_bits, rrpct=100.0*rrc/ac_bits;
                    double diff=rpct-rrpct;
                    /* Only show if real is between 90-110% and differs from random */
                    if(rpct>=90 && rpct<=115 && fabs(diff)>5){
                        printf("BH(%d+%d+%d)  %5.1f%% %5.1f%% %+5.1f  %5d  %5d   ",
                               cb,pb,vb, rpct, rrpct, diff, rnz, rrnd_nz);
                        printf("%d %d %d %d %d %d %d %d  ",
                               rbands[0],rbands[1],rbands[2],rbands[3],
                               rbands[4],rbands[5],rbands[6],rbands[7]);
                        printf("%d %d %d %d %d %d %d %d\n",
                               rnd_bands[0],rnd_bands[1],rnd_bands[2],rnd_bands[3],
                               rnd_bands[4],rnd_bands[5],rnd_bands[6],rnd_bands[7]);
                    }
                }
            }
        }
    }

    /* BlockHdr with VLC values */
    printf("\n=== BlockHdr with VLC values ===\n");
    for(int cb=3;cb<=6;cb++){
        for(int pb=5;pb<=6;pb++){
            int rnz, rnd_nz, rbands[8], rnd_bands[8];
            int rc = test_blockhdr_vlc(ac_data, ac_bits, cb, pb, &rnz, rbands);
            int rrc = test_blockhdr_vlc(rnd, ac_bits, cb, pb, &rnd_nz, rnd_bands);
            if(rc>0 && rrc>0){
                double rpct=100.0*rc/ac_bits, rrpct=100.0*rrc/ac_bits;
                printf("BH_VLC(%d+%d):  REAL=%5.1f%% NZ=%d  RAND=%5.1f%% NZ=%d  diff=%+.1f\n",
                       cb,pb, rpct, rnz, rrpct, rnd_nz, rpct-rrpct);
                if(rpct>=80 && rpct<=120){
                    printf("  REAL bands: %d %d %d %d %d %d %d %d\n",
                           rbands[0],rbands[1],rbands[2],rbands[3],rbands[4],rbands[5],rbands[6],rbands[7]);
                }
            } else {
                printf("BH_VLC(%d+%d):  REAL=%s  RAND=%s\n",
                       cb, pb, rc>0?"OK":"FAIL", rrc>0?"OK":"FAIL");
            }
        }
    }

    /* BlockHdr with VLC count */
    printf("\n=== BlockHdr with VLC count ===\n");
    for(int pb=5;pb<=6;pb++){
        for(int vb=2;vb<=5;vb++){
            int rnz, rnd_nz, rbands[8], rnd_bands[8];
            int rc = test_blockhdr_vlccnt(ac_data, ac_bits, pb, vb, &rnz, rbands);
            int rrc = test_blockhdr_vlccnt(rnd, ac_bits, pb, vb, &rnd_nz, rnd_bands);
            if(rc>0){
                double rpct=100.0*rc/ac_bits;
                double rrpct=rrc>0?100.0*rrc/ac_bits:0;
                printf("BH_VLCCNT(%d+%d):  REAL=%5.1f%% NZ=%d  RAND=%5.1f%% NZ=%d\n",
                       pb,vb, rpct, rnz, rrpct, rnd_nz);
                if(rpct>=80 && rpct<=120){
                    printf("  REAL bands: %d %d %d %d %d %d %d %d\n",
                           rbands[0],rbands[1],rbands[2],rbands[3],rbands[4],rbands[5],rbands[6],rbands[7]);
                }
            }
        }
    }

    /* Now test best candidates on the OTHER padded frame and non-padded frames */
    printf("\n=== Cross-frame validation on F00 (LBA 502, QS=8, pad=217) ===\n");
    if(load_frame(binfile, 502, 0)==0){
        int de2=flen; while(de2>0&&fbuf[de2-1]==0xFF)de2--;
        int tb2=(de2-40)*8;
        int bp2=0;
        for(int b=0;b<NBLK;b++){int dv;bp2+=dec_dc(fbuf+40,bp2,&dv,tb2);}
        int ac2=tb2-bp2;

        uint8_t *ac2d=calloc((ac2+7)/8+1,1);
        for(int i=0;i<ac2;i++)
            if(get_bit(fbuf+40,bp2+i)) ac2d[i>>3]|=(1<<(7-(i&7)));

        printf("F00: QS=%d, AC=%d bits\n", fbuf[3], ac2);

        /* Test BH(5+6+3) on F00 */
        int nz, bands[8];
        int c = test_blockhdr(ac2d, ac2, 5, 6, 3, &nz, NULL, bands);
        if(c>0) printf("BH(5+6+3): %d bits (%.1f%%), NZ=%d, bands: %d %d %d %d %d %d %d %d\n",
                        c, 100.0*c/ac2, nz,
                        bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);

        /* Also test on the count distribution for BH(5+6+3) */
        int counts[32]={0};
        test_blockhdr(ac2d, ac2, 5, 6, 3, &nz, counts, bands);
        printf("BH(5+6+3) count distribution: ");
        for(int i=0;i<32;i++) if(counts[i]) printf("[%d]=%d ", i, counts[i]);
        printf("\n");

        free(ac2d);
    }

    /* Also test on the logo frame (LBA 150, QS=13) */
    printf("\n=== Test on logo frame (LBA 150, QS=13) ===\n");
    if(load_frame(binfile, 150, 0)==0){
        int de3=flen; while(de3>0&&fbuf[de3-1]==0xFF)de3--;
        int tb3=(de3-40)*8;
        int bp3=0;
        for(int b=0;b<NBLK;b++){int dv;bp3+=dec_dc(fbuf+40,bp3,&dv,tb3);}
        int ac3=tb3-bp3;

        uint8_t *ac3d=calloc((ac3+7)/8+1,1);
        for(int i=0;i<ac3;i++)
            if(get_bit(fbuf+40,bp3+i)) ac3d[i>>3]|=(1<<(7-(i&7)));

        printf("Logo: QS=%d, AC=%d bits\n", fbuf[3], ac3);

        int nz, bands[8], counts[32]={0};
        int c = test_blockhdr(ac3d, ac3, 5, 6, 3, &nz, counts, bands);
        if(c>0){
            printf("BH(5+6+3): %d bits (%.1f%%), NZ=%d, bands: %d %d %d %d %d %d %d %d\n",
                   c, 100.0*c/ac3, nz,
                   bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
            printf("Count distribution: ");
            for(int i=0;i<32;i++) if(counts[i]) printf("[%d]=%d ", i, counts[i]);
            printf("\n");
        }
        free(ac3d);
    }

    /* Test on a frame from a DIFFERENT game */
    printf("\n=== Test on Dragon Ball Z (LBA 502, QS=?) ===\n");
    const char *dbz = "/home/wizzard/share/GitHub/playdia-roms/Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku - Chikyuu-hen (Japan) (Track 2).bin";
    /* Find first non-logo frame */
    for(int fi=0;fi<10;fi++){
        if(load_frame(dbz, 502, fi)!=0) break;
        if(fbuf[0]!=0x00||fbuf[1]!=0x80||fbuf[2]!=0x04) break;

        int de4=flen; while(de4>0&&fbuf[de4-1]==0xFF)de4--;
        int tb4=(de4-40)*8;
        int bp4=0;
        int ok=1;
        for(int b=0;b<NBLK;b++){int dv;int c=dec_dc(fbuf+40,bp4,&dv,tb4);if(c<0){ok=0;break;}bp4+=c;}
        if(!ok) continue;
        int ac4=tb4-bp4;

        uint8_t *ac4d=calloc((ac4+7)/8+1,1);
        for(int i=0;i<ac4;i++)
            if(get_bit(fbuf+40,bp4+i)) ac4d[i>>3]|=(1<<(7-(i&7)));

        int nz, bands[8];
        int c=test_blockhdr(ac4d, ac4, 5, 6, 3, &nz, NULL, bands);
        printf("F%d: QS=%d type=%d AC=%d → BH(5+6+3): %d (%.1f%%), NZ=%d, bands: %d %d %d %d %d %d %d %d\n",
               fi, fbuf[3], fbuf[39], ac4, c>0?c:-1, c>0?100.0*c/ac4:0, nz,
               bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
        free(ac4d);
    }

    /* Analyze the exact end-of-data boundary for padded frame */
    printf("\n=== Padding boundary analysis for F03 ===\n");
    if(load_frame(binfile, 502, 3)==0){
        int fde=flen;
        /* Show last 32 bytes before 0xFF padding */
        while(fde>0 && fbuf[fde-1]==0xFF) fde--;
        printf("Data ends at byte %d (padding starts at %d)\n", fde, fde);
        printf("Last 32 bytes of data: ");
        for(int i=fde-32;i<fde;i++) printf("%02X ", fbuf[i]);
        printf("\n");
        printf("First 8 bytes of padding: ");
        for(int i=fde;i<fde+8&&i<flen;i++) printf("%02X ", fbuf[i]);
        printf("\n");

        /* Check if data ends with specific patterns */
        printf("Last 4 bytes: %02X %02X %02X %02X\n",
               fbuf[fde-4], fbuf[fde-3], fbuf[fde-2], fbuf[fde-1]);

        /* What if padding includes trailing 00 bytes too? */
        int fde2=fde;
        while(fde2>0 && fbuf[fde2-1]==0x00) fde2--;
        if(fde2<fde){
            printf("If we also strip trailing 00s: data ends at byte %d (%d bytes shorter)\n",
                   fde2, fde-fde2);
            int ac_bits2 = (fde2-40)*8 - dc_bits;
            printf("  Adjusted AC bits: %d (was %d, diff=%d)\n", ac_bits2, ac_bits, ac_bits-ac_bits2);
            int c2=test_blockhdr(ac_data, ac_bits2, 5, 6, 3, NULL, NULL, NULL);
            if(c2>0) printf("  BH(5+6+3) on adjusted: %d (%.1f%%)\n", c2, 100.0*c2/ac_bits2);
        }
    }

    free(ac_data);
    free(rnd);
    printf("\nDone.\n");
    return 0;
}
