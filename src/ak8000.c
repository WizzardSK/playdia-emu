#include "ak8000.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static void init_video_codec(AK8000 *v) {
    v->vid_codec = avcodec_find_decoder(AV_CODEC_ID_MPEG1VIDEO);
    if (!v->vid_codec) { fprintf(stderr,"[AK8000] No MPEG-1 video codec\n"); return; }
    v->vid_ctx = avcodec_alloc_context3(v->vid_codec);
    if (!v->vid_ctx) return;
    v->vid_ctx->width  = SCREEN_W;
    v->vid_ctx->height = SCREEN_H;
    if (avcodec_open2(v->vid_ctx, v->vid_codec, NULL) < 0) {
        fprintf(stderr,"[AK8000] Failed to open video codec\n");
        avcodec_free_context(&v->vid_ctx); return;
    }
    v->vid_frame  = av_frame_alloc();
    v->vid_pkt    = av_packet_alloc();
    v->codec_ready = true;
    printf("[AK8000] MPEG-1 video decoder ready\n");
}

static void init_audio_codec(AK8000 *v) {
    v->aud_codec = avcodec_find_decoder(AV_CODEC_ID_MP2);
    if (!v->aud_codec) v->aud_codec = avcodec_find_decoder(AV_CODEC_ID_MP3);
    if (!v->aud_codec) { fprintf(stderr,"[AK8000] No MP2 audio codec\n"); return; }
    v->aud_ctx = avcodec_alloc_context3(v->aud_codec);
    if (!v->aud_ctx) return;
    if (avcodec_open2(v->aud_ctx, v->aud_codec, NULL) < 0) {
        fprintf(stderr,"[AK8000] Failed to open audio codec\n");
        avcodec_free_context(&v->aud_ctx); return;
    }
    v->aud_frame  = av_frame_alloc();
    v->aud_pkt    = av_packet_alloc();
    v->acodec_ready = true;
    printf("[AK8000] MP2 audio decoder ready\n");
}

void ak8000_init(AK8000 *v) {
    memset(v, 0, sizeof *v);
    ak8000_reset(v);
    init_video_codec(v);
    init_audio_codec(v);
}

void ak8000_reset(AK8000 *v) {
    memset(v->regs, 0, sizeof v->regs);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        v->framebuffer[i*3+0] = 0x00;
        v->framebuffer[i*3+1] = 0x00;
        v->framebuffer[i*3+2] = 0xAA;
    }
    v->video_active = v->audio_active = false;
    v->frame_count = 0; v->got_video_frame = false;
    v->vid_es_len  = v->aud_es_len = 0;
    v->audio_write_pos = v->audio_read_pos = 0;
}

void ak8000_free(AK8000 *v) {
    if (v->sws_ctx)   { sws_freeContext(v->sws_ctx);        v->sws_ctx   = NULL; }
    if (v->vid_frame) { av_frame_free(&v->vid_frame); }
    if (v->vid_pkt)   { av_packet_free(&v->vid_pkt); }
    if (v->vid_ctx)   { avcodec_free_context(&v->vid_ctx); }
    if (v->aud_frame) { av_frame_free(&v->aud_frame); }
    if (v->aud_pkt)   { av_packet_free(&v->aud_pkt); }
    if (v->aud_ctx)   { avcodec_free_context(&v->aud_ctx); }
}

void ak8000_write_reg(AK8000 *v, uint8_t reg, uint8_t val) {
    if (reg >= 16) return;
    v->regs[reg] = val;
    if (reg == AK8000_REG_CTRL) {
        v->video_active = (val & 0x01) != 0;
        v->audio_active = (val & 0x02) != 0;
    }
}
uint8_t ak8000_read_reg(AK8000 *v, uint8_t reg) {
    if (reg == AK8000_REG_STATUS) return 0x03;
    return (reg < 16) ? v->regs[reg] : 0xFF;
}

/* YUV frame → RGB888 framebuffer via swscale */
static void yuv_to_rgb(AK8000 *v, AVFrame *f) {
    if (!v->sws_ctx || v->vid_ctx->width != f->width || v->vid_ctx->height != f->height) {
        if (v->sws_ctx) sws_freeContext(v->sws_ctx);
        v->sws_ctx = sws_getContext(f->width, f->height, (enum AVPixelFormat)f->format,
                                    SCREEN_W, SCREEN_H, AV_PIX_FMT_RGB24,
                                    SWS_BILINEAR, NULL, NULL, NULL);
    }
    if (!v->sws_ctx) return;
    uint8_t *dst[1]    = { v->framebuffer };
    int      stride[1] = { SCREEN_W * 3 };
    sws_scale(v->sws_ctx, (const uint8_t * const *)f->data, f->linesize,
              0, f->height, dst, stride);
}

static void flush_video_es(AK8000 *v) {
    if (!v->codec_ready || v->vid_es_len == 0) return;
    v->vid_pkt->data = v->vid_es;
    v->vid_pkt->size = v->vid_es_len;
    if (avcodec_send_packet(v->vid_ctx, v->vid_pkt) == 0)
        while (avcodec_receive_frame(v->vid_ctx, v->vid_frame) == 0) {
            yuv_to_rgb(v, v->vid_frame);
            v->got_video_frame = true;
            v->frame_count++;
        }
    v->vid_es_len = 0;
}

static void flush_audio_es(AK8000 *v) {
    if (!v->acodec_ready || v->aud_es_len == 0) return;
    v->aud_pkt->data = v->aud_es;
    v->aud_pkt->size = v->aud_es_len;
    if (avcodec_send_packet(v->aud_ctx, v->aud_pkt) != 0) { v->aud_es_len = 0; return; }
    int ring_size = (int)(sizeof v->audio_buf / sizeof v->audio_buf[0]);
    while (avcodec_receive_frame(v->aud_ctx, v->aud_frame) == 0) {
        int n  = v->aud_frame->nb_samples;
        // FFmpeg 6.x: use ch_layout for channel count (replaces deprecated .channels)
        int nch = v->aud_frame->ch_layout.nb_channels;
        if (nch < 1) nch = 1;
        bool planar = (v->aud_frame->format == AV_SAMPLE_FMT_S16P ||
                       v->aud_frame->format == AV_SAMPLE_FMT_FLTP);
        // Convert to int16 if float planar
        bool is_float = (v->aud_frame->format == AV_SAMPLE_FMT_FLTP ||
                         v->aud_frame->format == AV_SAMPLE_FMT_FLT);

        for (int i = 0; i < n; i++) {
            int16_t s_L, s_R;
            if (is_float) {
                // Float → int16 conversion
                float fl, fr;
                if (planar) {
                    fl = ((float *)v->aud_frame->data[0])[i];
                    fr = (nch > 1) ? ((float *)v->aud_frame->data[1])[i] : fl;
                } else {
                    float *interleaved = (float *)v->aud_frame->data[0];
                    fl = interleaved[i * nch];
                    fr = (nch > 1) ? interleaved[i * nch + 1] : fl;
                }
                // Clamp and scale
                if (fl >  1.0f) fl =  1.0f;
                if (fl < -1.0f) fl = -1.0f;
                if (fr >  1.0f) fr =  1.0f;
                if (fr < -1.0f) fr = -1.0f;
                s_L = (int16_t)(fl * 32767.0f);
                s_R = (int16_t)(fr * 32767.0f);
            } else {
                // int16 (S16 or S16P)
                int16_t *ch0 = (int16_t *)v->aud_frame->data[0];
                int16_t *ch1 = planar
                    ? (nch > 1 ? (int16_t *)v->aud_frame->data[1] : ch0)
                    : ch0;
                int step = planar ? 1 : nch;
                s_L = ch0[i * step];
                s_R = ch1[i * (planar ? 1 : step) + (planar ? 0 : (nch > 1 ? 1 : 0))];
            }
            int wp = v->audio_write_pos;
            v->audio_buf[wp]                 = s_L;
            v->audio_buf[(wp+1) % ring_size] = s_R;
            v->audio_write_pos = (wp + 2) % ring_size;
        }
    }
    v->aud_es_len = 0;
}

static inline uint32_t r32be(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static void append_vid(AK8000 *v, const uint8_t *d, int n) {
    int s = ES_BUF_SIZE - v->vid_es_len; if (n>s) n=s;
    memcpy(v->vid_es + v->vid_es_len, d, n); v->vid_es_len += n;
}
static void append_aud(AK8000 *v, const uint8_t *d, int n) {
    int s = ES_BUF_SIZE - v->aud_es_len; if (n>s) n=s;
    memcpy(v->aud_es + v->aud_es_len, d, n); v->aud_es_len += n;
}

void ak8000_feed_sector(AK8000 *v, const uint8_t *sec, int len) {
    bool found_pes = false;

    /* Scan for MPEG-PS / VCD PES packets */
    for (int i = 0; i + 6 < len; i++) {
        if (sec[i]!=0 || sec[i+1]!=0 || sec[i+2]!=1) continue;
        uint32_t sc = r32be(sec+i);

        if (sc == 0x000001BA) { i += 13; continue; } /* pack header */
        if (sc == 0x000001BB) {
            if (i+6<len){ int hl=(sec[i+4]<<8)|sec[i+5]; i+=5+hl; } continue;
        }

        /* Video PES 0xE0..0xEF */
        if ((sc & 0xFFFFFFF0) == 0x000001E0 && i+8 < len) {
            int pes_len = (sec[i+4]<<8)|sec[i+5];
            int hdr_ext = sec[i+8];
            int hdr     = 9 + hdr_ext;
            int es_len  = pes_len - (hdr - 6);
            if (es_len > 0 && i + hdr + es_len <= len) {
                append_vid(v, sec+i+hdr, es_len);
                found_pes = true;
                i += hdr + es_len - 1;
            }
            continue;
        }

        /* Audio PES 0xC0..0xDF */
        if ((sc & 0xFFFFFFE0) == 0x000001C0 && i+8 < len) {
            int pes_len = (sec[i+4]<<8)|sec[i+5];
            int hdr_ext = sec[i+8];
            int hdr     = 9 + hdr_ext;
            int es_len  = pes_len - (hdr - 6);
            if (es_len > 0 && i + hdr + es_len <= len) {
                append_aud(v, sec+i+hdr, es_len);
                found_pes = true;
                i += hdr + es_len - 1;
            }
            continue;
        }
    }

    /* No PES found — try raw ES */
    if (!found_pes) {
        bool has_vid = false, has_aud = false;
        for (int i = 0; i+4 < len; i++) {
            uint32_t sc = r32be(sec+i);
            if (sc == 0x000001B3 || sc == 0x000001B8 ||
                (sc & 0xFFFFFF00) == 0x00000100) { has_vid = true; break; }
        }
        for (int i = 0; i+1 < len; i++) {
            if (sec[i]==0xFF && (sec[i+1]&0xE0)==0xE0) { has_aud = true; break; }
        }
        if (has_vid) append_vid(v, sec, len);
        if (has_aud) append_aud(v, sec, len);
    }

    if (v->vid_es_len > 4096) flush_video_es(v);
    if (v->aud_es_len > 512)  flush_audio_es(v);
}

void ak8000_tick(AK8000 *v) {
    if (v->vid_es_len > 0) flush_video_es(v);
    if (v->aud_es_len > 0) flush_audio_es(v);
    v->got_video_frame = false;
}

// ─────────────────────────────────────────────────────────────
//  Playdia Proprietary Video Decoder (AK8000 DCT codec)
//
//  Frame format (assembled from 6 × F1 sectors, 12282 bytes):
//    Bytes 0-2:   00 80 04 — frame marker
//    Byte 3:      quantization scale (QS)
//    Bytes 4-19:  16-byte quantization table
//    Bytes 20-35: repeat of qtable
//    Bytes 36-38: 00 80 24
//    Byte 39:     frame type (0=I, 1=P, etc.)
//    Bytes 40+:   bitstream containing 2-4 independent frames
//
//  Each frame in the bitstream:
//    1. DC section: 864 DC coefficients via per-component DPCM
//       using MPEG-1 luminance DC VLC (extended to sizes 0-16)
//    2. AC section: 864 blocks of run-level coded AC coefficients
//       - 6 zero bits (000000) = end of block
//       - Unary run code: count leading zeros (max 5) + "1" delimiter
//       - Level VLC: same extended table; "100" (size 0) = alternate EOB
//    3. Byte-aligned boundary before next frame
//
//  Resolution: 256×144, YCbCr 4:2:0, 16×9 macroblocks = 864 blocks
//  IDCT: standard orthonormal 8×8 DCT, pixel = IDCT(coeff) + 128
// ─────────────────────────────────────────────────────────────

#define PD_W  256
#define PD_H  144
#define PD_MW (PD_W / 16)
#define PD_MH (PD_H / 16)
#define PD_NBLOCKS (PD_MW * PD_MH * 6)

// MPEG-1 luminance DC VLC table extended to sizes 0-16
static const struct { int len; uint32_t code; } pd_vlc[17] = {
    {3,0x4}, {2,0x0}, {2,0x1}, {3,0x5}, {3,0x6},
    {4,0xE}, {5,0x1E}, {6,0x3E}, {7,0x7E},
    {8,0xFE}, {9,0x1FE}, {10,0x3FE},
    {11,0x7FE}, {12,0xFFE}, {13,0x1FFE}, {14,0x3FFE}, {15,0x7FFE}
};

static const int pd_zigzag[64] = {
    0,  1,  8, 16,  9,  2,  3, 10,
   17, 24, 32, 25, 18, 11,  4,  5,
   12, 19, 26, 33, 40, 48, 41, 34,
   27, 20, 13,  6,  7, 14, 21, 28,
   35, 42, 49, 56, 57, 50, 43, 36,
   29, 22, 15, 23, 30, 37, 44, 51,
   58, 59, 52, 45, 38, 31, 39, 46,
   53, 60, 61, 54, 47, 55, 62, 63
};

typedef struct {
    const uint8_t *data;
    int total_bits;
    int pos;
} pd_bitstream;

static int pd_bs_peek(pd_bitstream *bs, int n) {
    if (n <= 0 || bs->pos + n > bs->total_bits) return -1;
    int v = 0;
    for (int i = 0; i < n; i++) {
        int bp = bs->pos + i;
        v = (v << 1) | ((bs->data[bp >> 3] >> (7 - (bp & 7))) & 1);
    }
    return v;
}

static int pd_bs_read(pd_bitstream *bs, int n) {
    if (n <= 0) return 0;
    if (bs->pos + n > bs->total_bits) return -1;
    int v = 0;
    for (int i = 0; i < n; i++) {
        int bp = bs->pos + i;
        v = (v << 1) | ((bs->data[bp >> 3] >> (7 - (bp & 7))) & 1);
    }
    bs->pos += n;
    return v;
}

static int pd_bs_bit(pd_bitstream *bs) {
    if (bs->pos >= bs->total_bits) return -1;
    int bp = bs->pos++;
    return (bs->data[bp >> 3] >> (7 - (bp & 7))) & 1;
}

// Read a signed value using the extended MPEG-1 DC VLC table
static int pd_read_vlc(pd_bitstream *bs) {
    for (int i = 0; i < 17; i++) {
        int bits = pd_bs_peek(bs, pd_vlc[i].len);
        if (bits < 0) continue;
        if (bits == (int)pd_vlc[i].code) {
            bs->pos += pd_vlc[i].len;
            if (i == 0) return 0;
            int val = pd_bs_read(bs, i);
            if (val < 0) return -9999;
            if (val < (1 << (i - 1)))
                val -= (1 << i) - 1;
            return val;
        }
    }
    return -9999;
}

static int pd_clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// Reference 8×8 IDCT (orthonormal, separable)
static void pd_idct_block(const int coeff[64], uint8_t out[8][8]) {
    double temp[8][8];
    int matrix[8][8];
    memset(matrix, 0, sizeof(matrix));
    for (int i = 0; i < 64; i++)
        matrix[pd_zigzag[i] / 8][pd_zigzag[i] % 8] = coeff[i];

    // Row transform
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            double sum = 0;
            for (int k = 0; k < 8; k++) {
                double c = (k == 0) ? (1.0 / sqrt(2.0)) : 1.0;
                sum += c * matrix[i][k] * cos((2 * j + 1) * k * M_PI / 16.0);
            }
            temp[i][j] = sum / 2.0;
        }
    }
    // Column transform + level shift
    for (int j = 0; j < 8; j++) {
        for (int i = 0; i < 8; i++) {
            double sum = 0;
            for (int k = 0; k < 8; k++) {
                double c = (k == 0) ? (1.0 / sqrt(2.0)) : 1.0;
                sum += c * temp[k][j] * cos((2 * i + 1) * k * M_PI / 16.0);
            }
            out[i][j] = (uint8_t)pd_clamp((int)round(sum / 2.0) + 128);
        }
    }
}

// Decode one video frame from the bitstream
// Returns number of DC blocks decoded (864 on success), 0 on failure
static int pd_decode_one_frame(pd_bitstream *bs, int coeff[PD_NBLOCKS][64]) {
    int dc_count = 0;
    memset(coeff, 0, sizeof(int) * PD_NBLOCKS * 64);

    // DC section: per-component DPCM
    int dc_pred[3] = {0, 0, 0};
    for (int mb = 0; mb < PD_MW * PD_MH && bs->pos < bs->total_bits; mb++) {
        for (int bl = 0; bl < 6 && bs->pos < bs->total_bits; bl++) {
            int comp = (bl < 4) ? 0 : (bl == 4) ? 1 : 2;
            int diff = pd_read_vlc(bs);
            if (diff == -9999) goto dc_done;
            dc_pred[comp] += diff;
            coeff[dc_count][0] = dc_pred[comp];
            dc_count++;
        }
    }
dc_done:
    if (dc_count < 6) return 0;

    // AC section: run-level coding with dual EOB
    for (int b = 0; b < dc_count && bs->pos < bs->total_bits; b++) {
        int k = 1;
        while (k < 64 && bs->pos < bs->total_bits) {
            // Check for 6-bit EOB (000000)
            int peek = pd_bs_peek(bs, 6);
            if (peek < 0) break;
            if (peek == 0) { bs->pos += 6; break; }

            // Unary run: count leading zeros (max 5), then "1" delimiter
            int run = 0;
            int ok = 1;
            while (run < 5 && bs->pos < bs->total_bits) {
                int bit = pd_bs_bit(bs);
                if (bit < 0) { ok = 0; break; }
                if (bit == 1) break;
                run++;
            }
            if (!ok) break;

            // Check for "100" = alternate EOB (VLC size 0)
            int p3 = pd_bs_peek(bs, 3);
            if (p3 == 4) { bs->pos += 3; break; }

            // Level VLC (sizes 1-16)
            int level = pd_read_vlc(bs);
            if (level == -9999) break;

            k += run;
            if (k < 64) coeff[b][k] = level;
            k++;
        }
    }

    return dc_count;
}

static void playdia_decode_video_frame(AK8000 *v) {
    if (v->vid_frame_pos < 40) return;

    const uint8_t *f = v->vid_frame_buf;

    // Validate header
    if (f[0] != 0x00 || f[1] != 0x80 || f[2] != 0x04) return;

    uint8_t qscale = f[3];
    memcpy(v->qtable, f + 4, 16);
    v->qscale = qscale;

    // Find actual data end (strip 0xFF padding)
    int data_end = v->vid_frame_pos;
    while (data_end > 40 && f[data_end - 1] == 0xFF) data_end--;
    int total_bits = (data_end - 40) * 8;

    // Decode all frames packed in this bitstream
    pd_bitstream bs = { f + 40, total_bits, 0 };

    // Coefficient storage for current frame
    static int frame_coeff[PD_NBLOCKS][64];

    uint8_t Y[PD_H][PD_W];
    uint8_t Cb[PD_H / 2][PD_W / 2];
    uint8_t Cr[PD_H / 2][PD_W / 2];

    int frames_decoded = 0;

    while (bs.pos < total_bits - 16) {
        int dc_count = pd_decode_one_frame(&bs, frame_coeff);
        if (dc_count < 6) break;

        // IDCT and render this frame to YCbCr planes
        memset(Y, 128, sizeof(Y));
        memset(Cb, 128, sizeof(Cb));
        memset(Cr, 128, sizeof(Cr));

        for (int i = 0; i < dc_count; i++) {
            int mb = i / 6, bl = i % 6;
            int mx = mb % PD_MW, my = mb / PD_MW;

            uint8_t block[8][8];
            pd_idct_block(frame_coeff[i], block);

            if (bl < 4) {
                int bx = mx * 16 + (bl & 1) * 8;
                int by = my * 16 + (bl >> 1) * 8;
                for (int r = 0; r < 8; r++)
                    for (int c = 0; c < 8; c++)
                        if (by + r < PD_H && bx + c < PD_W)
                            Y[by + r][bx + c] = block[r][c];
            } else if (bl == 4) {
                for (int r = 0; r < 8; r++)
                    for (int c = 0; c < 8; c++)
                        if (my * 8 + r < PD_H / 2 && mx * 8 + c < PD_W / 2)
                            Cb[my * 8 + r][mx * 8 + c] = block[r][c];
            } else {
                for (int r = 0; r < 8; r++)
                    for (int c = 0; c < 8; c++)
                        if (my * 8 + r < PD_H / 2 && mx * 8 + c < PD_W / 2)
                            Cr[my * 8 + r][mx * 8 + c] = block[r][c];
            }
        }

        // YCbCr → RGB888 into framebuffer (centered in 320×240)
        int ox = (SCREEN_W - PD_W) / 2;
        int oy = (SCREEN_H - PD_H) / 2;
        memset(v->framebuffer, 0, SCREEN_W * SCREEN_H * 3);

        for (int y = 0; y < PD_H; y++) {
            for (int x = 0; x < PD_W; x++) {
                int yv = Y[y][x];
                int cb = Cb[y / 2][x / 2] - 128;
                int cr = Cr[y / 2][x / 2] - 128;
                int dst = ((y + oy) * SCREEN_W + (x + ox)) * 3;
                v->framebuffer[dst + 0] = (uint8_t)pd_clamp(yv + (int)(1.402 * cr));
                v->framebuffer[dst + 1] = (uint8_t)pd_clamp(yv - (int)(0.344 * cb + 0.714 * cr));
                v->framebuffer[dst + 2] = (uint8_t)pd_clamp(yv + (int)(1.772 * cb));
            }
        }

        v->got_video_frame = true;
        v->vid_frame_ready = true;
        v->frame_count++;
        frames_decoded++;

        // Byte-align for next frame
        bs.pos = (bs.pos + 7) & ~7;
    }
}

// ─────────────────────────────────────────────────────────────
//  XA ADPCM Decoder (CD-ROM XA / Green Book standard)
//
//  Each audio sector contains 18 sound groups of 128 bytes each.
//  Each sound group: 16 bytes header + 112 bytes data (28 words).
//  4-bit ADPCM: each byte = 2 samples.
//  Mono 18900Hz → 18 groups × 28 words × 8 samples = 4032 samples/sector
// ─────────────────────────────────────────────────────────────

// XA ADPCM filter coefficients (K0, K1) — fixed point ×64
static const int32_t xa_k0[4] = {   0,  60, 115,  98 };
static const int32_t xa_k1[4] = {   0,   0, -52, -55 };

static int32_t xa_clamp(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return v;
}

// Check how many free slots are in the audio ring buffer
static int ring_free(AK8000 *v) {
    int ring_size = (int)(sizeof v->audio_buf / sizeof v->audio_buf[0]);
    int used = (v->audio_write_pos - v->audio_read_pos + ring_size) % ring_size;
    return ring_size - used - 1; // -1 to distinguish full from empty
}

// Maximum raw samples per sector: 18 groups × 8 units × 28 = 4032 per channel
#define XA_MAX_RAW_SAMPLES  4096

static void xa_decode_sector(AK8000 *v, const uint8_t *sector_data,
                              int coding_byte) {
    bool stereo    = (coding_byte & 1) != 0;
    bool half_rate = (coding_byte & 4) != 0;  // bit 2: 0=37800Hz, 1=18900Hz
    int ring_size  = (int)(sizeof v->audio_buf / sizeof v->audio_buf[0]);

    // Decode XA ADPCM into temporary buffer at native sample rate
    // Stereo: L/R interleaved. Mono: single channel.
    int16_t raw_L[XA_MAX_RAW_SAMPLES], raw_R[XA_MAX_RAW_SAMPLES];
    int raw_count = 0;

    // XA ADPCM sound group layout (128 bytes each):
    //   Bytes 0-3:   Headers for sound units 0-3
    //   Bytes 4-7:   Copy of 0-3 (error correction)
    //   Bytes 8-11:  Headers for sound units 4-7
    //   Bytes 12-15: Copy of 8-11
    //   Bytes 16-127: 112 bytes sample data (28 words × 4 bytes)
    //
    // Stereo: units 0,2,4,6 = Left; units 1,3,5,7 = Right
    // Mono:   all 8 units are sequential mono samples

    for (int sg = 0; sg < 18; sg++) {
        const uint8_t *grp = sector_data + sg * 128;

        // Process 8 sound units
        for (int unit = 0; unit < 8; unit++) {
            // Header byte index: units 0-3 at bytes 0-3, units 4-7 at bytes 8-11
            int hi = unit < 4 ? unit : (unit + 4);
            uint8_t h = grp[hi];
            int filter = (h >> 4) & 3;
            int range  = h & 0xF;
            if (range > 12) range = 12; // clamp to valid range

            int k0 = xa_k0[filter];
            int k1 = xa_k1[filter];

            // Channel: for stereo, even units=L, odd units=R
            int ch = stereo ? (unit & 1) : 0;

            // 28 samples for this sound unit
            for (int s = 0; s < 28; s++) {
                // Byte offset in data area: 16 + s*4 + (unit/2)
                int byte_idx = 16 + s * 4 + (unit / 2);
                uint8_t byte = grp[byte_idx];

                // Even unit → low nibble, odd unit → high nibble
                int32_t nibble;
                if (unit & 1)
                    nibble = (int32_t)(int8_t)(byte) >> 4;        // high nibble (sign-extended)
                else
                    nibble = (int32_t)(int8_t)(byte << 4) >> 4;   // low nibble (sign-extended)

                // Apply range shift and filter
                int32_t sample = nibble << (12 - range);
                int32_t out = sample + (k0 * v->xa_prev[ch] + k1 * v->xa_prev2[ch] + 32) / 64;
                out = xa_clamp(out);

                v->xa_prev2[ch] = v->xa_prev[ch];
                v->xa_prev[ch]  = out;

                // Store into raw buffer
                if (stereo) {
                    // For stereo, even units fill L, odd units fill R
                    // Each sound group produces 28 samples per unit,
                    // interleaved as L0-L27, R0-R27, L28-L55, R28-R55, ...
                    int idx = (sg * 4 + unit / 2) * 28 + s;
                    if (idx < XA_MAX_RAW_SAMPLES) {
                        if (unit & 1)
                            raw_R[idx] = (int16_t)out;
                        else
                            raw_L[idx] = (int16_t)out;
                        if (idx >= raw_count) raw_count = idx + 1;
                    }
                } else {
                    // Mono: sequential samples from all units
                    int idx = (sg * 8 + unit) * 28 + s;
                    if (idx < XA_MAX_RAW_SAMPLES) {
                        raw_L[idx] = (int16_t)out;
                        raw_R[idx] = (int16_t)out;
                        if (idx >= raw_count) raw_count = idx + 1;
                    }
                }
            }
        }
    }

    // Resample from native XA rate to 44100 Hz
    // Native rates: 37800 Hz (half_rate=0) or 18900 Hz (half_rate=1)
    // Ratio: 44100/37800 = 441/378 = 7/6,  44100/18900 = 441/189 = 7/3
    int native_rate = half_rate ? 18900 : 37800;
    // Number of output samples = raw_count * 44100 / native_rate
    int out_count = (int)((int64_t)raw_count * 44100 / native_rate);

    for (int i = 0; i < out_count; i++) {
        // Map output sample i back to fractional position in raw buffer
        // pos = i * native_rate / 44100
        double pos = (double)i * native_rate / 44100.0;
        int idx = (int)pos;
        double frac = pos - idx;

        int16_t sL, sR;
        if (idx + 1 < raw_count) {
            // Linear interpolation
            sL = (int16_t)(raw_L[idx] * (1.0 - frac) + raw_L[idx + 1] * frac);
            sR = (int16_t)(raw_R[idx] * (1.0 - frac) + raw_R[idx + 1] * frac);
        } else if (idx < raw_count) {
            sL = raw_L[idx];
            sR = raw_R[idx];
        } else {
            break;
        }

        // Write stereo pair to ring buffer
        if (ring_free(v) < 2) break;
        int wp = v->audio_write_pos;
        v->audio_buf[wp]                   = sL;
        v->audio_buf[(wp + 1) % ring_size] = sR;
        v->audio_write_pos = (wp + 2) % ring_size;
    }
}

// ─────────────────────────────────────────────────────────────
//  Feed CD-XA sector with channel-based demuxing
//  Reads subheader at offset 16 to route audio vs video
// ─────────────────────────────────────────────────────────────
void ak8000_feed_xa_sector(AK8000 *v, const uint8_t *raw_sector) {
    // Subheader: [16]=file, [17]=channel, [18]=submode, [19]=coding
    uint8_t channel = raw_sector[17];
    uint8_t submode = raw_sector[18];
    uint8_t coding  = raw_sector[19];

    // Audio sector: submode bit 2 set (0x04)
    if (submode & 0x04) {
        // XA ADPCM audio — decode directly
        // Form 2 data starts at offset 24, 2304 bytes (18 groups × 128)
        xa_decode_sector(v, raw_sector + 24, coding);
        (void)channel; // used for future channel filtering
        return;
    }

    // Video/data sector: channel 0, submode 0x08
    if (channel == 0 && (submode & 0x08)) {
        // Mode 2 Form 1 data at offset 24, 2048 bytes
        const uint8_t *payload = raw_sector + 24;
        uint8_t marker = payload[0];

        if (marker == 0xF1) {
            // Video data sector — append to frame buffer
            int copy = 2047; // skip marker byte
            if (v->vid_frame_pos + copy <= (int)sizeof(v->vid_frame_buf)) {
                memcpy(v->vid_frame_buf + v->vid_frame_pos, payload + 1, copy);
                v->vid_frame_pos += copy;
            }
        } else if (marker == 0xF2) {
            // Frame end marker — decode the assembled video frame
            if (v->vid_frame_pos >= 6 * 2047 &&
                v->vid_frame_buf[0] == 0x00 &&
                v->vid_frame_buf[1] == 0x80 &&
                v->vid_frame_buf[2] == 0x04) {
                v->vid_frame_pos = 6 * 2047;
                playdia_decode_video_frame(v);
            }
            v->vid_frame_pos = 0;
        } else if (marker == 0xF3) {
            // Scene marker — reset frame accumulator
            v->vid_frame_pos = 0;
        }
    }
}
