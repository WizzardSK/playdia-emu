#pragma once
#include "playdia.h"
#include "ak8000.h"
#include <SDL2/SDL.h>
#include <stdbool.h>

// ─────────────────────────────────────────────────────────────
//  SDL2 Frontend for Playdia Emulator
//  - 320×240 window (×2 scale = 640×480)
//  - 44100Hz stereo audio callback
//  - Keyboard → controller mapping
// ─────────────────────────────────────────────────────────────

#define SDL_SCALE   2                        // window scale factor
#define SDL_WIN_W   (SCREEN_W * SDL_SCALE)   // 640
#define SDL_WIN_H   (SCREEN_H * SDL_SCALE)   // 480

// Audio buffer: hold 4 frames worth to allow for jitter
#define SDL_AUDIO_BUF_FRAMES  4
#define SDL_AUDIO_BUF_SAMPLES (SAMPLES_PER_FRAME * SDL_AUDIO_BUF_FRAMES)
#define SDL_AUDIO_BUF_BYTES   (SDL_AUDIO_BUF_SAMPLES * CHANNELS * sizeof(int16_t))

typedef struct SDLFrontend {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;   // SCREEN_W × SCREEN_H, SDL_PIXELFORMAT_RGB24

    SDL_AudioDeviceID audio_dev;

    // Ring buffer for audio (int16_t stereo)
    int16_t  audio_ring[SDL_AUDIO_BUF_SAMPLES * CHANNELS];
    volatile int audio_write;  // write head (samples, not bytes)
    volatile int audio_read;   // read head

    bool     running;
    uint64_t frame_count;
} SDLFrontend;

// ── API ────────────────────────────────────────────────────────
int  sdl_init     (SDLFrontend *fe, const char *title);
void sdl_shutdown (SDLFrontend *fe);

// Present one frame from AK8000 framebuffer; returns false if window closed
bool sdl_present_frame (SDLFrontend *fe, const uint8_t *rgb888, int w, int h);

// Queue audio samples produced by AK8000
void sdl_queue_audio   (SDLFrontend *fe, const int16_t *samples, int n_samples);

// Poll events, fill controller bitmask; returns false if quit
bool sdl_poll_events   (SDLFrontend *fe, uint8_t *controller);
// Extended: also handles codec tuning keys
bool sdl_poll_events_ex(SDLFrontend *fe, uint8_t *controller, CodecParams *cp);

// Keyboard → Playdia button map
// Arrow keys → D-pad, Z=A, X=B, Enter=Start, Space=Select
uint8_t sdl_key_to_btn (SDL_Keycode k);

// Codec tuning key handler — returns true if key was consumed
bool sdl_handle_codec_key(SDL_Keycode k, CodecParams *cp);
