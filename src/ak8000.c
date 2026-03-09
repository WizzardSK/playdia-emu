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
//  Playdia Proprietary Video Decoder
//
//  Frame format (assembled from F1 sectors):
//    Bytes 0-1:  00 80 — frame marker
//    Byte 2:     block size indicator (always 0x04 = 4×4 blocks)
//    Byte 3:     quantization scale
//    Bytes 4-19: 16-entry quantization table (4×4 DCT coefficients)
//    Bytes 20-35: repeat of qtable (redundancy)
//    Bytes 36+:  compressed bitstream (VLC-coded 4×4 blocks)
//
//  Output: 256×192 image rendered into 320×240 framebuffer
//  (centered/scaled with borders)
//
//  NOTE: This is a best-effort decode. The exact bitstream format
//  is not fully reverse-engineered. We use a simplified approach:
//  interpret the data as 2-bit-per-pixel blocks with qtable as
//  luminance palette, producing a grayscale approximation.
// ─────────────────────────────────────────────────────────────

// Playdia native resolution (256×192 or similar, within 320×240 output)
#define PD_W  256
#define PD_H  192

static void playdia_decode_video_frame(AK8000 *v) {
    if (v->vid_frame_pos < 36) return; // too short

    // Parse header
    uint8_t block_type = v->vid_frame_buf[2];
    uint8_t qscale     = v->vid_frame_buf[3];
    (void)block_type;

    // Extract quantization table (16 entries)
    memcpy(v->qtable, v->vid_frame_buf + 4, 16);
    v->qscale = qscale;

    // Bitstream starts at offset 36
    const uint8_t *bs = v->vid_frame_buf + 36;
    int bs_len = v->vid_frame_pos - 36;

    // Simple decode: treat each byte as 4 pixels (2 bits each)
    // Map 2-bit index to luminance using qtable values as palette
    // qtable values range ~10-37, scale to 0-255
    uint8_t palette[4];
    // Use qtable entries 0,5,10,15 (diagonal) as representative luminances
    for (int i = 0; i < 4; i++) {
        int idx = i * 5; // 0, 5, 10, 15
        if (idx >= 16) idx = 15;
        int lum = v->qtable[idx] * 255 / 40;
        if (lum > 255) lum = 255;
        palette[i] = (uint8_t)lum;
    }

    // Decode pixels row by row into a temporary buffer
    uint8_t pixels[PD_W * PD_H];
    memset(pixels, 0, sizeof(pixels));

    int byte_idx = 0;
    for (int y = 0; y < PD_H && byte_idx < bs_len; y++) {
        for (int x = 0; x < PD_W && byte_idx < bs_len; x += 4) {
            uint8_t b = bs[byte_idx++];
            // 4 pixels per byte, 2 bits each (MSB first)
            for (int p = 0; p < 4 && (x + p) < PD_W; p++) {
                int idx = (b >> (6 - p * 2)) & 0x03;
                pixels[y * PD_W + x + p] = palette[idx];
            }
        }
    }

    // Render into 320×240 RGB888 framebuffer (centered)
    int ox = (SCREEN_W - PD_W) / 2;  // 32
    int oy = (SCREEN_H - PD_H) / 2;  // 24

    // Clear borders to black
    memset(v->framebuffer, 0, SCREEN_W * SCREEN_H * 3);

    for (int y = 0; y < PD_H; y++) {
        for (int x = 0; x < PD_W; x++) {
            uint8_t lum = pixels[y * PD_W + x];
            int dst = ((y + oy) * SCREEN_W + (x + ox)) * 3;
            v->framebuffer[dst + 0] = lum;
            v->framebuffer[dst + 1] = lum;
            v->framebuffer[dst + 2] = lum;
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
            if (v->vid_frame_pos + copy > (int)sizeof(v->vid_frame_buf))
                copy = (int)sizeof(v->vid_frame_buf) - v->vid_frame_pos;
            if (copy > 0) {
                memcpy(v->vid_frame_buf + v->vid_frame_pos, payload + 1, copy);
                v->vid_frame_pos += copy;
            }
        } else if (marker == 0xF2) {
            // Frame end marker — decode the assembled video frame
            if (v->vid_frame_pos > 0) {
                playdia_decode_video_frame(v);
                v->vid_frame_pos = 0;
            }
        } else if (marker == 0xF3) {
            // Scene marker — reset frame accumulator
            v->vid_frame_pos = 0;
        }
    }
}
