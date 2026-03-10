#include "ak8000.h"
#include <string.h>
#include <stdio.h>

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
//  Playdia Proprietary Video Decoder (DC-only, AC not yet decoded)
//
//  Frame format (assembled from 6 × F1 sectors, 12282 bytes):
//    Bytes 0-2:   00 80 04 — frame marker
//    Byte 3:      quantization scale (QS)
//    Bytes 4-19:  16-byte quantization table
//    Bytes 20-35: repeat of qtable
//    Bytes 36-38: 00 80 24
//    Byte 39:     frame type (0=I, 1=P, etc.)
//    Bytes 40+:   bitstream (MPEG-1 luminance DC VLC with DPCM,
//                 followed by AC data in unknown format)
//
//  Resolution: 256×144, 4:2:0, 16×9 macroblocks = 864 blocks
//  Currently decodes DC coefficients only (produces blocky but
//  recognizable color images).
// ─────────────────────────────────────────────────────────────

#define PD_W  256
#define PD_H  144

// MPEG-1 luminance DC VLC table (size 0-11)
static const struct { int len; uint32_t code; } pd_dc_vlc[] = {
    {3,0x4}, {2,0x0}, {2,0x1}, {3,0x5}, {3,0x6},
    {4,0xE}, {5,0x1E}, {6,0x3E}, {7,0x7E},
    {8,0xFE}, {9,0x1FE}, {10,0x3FE}
};

static int pd_get_bit(const uint8_t *d, int bp) {
    return (d[bp >> 3] >> (7 - (bp & 7))) & 1;
}

static uint32_t pd_get_bits(const uint8_t *d, int bp, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | pd_get_bit(d, bp + i);
    return v;
}

// Decode one DC VLC value, return bits consumed or -1 on error
static int pd_dec_dc(const uint8_t *d, int bp, int *val, int total_bits) {
    for (int i = 0; i < 12; i++) {
        if (bp + pd_dc_vlc[i].len > total_bits) continue;
        uint32_t b = pd_get_bits(d, bp, pd_dc_vlc[i].len);
        if (b == pd_dc_vlc[i].code) {
            int sz = i, c = pd_dc_vlc[i].len;
            if (sz == 0) {
                *val = 0;
            } else {
                if (bp + c + sz > total_bits) return -1;
                uint32_t r = pd_get_bits(d, bp + c, sz);
                c += sz;
                *val = (r < (1u << (sz - 1)))
                     ? (int)r - (1 << sz) + 1
                     : (int)r;
            }
            return c;
        }
    }
    return -1;
}

static int pd_clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void playdia_decode_video_frame(AK8000 *v) {
    if (v->vid_frame_pos < 40) return;

    const uint8_t *f = v->vid_frame_buf;

    // Validate header
    if (f[0] != 0x00 || f[1] != 0x80 || f[2] != 0x04) return;

    uint8_t qscale = f[3];
    uint8_t frame_type = f[39];
    memcpy(v->qtable, f + 4, 16);
    v->qscale = qscale;

    // Find actual data end (strip 0xFF padding)
    int data_end = v->vid_frame_pos;
    while (data_end > 40 && f[data_end - 1] == 0xFF) data_end--;
    int total_bits = data_end * 8;

    // Reset DC predictors on I-frames
    if (frame_type == 0) {
        v->dc_pred[0] = v->dc_pred[1] = v->dc_pred[2] = 0;
    }

    // Decode DC for all 864 blocks (144 macroblocks × 6 blocks)
    // Block order per MB: Y0, Y1, Y2, Y3, Cb, Cr
    int bp = 40 * 8;
    int dc_vals[864];
    int decoded = 0;

    for (int mb = 0; mb < 144 && bp < total_bits; mb++) {
        for (int bl = 0; bl < 6 && bp < total_bits; bl++) {
            int comp = (bl < 4) ? 0 : (bl == 4) ? 1 : 2;
            int diff;
            int used = pd_dec_dc(f, bp, &diff, total_bits);
            if (used < 0) goto dc_done;
            v->dc_pred[comp] += diff;
            dc_vals[decoded] = v->dc_pred[comp];
            decoded++;
            bp += used;
        }
    }
dc_done:

    // Build DC-only image: each 8×8 block gets a flat color from DC
    // DC value scaling: multiply by 8 and add 128 for level shift
    uint8_t Y[PD_H][PD_W];
    uint8_t Cb[PD_H / 2][PD_W / 2];
    uint8_t Cr[PD_H / 2][PD_W / 2];
    memset(Y, 128, sizeof(Y));
    memset(Cb, 128, sizeof(Cb));
    memset(Cr, 128, sizeof(Cr));

    for (int i = 0; i < decoded; i++) {
        int mb = i / 6;
        int bl = i % 6;
        int mx = mb % 16; // macroblock column (0-15)
        int my = mb / 16; // macroblock row (0-8)
        int pixel_val = pd_clamp(dc_vals[i] * 8 + 128);

        if (bl < 4) {
            // Luma block: 8×8 within 16×16 macroblock
            int bx = mx * 16 + (bl & 1) * 8;
            int by = my * 16 + (bl >> 1) * 8;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    if (by + y < PD_H && bx + x < PD_W)
                        Y[by + y][bx + x] = (uint8_t)pixel_val;
        } else if (bl == 4) {
            // Cb block: 8×8 covers 16×16 macroblock area
            int bx = mx * 8;
            int by = my * 8;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    if (by + y < PD_H / 2 && bx + x < PD_W / 2)
                        Cb[by + y][bx + x] = (uint8_t)pixel_val;
        } else {
            // Cr block
            int bx = mx * 8;
            int by = my * 8;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    if (by + y < PD_H / 2 && bx + x < PD_W / 2)
                        Cr[by + y][bx + x] = (uint8_t)pixel_val;
        }
    }

    // YCbCr → RGB888 into framebuffer (centered in 320×240)
    int ox = (SCREEN_W - PD_W) / 2;  // 32
    int oy = (SCREEN_H - PD_H) / 2;  // 48
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

static void xa_decode_sector(AK8000 *v, const uint8_t *sector_data,
                              int coding_byte) {
    bool stereo = (coding_byte & 1) != 0;
    int ring_size = (int)(sizeof v->audio_buf / sizeof v->audio_buf[0]);

    // XA ADPCM sound group layout (128 bytes each):
    //   Bytes 0-3:   Headers for sound units 0-3
    //   Bytes 4-7:   Copy of 0-3 (error correction)
    //   Bytes 8-11:  Headers for sound units 4-7
    //   Bytes 12-15: Copy of 8-11
    //   Bytes 16-127: 112 bytes sample data (28 words × 4 bytes)
    //
    // Data byte at offset 16 + s*4 + b:
    //   Low nibble  → sound unit b*2
    //   High nibble → sound unit b*2 + 1
    //
    // Header byte for unit u: u<4 → byte u, u>=4 → byte u+4 (skip copies)

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

                // Write to ring buffer with overflow protection
                int16_t s16 = (int16_t)out;
                if (ring_free(v) < 2) break; // ring full, drop remaining

                int wp = v->audio_write_pos;
                if (stereo) {
                    v->audio_buf[wp] = s16;
                    v->audio_write_pos = (wp + 1) % ring_size;
                } else {
                    // Mono → duplicate to L+R
                    v->audio_buf[wp]                   = s16;
                    v->audio_buf[(wp + 1) % ring_size] = s16;
                    v->audio_write_pos = (wp + 2) % ring_size;
                }
            }
        }
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
