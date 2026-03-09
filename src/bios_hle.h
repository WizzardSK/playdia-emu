#pragma once
#include "playdia.h"
#include "cpu_tlcs870.h"
#include "cpu_nec78k.h"
#include "cdrom.h"
#include "ak8000.h"

// ═══════════════════════════════════════════════════════════════
//  HLE BIOS — High Level Emulation of Playdia BIOS ROMs
//
//  The real BIOS ROMs were never dumped:
//    - TLCS-870 internal 8KB ROM at 0x0000-0x1FFF
//    - NEC 78K  internal 16KB ROM at its 0x0000
//
//  Strategy: write synthetic TLCS-870 machine code into the ROM
//  area that mimics what the real BIOS would do.  Hardware-
//  specific operations (CD reads, AK8000 init) are intercepted
//  at known "hook" addresses and executed as C functions.
//
//  ── TLCS-870 ROM layout (0x0000–0x01FF) ──────────────────────
//
//  0x0000  Reset entry:
//            LD SP, 0x5EFE       ; stack just below mailbox
//            XOR A
//            LD HL, 0x2000       ; start of RAM
//            LD BC, 0x3F00       ; ~16KB
//          .clear_loop:
//            LD (HL), A
//            INC HL
//            DEC BC
//            LD A, B / OR C / JP NZ .clear_loop
//            CALL 0x0080         ; → HLE_INIT_HW
//            CALL 0x0090         ; → HLE_BOOT  (loads game)
//            JP   0x2000         ; jump to game entry
//
//  0x0080  HLE_INIT_HW  stub: NOP + RET
//  0x0090  HLE_BOOT     stub: NOP + RET
//  0x00A0  HLE_READ_SECTOR stub (BC=LBA, HL=dest RAM addr)
//  0x00B0  HLE_VSYNC    stub
//  0x00C0  HLE_CTRL     stub  (returns A = button mask)
//
//  ── NEC 78K I/O ROM layout (io_mem[0x0000–0x01FF]) ──────────
//
//  0x0000  Reset vector → 0x0100 (little-endian word)
//  0x0100  Command loop: poll mailbox, execute CD commands,
//          fire IRQ to TLCS-870 when sector is ready
//
//  ── Shared mailbox (16 bytes at p->mem[0x5F00]) ──────────────
//
//   +0  CMD    byte written by TLCS-870 (0=idle)
//   +1  ARG0   LBA byte 2 (most significant)
//   +2  ARG1   LBA byte 1
//   +3  ARG2   LBA byte 0 (least significant)
//   +4  STATUS byte written by NEC (0=busy, 1=ready, 0x80=error)
//   +5  RESULT_LO
//   +6  RESULT_HI
//
//  ── NEC mailbox mirror (io_mem[0xF000]) ─────────────────────
//  Kept in sync manually via bios_hle_sync_mailbox().
// ═══════════════════════════════════════════════════════════════

// Hook addresses in TLCS-870 ROM
#define HLE_ADDR_INIT_HW     0x0080
#define HLE_ADDR_BOOT        0x0090
#define HLE_ADDR_READ_SECTOR 0x00A0
#define HLE_ADDR_VSYNC       0x00B0
#define HLE_ADDR_CTRL        0x00C0

// Mailbox offsets (in p->mem[] at MAILBOX_BASE)
#define MAILBOX_BASE    0x5F00
#define MBOX_CMD        0
#define MBOX_ARG0       1
#define MBOX_ARG1       2
#define MBOX_ARG2       3
#define MBOX_STATUS     4
#define MBOX_RESULT_LO  5
#define MBOX_RESULT_HI  6

// Mailbox commands (TLCS-870 → NEC)
#define MCMD_IDLE       0x00
#define MCMD_SEEK       0x01
#define MCMD_READ       0x02
#define MCMD_STREAM_ON  0x03
#define MCMD_STREAM_OFF 0x04

// Mailbox status (NEC → TLCS-870)
#define MSTAT_BUSY   0x00
#define MSTAT_READY  0x01
#define MSTAT_ERROR  0x80

// NEC mailbox mirror in io_mem
#define NEC_MBOX_BASE  0xF000

// Game entry point in RAM (where HLE_BOOT loads the executable)
#define GAME_LOAD_ADDR 0x2000

// Forward declaration (full Playdia struct defined in playdia_sys.h)
typedef struct Playdia Playdia;

// ── API ───────────────────────────────────────────────────────
// Install synthetic ROM code into p->mem[0x0000-0x01FF]
// and into p->io_mem[0x0000-0x01FF].
// Call once after playdia_init(), before the first run_frame().
void bios_hle_install(Playdia *p);

// Call this every frame BEFORE cpu_tlcs870_step() to intercept
// hook addresses.  Returns true if a hook was executed.
bool bios_hle_hook_tlcs(Playdia *p);

// Call this every frame BEFORE cpu_nec78k_step() to run the
// NEC command-dispatch HLE logic.
void bios_hle_hook_nec(Playdia *p);

// Sync the TLCS mailbox → NEC mirror (and vice-versa)
void bios_hle_sync_mailbox(Playdia *p);
