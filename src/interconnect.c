#include "interconnect.h"
#include "playdia_sys.h"
#include "ak8000.h"
#include "pipeline.h"
#include <string.h>
#include <stdio.h>

// ─────────────────────────────────────────────────────────────
//  CDROM IRQ callback — fires from cdrom_read_sector()
//  Called in the emulation thread, safe to mutate state.
// ─────────────────────────────────────────────────────────────
void interconnect_sector_cb(void *ctx) {
    Playdia *p = (Playdia *)ctx;

    // 1. DMA sector into TLCS-870 CD window 0x8000
    int len = p->cdrom.data_len;
    if (len > 0x7FFF) len = 0x7FFF;
    memcpy(p->mem + 0x8000, p->cdrom.data_ptr, len);

    // 2. AK8000 routing is handled by pipeline_run_frame / route_sector.
    //    Do NOT feed here to avoid double-feeding (the callback fires on
    //    every cdrom_read_sector including boot scan reads).

    // 3. Update mailbox: STATUS = READY, store sector length
    p->mem[MAILBOX_BASE + MBOX_STATUS]    = MSTAT_READY;
    p->mem[MAILBOX_BASE + MBOX_RESULT_LO] = (uint8_t)(len & 0xFF);
    p->mem[MAILBOX_BASE + MBOX_RESULT_HI] = (uint8_t)(len >> 8);
    bios_hle_sync_mailbox(p);

    // 4. Set flags byte in TLCS RAM
    p->mem[IFLAGS_ADDR] |= IFLAG_SECTOR_READY;

    // 5. Fire NEC 78K IRQ (INTP0 → handler at NVEC_SECTOR)
    //    NEC handler will then fire TLCS IRQ
    cpu_nec78k_irq(&p->io_cpu, NVEC_SECTOR);

    // 6. Fire TLCS-870 IRQ directly too (in case NEC is halted/busy)
    //    vector 0x38 → reads handler addr from mem[0x38..0x39]
    cpu_tlcs870_irq(&p->cpu, TVEC_SECTOR);
}

// ─────────────────────────────────────────────────────────────
//  Install interrupt vectors + stubs
// ─────────────────────────────────────────────────────────────

// TLCS-870 stub: EI + RETI  (0xFB 0xED 0x4D)
// EI re-enables interrupts, RETI returns from ISR and restores IFF
static const uint8_t tlcs_isr_stub[] = {
    0xFB,       // EI
    0xED, 0x4D, // RETI
};

// NEC 78K stub: RETI  (0x8A)
static const uint8_t nec_isr_stub[] = {
    0x9F,       // RETI (NEC 78K opcode)
};

void interconnect_install(Playdia *p) {
    // ── TLCS-870: write 16-bit handler addresses into vector table ──
    // The vector table holds the ADDRESS of the handler, not the opcode.
    // cpu_tlcs870_irq reads: cpu->PC = mr16(cpu, vector)

    // TVEC_SECTOR (0x0038) → TISRV_SECTOR (0x00D0)
    p->mem[TVEC_SECTOR]   = (uint8_t)(TISRV_SECTOR & 0xFF);
    p->mem[TVEC_SECTOR+1] = (uint8_t)(TISRV_SECTOR >> 8);

    // TVEC_VSYNC (0x0040) → TISRV_VSYNC (0x00D8)
    p->mem[TVEC_VSYNC]   = (uint8_t)(TISRV_VSYNC & 0xFF);
    p->mem[TVEC_VSYNC+1] = (uint8_t)(TISRV_VSYNC >> 8);

    // TVEC_CTRL (0x0048) → TISRV_CTRL (0x00E0)
    p->mem[TVEC_CTRL]   = (uint8_t)(TISRV_CTRL & 0xFF);
    p->mem[TVEC_CTRL+1] = (uint8_t)(TISRV_CTRL >> 8);

    // ── TLCS-870 ISR stubs in ROM ─────────────────────────────
    // sector ISR: NOP (HLE hook fires), then EI + RETI
    p->mem[TISRV_SECTOR]   = 0x00;   // NOP — HLE intercept point
    p->mem[TISRV_SECTOR+1] = 0xFB;   // EI
    p->mem[TISRV_SECTOR+2] = 0xED;   // RETI hi
    p->mem[TISRV_SECTOR+3] = 0x4D;   // RETI lo

    // vsync ISR
    p->mem[TISRV_VSYNC]    = 0x00;
    p->mem[TISRV_VSYNC+1]  = 0xFB;
    p->mem[TISRV_VSYNC+2]  = 0xED;
    p->mem[TISRV_VSYNC+3]  = 0x4D;

    // ctrl ISR
    p->mem[TISRV_CTRL]     = 0x00;
    p->mem[TISRV_CTRL+1]   = 0xFB;
    p->mem[TISRV_CTRL+2]   = 0xED;
    p->mem[TISRV_CTRL+3]   = 0x4D;

    // ── Add EI to TLCS-870 boot ROM — right before JP 0x2000 ──
    // Boot ROM ends with: CALL 0x0090 / JP 0x2000
    // The JP is at offset 0x0018 (3+3+3+3+3+9+3+3 = 24 = 0x18... let's check)
    // Actually: LD SP(3) + XOR A(1) + LD HL(3) + LD BC(3) + loop(5 bytes) +
    //           CALL(3) + CALL(3) + JP(3) = offset 0x1A for JP
    // We insert EI right after the second CALL, before JP.
    // Easiest: just enable IME directly after bios_hle_install runs.
    p->cpu.ime = true;

    // Also enable NEC interrupts
    p->io_cpu.PSW |= NEC_IE;

    // ── NEC 78K: write interrupt vector TABLE entries ──────────
    // NEC 78K vector table: io_mem[vector_addr] = LE 16-bit handler addr
    p->io_mem[NVEC_SECTOR]   = (uint8_t)(NISRV_SECTOR & 0xFF);
    p->io_mem[NVEC_SECTOR+1] = (uint8_t)(NISRV_SECTOR >> 8);

    p->io_mem[NVEC_VSYNC]    = (uint8_t)(NISRV_VSYNC & 0xFF);
    p->io_mem[NVEC_VSYNC+1]  = (uint8_t)(NISRV_VSYNC >> 8);

    // ── NEC ISR stubs (HLE hook + RETI) ──────────────────────
    p->io_mem[NISRV_SECTOR]   = 0x00;  // NOP — HLE intercept
    p->io_mem[NISRV_SECTOR+1] = 0x9F;  // RETI (NEC 78K opcode = 0x9F)

    p->io_mem[NISRV_VSYNC]    = 0x00;
    p->io_mem[NISRV_VSYNC+1]  = 0x9F;  // RETI

    // ── Wire CDROM IRQ callback ───────────────────────────────
    p->cdrom.irq_cb  = interconnect_sector_cb;
    p->cdrom.irq_ctx = p;

    printf("[Interconnect] Installed.  TVEC_SECTOR→0x%04X  NVEC_SECTOR→0x%04X\n",
           TISRV_SECTOR, NISRV_SECTOR);
    printf("[Interconnect] TLCS IME=%d  NEC IE=%d\n",
           p->cpu.ime, (p->io_cpu.PSW & NEC_IE) ? 1 : 0);
}

// ─────────────────────────────────────────────────────────────
//  interconnect_tick — per-frame delivery of vsync + ctrl IRQs
// ─────────────────────────────────────────────────────────────
void interconnect_tick(Playdia *p) {
    // ── Vsync IRQ: fire every frame ───────────────────────────
    p->mem[IFLAGS_ADDR] |= IFLAG_VSYNC;
    cpu_nec78k_irq(&p->io_cpu, NVEC_VSYNC);
    cpu_tlcs870_irq(&p->cpu,   TVEC_VSYNC);

    // ── Controller change IRQ ─────────────────────────────────
    static uint8_t last_ctrl = 0;
    if (p->controller != last_ctrl) {
        last_ctrl = p->controller;
        p->mem[IFLAGS_ADDR] |= IFLAG_CTRL_CHANGED;
        cpu_tlcs870_irq(&p->cpu, TVEC_CTRL);
    }

    // Note: we do NOT force-enable IME/IE here.
    // Game code may use DI sections legitimately (e.g. during DMA).
    // IRQs are delivered on the next frame after EI is executed.
}

// ─────────────────────────────────────────────────────────────
//  interconnect_hook_tlcs — intercept ISR stub NOPs
// ─────────────────────────────────────────────────────────────
bool interconnect_hook_tlcs(Playdia *p) {
    uint16_t pc = p->cpu.PC;

    if (pc == TISRV_SECTOR) {
        // Sector-ready ISR: clear flag, game code can read 0x8000
        p->mem[IFLAGS_ADDR] &= ~IFLAG_SECTOR_READY;
        // Optional: update A register with sector status for game poll loops
        p->cpu.A = p->mem[MAILBOX_BASE + MBOX_STATUS];
        p->cpu.PC = pc + 1; // advance to EI
        return true;
    }
    if (pc == TISRV_VSYNC) {
        p->mem[IFLAGS_ADDR] &= ~IFLAG_VSYNC;
        p->cpu.PC = pc + 1;
        return true;
    }
    if (pc == TISRV_CTRL) {
        // Load current controller state into A
        p->cpu.A = p->controller;
        p->mem[IFLAGS_ADDR] &= ~IFLAG_CTRL_CHANGED;
        p->cpu.PC = pc + 1;
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────
//  interconnect_hook_nec — intercept NEC ISR stub NOPs
// ─────────────────────────────────────────────────────────────
bool interconnect_hook_nec(Playdia *p) {
    uint16_t pc = p->io_cpu.PC;

    if (pc == NISRV_SECTOR) {
        // NEC sector ISR: sector already DMA'd by sector_cb.
        // Notify TLCS-870 via direct IRQ (in case the TLCS missed it).
        cpu_tlcs870_irq(&p->cpu, TVEC_SECTOR);
        p->io_cpu.PC = pc + 1; // advance to RETI
        return true;
    }
    if (pc == NISRV_VSYNC) {
        cpu_tlcs870_irq(&p->cpu, TVEC_VSYNC);
        p->io_cpu.PC = pc + 1;
        return true;
    }
    return false;
}
