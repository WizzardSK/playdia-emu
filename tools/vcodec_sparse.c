/*
 * vcodec_sparse.c - Analyze ultra-sparse padded frames
 *
 * The Ie Naki Ko game has frames with 5052 bytes padding = only 53867 AC bits.
 * With so few AC bits per block (62.35 avg), many blocks likely have zero or
 * very few AC coefficients. This makes the coding structure much easier to find.
 *
 * Also look at multiple sparse frames to find patterns in how AC data is organized.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static uint8_t frame[16384];
static int framelen;

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

typedef struct {
    uint8_t data[16384];
    int datalen;
    int qs, ftype;
    int padding;
    int dc_bits, ac_bits;
    int ac_start_bit;
    int dc_vals[864];
} Frame;

static int load_padded_frames(const char *binfile, Frame *out, int maxframes) {
    FILE *fp=fopen(binfile,"rb"); if(!fp)return 0;
    int nf=0, f1c=0;
    uint8_t tmpf[16384];
    for(int s=0;s<20000&&nf<maxframes;s++){
        long off=(long)(148+s)*2352;
        uint8_t sec[2352]; fseek(fp,off,SEEK_SET);
        if(fread(sec,1,2352,fp)!=2352)break;
        uint8_t t=sec[24];
        if(t==0xF1){
            if(f1c<6) memcpy(tmpf+f1c*2047,sec+25,2047);
            f1c++;
        } else if(t==0xF2){
            if(f1c==6 && tmpf[0]==0x00 && tmpf[1]==0x80 && tmpf[2]==0x04){
                int fl=6*2047;
                int de=fl; while(de>0&&tmpf[de-1]==0xFF)de--;
                int pad=fl-de;
                if(pad>=50){
                    Frame *f=&out[nf];
                    memcpy(f->data,tmpf,fl);
                    f->datalen=fl;
                    f->qs=tmpf[3];
                    f->ftype=tmpf[39];
                    f->padding=pad;
                    /* Decode DC */
                    int bp=40*8;
                    int dc_pred[3]={0,0,0};
                    int ok=1;
                    for(int mb=0;mb<144&&ok;mb++){
                        for(int bl=0;bl<6&&ok;bl++){
                            int comp=(bl<4)?0:(bl==4)?1:2;
                            int dv;
                            int used=dec_dc(tmpf,bp,&dv,de*8);
                            if(used<0){ok=0;break;}
                            dc_pred[comp]+=dv;
                            f->dc_vals[mb*6+bl]=dc_pred[comp];
                            bp+=used;
                        }
                    }
                    if(ok){
                        f->ac_start_bit=bp;
                        f->dc_bits=bp-40*8;
                        f->ac_bits=de*8-bp;
                        nf++;
                    }
                }
            }
            f1c=0;
        } else { f1c=0; }
    }
    fclose(fp);
    return nf;
}

int main() {
    /* Find all padded frames from multiple games */
    const char *games[] = {
        "/home/wizzard/share/GitHub/playdia-roms/Ie Naki Ko - Suzu no Sentaku (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Mari-nee no Heya (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku - Chikyuu-hen (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Aqua Adventure - Blue Lilty (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Ultraman - Hikou no Himitsu (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Hello Kitty - Yume no Kuni Daibouken (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/Bishojo Senshi Sailor Moon S - Quiz Taiketsu! Sailor Power Kesshuu (Japan) (Track 2).bin",
        "/home/wizzard/share/GitHub/playdia-roms/SD Gundam Daizukan (Japan) (Track 2).bin",
        NULL
    };

    Frame frames[200];
    int total_frames = 0;

    for(int gi=0; games[gi]; gi++){
        const char *gn = strrchr(games[gi],'/');
        if(gn) gn++; else gn=games[gi];
        int nf = load_padded_frames(games[gi], frames+total_frames, 200-total_frames);
        if(nf > 0) printf("%-60s: %d padded frames\n", gn, nf);
        total_frames += nf;
    }
    printf("\nTotal padded frames: %d\n\n", total_frames);

    /* Sort by AC bits (ascending) to find sparsest */
    for(int i=0;i<total_frames-1;i++)
        for(int j=i+1;j<total_frames;j++)
            if(frames[j].ac_bits < frames[i].ac_bits){
                Frame tmp=frames[i]; frames[i]=frames[j]; frames[j]=tmp;
            }

    /* Show all padded frames sorted by sparsity */
    printf("=== Padded frames sorted by AC bits ===\n");
    for(int i=0;i<total_frames;i++){
        Frame *f=&frames[i];
        printf("  QS=%2d type=%3d pad=%5d AC=%5d bits  %.1f/blk\n",
               f->qs, f->ftype, f->padding, f->ac_bits, (float)f->ac_bits/864);
    }

    /* ===== Analyze the sparsest frame in detail ===== */
    if(total_frames == 0){ printf("No padded frames found\n"); return 1; }

    Frame *sparse = &frames[0]; /* sparsest */
    printf("\n=== DETAILED ANALYSIS: Sparsest frame ===\n");
    printf("QS=%d type=%d AC=%d bits (%.2f/block)\n",
           sparse->qs, sparse->ftype, sparse->ac_bits, (float)sparse->ac_bits/864);

    /* Dump first 256 bits of AC data */
    printf("\nFirst 256 AC bits:\n");
    for(int i=0;i<256&&i<sparse->ac_bits;i++){
        printf("%d",get_bit(sparse->data,sparse->ac_start_bit+i));
        if((i+1)%64==0) printf("\n");
        else if((i+1)%8==0) printf(" ");
    }
    printf("\n");

    /* Dump last 256 bits of AC data */
    int ac_end = sparse->ac_start_bit + sparse->ac_bits;
    printf("\nLast 256 AC bits:\n");
    int start = sparse->ac_bits > 256 ? sparse->ac_bits - 256 : 0;
    for(int i=start;i<sparse->ac_bits;i++){
        printf("%d",get_bit(sparse->data,sparse->ac_start_bit+i));
        if(((i-start)+1)%64==0) printf("\n");
        else if(((i-start)+1)%8==0) printf(" ");
    }
    printf("\n");

    /* Last 16 bytes before padding */
    int de = sparse->datalen;
    while(de>0 && sparse->data[de-1]==0xFF) de--;
    printf("\nLast 32 bytes before padding:\n");
    for(int i=de-32;i<de;i++) printf("%02X ",sparse->data[i]);
    printf("\n");

    /* ===== Try treating AC as MPEG-1 DC VLC per coefficient ===== */
    printf("\n=== DC VLC per coefficient on sparsest frame ===\n");
    {
        int bp = sparse->ac_start_bit;
        int ac_end = bp + sparse->ac_bits;
        int blocks=0, nz=0, errors=0;
        int band_nz[64]={0};
        int size_hist[12]={0};

        for(int blk=0;blk<864;blk++){
            int blk_nz=0;
            for(int pos=0;pos<63;pos++){
                if(bp>=ac_end){
                    blocks=blk;
                    goto dcvlc_done;
                }
                int dv;
                int used=dec_dc(sparse->data,bp,&dv,ac_end);
                if(used<0){errors++;bp++;continue;}
                /* Track size */
                for(int i=0;i<12;i++){
                    if(bp+dcv[i].len<=ac_end){
                        uint32_t b=get_bits(sparse->data,bp,dcv[i].len);
                        if(b==dcv[i].code){size_hist[i]++;break;}
                    }
                }
                bp+=used;
                if(dv!=0){nz++;blk_nz++;band_nz[pos]++;}
            }
            blocks++;
        }
        dcvlc_done:
        printf("Decoded: %d blocks, consumed %d/%d bits (%.1f%%), NZ=%d, errors=%d\n",
               blocks, bp-sparse->ac_start_bit, sparse->ac_bits,
               100.0*(bp-sparse->ac_start_bit)/sparse->ac_bits, nz, errors);
        printf("Size histogram: ");
        for(int i=0;i<12;i++) if(size_hist[i]) printf("s%d=%d ",i,size_hist[i]);
        printf("\n");
        printf("Band NZ [0..14]: ");
        for(int i=0;i<15;i++) printf("%d ",band_nz[i]);
        printf("\n");
    }

    /* ===== Try simple EOB-based scheme ===== */
    printf("\n=== EOB-based schemes on sparsest frame ===\n");
    /* If blocks have mostly zero ACs, there should be an early termination signal */
    /* Try: DC VLC gives a value, size=0 means EOB for the block */
    {
        int bp = sparse->ac_start_bit;
        int ac_end = bp + sparse->ac_bits;
        int blocks=0, vals=0, nz=0, eobs=0;

        for(int blk=0;blk<864&&bp<ac_end;blk++){
            for(int pos=0;pos<63;pos++){
                if(bp>=ac_end) goto eob_done;
                int dv;
                int used=dec_dc(sparse->data,bp,&dv,ac_end);
                if(used<0){bp++;continue;}
                bp+=used;
                vals++;
                if(dv==0){
                    /* size=0 = EOB for this block */
                    eobs++;
                    break;
                }
                nz++;
            }
            blocks++;
        }
        eob_done:
        printf("DC-VLC with size=0=EOB: %d blocks, consumed %d/%d (%.1f%%), NZ=%d, EOBs=%d\n",
               blocks, bp-sparse->ac_start_bit, sparse->ac_bits,
               100.0*(bp-sparse->ac_start_bit)/sparse->ac_bits, nz, eobs);
        if(blocks>0) printf("  Avg NZ/block: %.2f, Avg pos before EOB: %.2f\n",
               (float)nz/blocks, (float)(nz+eobs)/blocks);
    }

    /* Try: 0-bit = zero coeff, 1-bit = DC VLC value, all-zero check first */
    {
        int bp = sparse->ac_start_bit;
        int ac_end = bp + sparse->ac_bits;
        int blocks=0, nz=0;

        for(int blk=0;blk<864&&bp<ac_end;blk++){
            for(int pos=0;pos<63;pos++){
                if(bp>=ac_end) goto flag_done;
                int flag = get_bit(sparse->data,bp); bp++;
                if(flag){
                    int dv;
                    int used=dec_dc(sparse->data,bp,&dv,ac_end);
                    if(used<0){bp++;continue;}
                    bp+=used;
                    nz++;
                }
            }
            blocks++;
        }
        flag_done:
        printf("Flag(1-bit)+DC-VLC:     %d blocks, consumed %d/%d (%.1f%%), NZ=%d\n",
               blocks, bp-sparse->ac_start_bit, sparse->ac_bits,
               100.0*(bp-sparse->ac_start_bit)/sparse->ac_bits, nz);
    }

    /* Try: 0=zero, 10=EOB, 11+DCVLC=value */
    {
        int bp = sparse->ac_start_bit;
        int ac_end = bp + sparse->ac_bits;
        int blocks=0, nz=0, eobs=0;

        for(int blk=0;blk<864&&bp<ac_end;blk++){
            for(int pos=0;pos<63;pos++){
                if(bp>=ac_end) goto eob2_done;
                int b0 = get_bit(sparse->data,bp); bp++;
                if(b0==0) continue; /* zero */
                if(bp>=ac_end) goto eob2_done;
                int b1 = get_bit(sparse->data,bp); bp++;
                if(b1==0){ eobs++; break; } /* EOB */
                /* value */
                int dv;
                int used=dec_dc(sparse->data,bp,&dv,ac_end);
                if(used<0){bp++;continue;}
                bp+=used; nz++;
            }
            blocks++;
        }
        eob2_done:
        printf("0=Z,10=EOB,11+VLC=V:    %d blocks, consumed %d/%d (%.1f%%), NZ=%d, EOB=%d\n",
               blocks, bp-sparse->ac_start_bit, sparse->ac_bits,
               100.0*(bp-sparse->ac_start_bit)/sparse->ac_bits, nz, eobs);
    }

    /* Try: pure run-level with DC VLC for run AND level */
    {
        int bp = sparse->ac_start_bit;
        int ac_end = bp + sparse->ac_bits;
        int blocks=0, nz=0;

        for(int blk=0;blk<864&&bp<ac_end;blk++){
            int pos=0;
            while(pos<63 && bp<ac_end){
                int run;
                int used=dec_dc(sparse->data,bp,&run,ac_end);
                if(used<0){bp++;break;}
                bp+=used;
                if(run==0 && pos>0){break;} /* EOB: run=0 after first */
                pos+=run;
                if(pos>=63) break;
                /* Level */
                int lev;
                used=dec_dc(sparse->data,bp,&lev,ac_end);
                if(used<0){bp++;break;}
                bp+=used;
                nz++;
                pos++;
            }
            blocks++;
        }
        printf("DCVLC-run+DCVLC-level:  %d blocks, consumed %d/%d (%.1f%%), NZ=%d\n",
               blocks, bp-sparse->ac_start_bit, sparse->ac_bits,
               100.0*(bp-sparse->ac_start_bit)/sparse->ac_bits, nz);
    }

    /* ===== Try treating AC data as interleaved DC+AC with MPEG-1 table ===== */
    /* What if DC and AC are NOT separate but interleaved? */
    printf("\n=== Interleaved DC+AC hypothesis (sparsest frame) ===\n");
    {
        /* Re-read from byte 40, treating ALL bits as interleaved DC+AC */
        Frame *f = sparse;
        int bp = 40*8;
        int total_bits = (f->datalen - f->padding) * 8;
        int dc_pred[3]={0,0,0};
        int blocks=0, nz_total=0;

        for(int mb=0;mb<144&&bp<total_bits;mb++){
            for(int bl=0;bl<6&&bp<total_bits;bl++){
                int comp=(bl<4)?0:(bl==4)?1:2;
                /* DC */
                int dv;
                int used=dec_dc(f->data,bp,&dv,total_bits);
                if(used<0){goto inter_done;}
                dc_pred[comp]+=dv;
                bp+=used;
                /* AC: use DC VLC with size=0=EOB */
                int nz=0;
                for(int pos=0;pos<63&&bp<total_bits;pos++){
                    int av;
                    used=dec_dc(f->data,bp,&av,total_bits);
                    if(used<0){goto inter_done;}
                    bp+=used;
                    if(av==0) break; /* EOB */
                    nz++;
                }
                nz_total+=nz;
                blocks++;
            }
        }
        inter_done:
        printf("Interleaved DC+AC(VLC,size0=EOB): %d blocks, %d/%d bits (%.1f%%), NZ=%d\n",
               blocks, bp-40*8, total_bits-40*8,
               100.0*(bp-40*8)/(total_bits-40*8), nz_total);
        if(blocks>0) printf("  Avg NZ/block: %.2f\n", (float)nz_total/blocks);
    }

    /* ===== Try interleaved DC+AC with run-level pairs ===== */
    {
        Frame *f = sparse;
        int bp = 40*8;
        int total_bits = (f->datalen - f->padding) * 8;
        int dc_pred[3]={0,0,0};
        int blocks=0, nz_total=0;

        for(int mb=0;mb<144&&bp<total_bits;mb++){
            for(int bl=0;bl<6&&bp<total_bits;bl++){
                int comp=(bl<4)?0:(bl==4)?1:2;
                int dv;
                int used=dec_dc(f->data,bp,&dv,total_bits);
                if(used<0) goto inter2_done;
                dc_pred[comp]+=dv;
                bp+=used;
                /* AC: run-level pairs using DC VLC */
                int pos=0, nz=0;
                while(pos<63 && bp<total_bits){
                    /* Run (skip zeros) */
                    int run;
                    used=dec_dc(f->data,bp,&run,total_bits);
                    if(used<0) goto inter2_done;
                    bp+=used;
                    if(run==0 && pos>0) break; /* EOB */
                    if(run<0) run=0;
                    pos+=run;
                    if(pos>=63) break;
                    /* Level */
                    int lev;
                    used=dec_dc(f->data,bp,&lev,total_bits);
                    if(used<0) goto inter2_done;
                    bp+=used;
                    nz++; pos++;
                }
                nz_total+=nz;
                blocks++;
            }
        }
        inter2_done:
        printf("Interleaved DC+AC(RL):            %d blocks, %d/%d bits (%.1f%%), NZ=%d\n",
               blocks, bp-40*8, total_bits-40*8,
               100.0*(bp-40*8)/(total_bits-40*8), nz_total);
        if(blocks>0) printf("  Avg NZ/block: %.2f\n", (float)nz_total/blocks);
    }

    /* ===== Byte-level pattern analysis ===== */
    printf("\n=== Byte-level patterns in AC data ===\n");
    {
        Frame *f = sparse;
        int ac_byte_start = (f->ac_start_bit + 7) / 8;
        int de = f->datalen;
        while(de>0 && f->data[de-1]==0xFF) de--;

        /* Byte frequency */
        int bytehist[256]={0};
        for(int i=ac_byte_start; i<de; i++) bytehist[f->data[i]]++;
        printf("Most common bytes in AC: ");
        for(int t=0;t<10;t++){
            int mx=0,mi=0;
            for(int i=0;i<256;i++) if(bytehist[i]>mx){mx=bytehist[i];mi=i;}
            if(mx==0)break;
            printf("%02X=%d ", mi, mx);
            bytehist[mi]=0;
        }
        printf("\n");

        /* Bigram analysis (2-byte patterns) */
        int bigram_hist[65536];
        memset(bigram_hist,0,sizeof(bigram_hist));
        for(int i=ac_byte_start; i<de-1; i++){
            int bg = (f->data[i]<<8)|f->data[i+1];
            bigram_hist[bg]++;
        }
        printf("Most common bigrams: ");
        for(int t=0;t<10;t++){
            int mx=0,mi=0;
            for(int i=0;i<65536;i++) if(bigram_hist[i]>mx){mx=bigram_hist[i];mi=i;}
            if(mx==0)break;
            printf("%04X=%d ", mi, mx);
            bigram_hist[mi]=0;
        }
        printf("\n");
    }

    /* ===== Check if all padded frames have common trailing bytes ===== */
    printf("\n=== Trailing bytes before padding ===\n");
    for(int i=0;i<total_frames&&i<20;i++){
        Frame *f=&frames[i];
        int de=f->datalen;
        while(de>0 && f->data[de-1]==0xFF) de--;
        printf("  QS=%2d type=%3d AC=%5d: last8=", f->qs, f->ftype, f->ac_bits);
        for(int j=de-8;j<de;j++) printf("%02X ",f->data[j]);
        /* Check last few bits */
        int last_bits = de*8;
        printf(" lastbits=");
        for(int j=last_bits-16;j<last_bits;j++) printf("%d",get_bit(f->data,j));
        printf("\n");
    }

    /* ===== Entropy analysis per 8-byte chunk within AC ===== */
    printf("\n=== Entropy variation within AC data (sparsest) ===\n");
    {
        Frame *f = &frames[0];
        int ac_byte = (f->ac_start_bit + 7) / 8;
        int de = f->datalen;
        while(de>0 && f->data[de-1]==0xFF) de--;
        int ac_bytes = de - ac_byte;

        /* Split into 64-byte chunks, compute entropy of each */
        int nchunks = ac_bytes / 64;
        printf("AC data: %d bytes in %d chunks of 64:\n", ac_bytes, nchunks);
        for(int c=0;c<nchunks&&c<20;c++){
            int hist[256]={0};
            for(int i=0;i<64;i++) hist[f->data[ac_byte+c*64+i]]++;
            double ent=0;
            for(int i=0;i<256;i++){
                if(hist[i]==0)continue;
                double p=(double)hist[i]/64;
                ent-=p*log2(p);
            }
            int ones=0;
            for(int i=0;i<512;i++) ones+=get_bit(f->data,(ac_byte+c*64)*8+i);
            printf("  Chunk %2d: H=%.2f bits/byte, 1s=%d/512 (%.1f%%)\n",
                   c, ent, ones, 100.0*ones/512);
        }
    }

    printf("\nDone.\n");
    return 0;
}
