#include "playdia_sys.h"
#include "sdl_frontend.h"
#include "pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ─────────────────────────────────────────────────────────────
//  Playdia Emulator - Main Entry Point
//
//  Usage:
//    ./playdia game.iso            <- run a disc
//    ./playdia game.iso --debug    <- run + debug overlay
//    ./playdia --test              <- TLCS-870 CPU self-test
//    ./playdia --headless game.iso <- no window, console only
// ─────────────────────────────────────────────────────────────

static void print_banner(void) {
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║   Playdia Emulator  v0.2                         ║\n");
    printf("║   Bandai Playdia Quick Interactive System (1994) ║\n");
    printf("║   Main: Toshiba TLCS-870 @ 8MHz                  ║\n");
    printf("║   I/O:  NEC µPD78214 (78K/0) @ 12MHz             ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");
}

// ─── CPU self-test ────────────────────────────────────────────
static void run_cpu_test(void) {
    printf("=== TLCS-870 CPU Self-Test ===\n\n");

    Playdia p;
    playdia_init(&p);

    uint8_t prog[] = {
        0x3E, 0x42,   // LD A, 0x42
        0x06, 0x10,   // LD B, 0x10
        0x80,         // ADD A, B  → A = 0x52
        0x3C,         // INC A     → A = 0x53
        0xFE, 0x53,   // CP  A, 0x53 → Z=1
        0x76,         // HALT
    };
    memcpy(&p.mem[0x2000], prog, sizeof(prog));
    p.cpu.PC = 0x2000;

    printf("Before:\n"); cpu_tlcs870_dump(&p.cpu);

    int steps = 100;
    while (!p.cpu.halted && steps-- > 0)
        cpu_tlcs870_step(&p.cpu);

    printf("After:\n"); cpu_tlcs870_dump(&p.cpu);

    bool pass = (p.cpu.A == 0x53) && (p.cpu.F & FLAG_Z);
    printf("Result: %s  (A=0x%02X Z=%d)\n\n",
           pass ? "✓ PASS" : "✗ FAIL",
           p.cpu.A, (p.cpu.F & FLAG_Z) ? 1 : 0);
}

// ─── Headless run (no SDL) ────────────────────────────────────
static void run_headless(Playdia *p, uint32_t max_frames) {
    printf("[Headless] Running %u frames...\n", max_frames);
    for (uint32_t f = 0; f < max_frames && p->running; f++) {
        // Auto-advance in headless: break loops and auto-choose
        if (p->video.is_loop && p->video.interactive_cmd == 0x40)
            p->controller = BTN_START;
        else if (p->video.waiting_for_input)
            p->controller = BTN_A;
        else
            p->controller = 0;

        playdia_run_frame(p);
        // Drain audio even in headless (for stats + to prevent ring overflow)
        pipeline_drain_audio(&p->pipe, &p->video);
        // Save frames for debugging (every 3rd frame, first 90)
        if (p->video.frame_count > 0 && p->video.frame_count <= 400 && p->video.frame_count % 20 == 0) {
            char fname[64];
            snprintf(fname, sizeof fname, "/tmp/pd_frame_%03u.ppm", p->video.frame_count);
            FILE *ff = fopen(fname, "wb");
            if (ff) {
                fprintf(ff, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
                fwrite(p->video.framebuffer, SCREEN_W * SCREEN_H * 3, 1, ff);
                fclose(ff);
            }
        }
        if (f % 30 == 0)
            printf("[Headless] frame=%u  ~%.1fs\n", f, f / 30.0f);
    }
    printf("[Headless] Done. %u frames total.\n", p->frames);
    // Save last framebuffer as PPM
    {
        FILE *f = fopen("/tmp/pd_headless.ppm", "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
            fwrite(p->video.framebuffer, SCREEN_W * SCREEN_H * 3, 1, f);
            fclose(f);
            (void)!system("convert /tmp/pd_headless.ppm /tmp/pd_headless.png 2>/dev/null");
            printf("[Headless] Saved framebuffer to /tmp/pd_headless.png\n");
        }
    }
    playdia_dump(p);
}

// ─── Frame timer helpers ──────────────────────────────────────
static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

// ─────────────────────────────────────────────────────────────
//  MAIN
// ─────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    print_banner();

    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s <game.iso>              Run disc\n", argv[0]);
        printf("  %s <game.iso> --debug      Run with debug info\n", argv[0]);
        printf("  %s <game.iso> --headless   Run without window\n", argv[0]);
        printf("  %s --test                  CPU self-test\n\n", argv[0]);
        return 1;
    }

    // ── Self-test mode ────────────────────────────────────
    if (strcmp(argv[1], "--test") == 0) {
        run_cpu_test();
        return 0;
    }

    // ── Parse flags ───────────────────────────────────────
    const char *iso_path = NULL;
    bool debug    = false;
    bool headless = false;

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--debug")    == 0) debug    = true;
        else if (strcmp(argv[i], "--headless") == 0) headless = true;
        else if (argv[i][0] != '-')                  iso_path = argv[i];
    }

    if (!iso_path) {
        fprintf(stderr, "Error: no ISO file specified.\n");
        return 1;
    }

    // ── Init system (heap-allocated — struct is ~800KB) ───
    Playdia *playdia = calloc(1, sizeof(Playdia));
    if (!playdia) { fprintf(stderr, "Out of memory\n"); return 1; }
    playdia_init(playdia);
    playdia->debug_mode = debug;

    if (playdia_load_disc(playdia, iso_path) != 0) {
        fprintf(stderr, "Failed to load: %s\n", iso_path);
        free(playdia);
        return 1;
    }

    printf("[Main] Disc loaded: %s\n", iso_path);

    // ── Headless mode ─────────────────────────────────────
    if (headless) {
        run_headless(playdia, 300);
        free(playdia);
        return 0;
    }

    // ── SDL2 mode ─────────────────────────────────────────
    SDLFrontend fe;
    if (sdl_init(&fe, "Playdia Emulator") != 0) {
        fprintf(stderr, "[Main] SDL init failed — falling back to headless\n");
        run_headless(playdia, 300);
        free(playdia);
        return 0;
    }

    printf("[Main] Controls:\n");
    printf("  Arrow keys  = D-pad\n");
    printf("  Z / X       = A / B buttons\n");
    printf("  Enter       = Start\n");
    printf("  Space       = Select\n");
    printf("  F1          = Toggle fullscreen\n");
    printf("  Escape      = Quit\n\n");

    // ── Main loop ─────────────────────────────────────────
    const uint64_t FRAME_US = 1000000ULL / FPS;   // 33333µs per frame @ 30fps
    uint64_t frame_start = now_us();

    while (playdia->running && fe.running) {
        // Poll SDL events → update controller
        if (!sdl_poll_events(&fe, &playdia->controller))
            break;

        // Emulate one frame
        playdia_run_frame(playdia);

        // Push video framebuffer to SDL texture
        sdl_present_frame(&fe, playdia->video.framebuffer, SCREEN_W, SCREEN_H);

        // Drain decoded audio from AK8000 ring buffer → SDL
        int pairs = pipeline_drain_audio(&playdia->pipe, &playdia->video);
        if (pairs > 0)
            sdl_queue_audio(&fe, playdia->pipe.drain_buf, pairs);

        // Debug dump every 60 frames
        if (debug && (playdia->frames % 60 == 0)) {
            printf("\n[Debug] Frame %u:\n", playdia->frames);
            cpu_tlcs870_dump(&playdia->cpu);
            cpu_nec78k_dump(&playdia->io_cpu);
        }

        // ── Frame rate limiter ────────────────────────────
        uint64_t elapsed = now_us() - frame_start;
        if (elapsed < FRAME_US) {
            uint64_t sleep_us = FRAME_US - elapsed;
            struct timespec ts = {
                .tv_sec  = (time_t)(sleep_us / 1000000),
                .tv_nsec = (long)((sleep_us % 1000000) * 1000)
            };
            nanosleep(&ts, NULL);
        }
        frame_start = now_us();
    }

    printf("\n[Main] Exiting after %u frames (%.1f seconds).\n",
           playdia->frames, playdia->frames / (float)FPS);

    sdl_shutdown(&fe);
    free(playdia);
    return 0;
}
