/*
 * Test: what if the codec is NOT DCT-based but DPCM?
 * VLC values = pixel differences, accumulated to build image.
 * Try various widths and with/without the 1-bit AC flag.
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

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    int slba = argc > 2 ? atoi(argv[2]) : 502;

    int err; zip_t *z = zip_open(argv[1], ZIP_RDONLY, &err); if (!z) return 1;
    int bi=-1; zip_uint64_t bs2=0;
    for (int i=0; i<(int)zip_get_num_entries(z,0); i++) {
        zip_stat_t st; if(zip_stat_index(z,i,0,&st)==0 && st.size>bs2){bs2=st.size;bi=i;}}
    zip_stat_t st; zip_stat_index(z,bi,0,&st);
    zip_file_t *zf = zip_fopen_index(z,bi,0);
    uint8_t *disc = malloc(st.size);
    zip_int64_t rd=0;
    while(rd<(zip_int64_t)st.size){zip_int64_t r=zip_fread(zf,disc+rd,st.size-rd);if(r<=0)break;rd+=r;}
    zip_fclose(zf);
    int tsec = (int)(st.size/SECTOR_RAW);

    static uint8_t frames[16][MAX_FRAME]; int fsizes[16];
    int nf = assemble_frames(disc,tsec,slba,frames,fsizes,16);

    uint8_t *f = frames[0];
    int fsize = fsizes[0];
    const uint8_t *bs = f + 40;
    int bslen = fsize - 40;

    printf("Frame 0: %d bytes bitstream\n", bslen);

    /* Simple DPCM: read VLC values, accumulate, output as pixels */
    /* Try widths: 128, 144, 160, 256, 320 */
    int widths[] = {128, 144, 160, 256, 320};
    for (int wi = 0; wi < 5; wi++) {
        int w = widths[wi];
        BR br; br_init(&br, bs, bslen);
        
        int pixels[32768];
        int n = 0;
        int acc = 128; /* Start at mid-gray */
        
        while (!br_eof(&br) && n < 32768) {
            int val = vlc_coeff(&br);
            acc += val;
            pixels[n++] = acc;
        }
        
        int h = n / w;
        printf("DPCM w=%d: %d pixels, %dx%d, range: ", w, n, w, h);
        
        int mn = 99999, mx = -99999;
        for (int i = 0; i < n; i++) { if(pixels[i]<mn)mn=pixels[i]; if(pixels[i]>mx)mx=pixels[i]; }
        printf("%d..%d\n", mn, mx);
        
        /* Output with auto-range */
        uint8_t *img = calloc(w * h, 1);
        for (int i = 0; i < w*h && i < n; i++)
            img[i] = (mx!=mn) ? clamp8((pixels[i]-mn)*255/(mx-mn)) : 128;
        char path[256];
        snprintf(path, sizeof(path), OUT_DIR "dpcm_w%d_f0.pgm", w);
        write_pgm(path, img, w, h);
        
        /* Also output with row-reset (reset accumulator at each row start) */
        br_init(&br, bs, bslen);
        n = 0;
        for (int y = 0; y < h + 10 && !br_eof(&br); y++) {
            acc = 128;
            for (int x = 0; x < w && !br_eof(&br); x++) {
                int val = vlc_coeff(&br);
                acc += val;
                if (n < 32768) pixels[n++] = acc;
            }
        }
        h = n / w;
        mn = 99999; mx = -99999;
        for (int i = 0; i < n; i++) { if(pixels[i]<mn)mn=pixels[i]; if(pixels[i]>mx)mx=pixels[i]; }
        
        img = realloc(img, w * h);
        memset(img, 0, w * h);
        for (int i = 0; i < w*h && i < n; i++)
            img[i] = (mx!=mn) ? clamp8((pixels[i]-mn)*255/(mx-mn)) : 128;
        snprintf(path, sizeof(path), OUT_DIR "dpcm_rr_w%d_f0.pgm", w);
        write_pgm(path, img, w, h);
        
        free(img);
    }
    
    /* Also try: quantized DPCM (val * qscale) */
    int qscale = f[3];
    int w = 128;
    BR br; br_init(&br, bs, bslen);
    int pixels[32768];
    int n = 0;
    int acc = 128 * qscale;
    while (!br_eof(&br) && n < 32768) {
        int val = vlc_coeff(&br);
        acc += val * qscale;
        pixels[n++] = acc / qscale;
    }
    int h = n / w;
    int mn = 99999, mx = -99999;
    for (int i = 0; i < n; i++) { if(pixels[i]<mn)mn=pixels[i]; if(pixels[i]>mx)mx=pixels[i]; }
    uint8_t *img = calloc(w * h, 1);
    for (int i = 0; i < w*h && i < n; i++)
        img[i] = (mx!=mn) ? clamp8((pixels[i]-mn)*255/(mx-mn)) : 128;
    char path[256];
    snprintf(path, sizeof(path), OUT_DIR "dpcm_qs_w128_f0.pgm");
    write_pgm(path, img, w, h);
    free(img);

    free(disc); zip_close(z);
    return 0;
}
