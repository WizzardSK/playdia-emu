#pragma once
#include "playdia.h"
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>

// ─────────────────────────────────────────────────────────────
//  Asahi Kasei AK8000  —  Audio/Video Processor
//
//  The AK8000 uses a proprietary video codec (NOT standard MPEG-1).
//  Video frames are assembled from 6-13 F1 sectors (12282-26611 bytes)
//  with a 40-byte header and DCT-based compression.
//  Most packets use 8 F1 sectors (16376 bytes) containing 3 frames.
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

// ── Runtime-tunable codec parameters ─────────────────────────
typedef struct CodecParams {
    int  ac_count;       // AC coefficients per block (default 10)
    int  dc_mode;        // 0=init+diff, 1=DPCM accumulate
    int  dc_scale;       // DC multiplier (default 8)
    int  bs_offset;      // bitstream start byte (default 44)
    int  width;          // frame width (default 192)
    int  height;         // frame height (default 144)
    int  level_shift;    // added to pixel after IDCT (default 0)
    bool use_eob;        // treat VLC 0 as EOB (default false)
    int  ac_dequant;     // 0=none (raw VLC), 1=qtable*qscale/8
    int  scan_order;     // 0=row-major, 1=col-major, 2=zigzag, 3=boustrophedon
                         // 4=interleaved-2, 5=reverse-row, 6=bottom-up
    int  block_order;    // Y block layout in MB: 0=Z(00,10,01,11), 1=N(00,01,10,11)
                         // 2=U-pattern, 3=reverse-Z
    int  dc_only;        // 0=normal, 1=DC only (no AC, shows block-level thumbnail)
    int  grid_overlay;   // 0=off, 1=8x8 block grid, 2=MB grid, 3=both
    int  chroma_mode;    // 0=4:2:0 (6 blocks/MB), 1=4:2:2 (8), 2=4:1:1 (6alt), 3=mono(4)
    int  zigzag_alt;     // 0=standard MPEG-1, 1=alternate (MPEG-2), 2=raster (no zigzag)
    int  mb_size;        // 0=16x16, 1=8x8 (each "MB" is one block)
    int  interleave;     // 0=MB-interleaved (4Y+Cb+Cr per MB)
                         // 1=plane (all Y, then all Cb, then all Cr)
                         // 2=Y-only (treat all blocks as Y)
    int  vlc_invert;     // 0=normal MPEG-1 size mapping (short code -> small size)
                         // 1=inverted (short code -> large size). Test: 6× wider DC
                         //   diff std on Keroppi but with -29 mean bias.
    int  dc_diff_mult;   // 0=raw diff, 1=diff*qscale, 2=diff*qtable[0]
                         // Hypothesis: DC = init + diff × qscale gives needed range.

    int  selected;       // currently selected param for UI

    // ── Auto-tune state ──────────────────────────────────────
    bool autotune;       // auto-tune active
    int  tune_param;     // which param we're currently tuning
    int  tune_step;      // current step: 0=baseline, 1=try+, 2=try-
    double best_score;   // best score so far
    int  tune_wait;      // frames to wait before measuring
    int  stale_count;    // consecutive params with no improvement
    bool save_frame;     // flag: save next frame to /tmp
} CodecParams;

#define CODEC_PARAM_COUNT 19

void    codec_params_init   (CodecParams *cp);
void    codec_params_adjust (CodecParams *cp, int delta);
void    codec_params_next   (CodecParams *cp);
void    codec_params_prev   (CodecParams *cp);
void    codec_params_print  (const CodecParams *cp);

// Frame quality score (higher = better image structure)
double  codec_frame_score   (const uint8_t *framebuffer, int fb_w, int fb_h,
                             int img_w, int img_h);
// Auto-tune step — call once per frame after decode
void    codec_autotune_step (CodecParams *cp, double score);

typedef struct AK8000 {
    // ── Registers ─────────────────────────────────────────
    uint8_t  regs[16];

    // ── Output framebuffer: 320×240 RGB888 ───────────────
    uint8_t  framebuffer[SCREEN_W * SCREEN_H * 3];

    // ── Audio ring buffer (stereo int16) ──────────────────
    // XA ADPCM: after resampling to 44100Hz, ~5300 stereo pairs per sector
    // Need room for several sectors worth of audio
    int16_t  audio_buf[SAMPLES_PER_FRAME * CHANNELS * 16];
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

    // ── Frame queue (multi-frame packets produce 2-4 frames) ──
    #define PD_FRAME_QUEUE_SIZE 8
    uint8_t  frame_queue[PD_FRAME_QUEUE_SIZE][SCREEN_W * SCREEN_H * 3];
    int      fq_write;               // next write slot
    int      fq_read;                // next read slot
    int      fq_count;               // frames in queue

    // ── Interactive FMV state ────────────────────────────────
    bool     interactive_pending;    // F2 command ready for processing
    bool     waiting_for_input;     // paused waiting for player button
    uint8_t  interactive_cmd;       // F2 command type (0x40, 0x44, etc.)
    uint32_t button_dest[7];        // 7 button destination LBAs
    uint8_t  button_extra[7];       // extra byte per button slot
    uint32_t timeout_dest;          // F2 80 timeout destination LBA
    uint8_t  timeout_sub;           // F2 80 subcommand byte
    int      input_timer;           // frames remaining before timeout
    uint32_t seek_target;           // LBA to seek to (0 = none pending)
    uint32_t cmd_lba;               // LBA where last F2 command was found
    bool     is_loop;               // true if F2 40 is a backward jump (loop)

    // ── Codec tuning (runtime-adjustable) ─────────────────────
    CodecParams  codec_params;
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
