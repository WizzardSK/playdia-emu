// ─────────────────────────────────────────────────────────────
//  libretro front end for the Playdia emulator.
//
//  The emulator core is already frontend-agnostic: playdia_run_frame()
//  advances one 30 Hz frame, the decoded picture lands in the AK8000
//  framebuffer, audio comes out of the pipeline's ring buffer and input is a
//  single byte of button bits. This file is only the wiring.
// ─────────────────────────────────────────────────────────────

#include "libretro.h"
#include "playdia_sys.h"
#include "ak8000_pd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The picture is PD_PIC_W x PD_PIC_H, blitted into the middle of the 320x240
// framebuffer. Only that region is handed over, the way the SDL window shows it.
#define PD_OX ((SCREEN_W - PD_PIC_W) / 2)
#define PD_OY ((SCREEN_H - PD_PIC_H) / 2)

static retro_environment_t   environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t    input_poll_cb;
static retro_input_state_t   input_state_cb;
static retro_log_printf_t    log_cb;

static Playdia  *g_playdia;
static uint32_t *g_frame;   // PD_PIC_W * PD_PIC_H, XRGB8888

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
    (void)level; (void)fmt;
}

void retro_init(void)
{
    g_playdia = (Playdia *)calloc(1, sizeof(Playdia));
    g_frame   = (uint32_t *)calloc((size_t)PD_PIC_W * PD_PIC_H, sizeof(uint32_t));
}

void retro_deinit(void)
{
    free(g_playdia); g_playdia = NULL;
    free(g_frame);   g_frame   = NULL;
}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name     = "playdia-emu";
    info->library_version  = "0.1";
    info->valid_extensions = "cue|bin|iso|zip";
    info->need_fullpath    = true;   // the disc is opened by the emulator itself
    info->block_extract    = true;   // a Redump zip is read as it is
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    info->geometry.base_width   = PD_PIC_W;
    info->geometry.base_height  = PD_PIC_H;
    info->geometry.max_width    = PD_PIC_W;
    info->geometry.max_height   = PD_PIC_H;
    info->geometry.aspect_ratio = 4.0f / 3.0f;   // the console's output is 4:3
    info->timing.fps            = FPS;
    info->timing.sample_rate    = SAMPLE_RATE;
}

void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;

    bool no_game = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);

    struct retro_log_callback logging;
    log_cb = cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging) && logging.log
           ? logging.log : fallback_log;

    // B1 is the default/Start slot of an F2 choice, then Up, Down, Left,
    // Right, A, B - which is the order the disc's destination table uses.
    static const struct retro_input_descriptor desc[] = {
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
        { 0 },
    };
    cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)desc);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)   { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb)       { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb)     { input_state_cb = cb; }

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    (void)port; (void)device;
}

bool retro_load_game(const struct retro_game_info *game)
{
    if (!game || !game->path || !g_playdia || !g_frame)
        return false;

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
    {
        log_cb(RETRO_LOG_ERROR, "playdia-emu: XRGB8888 is required\n");
        return false;
    }

    playdia_init(g_playdia);

    if (playdia_load_disc(g_playdia, game->path) != 0)
    {
        log_cb(RETRO_LOG_ERROR, "playdia-emu: cannot load %s\n", game->path);
        return false;
    }

    log_cb(RETRO_LOG_INFO, "playdia-emu: loaded %s\n", game->path);
    return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
    (void)type; (void)info; (void)num;
    return false;
}

void retro_unload_game(void)
{
    if (g_playdia)
        ak8000_free(&g_playdia->video);
}

void retro_reset(void)
{
    if (g_playdia)
        playdia_reset(g_playdia);
}

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

static uint8_t read_buttons(void)
{
    static const struct { unsigned id; uint8_t bit; } map[] = {
        { RETRO_DEVICE_ID_JOYPAD_UP,     BTN_UP },
        { RETRO_DEVICE_ID_JOYPAD_DOWN,   BTN_DOWN },
        { RETRO_DEVICE_ID_JOYPAD_LEFT,   BTN_LEFT },
        { RETRO_DEVICE_ID_JOYPAD_RIGHT,  BTN_RIGHT },
        { RETRO_DEVICE_ID_JOYPAD_A,      BTN_A },
        { RETRO_DEVICE_ID_JOYPAD_B,      BTN_B },
        { RETRO_DEVICE_ID_JOYPAD_START,  BTN_START },
        { RETRO_DEVICE_ID_JOYPAD_SELECT, BTN_SELECT },
    };

    uint8_t buttons = 0;

    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, map[i].id))
            buttons |= map[i].bit;

    return buttons;
}

void retro_run(void)
{
    if (!g_playdia || !g_frame)
        return;

    input_poll_cb();
    g_playdia->controller = read_buttons();

    playdia_run_frame(g_playdia);

    // RGB888 out of the emulator, XRGB8888 into the frontend.
    const uint8_t *src = g_playdia->video.framebuffer;

    for (int y = 0; y < PD_PIC_H; y++)
    {
        const uint8_t *row = src + (((size_t)(y + PD_OY) * SCREEN_W) + PD_OX) * 3;
        uint32_t *dst = g_frame + (size_t)y * PD_PIC_W;

        for (int x = 0; x < PD_PIC_W; x++, row += 3)
            dst[x] = ((uint32_t)row[0] << 16) | ((uint32_t)row[1] << 8) | row[2];
    }

    video_cb(g_frame, PD_PIC_W, PD_PIC_H, PD_PIC_W * sizeof(uint32_t));

    const int pairs = pipeline_drain_audio(&g_playdia->pipe, &g_playdia->video);

    if (pairs > 0)
        audio_batch_cb(g_playdia->pipe.drain_buf, (size_t)pairs);
}

// Save states are not wired up yet: the emulator has no serialiser.
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index; (void)enabled; (void)code;
}

void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
