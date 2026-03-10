#pragma once
#include "playdia.h"
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>

// ─────────────────────────────────────────────────────────────
//  Asahi Kasei AK8000  —  Audio/Video Processor
//
//  The AK8000 uses a proprietary video codec (NOT standard MPEG-1).
//  Video frames are assembled from 6 × F1 sectors (12282 bytes)
//  with a 40-byte header and DCT-based compression.
//
//  Video: 256×144, 4:2:0, ~15fps, MPEG-1 DC VLC with DPCM
//         (AC coefficient coding not yet reverse-engineered)
//  Audio: CD-ROM XA ADPCM (decoded in hardware path)
//
//  The libavcodec path is kept for potential MPEG-PS/VCD content.
// ─────────────────────────────────────────────────────────────

// AK8000 register offsets (I/O mapped at 0x6000+)
#define AK8000_REG_CTRL     0x00
#define AK8000_REG_STATUS   0x01
#define AK8000_REG_VID_MODE 0x02
#define AK8000_REG_AUD_CTRL 0x03
#define AK8000_REG_DMA_ADDR 0x04
#define AK8000_REG_DMA_LEN  0x06

// MPEG-1 start codes
#define MPEG_SEQ_START   0x000001B3   // Sequence header
#define MPEG_GOP_START   0x000001B8   // Group of Pictures
#define MPEG_PIC_START   0x00000100   // Picture header
#define MPEG_PACK_START  0x000001BA   // Pack header (MPEG-PS/VCD)
#define MPEG_SYS_START   0x000001BB   // System header
#define MPEG_VID_STREAM  0x000001E0   // Video stream 0
#define MPEG_AUD_STREAM  0x000001C0   // Audio stream 0

// Internal ES (Elementary Stream) accumulator
#define ES_BUF_SIZE  (256 * 1024)   // 256KB — enough for several frames

typedef struct AK8000 {
    // ── Registers ─────────────────────────────────────────
    uint8_t  regs[16];

    // ── Output framebuffer: 320×240 RGB888 ───────────────
    uint8_t  framebuffer[SCREEN_W * SCREEN_H * 3];

    // ── Audio ring buffer (stereo int16) ──────────────────
    // XA ADPCM: up to 8064 int16 values per sector, need room for several
    int16_t  audio_buf[SAMPLES_PER_FRAME * CHANNELS * 8];
    int      audio_write_pos;
    int      audio_read_pos;

    // ── State ─────────────────────────────────────────────
    bool     video_active;
    bool     audio_active;
    uint32_t frame_count;
    bool     got_video_frame;  // new frame available this tick

    // ── MPEG ES accumulators ──────────────────────────────
    uint8_t  vid_es[ES_BUF_SIZE];   // Video elementary stream
    int      vid_es_len;
    uint8_t  aud_es[ES_BUF_SIZE];   // Audio elementary stream
    int      aud_es_len;

    // ── libavcodec video decoder ──────────────────────────
    const AVCodec      *vid_codec;
    AVCodecContext     *vid_ctx;
    AVFrame            *vid_frame;
    AVPacket           *vid_pkt;
    struct SwsContext  *sws_ctx;
    bool                codec_ready;

    // ── libavcodec audio decoder ──────────────────────────
    const AVCodec      *aud_codec;
    AVCodecContext     *aud_ctx;
    AVFrame            *aud_frame;
    AVPacket           *aud_pkt;
    bool                acodec_ready;

    // ── XA ADPCM decoder (hardware path) ─────────────────────
    int32_t  xa_prev[2];    // previous samples per channel
    int32_t  xa_prev2[2];   // 2nd previous samples

    // ── Video frame assembly (Playdia proprietary codec) ─────
    uint8_t  vid_frame_buf[65536];   // assembled video frame (F1 sectors)
    int      vid_frame_pos;          // write position
    bool     vid_frame_ready;        // F2 received → frame complete

    // ── Playdia video state ───────────────────────────────────
    uint8_t  qtable[16];             // 4×4 quantization table
    uint8_t  qscale;                 // quantization scale factor
    int      dc_pred[3];             // DC DPCM predictors (Y, Cb, Cr)
} AK8000;

// ── API ───────────────────────────────────────────────────────
void    ak8000_init        (AK8000 *v);
void    ak8000_reset       (AK8000 *v);
void    ak8000_free        (AK8000 *v);
void    ak8000_write_reg   (AK8000 *v, uint8_t reg, uint8_t val);
uint8_t ak8000_read_reg    (AK8000 *v, uint8_t reg);

// Feed raw CD sector data; internally scans for MPEG start codes
void    ak8000_feed_sector (AK8000 *v, const uint8_t *sector, int len);

// Feed CD-XA sector with subheader info for channel-based demuxing
void    ak8000_feed_xa_sector(AK8000 *v, const uint8_t *raw_sector);

// Legacy alias
static inline void ak8000_decode_frame(AK8000 *v, uint8_t *data, int len) {
    ak8000_feed_sector(v, data, len);
}

void    ak8000_tick        (AK8000 *v);
