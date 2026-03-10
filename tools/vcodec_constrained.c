/*
 * vcodec_constrained.c - Constrained AC codec search
 *
 * KEY INSIGHT: For padded frames, we know the EXACT bit boundary.
 * The correct codec must decode exactly 864 blocks consuming exactly N AC bits.
 * We test many schemes and check: decode 864 blocks → how many bits consumed?
 * If consumed == expected → CANDIDATE. If not → ruled out.
 *
 * Also tests: first-block AC values (should be small, concentrated at low freq)
 * Also scans multiple frames including P-frames and different QS values
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define MAX_FRAMES 100
static uint8_t frames[MAX_FRAMES][16384];
static int frame_lens[MAX_FRAMES];
static int frame_qs[MAX_FRAMES];
static int frame_type[MAX_FRAMES];
static int frame_pad[MAX_FRAMES];
static int nframes;

static int get_bit(const uint8_t *d, int bp) { return (d[bp>>3]>>(7-(bp&7)))&1; }
static uint32_t get_bits(const uint8_t *d, int bp, int n) {
    uint32_t v=0; for(int i=0;i<n;i++) v=(v<<1)|get_bit(d,bp+i); return v;
}

/* LSB-first bit reading */
static int get_bit_lsb(const uint8_t *d, int bp) { return (d[bp>>3]>>(bp&7))&1; }
static uint32_t get_bits_lsb(const uint8_t *d, int bp, int n) {
    uint32_t v=0; for(int i=0;i<n;i++) v|=((uint32_t)get_bit_lsb(d,bp+i))<<i; return v;
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

static int load_frames_from(const char *binfile, int start_lba, int max_frames) {
    FILE *fp=fopen(binfile,"rb"); if(!fp)return 0;
    nframes=0;
    int f1c=0;
    uint8_t tmpframe[16384];
    for(int s=0;s<2000&&nframes<max_frames;s++){
        long off=(long)(start_lba+s)*2352;
        uint8_t sec[2352]; fseek(fp,off,SEEK_SET);
        if(fread(sec,1,2352,fp)!=2352)break;
        uint8_t t=sec[24];
        if(t==0xF1){
            if(f1c<6) memcpy(tmpframe+f1c*2047,sec+25,2047);
            f1c++;
        } else if(t==0xF2){
            if(f1c==6){
                int flen=6*2047;
                if(tmpframe[0]==0x00&&tmpframe[1]==0x80&&tmpframe[2]==0x04){
                    memcpy(frames[nframes],tmpframe,flen);
                    frame_lens[nframes]=flen;
                    frame_qs[nframes]=tmpframe[3];
                    frame_type[nframes]=tmpframe[39];
                    int de=flen; while(de>0&&tmpframe[de-1]==0xFF)de--;
                    frame_pad[nframes]=flen-de;
                    nframes++;
                }
            }
            f1c=0;
        } else if(t==0xF3){f1c=0;}
        else {f1c=0;} /* audio or other */
    }
    fclose(fp);
    return nframes;
}

/* Decode DC for all blocks, return AC start bit position, or -1 on failure */
static int decode_all_dc(const uint8_t *fdata, int total_bits, int *dc_vals) {
    int bp=0;
    for(int b=0;b<864;b++){
        int dv;
        int c=dec_dc(fdata+40,bp,&dv,total_bits);
        if(c<0) return -1;
        if(dc_vals) dc_vals[b]=dv;
        bp+=c;
    }
    return bp;
}

/*=== AC CODING SCHEME TESTS ===*/
/* Each returns bits consumed for exactly 864 blocks × 63 AC positions */

/* Scheme 1: Per-position flag (0=zero, 1=nonzero) + N-bit sign-magnitude */
static int test_flag_signmag(const uint8_t *ac, int abits, int vbits, int *nz_out, int *bands) {
    int bp=0, nz=0;
    if(bands) memset(bands,0,8*sizeof(int));
    for(int b=0;b<864;b++){
        for(int pos=0;pos<63;pos++){
            if(bp>=abits) return -1;
            int flag=get_bit(ac,bp); bp++;
            if(flag){
                if(bp+vbits>abits) return -1;
                bp+=vbits;
                nz++;
                if(bands){int band=pos/8;if(band<8)bands[band]++;}
            }
        }
    }
    if(nz_out) *nz_out=nz;
    return bp;
}

/* Scheme 2: Per-position DC-style VLC (size=0 means zero coeff) */
static int test_per_pos_dcvlc(const uint8_t *ac, int abits, int *nz_out, int *bands) {
    int bp=0, nz=0, errs=0;
    if(bands) memset(bands,0,8*sizeof(int));
    for(int b=0;b<864;b++){
        for(int pos=0;pos<63;pos++){
            int val;
            int c=dec_dc(ac,bp,&val,abits);
            if(c<0) return -1;
            bp+=c;
            if(val!=0){
                nz++;
                if(bands){int band=pos/8;if(band<8)bands[band]++;}
            }
        }
    }
    if(nz_out) *nz_out=nz;
    return bp;
}

/* Scheme 3: Unary run (0s count) + DC VLC level + sign, EOB when run reaches end */
static int test_unary0_run_dcvlc(const uint8_t *ac, int abits, int *nz_out, int *bands) {
    int bp=0, nz=0;
    if(bands) memset(bands,0,8*sizeof(int));
    for(int b=0;b<864;b++){
        int pos=0;
        while(pos<63 && bp<abits){
            /* Count 0-bits = run of zeros */
            int run=0;
            while(bp<abits && get_bit(ac,bp)==0){run++;bp++;}
            if(bp>=abits) return -1;
            bp++; /* skip terminating 1 */
            pos+=run;
            if(pos>=63) break;
            /* Read level with DC VLC */
            int val;
            int c=dec_dc(ac,bp,&val,abits);
            if(c<0) return -1;
            bp+=c;
            nz++;
            if(bands){int band=pos/8;if(band<8)bands[band]++;}
            pos++;
        }
    }
    if(nz_out) *nz_out=nz;
    return bp;
}

/* Scheme 4: Reversed unary run (1s count) + DC VLC level */
static int test_unary1_run_dcvlc(const uint8_t *ac, int abits, int *nz_out, int *bands) {
    int bp=0, nz=0;
    if(bands) memset(bands,0,8*sizeof(int));
    for(int b=0;b<864;b++){
        int pos=0;
        while(pos<63 && bp<abits){
            int run=0;
            while(bp<abits && get_bit(ac,bp)==1){run++;bp++;}
            if(bp>=abits) return -1;
            bp++; /* skip terminating 0 */
            pos+=run;
            if(pos>=63) break;
            int val;
            int c=dec_dc(ac,bp,&val,abits);
            if(c<0) return -1;
            bp+=c;
            nz++;
            if(bands){int band=pos/8;if(band<8)bands[band]++;}
            pos++;
        }
    }
    if(nz_out) *nz_out=nz;
    return bp;
}

/* Scheme 5: 2-bit code per position: 00=zero, 01=±1(+sign), 10=±2..3(+sign+1bit), 11=escape(DC VLC) */
static int test_2bit_prefix(const uint8_t *ac, int abits, int *nz_out, int *bands) {
    int bp=0, nz=0;
    if(bands) memset(bands,0,8*sizeof(int));
    for(int b=0;b<864;b++){
        for(int pos=0;pos<63;pos++){
            if(bp+2>abits) return -1;
            int code=get_bits(ac,bp,2); bp+=2;
            if(code==0) continue; /* zero */
            nz++;
            if(bands){int band=pos/8;if(band<8)bands[band]++;}
            if(code==1){if(bp>=abits)return-1;bp++;} /* ±1: sign bit */
            else if(code==2){if(bp+2>abits)return-1;bp+=2;} /* ±2..3: sign+1bit */
            else{/* escape: DC VLC */
                int val;int c=dec_dc(ac,bp,&val,abits);if(c<0)return-1;bp+=c;}
        }
    }
    if(nz_out) *nz_out=nz;
    return bp;
}

/* Scheme 6: Truncated unary per coefficient */
/* 0→0, 10→±1, 110→±2, 1110→±3, ... 1^k 0 → ±k, max_k then fixed bits */
static int test_truncated_unary(const uint8_t *ac, int abits, int max_k, int *nz_out, int *bands) {
    int bp=0, nz=0;
    if(bands) memset(bands,0,8*sizeof(int));
    for(int b=0;b<864;b++){
        for(int pos=0;pos<63;pos++){
            if(bp>=abits) return -1;
            /* Count 1-bits */
            int k=0;
            while(bp<abits && get_bit(ac,bp)==1 && k<max_k){k++;bp++;}
            if(k<max_k){
                if(bp>=abits)return-1;
                bp++; /* terminating 0 */
            }
            if(k==0) continue; /* zero */
            nz++;
            if(bands){int band=pos/8;if(band<8)bands[band]++;}
            /* sign bit */
            if(bp>=abits) return -1;
            bp++;
        }
    }
    if(nz_out) *nz_out=nz;
    return bp;
}

/* Scheme 7: MPEG-1 Table B.14 AC VLC (2D run-level) with EOB */
/* Simplified: just test EOB='10', escape='000001' + 6-bit run + 8-bit level */
/* Full table has 111 entries - use a subset for feasibility */
static int test_mpeg1_simple_eob(const uint8_t *ac, int abits, int *nz_out, int *bands) {
    int bp=0, nz=0;
    if(bands) memset(bands,0,8*sizeof(int));
    for(int b=0;b<864;b++){
        int pos=0;
        int eob_found=0;
        while(pos<63 && bp<abits){
            /* Check for EOB = '10' */
            if(bp+2<=abits && get_bits(ac,bp,2)==0x2){
                bp+=2; eob_found=1; break;
            }
            /* Check for escape = '000001' */
            if(bp+6<=abits && get_bits(ac,bp,6)==0x01){
                bp+=6;
                if(bp+6>abits)return-1; int run=get_bits(ac,bp,6); bp+=6;
                if(bp+8>abits)return-1; /*int level=*/get_bits(ac,bp,8); bp+=8;
                pos+=run;
                if(pos>=63) break;
                nz++;
                if(bands){int band=pos/8;if(band<8)bands[band]++;}
                pos++;
                continue;
            }
            /* Simple: try short VLC entries for most common run-level pairs */
            /* run=0,level=1: '11s' (3 bits) - first AC after DC */
            if(bp+2<=abits && get_bits(ac,bp,2)==0x3){
                bp+=3; /* 11 + sign */
                nz++;
                if(bands){int band=pos/8;if(band<8)bands[band]++;}
                pos++;
                continue;
            }
            /* run=1,level=1: '011s' (4 bits) */
            if(bp+3<=abits && get_bits(ac,bp,3)==0x3){
                bp+=4;
                pos+=1;if(pos>=63)break;
                nz++;
                if(bands){int band=pos/8;if(band<8)bands[band]++;}
                pos++;
                continue;
            }
            /* If nothing matches, treat as 1 bit and move on (error) */
            bp++; pos++;
        }
        if(!eob_found && pos<63){/* block ended without EOB - may be OK */}
    }
    if(nz_out) *nz_out=nz;
    return bp;
}

/* Scheme 8: Golomb-Rice per-coefficient, k bits fixed + unary quotient + sign */
static int test_rice_per_coeff(const uint8_t *ac, int abits, int k, int *nz_out, int *bands) {
    int bp=0, nz=0;
    if(bands) memset(bands,0,8*sizeof(int));
    for(int b=0;b<864;b++){
        for(int pos=0;pos<63;pos++){
            /* Unary quotient: count 0s until 1 */
            int q=0;
            while(bp<abits && get_bit(ac,bp)==0){q++;bp++;if(q>20)return-1;}
            if(bp>=abits)return-1;
            bp++; /* terminating 1 */
            /* k-bit remainder */
            if(bp+k>abits)return-1;
            int rem=get_bits(ac,bp,k); bp+=k;
            int mag=q*(1<<k)+rem;
            if(mag==0) continue;
            nz++;
            if(bands){int band=pos/8;if(band<8)bands[band]++;}
            /* sign bit */
            if(bp>=abits) return -1;
            bp++;
        }
    }
    if(nz_out) *nz_out=nz;
    return bp;
}

/* Scheme 9: Block header (N-bit count of non-zeros) + positions + values */
static int test_block_header(const uint8_t *ac, int abits, int cnt_bits, int pos_bits, int val_bits, int *nz_out) {
    int bp=0, nz=0;
    for(int b=0;b<864;b++){
        if(bp+cnt_bits>abits)return-1;
        int cnt=get_bits(ac,bp,cnt_bits); bp+=cnt_bits;
        for(int i=0;i<cnt;i++){
            if(bp+pos_bits+val_bits>abits)return-1;
            bp+=pos_bits+val_bits;
            nz++;
        }
    }
    if(nz_out) *nz_out=nz;
    return bp;
}

/* Scheme 10: Significance map (bitmap) per block + values */
/* First 63 bits = which positions are non-zero, then values for each NZ position */
static int test_sigmap(const uint8_t *ac, int abits, int val_bits, int *nz_out, int *bands) {
    int bp=0, nz=0;
    if(bands) memset(bands,0,8*sizeof(int));
    for(int b=0;b<864;b++){
        if(bp+63>abits)return-1;
        int block_nz=0;
        int nz_positions[63];
        for(int pos=0;pos<63;pos++){
            int flag=get_bit(ac,bp); bp++;
            if(flag){nz_positions[block_nz]=pos;block_nz++;}
        }
        /* Read values for non-zero positions */
        for(int i=0;i<block_nz;i++){
            if(bp+val_bits>abits)return-1;
            bp+=val_bits;
            nz++;
            if(bands){int band=nz_positions[i]/8;if(band<8)bands[band]++;}
        }
    }
    if(nz_out) *nz_out=nz;
    return bp;
}

/* Scheme 11: Reversed bit order (LSB-first) + per-position DC VLC */
static int test_lsb_per_pos_dcvlc(const uint8_t *ac, int abits, int *nz_out, int *bands) {
    /* Re-encode AC data LSB-first into a temporary buffer */
    int nbytes=(abits+7)/8;
    uint8_t *lsb=calloc(nbytes+1,1);
    for(int i=0;i<abits;i++){
        int bit=get_bit_lsb(ac,i);
        /* Store MSB-first so we can use standard dec_dc */
        if(bit) lsb[i>>3]|=(1<<(7-(i&7)));
    }
    int result=test_per_pos_dcvlc(lsb,abits,nz_out,bands);
    free(lsb);
    return result;
}

/* Scheme 12: Per-position signed exp-golomb-0 */
static int test_per_pos_eg0(const uint8_t *ac, int abits, int *nz_out, int *bands) {
    int bp=0, nz=0;
    if(bands) memset(bands,0,8*sizeof(int));
    for(int b=0;b<864;b++){
        for(int pos=0;pos<63;pos++){
            /* Count leading zeros */
            int lz=0;
            while(bp<abits && get_bit(ac,bp)==0){lz++;bp++;if(lz>16)return-1;}
            if(bp>=abits)return-1;
            bp++; /* skip 1 */
            if(bp+lz>abits)return-1;
            uint32_t suffix=0;
            if(lz>0) suffix=get_bits(ac,bp,lz);
            bp+=lz;
            int mag=(1<<lz)-1+suffix;
            if(mag==0) continue;
            nz++;
            if(bands){int band=pos/8;if(band<8)bands[band]++;}
            /* sign bit */
            if(bp>=abits) return -1;
            bp++;
        }
    }
    if(nz_out) *nz_out=nz;
    return bp;
}

/* Scheme 13: Signed exp-golomb-1 per coefficient */
static int test_per_pos_eg1(const uint8_t *ac, int abits, int *nz_out, int *bands) {
    int bp=0, nz=0;
    if(bands) memset(bands,0,8*sizeof(int));
    for(int b=0;b<864;b++){
        for(int pos=0;pos<63;pos++){
            int lz=0;
            while(bp<abits && get_bit(ac,bp)==0){lz++;bp++;if(lz>16)return-1;}
            if(bp>=abits)return-1;
            bp++;
            int suffix_bits=lz+1; /* EG(1): suffix is lz+1 bits */
            if(bp+suffix_bits>abits)return-1;
            uint32_t suffix=get_bits(ac,bp,suffix_bits);
            bp+=suffix_bits;
            int mag=(1<<(lz+1))-2+suffix; /* EG(1) offset */
            if(mag==0) continue;
            nz++;
            if(bands){int band=pos/8;if(band<8)bands[band]++;}
            if(bp>=abits) return -1;
            bp++;
        }
    }
    if(nz_out) *nz_out=nz;
    return bp;
}

int main() {
    const char *binfile = "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin";

    /* Find first valid start LBA */
    int start_lba = -1;
    for(int try=150;try<=600;try+=50){
        int n=load_frames_from(binfile,try,1);
        if(n>0){start_lba=try;break;}
    }
    if(start_lba<0){printf("No frames found\n");return 1;}

    /* Load up to 40 frames */
    load_frames_from(binfile, start_lba, 40);
    printf("Loaded %d frames from LBA %d\n\n", nframes, start_lba);

    /* Find padded frames (our ground truth) */
    printf("=== Padded frames (ground truth) ===\n");
    int padded_idx = -1;
    for(int f=0;f<nframes;f++){
        if(frame_pad[f] > 100){
            printf("F%02d: QS=%d, type=%d, pad=%d bytes\n", f, frame_qs[f], frame_type[f], frame_pad[f]);
            if(padded_idx<0) padded_idx=f;
        }
    }

    /* Use the best padded frame for constrained testing */
    if(padded_idx<0){
        /* No heavily padded frame - use first I-frame instead */
        for(int f=0;f<nframes;f++){
            if(frame_type[f]==0){padded_idx=f;break;}
        }
    }
    if(padded_idx<0){printf("No suitable test frame\n");return 1;}

    printf("\nUsing frame F%02d for constrained tests (QS=%d, type=%d, pad=%d)\n",
           padded_idx, frame_qs[padded_idx], frame_type[padded_idx], frame_pad[padded_idx]);

    uint8_t *fdata = frames[padded_idx];
    int de = frame_lens[padded_idx];
    while(de>0 && fdata[de-1]==0xFF) de--;
    int total_data_bits = (de-40)*8;

    int dc_vals[864];
    int dc_bits = decode_all_dc(fdata, total_data_bits, dc_vals);
    if(dc_bits<0){printf("DC decode failed!\n");return 1;}

    int ac_bits = total_data_bits - dc_bits;
    printf("DC: %d bits, AC: %d bits (%.2f bits/coeff)\n", dc_bits, ac_bits, (double)ac_bits/(864*63));

    /* Extract AC data */
    int ac_bytes = (ac_bits+7)/8+1;
    uint8_t *ac_data = calloc(ac_bytes,1);
    for(int i=0;i<ac_bits;i++){
        if(get_bit(fdata+40, dc_bits+i))
            ac_data[i>>3] |= (1<<(7-(i&7)));
    }

    printf("\n=== CONSTRAINED TESTS: decode 864 blocks, compare consumed vs %d AC bits ===\n", ac_bits);
    printf("%-40s  consumed   %%    NZ    err  bands\n", "Scheme");

    int nz, bands[8];
    int consumed;

    /* Test various flag+signmag bit widths */
    for(int vb=1;vb<=8;vb++){
        consumed = test_flag_signmag(ac_data, ac_bits*2, vb, &nz, bands);
        if(consumed>0){
            int expected = 864*63*(1) + nz*vb; /* rough check */
            printf("Flag+SM(%d):                              %6d %5.1f%% %5d        %d %d %d %d %d %d %d %d\n",
                   vb, consumed, 100.0*consumed/ac_bits, nz,
                   bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
        }
    }

    /* Per-position DC VLC */
    consumed = test_per_pos_dcvlc(ac_data, ac_bits*2, &nz, bands);
    if(consumed>0)
        printf("PerPos-DCVLC:                            %6d %5.1f%% %5d        %d %d %d %d %d %d %d %d\n",
               consumed, 100.0*consumed/ac_bits, nz,
               bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);

    /* Unary-0 run + DC VLC level */
    consumed = test_unary0_run_dcvlc(ac_data, ac_bits*2, &nz, bands);
    if(consumed>0)
        printf("Unary0-Run+DCVLC:                        %6d %5.1f%% %5d        %d %d %d %d %d %d %d %d\n",
               consumed, 100.0*consumed/ac_bits, nz,
               bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);

    /* Unary-1 run + DC VLC level */
    consumed = test_unary1_run_dcvlc(ac_data, ac_bits*2, &nz, bands);
    if(consumed>0)
        printf("Unary1-Run+DCVLC:                        %6d %5.1f%% %5d        %d %d %d %d %d %d %d %d\n",
               consumed, 100.0*consumed/ac_bits, nz,
               bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);

    /* 2-bit prefix codes */
    consumed = test_2bit_prefix(ac_data, ac_bits*2, &nz, bands);
    if(consumed>0)
        printf("2bitPrefix(00/01s/10ss/11vlc):            %6d %5.1f%% %5d        %d %d %d %d %d %d %d %d\n",
               consumed, 100.0*consumed/ac_bits, nz,
               bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);

    /* Truncated unary */
    for(int mk=3;mk<=8;mk++){
        consumed = test_truncated_unary(ac_data, ac_bits*2, mk, &nz, bands);
        if(consumed>0)
            printf("TruncUnary(max=%d):                       %6d %5.1f%% %5d        %d %d %d %d %d %d %d %d\n",
                   mk, consumed, 100.0*consumed/ac_bits, nz,
                   bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
    }

    /* Rice per-coefficient */
    for(int k=0;k<=4;k++){
        consumed = test_rice_per_coeff(ac_data, ac_bits*2, k, &nz, bands);
        if(consumed>0)
            printf("Rice(k=%d):                               %6d %5.1f%% %5d        %d %d %d %d %d %d %d %d\n",
                   k, consumed, 100.0*consumed/ac_bits, nz,
                   bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
        else
            printf("Rice(k=%d):                               FAILED (overflow)\n", k);
    }

    /* Block header schemes */
    for(int cb=4;cb<=6;cb++){
        for(int pb=6;pb<=6;pb++){
            for(int vb=3;vb<=5;vb++){
                consumed = test_block_header(ac_data, ac_bits*2, cb, pb, vb, &nz);
                if(consumed>0)
                    printf("BlockHdr(%d+%d+%d):                        %6d %5.1f%% %5d\n",
                           cb,pb,vb, consumed, 100.0*consumed/ac_bits, nz);
            }
        }
    }

    /* Significance map */
    for(int vb=2;vb<=6;vb++){
        consumed = test_sigmap(ac_data, ac_bits*2, vb, &nz, bands);
        if(consumed>0)
            printf("SigMap+Val(%d):                           %6d %5.1f%% %5d        %d %d %d %d %d %d %d %d\n",
                   vb, consumed, 100.0*consumed/ac_bits, nz,
                   bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
    }

    /* Per-position exp-golomb-0 */
    consumed = test_per_pos_eg0(ac_data, ac_bits*2, &nz, bands);
    if(consumed>0)
        printf("PerPos-EG0:                              %6d %5.1f%% %5d        %d %d %d %d %d %d %d %d\n",
               consumed, 100.0*consumed/ac_bits, nz,
               bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
    else
        printf("PerPos-EG0:                              FAILED\n");

    /* Per-position exp-golomb-1 */
    consumed = test_per_pos_eg1(ac_data, ac_bits*2, &nz, bands);
    if(consumed>0)
        printf("PerPos-EG1:                              %6d %5.1f%% %5d        %d %d %d %d %d %d %d %d\n",
               consumed, 100.0*consumed/ac_bits, nz,
               bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
    else
        printf("PerPos-EG1:                              FAILED\n");

    /* Now test on RANDOM data as control */
    printf("\n=== RANDOM DATA CONTROL (same tests) ===\n");
    uint8_t *rnd = malloc(ac_bytes);
    srand(12345);
    for(int i=0;i<ac_bytes;i++) rnd[i]=rand()&0xFF;

    for(int vb=1;vb<=4;vb++){
        consumed = test_flag_signmag(rnd, ac_bits*2, vb, &nz, bands);
        if(consumed>0)
            printf("Flag+SM(%d):                              %6d %5.1f%% %5d        %d %d %d %d %d %d %d %d\n",
                   vb, consumed, 100.0*consumed/ac_bits, nz,
                   bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);
    }
    consumed = test_per_pos_dcvlc(rnd, ac_bits*2, &nz, bands);
    if(consumed>0)
        printf("PerPos-DCVLC:                            %6d %5.1f%% %5d        %d %d %d %d %d %d %d %d\n",
               consumed, 100.0*consumed/ac_bits, nz,
               bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);

    consumed = test_per_pos_eg0(rnd, ac_bits*2, &nz, bands);
    if(consumed>0)
        printf("PerPos-EG0:                              %6d %5.1f%% %5d        %d %d %d %d %d %d %d %d\n",
               consumed, 100.0*consumed/ac_bits, nz,
               bands[0],bands[1],bands[2],bands[3],bands[4],bands[5],bands[6],bands[7]);

    /* === MULTI-FRAME ANALYSIS: Check if scheme gives consistent results across QS values === */
    printf("\n=== MULTI-FRAME: Flag+SM bits consumed per frame (target ~100%%) ===\n");
    printf("Frame QS Type  Pad   ACbits  F+SM1%%  F+SM2%%  F+SM3%%  DCVLC%%  EG0%%\n");

    for(int f=0;f<nframes && f<20;f++){
        uint8_t *fd=frames[f];
        int fde=frame_lens[f];
        while(fde>0&&fd[fde-1]==0xFF)fde--;
        int ftb=(fde-40)*8;
        int fdc=decode_all_dc(fd,ftb,NULL);
        if(fdc<0) continue;
        int fac=ftb-fdc;

        int facbytes=(fac+7)/8+1;
        uint8_t *facd=calloc(facbytes,1);
        for(int i=0;i<fac;i++)
            if(get_bit(fd+40,fdc+i)) facd[i>>3]|=(1<<(7-(i&7)));

        int c1=test_flag_signmag(facd,fac*2,1,&nz,NULL);
        int c2=test_flag_signmag(facd,fac*2,2,&nz,NULL);
        int c3=test_flag_signmag(facd,fac*2,3,&nz,NULL);
        int c4=test_per_pos_dcvlc(facd,fac*2,&nz,NULL);
        int c5=test_per_pos_eg0(facd,fac*2,&nz,NULL);

        printf("F%02d  %2d   %d  %4d  %5d  %5.1f%% %5.1f%% %5.1f%% %5.1f%% %5.1f%%\n",
               f, frame_qs[f], frame_type[f], frame_pad[f], fac,
               c1>0?100.0*c1/fac:0, c2>0?100.0*c2/fac:0,
               c3>0?100.0*c3/fac:0, c4>0?100.0*c4/fac:0,
               c5>0?100.0*c5/fac:0);

        free(facd);
    }

    /* Dump first 64 bits of AC for visual inspection */
    printf("\n=== First 64 AC bits (MSB-first) ===\n");
    for(int i=0;i<64&&i<ac_bits;i++){
        printf("%d",get_bit(ac_data,i));
        if(i%8==7) printf(" ");
    }
    printf("\n");

    printf("\n=== First 64 AC bits (LSB-first from bytes) ===\n");
    for(int i=0;i<64&&i<ac_bits;i++){
        printf("%d",get_bit_lsb(ac_data,i));
        if(i%8==7) printf(" ");
    }
    printf("\n");

    /* Show DC values for first macroblock (6 blocks: Y0,Y1,Y2,Y3,Cb,Cr) */
    printf("\n=== First MB DC values (Y0,Y1,Y2,Y3,Cb,Cr) ===\n");
    int pred[3]={0,0,0};
    int bp=0;
    for(int b=0;b<6;b++){
        int dv;
        int c=dec_dc(fdata+40,bp,&dv,total_data_bits);
        bp+=c;
        int comp=(b<4)?0:(b==4)?1:2;
        pred[comp]+=dv;
        printf("Block %d (comp %d): diff=%d, DC=%d\n", b, comp, dv, pred[comp]);
    }

    free(ac_data);
    free(rnd);
    printf("\nDone.\n");
    return 0;
}
