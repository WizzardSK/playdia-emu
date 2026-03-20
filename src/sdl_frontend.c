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

    // Upload pixels — RGB24 pitch = w*3
    SDL_UpdateTexture(fe->texture, NULL, rgb888, w * 3);

    SDL_RenderClear(fe->renderer);
    SDL_RenderCopy(fe->renderer, fe->texture, NULL, NULL);

    // ── OSD: frame counter in top-left ────────────────────
    // (no font rendering — just present the frame)
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
//  Codec tuning key handler
//  Tab/Shift+Tab = select param, +/- = adjust, P = print, R = reset
// ─────────────────────────────────────────────────────────────
bool sdl_handle_codec_key(SDL_Keycode k, CodecParams *cp) {
    if (!cp) return false;
    switch (k) {
    case SDLK_TAB: {
        SDL_Keymod mod = SDL_GetModState();
        if (mod & KMOD_SHIFT)
            codec_params_prev(cp);
        else
            codec_params_next(cp);
        return true;
    }
    case SDLK_EQUALS:  // + key (=/+)
    case SDLK_KP_PLUS:
        codec_params_adjust(cp, 1);
        return true;
    case SDLK_MINUS:
    case SDLK_KP_MINUS:
        codec_params_adjust(cp, -1);
        return true;
    case SDLK_RIGHTBRACKET:
        codec_params_adjust(cp, 5);
        return true;
    case SDLK_LEFTBRACKET:
        codec_params_adjust(cp, -5);
        return true;
    case SDLK_p:
        codec_params_print(cp);
        return true;
    case SDLK_r:
        codec_params_init(cp);
        printf("[CODEC] Reset to defaults\n");
        codec_params_print(cp);
        return true;
    case SDLK_s:
        cp->save_frame = true;
        printf("[CODEC] Frame save requested\n");
        return true;
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
        return true;
    default:
        return false;
    }
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
            // Try codec keys first
            if (sdl_handle_codec_key(e.key.keysym.sym, cp))
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
