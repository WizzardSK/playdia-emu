#include "pipeline.h"
#include "bios_hle.h"
#include "playdia_sys.h"
#include <string.h>
#include <stdio.h>

// ─────────────────────────────────────────────────────────────
//  Helpers: read/write TLCS-870 memory
// ─────────────────────────────────────────────────────────────
static inline uint8_t  mr(Playdia *p, uint16_t a)             { return p->mem[a]; }
static inline void     mw(Playdia *p, uint16_t a, uint8_t v)  { p->mem[a] = v; }
static inline uint16_t mr16(Playdia *p, uint16_t a)           { return p->mem[a] | ((uint16_t)p->mem[a+1]<<8); }
static inline void     mw16(Playdia *p, uint16_t a, uint16_t v){ p->mem[a]=v&0xFF; p->mem[a+1]=v>>8; }

// TLCS-870 stack push/pop helpers
static inline void push16(Playdia *p, uint16_t v) {
    p->cpu.SP -= 2;
    mw16(p, p->cpu.SP, v);
}
static inline uint16_t pop16(Playdia *p) {
    uint16_t v = mr16(p, p->cpu.SP);
    p->cpu.SP += 2;
    return v;
}

// ─────────────────────────────────────────────────────────────
//  Synthetic TLCS-870 ROM  (Z80-compatible opcodes)
//
//  Written as raw bytes so no assembler is needed.
//  Layout documented in bios_hle.h.
// ─────────────────────────────────────────────────────────────
static const uint8_t tlcs_rom[] = {
    // ── 0x0000: Reset entry ──────────────────────────────────
    // LD SP, 0x5EFE
    0x31, 0xFE, 0x5E,
    // XOR A
    0xAF,
    // LD HL, 0x2000
    0x21, 0x00, 0x20,
    // LD BC, 0x3F00  (16128 iterations — clears 0x2000-0x5EFF)
    0x01, 0x00, 0x3F,
    // .clear_loop: (addr 0x000A)
    // LD (HL), A
    0x77,
    // INC HL
    0x23,
    // DEC BC
    0x0B,
    // LD A, B
    0x78,
    // OR C
    0xB1,
    // JP NZ, 0x000A
    0xC2, 0x0A, 0x00,
    // CALL 0x0080   (HLE_INIT_HW)
    0xCD, 0x80, 0x00,
    // CALL 0x0090   (HLE_BOOT)
    0xCD, 0x90, 0x00,
    // JP 0x2000
    0xC3, 0x00, 0x20,

    // ── Padding to 0x0080 ────────────────────────────────────
    // (ROM bytes 0x001E..0x007F are NOPs)
};

// Hook stubs: each is NOP (0x00) + RET (0xC9).
// The NOP is the intercept point — C code fires when PC == stub.
// After C work, PC advances to the RET which pops the return addr.
#define STUB { 0x00, 0xC9 }
static const uint8_t stub_initHW [2] = STUB;  // 0x0080
static const uint8_t stub_boot   [2] = STUB;  // 0x0090
static const uint8_t stub_rdSect [2] = STUB;  // 0x00A0
static const uint8_t stub_vsync  [2] = STUB;  // 0x00B0
static const uint8_t stub_ctrl   [2] = STUB;  // 0x00C0

// ─────────────────────────────────────────────────────────────
//  Synthetic NEC 78K ROM
//
//  NEC 78K/0 opcodes used:
//   MOV  A, sfr    = 0xF2, sfr_addr
//   MOV  sfr, A    = 0xF0, sfr_addr
//   CMP  A, #n     = 0xCE, n        (compare A with imm)
//   BZ   rel       = 0xAD, rel      (branch if zero)
//   BNZ  rel       = 0xAC, rel      (branch if not zero)
//   BR   rel       = 0xFC, rel      (unconditional branch)
//   HALT           = 0xFF
//   NOP            = 0x00
//
//  Reset vector: mem[0x0000-0x0001] = 0x0100 (LE)
//  Code at 0x0100: command poll loop
// ─────────────────────────────────────────────────────────────
static const uint8_t nec_rom[] = {
    // 0x0000: reset vector = 0x0100 (little-endian)
    0x00, 0x01,
    // 0x0002..0x00FF: padding
};

// NEC command loop code at 0x0100
// This is a polling loop — the real chip uses interrupts but
// for HLE polling is fine (bios_hle_hook_nec does the work in C).
// We just need valid opcodes that keep the CPU busy without crashing.
static const uint8_t nec_loop[] = {
    // 0x0100:
    0x00,         // NOP
    0x00,         // NOP
    0x00,         // NOP
    0xFA, 0xFB,   // BR $-5  (loop back to 0x0100; 0xFA=BR rel8, rel=-5)
};

// ─────────────────────────────────────────────────────────────
//  bios_hle_install
// ─────────────────────────────────────────────────────────────
void bios_hle_install(Playdia *p) {
    // ── TLCS-870 ROM ─────────────────────────────────────────
    // Fill ROM with NOPs first
    memset(p->mem, 0x00, 0x0200);

    // Copy main boot sequence
    memcpy(p->mem, tlcs_rom, sizeof tlcs_rom);

    // Install hook stubs
    memcpy(p->mem + HLE_ADDR_INIT_HW,     stub_initHW,  2);
    memcpy(p->mem + HLE_ADDR_BOOT,        stub_boot,    2);
    memcpy(p->mem + HLE_ADDR_READ_SECTOR, stub_rdSect,  2);
    memcpy(p->mem + HLE_ADDR_VSYNC,       stub_vsync,   2);
    memcpy(p->mem + HLE_ADDR_CTRL,        stub_ctrl,    2);

    // ── NEC 78K ROM ──────────────────────────────────────────
    memset(p->io_mem, 0x00, 0x0200);
    // Reset vector at 0x0000 = 0x0100
    p->io_mem[0x0000] = 0x00;
    p->io_mem[0x0001] = 0x01;
    // Command loop at 0x0100
    memcpy(p->io_mem + 0x0100, nec_loop, sizeof nec_loop);

    // ── Initialize mailbox ───────────────────────────────────
    memset(p->mem + MAILBOX_BASE, 0, 16);
    p->mem[MAILBOX_BASE + MBOX_STATUS] = MSTAT_READY;

    // Sync to NEC mirror
    bios_hle_sync_mailbox(p);

    // ── Re-run NEC reset to pick up new vector ───────────────
    cpu_nec78k_reset(&p->io_cpu);

    printf("[BIOS HLE] Installed.  TLCS-870 PC=0x%04X  NEC PC=0x%04X\n",
           p->cpu.PC, p->io_cpu.PC);
}

// ─────────────────────────────────────────────────────────────
//  HLE_INIT_HW  —  mimics hardware register init sequence
// ─────────────────────────────────────────────────────────────
static void hle_init_hw(Playdia *p) {
    // Enable AK8000 video + audio
    ak8000_write_reg(&p->video, AK8000_REG_CTRL,     0x03); // video|audio on
    ak8000_write_reg(&p->video, AK8000_REG_VID_MODE, 0x01); // MPEG-1
    ak8000_write_reg(&p->video, AK8000_REG_AUD_CTRL, 0x01); // MP2 audio

    // Set up I/O registers in TLCS-870 address space
    // (these would normally be written by BIOS via OUT instructions)
    p->mem[0x6000] = 0x01;  // CD-ROM: motor on / ready
    p->mem[0x6010] = 0x03;  // AK8000: ctrl = video+audio
    p->mem[0x6020] = 0x00;  // controller: idle

    printf("[BIOS HLE] HW init: AK8000 enabled, CD motor on\n");
}

// ─────────────────────────────────────────────────────────────
//  HLE_BOOT  —  find game executable on disc and load to RAM
// ─────────────────────────────────────────────────────────────

// Candidate boot filenames (tried in order)
static const char * const boot_names[] = {
    "GAME.BIN", "BOOT.BIN", "SYSTEM.BIN", "MAIN.BIN",
    "START.BIN", "PROGRAM.BIN", "PLAYDIA.BIN", NULL
};

// Check if filename has a given extension (case-insensitive)
static bool has_ext(const char *name, const char *ext) {
    int nl = (int)strlen(name), el = (int)strlen(ext);
    if (nl < el) return false;
    for (int i = 0; i < el; i++) {
        char a = name[nl - el + i], b = ext[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return false;
    }
    return true;
}

static void hle_boot(Playdia *p) {
    if (!p->cdrom.disc_present) {
        printf("[BIOS HLE] No disc — halting\n");
        p->cpu.halted = true;
        return;
    }

    printf("[BIOS HLE] Boot: searching disc...\n");
    cdrom_list_files(&p->cdrom);

    // ── Find GLB (scene table) and AJS (FMV stream) ──────────
    ISOEntry *glb_entry = NULL;
    ISOEntry *ajs_entry = NULL;

    for (int i = 0; i < p->cdrom.n_files; i++) {
        ISOEntry *e = &p->cdrom.files[i];
        if (e->is_dir) continue;
        if (!glb_entry && has_ext(e->name, ".GLB")) glb_entry = e;
        if (!ajs_entry && has_ext(e->name, ".AJS")) ajs_entry = e;
    }

    // ── Playdia FMV boot (GLB + AJS found) ───────────────────
    if (glb_entry && ajs_entry) {
        printf("[BIOS HLE] Playdia disc detected\n");
        printf("[BIOS HLE]   GLB: %s (LBA=%u, %u bytes)\n",
               glb_entry->name, glb_entry->lba, glb_entry->size);
        printf("[BIOS HLE]   AJS: %s (LBA=%u, %u bytes)\n",
               ajs_entry->name, ajs_entry->lba, ajs_entry->size);

        // Load GLB scene table into RAM at 0x2000
        uint32_t lba = glb_entry->lba;
        uint32_t remain = glb_entry->size;
        uint16_t dst = GAME_LOAD_ADDR;
        uint32_t loaded = 0;

        while (remain > 0 && dst < (GAME_LOAD_ADDR + 0x1000)) {
            if (cdrom_seek(&p->cdrom, lba) != 0) break;
            if (cdrom_read_sector(&p->cdrom) != 0) break;
            int chunk = p->cdrom.data_len;
            if (chunk > (int)remain) chunk = (int)remain;
            memcpy(p->mem + dst, p->cdrom.data_ptr, chunk);
            dst += (uint16_t)chunk;
            remain -= (uint32_t)chunk;
            loaded += (uint32_t)chunk;
            lba++;
        }
        printf("[BIOS HLE] Loaded GLB %u bytes → RAM 0x%04X–0x%04X\n",
               loaded, GAME_LOAD_ADDR, dst - 1);

        // Parse GLB header
        uint8_t *glb = p->mem + GAME_LOAD_ADDR;
        printf("[BIOS HLE] GLB header: %02X %02X %02X %02X %02X %02X %02X %02X\n",
               glb[0], glb[1], glb[2], glb[3], glb[4], glb[5], glb[6], glb[7]);

        // Start streaming AJS data to AK8000
        printf("[BIOS HLE] Starting FMV stream from AJS LBA=%u\n", ajs_entry->lba);
        cdrom_seek(&p->cdrom, ajs_entry->lba);
        p->cdrom.streaming = true;
        p->cdrom.stream_end = 0; // unlimited

        // Write a stub program at GAME_LOAD_ADDR + 0x1000 that runs
        // the FMV playback loop: VSYNC → CTRL → repeat
        // This keeps the CPU busy while AK8000 handles video/audio
        uint16_t stub_addr = GAME_LOAD_ADDR + 0x1000; // 0x3000
        uint8_t *s = p->mem + stub_addr;
        // .loop:
        *s++ = 0xCD; *s++ = HLE_ADDR_VSYNC & 0xFF; *s++ = HLE_ADDR_VSYNC >> 8;  // CALL VSYNC
        *s++ = 0xCD; *s++ = HLE_ADDR_CTRL  & 0xFF; *s++ = HLE_ADDR_CTRL  >> 8;  // CALL CTRL
        *s++ = 0xC3; *s++ = stub_addr & 0xFF; *s++ = stub_addr >> 8;             // JP .loop

        // Redirect execution to our stub instead of raw GLB data
        p->cpu.PC = stub_addr;
        printf("[BIOS HLE] FMV playback loop at 0x%04X\n", stub_addr);
        return;
    }

    // ── Traditional boot (non-Playdia or no GLB/AJS) ─────────
    ISOEntry *entry = NULL;
    for (int i = 0; boot_names[i]; i++) {
        entry = cdrom_find_file(&p->cdrom, boot_names[i]);
        if (entry) {
            printf("[BIOS HLE] Found boot file: %s (LBA=%u size=%u)\n",
                   entry->name, entry->lba, entry->size);
            break;
        }
    }

    if (!entry) {
        for (int i = 0; i < p->cdrom.n_files; i++) {
            ISOEntry *e = &p->cdrom.files[i];
            if (e->is_dir) continue;
            if (has_ext(e->name, ".BIN")) {
                entry = e;
                printf("[BIOS HLE] Using first .BIN: %s\n", e->name);
                break;
            }
        }
    }

    if (!entry) {
        for (int i = 0; i < p->cdrom.n_files; i++) {
            ISOEntry *e = &p->cdrom.files[i];
            if (e->is_dir || e->size <= 32) continue;
            entry = e;
            printf("[BIOS HLE] Fallback: loading file: %s\n", entry->name);
            break;
        }
    }

    if (!entry) {
        printf("[BIOS HLE] No loadable file found — halting\n");
        p->cpu.halted = true;
        return;
    }

    // Load up to 16KB of the boot file into RAM at GAME_LOAD_ADDR
    uint32_t lba     = entry->lba;
    uint32_t remain  = entry->size;
    uint16_t dst     = GAME_LOAD_ADDR;
    uint32_t loaded  = 0;

    while (remain > 0 && dst < (MAILBOX_BASE - 1)) {
        if (cdrom_seek(&p->cdrom, lba) != 0) break;
        if (cdrom_read_sector(&p->cdrom) != 0) break;

        int chunk = p->cdrom.data_len;
        if (chunk > (int)remain)          chunk = (int)remain;
        if (chunk > (MAILBOX_BASE - dst)) chunk = MAILBOX_BASE - dst;

        memcpy(p->mem + dst, p->cdrom.data_ptr, chunk);
        dst    += (uint16_t)chunk;
        remain -= (uint32_t)chunk;
        loaded += (uint32_t)chunk;
        lba++;
    }

    printf("[BIOS HLE] Loaded %u bytes → RAM 0x%04X–0x%04X\n",
           loaded, GAME_LOAD_ADDR, dst - 1);
    printf("[BIOS HLE] Game entry: JP 0x%04X\n", GAME_LOAD_ADDR);
}

// ─────────────────────────────────────────────────────────────
//  HLE_READ_SECTOR  —  BC = 24-bit LBA, HL = dest in p->mem
// ─────────────────────────────────────────────────────────────
static void hle_read_sector(Playdia *p) {
    uint32_t lba = ((uint32_t)p->cpu.A << 16) |
                   ((uint32_t)p->cpu.B << 8)  |
                    (uint32_t)p->cpu.C;
    uint16_t dst = REG_HL(&p->cpu);

    if (cdrom_seek(&p->cdrom, lba) == 0 &&
        cdrom_read_sector(&p->cdrom) == 0) {
        int len = p->cdrom.data_len;
        if (dst + len > 0x6000) len = 0x6000 - dst; // stay in RAM
        memcpy(p->mem + dst, p->cdrom.data_ptr, len);
        // Return: A = 0 (OK), BC = bytes read
        p->cpu.A = 0;
        p->cpu.B = (uint8_t)(len >> 8);
        p->cpu.C = (uint8_t)(len & 0xFF);
    } else {
        p->cpu.A = 0xFF; // error
    }
}

// ─────────────────────────────────────────────────────────────
//  HLE_CTRL  —  return controller state in A
// ─────────────────────────────────────────────────────────────
static void hle_ctrl(Playdia *p) {
    p->cpu.A = p->controller;
}

// ─────────────────────────────────────────────────────────────
//  bios_hle_hook_tlcs  —  intercept before each TLCS-870 step
// ─────────────────────────────────────────────────────────────
bool bios_hle_hook_tlcs(Playdia *p) {
    uint16_t pc = p->cpu.PC;
    switch (pc) {
    case HLE_ADDR_INIT_HW:
        hle_init_hw(p);
        p->cpu.PC = pc + 1; // skip NOP, point at RET → pops return addr
        return true;
    case HLE_ADDR_BOOT:
        hle_boot(p);
        // If hle_boot set PC to the FMV stub, don't override it.
        // Otherwise advance past NOP to RET.
        if (p->cpu.PC == pc || p->cpu.PC == pc + 1)
            p->cpu.PC = pc + 1;
        else {
            // hle_boot redirected PC (e.g. FMV loop) — pop the return
            // address from stack since we won't execute the RET
            (void)pop16(p);
            // Also pop the main boot caller's return address
            // (we're skipping the JP 0x2000 after CALL BOOT)
            (void)pop16(p);
        }
        return true;
    case HLE_ADDR_READ_SECTOR:
        hle_read_sector(p);
        p->cpu.PC = pc + 1;
        return true;
    case HLE_ADDR_VSYNC:
        // Nothing to do — emulator is already frame-locked
        p->cpu.PC = pc + 1;
        return true;
    case HLE_ADDR_CTRL:
        hle_ctrl(p);
        p->cpu.PC = pc + 1;
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────
//  bios_hle_hook_nec  —  NEC 78K command dispatch (per frame)
//
//  The NEC CPU polls the mailbox.  We intercept here in C so
//  the NEC doesn't need working opcodes for every CD command.
// ─────────────────────────────────────────────────────────────
void bios_hle_hook_nec(Playdia *p) {
    uint8_t cmd = p->mem[MAILBOX_BASE + MBOX_CMD];
    if (cmd == MCMD_IDLE) return;

    // Decode 24-bit LBA from mailbox
    uint32_t lba = ((uint32_t)p->mem[MAILBOX_BASE + MBOX_ARG0] << 16) |
                   ((uint32_t)p->mem[MAILBOX_BASE + MBOX_ARG1] << 8)  |
                    (uint32_t)p->mem[MAILBOX_BASE + MBOX_ARG2];

    switch (cmd) {
    case MCMD_SEEK:
        cdrom_seek(&p->cdrom, lba);
        p->mem[MAILBOX_BASE + MBOX_STATUS] = MSTAT_READY;
        printf("[NEC HLE] SEEK LBA=%u\n", lba);
        break;

    case MCMD_READ:
        if (cdrom_seek(&p->cdrom, lba) == 0 &&
            cdrom_read_sector(&p->cdrom) == 0) {
            // DMA sector data into TLCS-870 CD window (0x8000)
            int len = p->cdrom.data_len;
            if (len > 0x7FFF) len = 0x7FFF;
            memcpy(p->mem + 0x8000, p->cdrom.data_ptr, len);
            p->mem[MAILBOX_BASE + MBOX_STATUS]    = MSTAT_READY;
            p->mem[MAILBOX_BASE + MBOX_RESULT_LO] = (uint8_t)(len & 0xFF);
            p->mem[MAILBOX_BASE + MBOX_RESULT_HI] = (uint8_t)(len >> 8);
            // Route sector through pipeline (detects FMV vs data)
            pipeline_feed_lba(&p->pipe, &p->cdrom, &p->video,
                               p->cdrom.lba - 1, 0); // already read, just route
            // Direct feed if explicitly streaming
            if (p->cdrom.streaming)
                ak8000_feed_sector(&p->video, p->cdrom.data_ptr, len);
        } else {
            p->mem[MAILBOX_BASE + MBOX_STATUS] = MSTAT_ERROR;
        }
        // Fire IRQ on TLCS-870 (vector 0x38 = standard Z80 RST 7)
        cpu_tlcs870_irq(&p->cpu, 0x38);
        break;

    case MCMD_STREAM_ON:
        p->cdrom.streaming  = true;
        p->cdrom.stream_end = 0; // unlimited
        p->mem[MAILBOX_BASE + MBOX_STATUS] = MSTAT_READY;
        printf("[NEC HLE] STREAM ON from LBA=%u\n", lba);
        break;

    case MCMD_STREAM_OFF:
        p->cdrom.streaming = false;
        p->mem[MAILBOX_BASE + MBOX_STATUS] = MSTAT_READY;
        printf("[NEC HLE] STREAM OFF\n");
        break;

    default:
        printf("[NEC HLE] Unknown cmd 0x%02X\n", cmd);
        p->mem[MAILBOX_BASE + MBOX_STATUS] = MSTAT_ERROR;
    }

    // Clear command after processing
    p->mem[MAILBOX_BASE + MBOX_CMD] = MCMD_IDLE;
    bios_hle_sync_mailbox(p);
}

// ─────────────────────────────────────────────────────────────
//  bios_hle_sync_mailbox  —  copy TLCS mailbox ↔ NEC mirror
// ─────────────────────────────────────────────────────────────
void bios_hle_sync_mailbox(Playdia *p) {
    // Copy 16 bytes from p->mem[MAILBOX_BASE] → p->io_mem[NEC_MBOX_BASE]
    memcpy(p->io_mem + NEC_MBOX_BASE, p->mem + MAILBOX_BASE, 16);
}
