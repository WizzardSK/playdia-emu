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

// ── Codec parameter tuning ────────────────────────────────────
static const char *cp_names[CODEC_PARAM_COUNT] = {
    "ac_count", "dc_mode", "dc_scale", "bs_offset",
    "width", "height", "level_shift", "use_eob", "ac_dequant",
    "scan_order", "block_order",
    "dc_only", "grid", "chroma", "zigzag", "mb_size", "interleave",
    "vlc_invert", "dc_diff_mult"
};

void codec_params_init(CodecParams *cp) {
    cp->ac_count    = 10;
    cp->dc_mode     = 0;    // init+diff
    cp->dc_scale    = 8;
    cp->bs_offset   = 44;
    cp->width       = 192;
    cp->height      = 144;
    cp->level_shift = 0;
    cp->use_eob     = false;
    cp->ac_dequant  = 0;
    cp->scan_order  = 0;    // row-major
    cp->block_order = 0;    // Z-pattern
    cp->dc_only     = 0;
    cp->grid_overlay = 0;
    cp->chroma_mode = 0;    // 4:2:0
    cp->zigzag_alt  = 0;    // standard MPEG-1
    cp->mb_size     = 0;    // 16x16
    cp->interleave  = 0;    // MB-interleaved
    cp->vlc_invert  = 0;    // standard MPEG-1 size mapping
    cp->dc_diff_mult = 0;   // raw diff (no qscale multiplication)
    cp->selected    = 0;
}

void codec_params_adjust(CodecParams *cp, int delta) {
    switch (cp->selected) {
    case 0: cp->ac_count    += delta; if (cp->ac_count < 0) cp->ac_count = 0; if (cp->ac_count > 63) cp->ac_count = 63; break;
    case 1: cp->dc_mode     = cp->dc_mode ? 0 : 1; break;
    case 2: cp->dc_scale    += delta; if (cp->dc_scale < 1) cp->dc_scale = 1; if (cp->dc_scale > 32) cp->dc_scale = 32; break;
    case 3: cp->bs_offset   += delta; if (cp->bs_offset < 40) cp->bs_offset = 40; if (cp->bs_offset > 52) cp->bs_offset = 52; break;
    case 4: cp->width       += delta * 16; if (cp->width < 128) cp->width = 128; if (cp->width > 256) cp->width = 256; break;
    case 5: cp->height      += delta * 16; if (cp->height < 96) cp->height = 96; if (cp->height > 192) cp->height = 192; break;
    case 6: cp->level_shift += delta * 8; if (cp->level_shift < -64) cp->level_shift = -64; if (cp->level_shift > 64) cp->level_shift = 64; break;
    case 7: cp->use_eob     = !cp->use_eob; break;
    case 8: cp->ac_dequant  = cp->ac_dequant ? 0 : 1; break;
    case 9:  cp->scan_order   += delta; if (cp->scan_order < 0) cp->scan_order = 6; if (cp->scan_order > 6) cp->scan_order = 0; break;
    case 10: cp->block_order  += delta; if (cp->block_order < 0) cp->block_order = 3; if (cp->block_order > 3) cp->block_order = 0; break;
    case 11: cp->dc_only      = cp->dc_only ? 0 : 1; break;
    case 12: cp->grid_overlay += delta; if (cp->grid_overlay < 0) cp->grid_overlay = 3; if (cp->grid_overlay > 3) cp->grid_overlay = 0; break;
    case 13: cp->chroma_mode  += delta; if (cp->chroma_mode < 0) cp->chroma_mode = 3; if (cp->chroma_mode > 3) cp->chroma_mode = 0; break;
    case 14: cp->zigzag_alt   += delta; if (cp->zigzag_alt < 0) cp->zigzag_alt = 2; if (cp->zigzag_alt > 2) cp->zigzag_alt = 0; break;
    case 15: cp->mb_size      = cp->mb_size ? 0 : 1; break;
    case 16: cp->interleave  += delta; if (cp->interleave < 0) cp->interleave = 2; if (cp->interleave > 2) cp->interleave = 0; break;
    case 17: cp->vlc_invert  = cp->vlc_invert ? 0 : 1; break;
    case 18: cp->dc_diff_mult += delta; if (cp->dc_diff_mult < 0) cp->dc_diff_mult = 2; if (cp->dc_diff_mult > 2) cp->dc_diff_mult = 0; break;
    }
    codec_params_print(cp);
}

void codec_params_next(CodecParams *cp) {
    cp->selected = (cp->selected + 1) % CODEC_PARAM_COUNT;
    codec_params_print(cp);
}

void codec_params_prev(CodecParams *cp) {
    cp->selected = (cp->selected + CODEC_PARAM_COUNT - 1) % CODEC_PARAM_COUNT;
    codec_params_print(cp);
}

void codec_params_print(const CodecParams *cp) {
    printf("\n[CODEC] ");
    int vals[CODEC_PARAM_COUNT] = {
        cp->ac_count, cp->dc_mode, cp->dc_scale, cp->bs_offset,
        cp->width, cp->height, cp->level_shift, cp->use_eob, cp->ac_dequant,
        cp->scan_order, cp->block_order,
        cp->dc_only, cp->grid_overlay, cp->chroma_mode, cp->zigzag_alt, cp->mb_size,
        cp->interleave, cp->vlc_invert, cp->dc_diff_mult
    };
    for (int i = 0; i < CODEC_PARAM_COUNT; i++) {
        if (i == cp->selected)
            printf(">>%s=%d<< ", cp_names[i], vals[i]);
        else
            printf("%s=%d ", cp_names[i], vals[i]);
    }
    printf("\n");
}

// ── Frame quality scoring ─────────────────────────────────────
// Measures image structure quality for a correctly decoded DCT image:
//
// 1. Block boundary vs interior ratio (boundary should NOT be much worse)
// 2. Multi-scale spatial coherence (2×2, 4×4 neighborhood correlation)
// 3. Histogram spread: a real image uses most of the dynamic range
// 4. Non-uniform content: penalize flat/repetitive images
//
// Score = coherence * spread / boundary_penalty
// Higher = better (more natural-looking image)

double codec_frame_score(const uint8_t *fb, int fb_w, int fb_h,
                         int img_w, int img_h) {
    int ox = (fb_w - img_w) / 2;
    int oy = (fb_h - img_h) / 2;
    if (img_w < 32 || img_h < 32) return 0;

    // Build luma array
    int npx = img_w * img_h;
    // Use static buffer to avoid large stack alloc
    static uint8_t luma_buf[320 * 240];
    for (int y = 0; y < img_h; y++)
        for (int x = 0; x < img_w; x++) {
            int idx = ((y + oy) * fb_w + (x + ox)) * 3;
            luma_buf[y * img_w + x] = (uint8_t)((fb[idx] * 77 + fb[idx+1] * 150 + fb[idx+2] * 29) >> 8);
        }

    // 1. Histogram: count used bins (out of 32 bins)
    int hist[32] = {0};
    long sum = 0;
    for (int i = 0; i < npx; i++) { hist[luma_buf[i] >> 3]++; sum += luma_buf[i]; }
    int used_bins = 0;
    for (int i = 0; i < 32; i++) if (hist[i] > 0) used_bins++;
    double mean = (double)sum / npx;

    // Penalize images that are mostly one value (flat)
    int peak_bin = 0;
    for (int i = 0; i < 32; i++) if (hist[i] > hist[peak_bin]) peak_bin = i;
    double peak_frac = (double)hist[peak_bin] / npx;
    if (peak_frac > 0.8) return 0.01; // >80% same value = garbage

    // Histogram spread score: more bins = more realistic
    double spread = used_bins / 32.0;

    // 2. Block boundary vs interior
    double boundary_diff = 0, interior_diff = 0;
    int boundary_n = 0, interior_n = 0;

    for (int y = 0; y < img_h - 1; y++) {
        for (int x = 0; x < img_w - 1; x++) {
            int L = luma_buf[y * img_w + x];
            int R = luma_buf[y * img_w + x + 1];
            int D = luma_buf[(y+1) * img_w + x];
            double dh = abs(L - R);
            double dv = abs(L - D);

            if ((x + 1) % 8 == 0) { boundary_diff += dh; boundary_n++; }
            else { interior_diff += dh; interior_n++; }
            if ((y + 1) % 8 == 0) { boundary_diff += dv; boundary_n++; }
            else { interior_diff += dv; interior_n++; }
        }
    }

    double avg_boundary = boundary_n > 0 ? boundary_diff / boundary_n : 999;
    double avg_interior = interior_n > 0 ? interior_diff / interior_n : 1;

    // Boundary ratio: 1.0=perfect, >1=blocky, <1=boundaries smoother than interior (weird)
    double bratio = (avg_interior > 0.5) ? avg_boundary / avg_interior : 5.0;

    // 3. 4×4 neighborhood coherence: average correlation in local patches
    double coh_sum = 0; int coh_n = 0;
    for (int y = 0; y + 3 < img_h; y += 4) {
        for (int x = 0; x + 3 < img_w; x += 4) {
            // Variance within 4×4 patch
            int psum = 0, psum2 = 0;
            for (int dy = 0; dy < 4; dy++)
                for (int dx = 0; dx < 4; dx++) {
                    int v = luma_buf[(y+dy) * img_w + (x+dx)];
                    psum += v; psum2 += v * v;
                }
            double pmean = psum / 16.0;
            double pvar = psum2 / 16.0 - pmean * pmean;
            // Low local variance = coherent patch (good for natural images)
            coh_sum += 1.0 / (1.0 + pvar / 100.0);
            coh_n++;
        }
    }
    double coherence = coh_n > 0 ? coh_sum / coh_n : 0;

    // 4. Variance bonus: real images have moderate variance
    double var = 0;
    for (int i = 0; i < npx; i++) { double d = luma_buf[i] - mean; var += d * d; }
    var /= npx;
    // Sweet spot: variance 200-2000
    double var_bonus = 1.0;
    if (var < 50) var_bonus = 0.2;       // too flat
    else if (var < 200) var_bonus = 0.5 + 0.5 * (var - 50) / 150.0;
    else if (var > 3000) var_bonus = 0.5; // too noisy

    // Combined score
    double score = coherence * spread * var_bonus / (bratio * bratio);
    return score;
}

// ── Auto-tune hill climbing ──────────────────────────────────
// Each step: measure baseline, try +delta, try -delta for current param
// Keep whichever is best, move to next param
static int at_get_val(const CodecParams *cp, int param) {
    switch (param) {
    case 0: return cp->ac_count;
    case 1: return cp->dc_mode;
    case 2: return cp->dc_scale;
    case 3: return cp->bs_offset;
    case 4: return cp->width;
    case 5: return cp->height;
    case 6: return cp->level_shift;
    case 7: return cp->use_eob ? 1 : 0;
    case 8: return cp->ac_dequant;
    case 9: return cp->scan_order;
    case 10: return cp->block_order;
    case 11: return cp->dc_only;
    case 12: return cp->grid_overlay;
    case 13: return cp->chroma_mode;
    case 14: return cp->zigzag_alt;
    case 15: return cp->mb_size;
    case 16: return cp->interleave;
    case 17: return cp->vlc_invert;
    case 18: return cp->dc_diff_mult;
    default: return 0;
    }
}

static int at_clamp(int val, int lo, int hi) { return val < lo ? lo : val > hi ? hi : val; }

static void at_set_val(CodecParams *cp, int param, int val) {
    switch (param) {
    case 0: cp->ac_count    = at_clamp(val, 0, 63); break;
    case 1: cp->dc_mode     = at_clamp(val, 0, 1); break;
    case 2: cp->dc_scale    = at_clamp(val, 1, 32); break;
    case 3: cp->bs_offset   = at_clamp(val, 40, 52); break;
    case 4: cp->width       = at_clamp(val, 128, 256); break;
    case 5: cp->height      = at_clamp(val, 96, 192); break;
    case 6: cp->level_shift = at_clamp(val, -64, 64); break;
    case 7: cp->use_eob     = at_clamp(val, 0, 1); break;
    case 8: cp->ac_dequant  = at_clamp(val, 0, 5); break;
    case 9: cp->scan_order  = at_clamp(val, 0, 6); break;
    case 10: cp->block_order = at_clamp(val, 0, 3); break;
    case 11: cp->dc_only    = at_clamp(val, 0, 1); break;
    case 12: cp->grid_overlay = at_clamp(val, 0, 3); break;
    case 13: cp->chroma_mode = at_clamp(val, 0, 3); break;
    case 14: cp->zigzag_alt = at_clamp(val, 0, 2); break;
    case 15: cp->mb_size    = at_clamp(val, 0, 1); break;
    case 16: cp->interleave = at_clamp(val, 0, 2); break;
    case 17: cp->vlc_invert = at_clamp(val, 0, 1); break;
    case 18: cp->dc_diff_mult = at_clamp(val, 0, 2); break;
    }
}

// Step sizes per param
static int at_delta(int param) {
    switch (param) {
    case 0: return 1;     // ac_count: ±1
    case 1: return 1;     // dc_mode: toggle
    case 2: return 1;     // dc_scale: ±1
    case 3: return 1;     // bs_offset: ±1
    case 4: return 16;    // width: ±16
    case 5: return 16;    // height: ±16
    case 6: return 8;     // level_shift: ±8
    case 7: return 1;     // use_eob: toggle
    case 8: return 1;     // ac_dequant: toggle
    case 9: return 1;     // scan_order: cycle
    case 10: return 1;    // block_order: cycle
    case 11: return 1;    // dc_only: toggle
    case 12: return 1;    // grid: cycle
    case 13: return 1;    // chroma: cycle
    case 14: return 1;    // zigzag: cycle
    case 15: return 1;    // mb_size: toggle
    case 16: return 1;    // interleave: cycle
    case 17: return 1;    // vlc_invert: toggle
    case 18: return 1;    // dc_diff_mult: cycle
    default: return 1;
    }
}

static int at_saved_val;
static double at_scores[3]; // baseline, try+, try-

void codec_autotune_step(CodecParams *cp, double score) {
    if (!cp->autotune) return;

    // Wait frames for decoder to settle after param change
    if (cp->tune_wait > 0) { cp->tune_wait--; return; }

    switch (cp->tune_step) {
    case 0: // Measure baseline
        at_scores[0] = score;
        at_saved_val = at_get_val(cp, cp->tune_param);
        // Try +delta
        if (cp->tune_param == 1 || cp->tune_param == 7 || cp->tune_param == 8) {
            // Boolean params: just toggle
            at_set_val(cp, cp->tune_param, at_saved_val ? 0 : 1);
            cp->tune_step = 1;
        } else {
            at_set_val(cp, cp->tune_param, at_saved_val + at_delta(cp->tune_param));
        }
        cp->tune_step = 1;
        cp->tune_wait = 5;
        printf("[TUNE] %s: baseline=%.2f, trying +%d (val=%d→%d)\n",
               cp_names[cp->tune_param], score,
               at_delta(cp->tune_param), at_saved_val,
               at_get_val(cp, cp->tune_param));
        break;

    case 1: // Measure +delta
        at_scores[1] = score;
        // For boolean params, skip try- (it's the same as baseline)
        if (cp->tune_param == 1 || cp->tune_param == 7 || cp->tune_param == 8) {
            cp->tune_step = 3; // go to decision
            // fall through to decision
        } else {
            // Try -delta
            at_set_val(cp, cp->tune_param, at_saved_val - at_delta(cp->tune_param));
            cp->tune_step = 2;
            cp->tune_wait = 5;
            printf("[TUNE] %s: +delta=%.2f, trying -%d (val=%d)\n",
                   cp_names[cp->tune_param], score,
                   at_delta(cp->tune_param),
                   at_get_val(cp, cp->tune_param));
            break;
        }
        // fallthrough for booleans
        __attribute__((fallthrough));

    case 2: // Measure -delta
        at_scores[2] = score;
        cp->tune_step = 3;
        // fallthrough to decision
        __attribute__((fallthrough));

    case 3: { // Decision
        int best = 0;
        double best_s = at_scores[0];
        int n_tries = (cp->tune_param == 1 || cp->tune_param == 7 || cp->tune_param == 8) ? 2 : 3;
        for (int i = 1; i < n_tries; i++) {
            if (at_scores[i] > best_s) { best_s = at_scores[i]; best = i; }
        }

        if (best == 0) {
            // Baseline was best — revert
            at_set_val(cp, cp->tune_param, at_saved_val);
            printf("[TUNE] %s: KEEP %d (scores: base=%.2f +d=%.2f -d=%.2f)\n",
                   cp_names[cp->tune_param], at_saved_val,
                   at_scores[0], at_scores[1],
                   n_tries > 2 ? at_scores[2] : -1.0);
            cp->stale_count++;
        } else if (best == 1) {
            at_set_val(cp, cp->tune_param, at_saved_val + at_delta(cp->tune_param));
            printf("[TUNE] %s: BETTER at %d (+delta, score %.2f→%.2f)\n",
                   cp_names[cp->tune_param],
                   at_get_val(cp, cp->tune_param),
                   at_scores[0], at_scores[1]);
            cp->stale_count = 0;
        } else {
            at_set_val(cp, cp->tune_param, at_saved_val - at_delta(cp->tune_param));
            printf("[TUNE] %s: BETTER at %d (-delta, score %.2f→%.2f)\n",
                   cp_names[cp->tune_param],
                   at_get_val(cp, cp->tune_param),
                   at_scores[0], at_scores[2]);
            cp->stale_count = 0;
        }

        // Move to next param
        cp->tune_param = (cp->tune_param + 1) % CODEC_PARAM_COUNT;
        cp->tune_step = 0;
        cp->tune_wait = 3;

        // Print current state every full cycle
        if (cp->tune_param == 0) {
            printf("[TUNE] === Cycle complete ===\n");
            codec_params_print(cp);
            // If all params stale for 2 full cycles, stop
            if (cp->stale_count >= CODEC_PARAM_COUNT * 2) {
                printf("[TUNE] Converged! Stopping auto-tune.\n");
                cp->autotune = false;
            }
        }
        break;
    }
    }
}

void ak8000_init(AK8000 *v) {
    memset(v, 0, sizeof *v);
    codec_params_init(&v->codec_params);
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
    v->fq_write = v->fq_read = v->fq_count = 0;
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

    // Pop one frame from queue into display framebuffer
    v->got_video_frame = false;
    if (v->fq_count > 0) {
        memcpy(v->framebuffer, v->frame_queue[v->fq_read],
               SCREEN_W * SCREEN_H * 3);
        v->fq_read = (v->fq_read + 1) % PD_FRAME_QUEUE_SIZE;
        v->fq_count--;
        v->got_video_frame = true;
        v->vid_frame_ready = true;
    }
}

// ─────────────────────────────────────────────────────────────
//  Playdia Proprietary Video Decoder (AK8000 DCT codec)
//
//  Packet format (assembled from 6-13 × F1 sectors):
//    Bytes 0-2:   00 80 04 — packet marker
//    Byte 3:      quantization scale (QS)
//    Bytes 4-19:  16-byte quantization table (4×4, expanded to 8×8)
//    Bytes 20-35: repeat of qtable
//    Bytes 36-38: 00 80 24
//    Byte 39:     unknown flags
//    Bytes 40+:   bitstream containing 1+ frames
//
//  Each frame: interleaved DC+AC per block, 8×9 macroblocks
//    For each MB (row-major): 4Y + 1Cb + 1Cr blocks
//      For each block: 64 VLC-coded coefficients (zigzag order)
//        coeff[0] = DC (DPCM per component, init=0)
//        coeff[1..63] = AC
//
//  VLC: Modified MPEG-1 luminance DC VLC
//    Sizes 0-6: standard MPEG-1
//    Size 7: 111110 (6 bits) — same as standard
//    Size 8: 111111 (6 bits) — compressed from standard 7-bit
//
//  Dequantization: DC × 8, AC × qtable[pos] × QS / 8
//  Resolution: 128×144, YCbCr 4:2:0, 8×9 macroblocks = 432 blocks
//  IDCT: standard orthonormal 8×8 DCT, pixel = IDCT(coeff) + 128
// ─────────────────────────────────────────────────────────────

#define PD_W  192
#define PD_H  144
#define PD_MW (PD_W / 16)       // 12
#define PD_MH (PD_H / 16)       // 9
#define PD_BPM 6                 // 4:2:0 = 6 blocks per macroblock
#define PD_NBLOCKS (PD_MW * PD_MH * PD_BPM)  // 12×9×6 = 648

// Zigzag scan orders
static const int pd_zigzag_standard[64] = {
    0,  1,  8, 16,  9,  2,  3, 10,
   17, 24, 32, 25, 18, 11,  4,  5,
   12, 19, 26, 33, 40, 48, 41, 34,
   27, 20, 13,  6,  7, 14, 21, 28,
   35, 42, 49, 56, 57, 50, 43, 36,
   29, 22, 15, 23, 30, 37, 44, 51,
   58, 59, 52, 45, 38, 31, 39, 46,
   53, 60, 61, 54, 47, 55, 62, 63
};
// MPEG-2 alternate scan (field/interlaced DCT)
static const int pd_zigzag_alt[64] = {
    0,  8, 16, 24,  1,  9,  2, 10,
   17, 25, 32, 40, 48, 56, 57, 49,
   41, 33, 26, 18,  3, 11,  4, 12,
   19, 27, 34, 42, 50, 58, 35, 43,
   51, 59, 20, 28,  5, 13,  6, 14,
   21, 29, 36, 44, 52, 60, 37, 45,
   53, 61, 22, 30,  7, 15, 23, 31,
   38, 46, 54, 62, 39, 47, 55, 63
};
// Raster scan (no zigzag — row by row)
static const int pd_zigzag_raster[64] = {
    0,  1,  2,  3,  4,  5,  6,  7,
    8,  9, 10, 11, 12, 13, 14, 15,
   16, 17, 18, 19, 20, 21, 22, 23,
   24, 25, 26, 27, 28, 29, 30, 31,
   32, 33, 34, 35, 36, 37, 38, 39,
   40, 41, 42, 43, 44, 45, 46, 47,
   48, 49, 50, 51, 52, 53, 54, 55,
   56, 57, 58, 59, 60, 61, 62, 63
};

// Active zigzag pointer (set per-frame based on codec params)
static const int *pd_zigzag = pd_zigzag_standard;

typedef struct {
    const uint8_t *data;
    int total_bits;
    int pos;
} pd_bitstream;

static inline int pd_bs_get1(pd_bitstream *bs) {
    if (bs->pos >= bs->total_bits) return 0;
    int bp = bs->pos++;
    return (bs->data[bp >> 3] >> (7 - (bp & 7))) & 1;
}

static int pd_bs_read(pd_bitstream *bs, int n) {
    if (n <= 0) return 0;
    int v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | pd_bs_get1(bs);
    return v;
}

// AK8000 VLC: MPEG-1 luminance DC VLC with modified sizes 7/8
// Size 7 = 111110 (6 bits), Size 8 = 111111 (6 bits)
// Both are 6 bits, differentiated by last bit (0=size7, 1=size8)
// Set by decode_frame() to point at the active CodecParams so pd_read_vlc can
// see vlc_invert without threading the param through every call site.
static const CodecParams *pd_active_cp = NULL;
static int pd_read_vlc(pd_bitstream *bs) {
    if (bs->pos >= bs->total_bits) return -9999;
    int size;
    int invert = (pd_active_cp && pd_active_cp->vlc_invert) ? 1 : 0;
    if (invert) {
        // Inverted mapping: short codes → large sizes.
        // 00→size 8, 01→size 7, 100→size 6, 101→size 5,
        // 110→size 4, 1110→size 3, 11110→size 2, 111110→size 1, 111111→size 0
        if (pd_bs_get1(bs) == 0) {
            size = pd_bs_get1(bs) ? 7 : 8;
        } else {
            if (pd_bs_get1(bs) == 0) {
                size = pd_bs_get1(bs) ? 5 : 6;
            } else {
                if (pd_bs_get1(bs) == 0) size = 4;
                else if (pd_bs_get1(bs) == 0) size = 3;
                else if (pd_bs_get1(bs) == 0) size = 2;
                else size = pd_bs_get1(bs) ? 0 : 1;
            }
        }
    } else if (pd_bs_get1(bs) == 0) {
        size = pd_bs_get1(bs) ? 2 : 1;          // 0x → 00=1, 01=2
    } else {
        if (pd_bs_get1(bs) == 0) {
            size = pd_bs_get1(bs) ? 3 : 0;      // 10x → 100=0, 101=3
        } else {
            if (pd_bs_get1(bs) == 0) size = 4;       // 110
            else if (pd_bs_get1(bs) == 0) size = 5;  // 1110
            else if (pd_bs_get1(bs) == 0) size = 6;  // 11110
            else size = pd_bs_get1(bs) ? 8 : 7;      // 111110=7, 111111=8
        }
    }
    if (size == 0) return 0;
    int val = pd_bs_read(bs, size);
    if (val < (1 << (size - 1)))
        val -= (1 << size) - 1;
    return val;
}

static int pd_clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }


// Integer 8×8 IDCT using precomputed cosine matrix (no cos()/sqrt() at runtime)
// C[k][n] = cos((2n+1)*k*pi/16) * 2048, k=0 scaled by 1/sqrt(2)
// k=0 row scaled by 1/sqrt(2) for orthonormal DCT
static const int16_t pd_cos[8][8] = {
    { 1448, 1448, 1448, 1448, 1448, 1448, 1448, 1448}, // k=0: 2048/sqrt(2)
    { 2009, 1703, 1138,  400, -400,-1138,-1703,-2009}, // k=1
    { 1892,  784, -784,-1892,-1892, -784,  784, 1892}, // k=2
    { 1703, -400,-2009,-1138, 1138, 2009,  400,-1703}, // k=3
    { 1448,-1448,-1448, 1448, 1448,-1448,-1448, 1448}, // k=4
    { 1138,-2009,  400, 1703,-1703, -400, 2009,-1138}, // k=5
    {  784,-1892, 1892, -784, -784, 1892,-1892,  784}, // k=6
    {  400,-1138, 1703,-2009, 2009,-1703, 1138, -400}, // k=7
};

static void pd_idct_block(const int coeff[64], uint8_t out[8][8]) {
    int matrix[8][8];
    memset(matrix, 0, sizeof(matrix));
    for (int i = 0; i < 64; i++)
        matrix[pd_zigzag[i] / 8][pd_zigzag[i] % 8] = coeff[i];

    int temp[8][8];

    // Row transform: temp[i][j] = sum_k(matrix[i][k] * cos[k][j]) / (2*2048)
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            int sum = 0;
            for (int k = 0; k < 8; k++)
                sum += matrix[i][k] * pd_cos[k][j];
            temp[i][j] = (sum + 2048) >> 12;
        }
    }
    // Column transform + level shift
    for (int j = 0; j < 8; j++) {
        for (int i = 0; i < 8; i++) {
            int sum = 0;
            for (int k = 0; k < 8; k++)
                sum += temp[k][j] * pd_cos[k][i];
            out[i][j] = (uint8_t)pd_clamp((sum + 2048) >> 12);  // no level shift
        }
    }
}

// Decode one video frame from the bitstream
// Interleaved DC+AC per block, 4:2:0: 4Y + 1Cb + 1Cr per macroblock
// Each block: 1 DC (DPCM) + AC coefficients until VLC value 0 (EOB)
// Returns number of blocks decoded (432 on success), 0 on failure
static int pd_decode_one_frame(pd_bitstream *bs, int coeff[PD_NBLOCKS][64],
                                int qscale, const uint8_t qtable[16],
                                int init_y, int init_cb, int init_cr,
                                const CodecParams *cp) {
    pd_active_cp = cp;  // expose to pd_read_vlc()
    int nblocks = 0;
    int mbpx = cp->mb_size ? 8 : 16;  // MB pixel size
    int mw = cp->width / mbpx;
    int mh = cp->height / mbpx;
    // Blocks per MB depends on chroma mode and MB size
    int bpm;
    if (cp->mb_size) {
        bpm = 1;  // 8x8 MB = just 1 block (Y only or mono)
    } else {
        switch (cp->chroma_mode) {
        case 0: bpm = 6; break;  // 4:2:0 = 4Y + Cb + Cr
        case 1: bpm = 8; break;  // 4:2:2 = 4Y + 2Cb + 2Cr
        case 2: bpm = 6; break;  // 4:1:1 = 4Y + Cb + Cr (different subsampling)
        case 3: bpm = 4; break;  // mono = 4Y only
        default: bpm = 6; break;
        }
    }
    int max_blocks = mw * mh * bpm;
    if (max_blocks > PD_NBLOCKS) max_blocks = PD_NBLOCKS;

    memset(coeff, 0, sizeof(int) * PD_NBLOCKS * 64);

    // Build 8×8 quant matrix from 4×4 qtable
    int qm[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            qm[i * 8 + j] = qtable[(i / 2) * 4 + (j / 2)];

    int inits[3] = {init_y, init_cb, init_cr};
    int dc_pred[3] = {init_y, init_cb, init_cr};

    // Plane-interleaved: all Y blocks first, then Cb, then Cr
    // Total blocks: Y=mw*mh*4, Cb=mw*mh, Cr=mw*mh (for 4:2:0)
    int total_y  = mw * mh * 4;
    int total_cb = mw * mh;

    for (int mb = 0; mb < mw * mh && bs->pos < bs->total_bits; mb++) {
        for (int bl = 0; bl < bpm && bs->pos < bs->total_bits; bl++) {
            if (nblocks >= max_blocks) return nblocks;

            int comp;
            if (cp->interleave == 1) {
                // Plane mode: first total_y=Y, then total_cb=Cb, then Cr
                if (nblocks < total_y) comp = 0;
                else if (nblocks < total_y + total_cb) comp = 1;
                else comp = 2;
            } else if (cp->interleave == 2) {
                comp = 0; // Y-only
            } else {
                // MB-interleaved: blocks 0-3=Y, 4=Cb, 5=Cr
                comp = (bl < 4) ? 0 : (bl < 5) ? 1 : 2;
            }

            // DC
            int diff = pd_read_vlc(bs);
            if (diff == -9999) return nblocks;
            int dc_val;
            if (cp->dc_mode == 0) {
                // Mode 0: init+diff (no accumulation)
                // dc_diff_mult is from new param: 0=raw diff, 1=diff*qscale, 2=diff*qtable[0]
                int dscaled = diff;
                if (cp->dc_diff_mult == 1) dscaled = diff * qscale;
                else if (cp->dc_diff_mult == 2) dscaled = diff * qtable[0];
                dc_val = inits[comp] + dscaled;
            } else {
                // Mode 1: DPCM accumulate
                dc_val = dc_pred[comp] + diff;
                dc_pred[comp] = dc_val;
            }
            coeff[nblocks][0] = dc_val * cp->dc_scale;
            // DEBUG: dump first frame's first 8 blocks raw VLC + final coeffs

            // AC coefficients (skip if dc_only mode)
            if (cp->dc_only) {
                // DC-only: skip AC, just read them to advance bitstream
                // but don't store them
                if (cp->use_eob) {
                    for (int k = 1; k < 64 && bs->pos < bs->total_bits; k++) {
                        int val = pd_read_vlc(bs);
                        if (val == 0) break;
                    }
                } else {
                    for (int k = 1; k <= cp->ac_count && k < 64 && bs->pos < bs->total_bits; k++)
                        pd_read_vlc(bs);
                }
            } else if (cp->use_eob) {
                // EOB mode: VLC 0 = end of block
                for (int k = 1; k < 64 && bs->pos < bs->total_bits; k++) {
                    int val = pd_read_vlc(bs);
                    if (val == 0) break;
                    if (cp->ac_dequant == 1)
                        val = val * qm[pd_zigzag[k]] * qscale / 8;
                    else if (cp->ac_dequant == 2)
                        val = val * qm[pd_zigzag[k]] * qscale / 16;
                    else if (cp->ac_dequant == 3)
                        val = val * qscale;
                    else if (cp->ac_dequant == 4)
                        val = val * qm[pd_zigzag[k]];
                    else if (cp->ac_dequant == 5)
                        val = val * qm[pd_zigzag[k]] / 2;
                    coeff[nblocks][pd_zigzag[k]] = val;
                }
            } else {
                // Fixed count mode
                for (int k = 1; k <= cp->ac_count && k < 64 && bs->pos < bs->total_bits; k++) {
                    int val = pd_read_vlc(bs);
                    if (cp->ac_dequant == 1)
                        val = val * qm[pd_zigzag[k]] * qscale / 8;
                    else if (cp->ac_dequant == 2)
                        val = val * qm[pd_zigzag[k]] * qscale / 16;
                    else if (cp->ac_dequant == 3)
                        val = val * qscale;
                    else if (cp->ac_dequant == 4)
                        val = val * qm[pd_zigzag[k]];
                    else if (cp->ac_dequant == 5)
                        val = val * qm[pd_zigzag[k]] / 2;
                    coeff[nblocks][pd_zigzag[k]] = val;
                }
            }

            nblocks++;
        }
    }

    return nblocks;
}

static void playdia_decode_video_frame(AK8000 *v) {
    if (v->vid_frame_pos < 40) return;

    const uint8_t *f = v->vid_frame_buf;
    const CodecParams *cp = &v->codec_params;

    // Select zigzag table
    switch (cp->zigzag_alt) {
    case 1:  pd_zigzag = pd_zigzag_alt; break;
    case 2:  pd_zigzag = pd_zigzag_raster; break;
    default: pd_zigzag = pd_zigzag_standard; break;
    }

    // Validate header
    if (f[0] != 0x00 || f[1] != 0x80 || f[2] != 0x04) return;

    uint8_t qscale = f[3];
    memcpy(v->qtable, f + 4, 16);
    v->qscale = qscale;

    // Dump qtable once
    static bool qtable_dumped = false;
    if (!qtable_dumped) {
        printf("[VID] QTable 4x4:");
        for (int i = 0; i < 16; i++) printf(" %d", v->qtable[i]);
        printf("\n[VID] Bytes 20-39:");
        for (int i = 20; i < 40; i++) printf(" %02X", f[i]);
        printf("\n");
        qtable_dumped = true;
    }

    // Find actual data end (strip 0xFF padding)
    int data_end = v->vid_frame_pos;
    while (data_end > 40 && f[data_end - 1] == 0xFF) data_end--;
    int total_bits = (data_end - 40) * 8;

    // Bytes 40-42: DC predictor init values (Y, Cb, Cr)
    int dc_init_y  = (int)f[40];
    int dc_init_cb = (int)f[41];
    int dc_init_cr = (int)f[42];
    printf("[VID] DC init: Y=%d Cb=%d Cr=%d  byte43=%02X  QS=%d\n",
           dc_init_y, dc_init_cb, dc_init_cr, f[43], qscale);

    // Bitstream starts at configurable offset
    int bso = cp->bs_offset;
    if (bso >= data_end) return;
    int bs_total = (data_end - bso) * 8;
    pd_bitstream bs = { f + bso, bs_total, 0 };

    // Use tunable dimensions
    int pw = cp->width;
    int ph = cp->height;
    int pmw = pw / 16;
    int pmh = ph / 16;
    if (pmw < 1) pmw = 1;
    if (pmh < 1) pmh = 1;

    // Coefficient storage for current frame
    static int frame_coeff[PD_NBLOCKS][64];

    // Dynamic plane buffers (stack-allocated, max 320x240)
    uint8_t Y[240][320];
    uint8_t Cb[120][160];
    uint8_t Cr[120][160];

    int frames_decoded = 0;

    while (bs.pos < total_bits - 64) {
        int nblocks = pd_decode_one_frame(&bs, frame_coeff,
                                          qscale, v->qtable,
                                          dc_init_y, dc_init_cb, dc_init_cr,
                                          cp);
        if (nblocks < 6) break;

        // IDCT + render to Y/Cb/Cr planes
        memset(Y, 128 + cp->level_shift, sizeof(Y));
        memset(Cb, 128, sizeof(Cb));
        memset(Cr, 128, sizeof(Cr));

        // Build macroblock scan order lookup table
        int nmb = pmw * pmh;
        int mb_scan_x[256], mb_scan_y[256]; // max 16x16 MBs
        for (int m = 0; m < nmb && m < 256; m++) {
            int sx, sy;
            switch (cp->scan_order) {
            default:
            case 0: // row-major (standard)
                sx = m % pmw; sy = m / pmw; break;
            case 1: // column-major
                sx = m / pmh; sy = m % pmh; break;
            case 2: { // zigzag (alternating row direction)
                sy = m / pmw; sx = m % pmw;
                if (sy & 1) sx = pmw - 1 - sx; // reverse odd rows
                break;
            }
            case 3: { // boustrophedon (snake)
                sy = m / pmw; sx = m % pmw;
                if (sy & 1) sx = pmw - 1 - sx;
                break;
            }
            case 4: { // interleaved: even MBs first, then odd
                int half = nmb / 2;
                if (m < half) { int mm = m * 2; sx = mm % pmw; sy = mm / pmw; }
                else { int mm = (m - half) * 2 + 1; sx = mm % pmw; sy = mm / pmw; }
                break;
            }
            case 5: // reverse row-major
                sx = (nmb - 1 - m) % pmw; sy = (nmb - 1 - m) / pmw; break;
            case 6: { // row-major but starting from bottom
                int rm = (pmh - 1 - m / pmw) * pmw + m % pmw;
                sx = rm % pmw; sy = rm / pmw; break;
            }
            }
            mb_scan_x[m] = sx;
            mb_scan_y[m] = sy;
        }

        // Y block order within macroblock (4 blocks)
        // Each entry: (dx, dy) offset in 8-pixel units within 16x16 MB
        static const int blk_orders[4][4][2] = {
            {{0,0},{1,0},{0,1},{1,1}},  // 0: Z-pattern (standard MPEG)
            {{0,0},{0,1},{1,0},{1,1}},  // 1: N-pattern (column-first)
            {{0,0},{1,0},{1,1},{0,1}},  // 2: U-pattern
            {{0,0},{0,1},{1,1},{1,0}},  // 3: reverse-Z
        };
        int bord = cp->block_order;
        if (bord < 0 || bord > 3) bord = 0;

        int mbpx = cp->mb_size ? 8 : 16;
        int bpm;
        if (cp->mb_size) { bpm = 1; }
        else {
            switch (cp->chroma_mode) {
            case 0: bpm = 6; break; case 1: bpm = 8; break;
            case 2: bpm = 6; break; case 3: bpm = 4; break;
            default: bpm = 6; break;
            }
        }

        // Plane-interleaved block counts
        int plane_total_y  = pmw * pmh * 4;
        int plane_total_cb = pmw * pmh;
        int yw = pw / 8, yh = ph / 8;  // Y blocks grid for plane mode
        int cw = pw / 16, ch = ph / 16; // chroma blocks grid

        for (int i = 0; i < nblocks; i++) {
            uint8_t block[8][8];
            pd_idct_block(frame_coeff[i], block);

            if (cp->level_shift != 0) {
                for (int r = 0; r < 8; r++)
                    for (int c = 0; c < 8; c++)
                        block[r][c] = (uint8_t)pd_clamp((int)block[r][c] + cp->level_shift);
            }

            if (cp->interleave == 1) {
                // Plane mode: blocks 0..total_y-1 = Y raster, then Cb, then Cr
                if (i < plane_total_y) {
                    int bi = i;
                    int bx = (bi % yw) * 8;
                    int by = (bi / yw) * 8;
                    for (int r = 0; r < 8; r++)
                        for (int c = 0; c < 8; c++)
                            if (by + r < ph && bx + c < pw)
                                Y[by + r][bx + c] = block[r][c];
                } else if (i < plane_total_y + plane_total_cb) {
                    int bi = i - plane_total_y;
                    int bx = (bi % cw) * 8;
                    int by = (bi / cw) * 8;
                    for (int r = 0; r < 8; r++)
                        for (int c = 0; c < 8; c++)
                            if (by + r < ph / 2 && bx + c < pw / 2)
                                Cb[by + r][bx + c] = block[r][c];
                } else {
                    int bi = i - plane_total_y - plane_total_cb;
                    int bx = (bi % cw) * 8;
                    int by = (bi / cw) * 8;
                    for (int r = 0; r < 8; r++)
                        for (int c = 0; c < 8; c++)
                            if (by + r < ph / 2 && bx + c < pw / 2)
                                Cr[by + r][bx + c] = block[r][c];
                }
            } else if (cp->interleave == 2) {
                // Y-only raster: all blocks are Y
                int bx = (i % yw) * 8;
                int by = (i / yw) * 8;
                for (int r = 0; r < 8; r++)
                    for (int c = 0; c < 8; c++)
                        if (by + r < ph && bx + c < pw)
                            Y[by + r][bx + c] = block[r][c];
            } else {
                // MB-interleaved (standard)
                int mb = i / bpm, bl = i % bpm;
                if (mb >= nmb || mb >= 256) break;
                int mx = mb_scan_x[mb], my = mb_scan_y[mb];

                if (cp->mb_size) {
                    int bx = mx * 8, by = my * 8;
                    for (int r = 0; r < 8; r++)
                        for (int c = 0; c < 8; c++)
                            if (by + r < ph && bx + c < pw)
                                Y[by + r][bx + c] = block[r][c];
                } else if (bl < 4) {
                    int bx = mx * 16 + blk_orders[bord][bl][0] * 8;
                    int by = my * 16 + blk_orders[bord][bl][1] * 8;
                    for (int r = 0; r < 8; r++)
                        for (int c = 0; c < 8; c++)
                            if (by + r < ph && bx + c < pw)
                                Y[by + r][bx + c] = block[r][c];
                } else if (bl == 4) {
                    for (int r = 0; r < 8; r++)
                        for (int c = 0; c < 8; c++)
                            if (my * 8 + r < ph / 2 && mx * 8 + c < pw / 2)
                                Cb[my * 8 + r][mx * 8 + c] = block[r][c];
                } else {
                    for (int r = 0; r < 8; r++)
                        for (int c = 0; c < 8; c++)
                            if (my * 8 + r < ph / 2 && mx * 8 + c < pw / 2)
                                Cr[my * 8 + r][mx * 8 + c] = block[r][c];
                }
            }
        }

        // Debug: save Y/Cb/Cr planes for first frame per packet
        static int y_save_count = 0;
        if (frames_decoded == 0 && y_save_count < 3) {
            int sn = y_save_count;
            char _yname[64]; snprintf(_yname, sizeof _yname, "/tmp/pd_y_%02d.pgm", sn);
            FILE *yf = fopen(_yname, "wb");
            if (yf) {
                fprintf(yf, "P5\n%d %d\n255\n", pw, ph);
                for (int yy = 0; yy < ph; yy++)
                    fwrite(Y[yy], 1, pw, yf);
                fclose(yf);
            }
            // Save Cb and Cr planes too
            char _cbname[64]; snprintf(_cbname, sizeof _cbname, "/tmp/pd_cb_%02d.pgm", sn);
            FILE *cbf = fopen(_cbname, "wb");
            if (cbf) {
                fprintf(cbf, "P5\n%d %d\n255\n", pw/2, ph/2);
                for (int yy = 0; yy < ph/2; yy++)
                    fwrite(Cb[yy], 1, pw/2, cbf);
                fclose(cbf);
            }
            char _crname[64]; snprintf(_crname, sizeof _crname, "/tmp/pd_cr_%02d.pgm", sn);
            FILE *crf = fopen(_crname, "wb");
            if (crf) {
                fprintf(crf, "P5\n%d %d\n255\n", pw/2, ph/2);
                for (int yy = 0; yy < ph/2; yy++)
                    fwrite(Cr[yy], 1, pw/2, crf);
                fclose(crf);
            }
        }
        if (frames_decoded == 0) y_save_count++;

        // YCbCr 4:2:0 → RGB888 into frame queue (centered in 320×240)
        if (v->fq_count < PD_FRAME_QUEUE_SIZE) {
            uint8_t *dst_buf = v->frame_queue[v->fq_write];
            int ox = (SCREEN_W - pw) / 2;
            int oy = (SCREEN_H - ph) / 2;
            memset(dst_buf, 0, SCREEN_W * SCREEN_H * 3);

            for (int y = 0; y < ph; y++) {
                for (int x = 0; x < pw; x++) {
                    int yv = Y[y][x];
                    int cb = Cb[y / 2][x / 2] - 128;
                    int cr = Cr[y / 2][x / 2] - 128;
                    int dst = ((y + oy) * SCREEN_W + (x + ox)) * 3;
                    if (y + oy >= 0 && y + oy < SCREEN_H && x + ox >= 0 && x + ox < SCREEN_W) {
                        dst_buf[dst + 0] = (uint8_t)pd_clamp(yv + (int)(1.402 * cr));
                        dst_buf[dst + 1] = (uint8_t)pd_clamp(yv - (int)(0.344 * cb + 0.714 * cr));
                        dst_buf[dst + 2] = (uint8_t)pd_clamp(yv + (int)(1.772 * cb));
                    }
                }
            }

            // Grid overlay
            if (cp->grid_overlay) {
                for (int y = 0; y < ph; y++) {
                    for (int x = 0; x < pw; x++) {
                        bool draw = false;
                        if ((cp->grid_overlay & 1) && (x % 8 == 0 || y % 8 == 0))
                            draw = true;  // 8x8 block grid
                        if ((cp->grid_overlay & 2) && (x % 16 == 0 || y % 16 == 0))
                            draw = true;  // MB grid
                        if (draw) {
                            int di = ((y + oy) * SCREEN_W + (x + ox)) * 3;
                            if (y + oy >= 0 && y + oy < SCREEN_H && x + ox >= 0 && x + ox < SCREEN_W) {
                                // Red for MB, green for block
                                bool mb_line = (x % 16 == 0 || y % 16 == 0);
                                dst_buf[di + 0] = mb_line ? 255 : 0;
                                dst_buf[di + 1] = mb_line ? 0 : 255;
                                dst_buf[di + 2] = 0;
                            }
                        }
                    }
                }
            }

            // Bitstream stats overlay (bottom of image)
            if (frames_decoded == 0) {
                int pct = (bs.pos * 100) / (bs_total > 0 ? bs_total : 1);
                printf("[VID] Bitstream: %d/%d bits used (%d%%), %d blocks decoded\n",
                       bs.pos, bs_total, pct, nblocks);
            }

            v->fq_write = (v->fq_write + 1) % PD_FRAME_QUEUE_SIZE;
            v->fq_count++;
        }

        v->frame_count++;
        frames_decoded++;
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
//  F2 Interactive Command Parser
//
//  F2 commands with submode=0x09 control interactive gameplay.
//  MSF destinations use binary encoding (NOT BCD):
//    abs_frame = M*4500 + S*75 + F
//    target_lba = abs_frame - 150  (LBA 0 = frame 150)
// ─────────────────────────────────────────────────────────────
static uint32_t f2_msf_to_lba(uint8_t m, uint8_t s, uint8_t f) {
    uint32_t abs_frame = (uint32_t)m * 4500 + (uint32_t)s * 75 + (uint32_t)f;
    if (abs_frame < 150) return 0;
    return abs_frame - 150;
}

static void ak8000_parse_f2_command(AK8000 *v, const uint8_t *payload,
                                     uint32_t current_lba) {
    uint8_t cmd_type = payload[1];
    v->interactive_cmd = cmd_type;
    v->cmd_lba = current_lba;

    // Parse 7 button destinations: payload[3..30] = 7 × (M S F extra)
    for (int i = 0; i < 7; i++) {
        int off = 3 + i * 4;
        uint8_t m = payload[off + 0];
        uint8_t s = payload[off + 1];
        uint8_t f = payload[off + 2];
        v->button_dest[i]  = f2_msf_to_lba(m, s, f);
        v->button_extra[i] = payload[off + 3];
    }

    switch (cmd_type) {
    case 0x40:
        // Unconditional jump — all 7 slots have same destination
        // Detect backward jumps (loops): dest <= current position
        v->is_loop = (v->button_dest[0] <= current_lba);
        v->seek_target = v->button_dest[0];
        v->interactive_pending = true;
        v->waiting_for_input = false; // loops and forward jumps both auto-seek
        if (v->is_loop) {
            printf("[AK8000] F2 40 LOOP @ LBA %u (dest=%u)\n",
                   current_lba, v->button_dest[0]);
        } else {
            printf("[AK8000] F2 40 JUMP @ LBA %u → LBA %u\n",
                   current_lba, v->seek_target);
        }
        break;

    case 0x44:
        // Player choice — 7 different destinations per button
        v->interactive_pending = true;
        v->waiting_for_input = true;
        v->input_timer = 300;  // ~10 seconds at 30fps default timeout
        printf("[AK8000] F2 44 CHOICE: ");
        for (int i = 0; i < 7; i++)
            printf("B%d=%u ", i + 1, v->button_dest[i]);
        printf("\n");
        break;

    case 0x50:
        // Quiz answer verification — all destinations same, extra byte = correct flag
        v->interactive_pending = true;
        v->waiting_for_input = true;
        v->input_timer = 300;
        printf("[AK8000] F2 50 QUIZ → LBA %u (", v->button_dest[0]);
        for (int i = 0; i < 6; i++)
            printf("%s%s", v->button_extra[i] ? "correct" : "wrong",
                   i < 5 ? "," : "");
        printf(")\n");
        break;

    case 0x60:
        // Timed jump / animation control
        v->seek_target = v->button_dest[0];
        v->interactive_pending = true;
        v->waiting_for_input = false;
        printf("[AK8000] F2 60 TIMED → LBA %u (flags=%02X)\n",
               v->seek_target, payload[2]);
        break;

    case 0x80:
        // Timeout handler — sets timeout destination for preceding F2 44
        v->timeout_sub = payload[2];
        if (payload[3] != 0 || payload[4] != 0 || payload[5] != 0) {
            v->timeout_dest = f2_msf_to_lba(payload[3], payload[4], payload[5]);
        } else {
            v->timeout_dest = 0; // no timeout
        }
        // F2 80 is a modifier, not a navigation command — don't set pending
        printf("[AK8000] F2 80 TIMEOUT: dest=%u sub=%02X\n",
               v->timeout_dest, v->timeout_sub);
        break;

    case 0x90:
        // Score/result display — auto-advance
        v->seek_target = v->button_dest[0];
        v->interactive_pending = true;
        v->waiting_for_input = false;
        printf("[AK8000] F2 90 RESULT → LBA %u\n", v->seek_target);
        break;

    case 0xA0:
        // Loop/animation control
        // Flag F0 = boot/reset marker — ignore (part of intro boot sequence)
        if (payload[2] == 0xF0) {
            printf("[AK8000] F2 A0 BOOT (F0) — skipping\n");
            break;
        }
        // Other flags: treat as auto-advance
        v->is_loop = (v->button_dest[0] <= current_lba);
        if (v->is_loop) {
            // Backward A0 is a scene loop — same as F2 40 loop
            v->seek_target = v->button_dest[0];
            v->interactive_pending = true;
            v->waiting_for_input = false;
            printf("[AK8000] F2 A0 LOOP @ LBA %u (dest=%u, flags=%02X)\n",
                   current_lba, v->button_dest[0], payload[2]);
        } else {
            v->seek_target = v->button_dest[0];
            v->interactive_pending = true;
            v->waiting_for_input = false;
            printf("[AK8000] F2 A0 JUMP → LBA %u (flags=%02X)\n",
                   v->seek_target, payload[2]);
        }
        break;

    default:
        printf("[AK8000] F2 %02X UNKNOWN\n", cmd_type);
        break;
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

    // Video/data sector: channel 0, submode has Data bit (0x08) set
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
            // Check submode for interactive command (0x09 = Data+EOR)
            // vs regular frame-end marker (0x08 = Data only)
            if ((submode & 0x01) && payload[1] != 0x00) {
                // ── Interactive F2 command (submode 0x09) ─────────
                // Extract current LBA from sector MSF header (BCD at bytes 12-14)
                uint8_t mm = raw_sector[12], ss = raw_sector[13], ff = raw_sector[14];
                uint32_t cur_lba = ((mm/16)*10 + (mm%16)) * 4500 +
                                   ((ss/16)*10 + (ss%16)) * 75 +
                                   ((ff/16)*10 + (ff%16));
                if (cur_lba >= 150) cur_lba -= 150;
                ak8000_parse_f2_command(v, payload, cur_lba);
            } else {
                // ── Regular frame-end marker (submode 0x08) ──────
                if (v->vid_frame_pos >= 6 * 2047 &&
                    v->vid_frame_buf[0] == 0x00 &&
                    v->vid_frame_buf[1] == 0x80 &&
                    v->vid_frame_buf[2] == 0x04) {
                    // Use all accumulated F1 data (6-13 sectors typical)
                    playdia_decode_video_frame(v);
                }
                v->vid_frame_pos = 0;
            }
        } else if (marker == 0xF3) {
            // Scene marker — reset frame accumulator
            v->vid_frame_pos = 0;
        }
    }
}
