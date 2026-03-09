#pragma once
#include "playdia.h"
#include "cpu_tlcs870.h"
#include "cpu_nec78k.h"
#include "cdrom.h"

// ═══════════════════════════════════════════════════════════════
//  CPU Interconnect — TLCS-870 ↔ NEC 78K ↔ CD-ROM ↔ AK8000
//
//  Signal flow:
//
//   CD-ROM sector read complete
//     → cdrom.irq_cb (interconnect_sector_cb)
//       → NEC 78K: fires IRQ at vector NVEC_SECTOR (0x0010)
//         → NEC handler: DMA sector to 0x8000, write mailbox STATUS=READY
//           → TLCS-870: fires IRQ vector TVEC_SECTOR (0x0038)
//             → TLCS handler: reads 0x8000, feeds AK8000, continues game
//
//  TLCS-870 interrupt vectors (Z80-style: address stored at vector)
//    0x0038 — sector-ready (from NEC via mailbox)
//    0x0040 — vsync  (end of frame)
//    0x0048 — controller input ready
//
//  NEC 78K interrupt vectors (stored in io_mem)
//    0x0010 — CD-ROM sector ready  (INTP0)
//    0x0018 — timer / vsync        (INTTM0)
//
//  TLCS-870 handler stubs (HLE hooks):
//    0x00D0 — ISRV_SECTOR  (sector-ready ISR)
//    0x00D8 — ISRV_VSYNC   (vsync ISR)
//    0x00E0 — ISRV_CTRL    (controller ISR)
//
//  NEC handler stubs (HLE hooks in io_mem):
//    0x0110 — ISRN_SECTOR  (DMA sector + notify TLCS)
//    0x0118 — ISRN_VSYNC
// ═══════════════════════════════════════════════════════════════

// TLCS-870 interrupt vector TABLE addresses (each holds a 16-bit addr)
#define TVEC_SECTOR   0x0038   // sector-ready
#define TVEC_VSYNC    0x0040   // vsync
#define TVEC_CTRL     0x0048   // controller

// TLCS-870 ISR stub addresses in ROM
#define TISRV_SECTOR  0x00D0
#define TISRV_VSYNC   0x00D8
#define TISRV_CTRL    0x00E0

// NEC 78K interrupt vector TABLE addresses (in io_mem)
#define NVEC_SECTOR   0x0010   // CD sector IRQ
#define NVEC_VSYNC    0x0018   // vsync

// NEC 78K ISR stub addresses in io_mem
#define NISRV_SECTOR  0x0110
#define NISRV_VSYNC   0x0118

// Flags byte in TLCS-870 RAM — used by ISRs to communicate with game
#define IFLAGS_ADDR   0x5EF0   // 1 byte in RAM
#define IFLAG_SECTOR_READY  0x01
#define IFLAG_VSYNC         0x02
#define IFLAG_CTRL_CHANGED  0x04

typedef struct Playdia Playdia;

// Exposed for testing only
extern void interconnect_sector_cb(void *ctx);

// ── API ───────────────────────────────────────────────────────
// Install interrupt vectors + handler stubs, wire cdrom.irq_cb.
// Call once after bios_hle_install().
void interconnect_install(Playdia *p);

// Called every frame AFTER CPUs run — delivers pending IRQs.
// Handles: vsync IRQ to both CPUs, ctrl-change IRQ.
void interconnect_tick(Playdia *p);

// HLE hook for TLCS ISR stubs (call before tlcs870_step)
bool interconnect_hook_tlcs(Playdia *p);

// HLE hook for NEC ISR stubs (call before nec78k_step)
bool interconnect_hook_nec(Playdia *p);
