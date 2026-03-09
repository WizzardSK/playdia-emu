#pragma once
#include "playdia.h"
#include "cpu_tlcs870.h"
#include "cpu_nec78k.h"
#include "cdrom.h"
#include "ak8000.h"
#include "bios_hle.h"
#include "pipeline.h"
#include "interconnect.h"

// ─────────────────────────────────────────────────────────────
//  Playdia system struct  -  the "motherboard"
// ─────────────────────────────────────────────────────────────

typedef struct Playdia {
    // ── CPUs ───────────────────────────────────────────────
    CPU_TLCS870  cpu;        // Main CPU  (Toshiba TLCS-870, 8MHz)
    CPU_NEC78K   io_cpu;     // I/O CPU   (NEC 78K, 12MHz)

    // ── Memory ─────────────────────────────────────────────
    uint8_t      mem[MEM_SIZE];      // 64KB flat for main CPU
    uint8_t      io_mem[0x10000];    // 64KB for NEC CPU

    // ── Subsystems ─────────────────────────────────────────
    CDROM        cdrom;
    AK8000       video;

    // ── Controller (infrared) ──────────────────────────────
    uint8_t      controller;         // button state bitmask

    // ── Timing ─────────────────────────────────────────────
    uint64_t     master_cycles;
    uint32_t     frames;

    // ── Pipeline ──────────────────────────────────────────
    Pipeline     pipe;

    // ── Debug ──────────────────────────────────────────────
    bool         debug_mode;
    bool         running;
} Playdia;

// Button bits (infrared controller)
#define BTN_LEFT    0x01
#define BTN_RIGHT   0x02
#define BTN_UP      0x04
#define BTN_DOWN    0x08
#define BTN_A       0x10
#define BTN_B       0x20
#define BTN_SELECT  0x40
#define BTN_START   0x80

// ── API ────────────────────────────────────────────────────────
void playdia_init        (Playdia *p);
void playdia_reset       (Playdia *p);
int  playdia_load_disc   (Playdia *p, const char *iso_path);
void playdia_run_frame   (Playdia *p);   // run ~1/30th second
void playdia_set_button  (Playdia *p, uint8_t btn, bool pressed);
void playdia_dump        (Playdia *p);

// Memory-mapped I/O
uint8_t playdia_mem_read (Playdia *p, uint16_t addr);
void    playdia_mem_write(Playdia *p, uint16_t addr, uint8_t val);
