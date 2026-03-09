#pragma once
#include <stdint.h>
#include <stdbool.h>

// ─── Memory Map ───────────────────────────────────────────────
// 0x0000 - 0xFFFF  : 64KB addressable space (TLCS-870)
// 0x0000 - 0x1FFF  : Internal ROM (8KB)
// 0x2000 - 0x5FFF  : Internal RAM (16KB)
// 0x6000 - 0x7FFF  : I/O mapped registers
// 0x8000 - 0xFFFF  : CD-ROM data / external

#define MEM_SIZE        0x10000   // 64KB
#define ROM_SIZE        0x2000    // 8KB
#define RAM_SIZE        0x4000    // 16KB
#define IO_BASE         0x6000
#define IO_SIZE         0x2000
#define CDROM_BASE      0x8000

// ─── CD-ROM ───────────────────────────────────────────────────
#define SECTOR_SIZE     2352      // raw CD sector
#define SECTOR_DATA     2048      // usable data per sector

// ─── Video (AK8000) ───────────────────────────────────────────
#define SCREEN_W        320
#define SCREEN_H        240
#define FPS             30        // FMV-based, mostly 30fps

// ─── Audio ────────────────────────────────────────────────────
#define SAMPLE_RATE       44100
#define CHANNELS          2
#define SAMPLES_PER_FRAME (SAMPLE_RATE / FPS)   // 1470

// ─── Forward declarations ─────────────────────────────────────
typedef struct CPU_TLCS870  CPU_TLCS870;
typedef struct CPU_NEC78K   CPU_NEC78K;
typedef struct CDROM        CDROM;
typedef struct AK8000       AK8000;
typedef struct Playdia      Playdia;
