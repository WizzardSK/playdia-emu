#include "sdl_frontend.h"
#include "playdia_sys.h"
#include "ak8000.h"
#include <stdio.h>
#include <string.h>

// ─────────────────────────────────────────────────────────────
//  Audio callback — SDL pulls samples from our ring buffer
// ─────────────────────────────────────────────────────────────
static void audio_callback(void *userdata, uint8_t *stream, int len) {
    SDLFrontend *fe = (SDLFrontend *)userdata;
    int16_t *out = (int16_t *)stream;
    int n_out = len / (int)sizeof(int16_t);
    int ring_size = SDL_AUDIO_BUF_SAMPLES * CHANNELS;

    for (int i = 0; i < n_out; i++) {
        int rd = fe->audio_read;
        int wr = fe->audio_write;
        int avail = (wr - rd + ring_size) % ring_size;
        if (avail > 0) {
            out[i] = fe->audio_ring[rd];
            fe->audio_read = (rd + 1) % ring_size;
        } else {
            out[i] = 0; // underrun — silence
        }
    }
}

// ─────────────────────────────────────────────────────────────
int sdl_init(SDLFrontend *fe, const char *title) {
    memset(fe, 0, sizeof *fe);
    fe->running = true;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[SDL] Init failed: %s\n", SDL_GetError());
        return -1;
    }

    // ── Window & renderer ──────────────────────────────────
    fe->window = SDL_CreateWindow(
        title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SDL_WIN_W, SDL_WIN_H,
        SDL_WINDOW_SHOWN);
    if (!fe->window) {
        fprintf(stderr, "[SDL] Window: %s\n", SDL_GetError());
        return -1;
    }

    fe->renderer = SDL_CreateRenderer(fe->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!fe->renderer) {
        // fallback to software
        fe->renderer = SDL_CreateRenderer(fe->window, -1,
            SDL_RENDERER_SOFTWARE);
    }
    if (!fe->renderer) {
        fprintf(stderr, "[SDL] Renderer: %s\n", SDL_GetError());
        return -1;
    }

    SDL_RenderSetLogicalSize(fe->renderer, SCREEN_W, SCREEN_H);
    SDL_SetRenderDrawColor(fe->renderer, 0, 0, 0, 255);
    SDL_RenderClear(fe->renderer);
    SDL_RenderPresent(fe->renderer);

    // ── Texture for framebuffer ───────────────────────────
    fe->texture = SDL_CreateTexture(
        fe->renderer,
        SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING,
        SCREEN_W, SCREEN_H);
    if (!fe->texture) {
        fprintf(stderr, "[SDL] Texture: %s\n", SDL_GetError());
        return -1;
    }

    // ── Audio device ───────────────────────────────────────
    SDL_AudioSpec want = {0}, have = {0};
    want.freq     = SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = CHANNELS;
    want.samples  = SAMPLES_PER_FRAME;   // ~1470 samples/frame @ 44100/30
    want.callback = audio_callback;
    want.userdata = fe;

    fe->audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (fe->audio_dev == 0) {
        fprintf(stderr, "[SDL] Audio: %s (continuing without audio)\n",
                SDL_GetError());
    } else {
        printf("[SDL] Audio: %dHz %dch, buffer=%d samples\n",
               have.freq, have.channels, have.samples);
        SDL_PauseAudioDevice(fe->audio_dev, 0); // start playing
    }

    printf("[SDL] Window: %dx%d (scale %dx)\n", SDL_WIN_W, SDL_WIN_H, SDL_SCALE);
    return 0;
}

// ─────────────────────────────────────────────────────────────
void sdl_shutdown(SDLFrontend *fe) {
    if (fe->audio_dev)  SDL_CloseAudioDevice(fe->audio_dev);
    if (fe->texture)    SDL_DestroyTexture(fe->texture);
    if (fe->renderer)   SDL_DestroyRenderer(fe->renderer);
    if (fe->window)     SDL_DestroyWindow(fe->window);
    SDL_Quit();
}

// ─────────────────────────────────────────────────────────────
bool sdl_present_frame(SDLFrontend *fe, const uint8_t *rgb888, int w, int h) {
    if (!fe->running) return false;
    (void)h;

    // Upload the whole 320×240 framebuffer into the texture.
    SDL_UpdateTexture(fe->texture, NULL, rgb888, w * 3);

    SDL_RenderClear(fe->renderer);

    // Stretch the centered 192×144 video region to fill the entire
    // window, no black borders.  Source rect picks just the active
    // video area inside the 320×240 framebuffer.
    int vid_w = 192;
    int vid_h = 144;
    int ox    = (SCREEN_W - vid_w) / 2;
    int oy    = (SCREEN_H - vid_h) / 2;
    SDL_Rect src = { ox, oy, vid_w, vid_h };
    SDL_RenderCopy(fe->renderer, fe->texture, &src, NULL);

    SDL_RenderPresent(fe->renderer);

    fe->frame_count++;
    return true;
}

// ─────────────────────────────────────────────────────────────
void sdl_queue_audio(SDLFrontend *fe, const int16_t *samples, int n_samples) {
    if (fe->audio_dev == 0) return;

    int ring_size = SDL_AUDIO_BUF_SAMPLES * CHANNELS;
    // n_samples is sample-pairs; we write n_samples * CHANNELS int16_t values
    int total = n_samples * CHANNELS;

    SDL_LockAudioDevice(fe->audio_dev);
    for (int i = 0; i < total; i++) {
        int next = (fe->audio_write + 1) % ring_size;
        if (next == fe->audio_read) break; // overflow — drop
        fe->audio_ring[fe->audio_write] = samples[i];
        fe->audio_write = next;
    }
    SDL_UnlockAudioDevice(fe->audio_dev);
}

// ─────────────────────────────────────────────────────────────
uint8_t sdl_key_to_btn(SDL_Keycode k) {
    switch (k) {
        case SDLK_LEFT:   return BTN_LEFT;
        case SDLK_RIGHT:  return BTN_RIGHT;
        case SDLK_UP:     return BTN_UP;
        case SDLK_DOWN:   return BTN_DOWN;
        case SDLK_z:      return BTN_A;
        case SDLK_x:      return BTN_B;
        case SDLK_RETURN: return BTN_START;
        case SDLK_SPACE:  return BTN_SELECT;
        default:          return 0;
    }
}

// ─────────────────────────────────────────────────────────────
//  Helper: refresh window title with the currently-selected codec
//  parameter and its value, so live tuning is visible in the GUI.
// ─────────────────────────────────────────────────────────────
static const char *cp_param_name(int idx) {
    static const char *names[CODEC_PARAM_COUNT] = {
        "ac_count","dc_mode","dc_scale","bs_offset",
        "width","height","level_shift","use_eob","ac_dequant",
        "scan_order","block_order",
        "dc_only","grid","chroma","zigzag","mb_size","interleave",
        "vlc_invert","dc_diff_mult"
    };
    if (idx < 0 || idx >= CODEC_PARAM_COUNT) return "?";
    return names[idx];
}
static int cp_param_value(const CodecParams *cp, int idx) {
    switch (idx) {
    case 0:  return cp->ac_count;
    case 1:  return cp->dc_mode;
    case 2:  return cp->dc_scale;
    case 3:  return cp->bs_offset;
    case 4:  return cp->width;
    case 5:  return cp->height;
    case 6:  return cp->level_shift;
    case 7:  return cp->use_eob ? 1 : 0;
    case 8:  return cp->ac_dequant;
    case 9:  return cp->scan_order;
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
static void sdl_refresh_title(SDLFrontend *fe, const CodecParams *cp) {
    if (!fe || !fe->window || !cp) return;
    char buf[160];
    snprintf(buf, sizeof buf,
             "Playdia — [%d/%d] %s=%d  (Tab=next  +/-=adj  P=print  R=reset)",
             cp->selected + 1, CODEC_PARAM_COUNT,
             cp_param_name(cp->selected),
             cp_param_value(cp, cp->selected));
    /* SDL_SetWindowTitle sets WM_NAME but on some XWayland setups
     * the WM continues to display the old _NET_WM_NAME instead.
     * The same status is always printed to stdout so terminal users
     * see live updates regardless. */
    SDL_SetWindowTitle(fe->window, buf);
}

// ─────────────────────────────────────────────────────────────
//  Codec tuning key handler
//  Tab/Shift+Tab = select param, +/- = adjust, P = print, R = reset
// ─────────────────────────────────────────────────────────────
bool sdl_handle_codec_key_fe(SDLFrontend *fe, SDL_Keycode k, CodecParams *cp);
bool sdl_handle_codec_key(SDL_Keycode k, CodecParams *cp) {
    return sdl_handle_codec_key_fe(NULL, k, cp);
}
bool sdl_handle_codec_key_fe(SDLFrontend *fe, SDL_Keycode k, CodecParams *cp) {
    if (!cp) return false;
    bool consumed = false;
    switch (k) {
    case SDLK_TAB: {
        SDL_Keymod mod = SDL_GetModState();
        if (mod & KMOD_SHIFT) codec_params_prev(cp);
        else                  codec_params_next(cp);
        consumed = true; break;
    }
    case SDLK_EQUALS:  // + key (=/+)
    case SDLK_KP_PLUS:
        codec_params_adjust(cp, 1);
        consumed = true; break;
    case SDLK_MINUS:
    case SDLK_KP_MINUS:
        codec_params_adjust(cp, -1);
        consumed = true; break;
    case SDLK_RIGHTBRACKET:
        codec_params_adjust(cp, 5);
        consumed = true; break;
    case SDLK_LEFTBRACKET:
        codec_params_adjust(cp, -5);
        consumed = true; break;
    case SDLK_p:
        codec_params_print(cp);
        consumed = true; break;
    case SDLK_r:
        codec_params_init(cp);
        printf("[CODEC] Reset to defaults\n");
        codec_params_print(cp);
        consumed = true; break;
    case SDLK_s:
        cp->save_frame = true;
        printf("[CODEC] Frame save requested\n");
        consumed = true; break;
    case SDLK_t:
        cp->autotune = !cp->autotune;
        if (cp->autotune) {
            cp->tune_param = 0;
            cp->tune_step = 0;
            cp->tune_wait = 3;
            cp->best_score = 0;
            cp->stale_count = 0;
            printf("[TUNE] Auto-tune STARTED\n");
        } else {
            printf("[TUNE] Auto-tune STOPPED\n");
        }
        codec_params_print(cp);
        consumed = true; break;
    default:
        break;
    }
    if (consumed) sdl_refresh_title(fe, cp);
    return consumed;
}

// ─────────────────────────────────────────────────────────────
bool sdl_poll_events_ex(SDLFrontend *fe, uint8_t *controller, CodecParams *cp) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            fe->running = false;
            return false;

        case SDL_KEYDOWN:
            if (e.key.keysym.sym == SDLK_ESCAPE) {
                fe->running = false;
                return false;
            }
            if (e.key.keysym.sym == SDLK_F1) {
                SDL_SetWindowFullscreen(fe->window,
                    (SDL_GetWindowFlags(fe->window) & SDL_WINDOW_FULLSCREEN_DESKTOP)
                    ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
            // Try codec keys first (refreshes window title)
            if (sdl_handle_codec_key_fe(fe, e.key.keysym.sym, cp))
                break;
            *controller |= sdl_key_to_btn(e.key.keysym.sym);
            break;

        case SDL_KEYUP:
            *controller &= ~sdl_key_to_btn(e.key.keysym.sym);
            break;

        default: break;
        }
    }
    return fe->running;
}

bool sdl_poll_events(SDLFrontend *fe, uint8_t *controller) {
    return sdl_poll_events_ex(fe, controller, NULL);
}
