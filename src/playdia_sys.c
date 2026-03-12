#include "playdia_sys.h"
#include "bios_hle.h"
#include "pipeline.h"
#include "interconnect.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>

// ─────────────────────────────────────────────────────────────
//  Cycles per frame
//  Main CPU: 8,000,000 Hz / 30 fps = 266,666 cycles/frame
// ─────────────────────────────────────────────────────────────
#define CYCLES_PER_FRAME  266666

void playdia_init(Playdia *p) {
    // NOTE: bios_hle_install called after disc load (needs CDROM ready)
    // see playdia_load_disc
    memset(p, 0, sizeof(*p));

    cpu_tlcs870_init(&p->cpu,    p->mem);
    cpu_nec78k_init (&p->io_cpu, p->io_mem);
    cdrom_init      (&p->cdrom);
    ak8000_init     (&p->video);

    playdia_reset(p);
}

void playdia_reset(Playdia *p) {
    cpu_tlcs870_reset(&p->cpu);
    cpu_nec78k_reset (&p->io_cpu);
    cdrom_reset      (&p->cdrom);
    ak8000_reset     (&p->video);

    p->controller    = 0;
    p->master_cycles = 0;
    p->frames        = 0;
    p->running       = true;
    pipeline_reset(&p->pipe);

    printf("[Playdia] System reset\n");
}

int playdia_load_disc(Playdia *p, const char *iso_path) {
    // Auto-detect CUE/BIN vs ISO
    int r;
    int len = (int)strlen(iso_path);
    if (len >= 4 && strcasecmp(iso_path + len - 4, ".cue") == 0)
        r = cdrom_load_cue(&p->cdrom, iso_path);
    else if (len >= 4 && strcasecmp(iso_path + len - 4, ".zip") == 0)
        r = cdrom_load_zip(&p->cdrom, iso_path);
    else
        r = cdrom_load_iso(&p->cdrom, iso_path);
    if (r == 0) {
        // Install HLE BIOS now that ISO is parsed (boot needs file list)
        bios_hle_install(p);
        // Wire interrupt vectors + CDROM callback
        interconnect_install(p);
    }
    return r;
}

// ─────────────────────────────────────────────────────────────
//  Memory-mapped I/O dispatch
// ─────────────────────────────────────────────────────────────
uint8_t playdia_mem_read(Playdia *p, uint16_t addr) {
    if (addr < ROM_SIZE) {
        // Internal ROM
        return p->mem[addr];
    }
    if (addr < ROM_SIZE + RAM_SIZE) {
        // Internal RAM
        return p->mem[addr];
    }
    if (addr >= IO_BASE && addr < IO_BASE + IO_SIZE) {
        // I/O registers
        uint8_t reg = (addr - IO_BASE) & 0xFF;
        switch (reg) {
        case 0x00: return cdrom_read_status(&p->cdrom);
        case 0x01: return p->cdrom.sector_ready ? p->cdrom.data_len & 0xFF : 0;
        case 0x02: return p->cdrom.sector_ready ? (p->cdrom.data_len >> 8) & 0xFF : 0;
        case 0x03: return p->cdrom.lba & 0xFF;
        case 0x04: return (p->cdrom.lba >> 8) & 0xFF;
        case 0x05: return (p->cdrom.lba >> 16) & 0xFF;
        case 0x06: return p->mem[MAILBOX_BASE + MBOX_STATUS];
        case 0x07: return p->mem[IFLAGS_ADDR];
        case 0x10: return ak8000_read_reg(&p->video, AK8000_REG_STATUS);
        case 0x11: return ak8000_read_reg(&p->video, AK8000_REG_CTRL);
        case 0x20: return p->controller;   // infrared controller state
        case 0x21: return p->mem[IFLAGS_ADDR]; // interrupt flags mirror
        default:   return 0xFF;
        }
    }
    if (addr >= CDROM_BASE) {
        // CD-ROM data window: expose data_ptr (skips raw sector headers)
        int offset = addr - CDROM_BASE;
        if (p->cdrom.sector_ready && p->cdrom.data_ptr
            && offset < p->cdrom.data_len)
            return p->cdrom.data_ptr[offset];
        return 0xFF;
    }
    return p->mem[addr];
}

void playdia_mem_write(Playdia *p, uint16_t addr, uint8_t val) {
    if (addr < ROM_SIZE) return;  // ROM is read-only

    if (addr >= IO_BASE && addr < IO_BASE + IO_SIZE) {
        uint8_t reg = (addr - IO_BASE) & 0xFF;
        uint8_t args[4] = {val, 0, 0, 0};
        switch (reg) {
        case 0x00: cdrom_write_cmd(&p->cdrom, val, args); break;
        // Mailbox write shortcuts (TLCS writes these to kick NEC)
        case 0x06: p->mem[MAILBOX_BASE + MBOX_CMD]  = val;
                   bios_hle_sync_mailbox(p); break;
        case 0x07: p->mem[MAILBOX_BASE + MBOX_ARG0] = val; break;
        case 0x08: p->mem[MAILBOX_BASE + MBOX_ARG1] = val; break;
        case 0x09: p->mem[MAILBOX_BASE + MBOX_ARG2] = val; break;
        // AK8000 registers
        case 0x10: ak8000_write_reg(&p->video, AK8000_REG_CTRL,     val); break;
        case 0x11: ak8000_write_reg(&p->video, AK8000_REG_VID_MODE, val); break;
        case 0x12: ak8000_write_reg(&p->video, AK8000_REG_AUD_CTRL, val); break;
        default: break;
        }
        return;
    }

    p->mem[addr] = val;
}

// ─────────────────────────────────────────────────────────────
//  Interactive FMV handler
//
//  Called each frame to process F2 commands from the AK8000.
//  Maps controller buttons to Playdia's 7 button slots:
//    B1 = default/timeout (no press or Start)
//    B2 = Up        B3 = Down
//    B4 = Left      B5 = Right
//    B6 = A         B7 = B
// ─────────────────────────────────────────────────────────────
static void playdia_handle_interactive(Playdia *p) {
    AK8000 *v = &p->video;

    if (!v->interactive_pending) return;

    uint8_t btn = p->controller;

    // ── F2 40 loop: button press breaks out ──────────────────
    if (v->is_loop && v->interactive_cmd == 0x40) {
        if (btn) {
            // Any button breaks the loop — continue streaming past the F2 40
            v->seek_target = 0;
            v->interactive_pending = false;
            v->is_loop = false;
            printf("[Interactive] BREAK LOOP @ LBA %u\n", v->cmd_lba);
        }
        // No button → seek_target already set, pipeline will loop
        return;
    }

    // ── Forward F2 40/60/90/A0: auto-seek, no input needed ──
    if (!v->waiting_for_input) return;

    // ── Waiting for player input (F2 44, F2 50) ─────────────
    int choice = -1;
    if (btn & BTN_UP)        choice = 1;  // B2
    else if (btn & BTN_DOWN) choice = 2;  // B3
    else if (btn & BTN_LEFT) choice = 3;  // B4
    else if (btn & BTN_RIGHT)choice = 4;  // B5
    else if (btn & BTN_A)    choice = 5;  // B6
    else if (btn & BTN_B)    choice = 6;  // B7
    else if (btn & BTN_START)choice = 0;  // B1 (default)

    if (choice >= 0) {
        v->seek_target = v->button_dest[choice];
        v->waiting_for_input = false;
        v->interactive_pending = false;
        printf("[Interactive] Button %d → LBA %u\n", choice + 1, v->seek_target);
        return;
    }

    // Countdown timeout
    if (v->input_timer > 0) {
        v->input_timer--;
        if (v->input_timer == 0) {
            if (v->timeout_dest > 0) {
                v->seek_target = v->timeout_dest;
                printf("[Interactive] TIMEOUT → LBA %u (F2 80 dest)\n", v->seek_target);
            } else {
                v->seek_target = v->button_dest[0];
                printf("[Interactive] TIMEOUT → LBA %u (B1 default)\n", v->seek_target);
            }
            v->waiting_for_input = false;
            v->interactive_pending = false;
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  Run one frame  (1/30 second)
// ─────────────────────────────────────────────────────────────
void playdia_run_frame(Playdia *p) {
    if (!p->running) return;

    int cycles_left = CYCLES_PER_FRAME;

    while (cycles_left > 0) {
        // HLE BIOS hook + interconnect hooks before CPU step
        int c;
        if (bios_hle_hook_tlcs(p) || interconnect_hook_tlcs(p))
            c = 4;   // hook cost: 1 NOP cycle
        else
            c = cpu_tlcs870_step(&p->cpu);

        cycles_left    -= c;
        p->master_cycles += (uint64_t)c;

        // NEC 78K: HLE + interconnect hooks + actual steps
        // NEC 12MHz vs TLCS 8MHz → ratio 1.5; run NEC step each time,
        // and an extra step when accumulated ratio demands it.
        bios_hle_hook_nec(p);
        if (!interconnect_hook_nec(p))
            cpu_nec78k_step(&p->io_cpu);
        // Extra NEC step every 2 TLCS instructions (approx 1.5 ratio)
        if (p->master_cycles % 8 < (uint64_t)c) {
            if (!interconnect_hook_nec(p))
                cpu_nec78k_step(&p->io_cpu);
        }
    }

    // ── Handle interactive FMV commands ─────────────────────
    playdia_handle_interactive(p);

    // End-of-frame: feed CD sectors → AK8000 via pipeline
    pipeline_run_frame(&p->pipe, &p->cdrom, &p->video);

    // Tick AK8000 (flush any remaining ES, swap buffers)
    ak8000_tick(&p->video);

    // Sync mailbox + deliver vsync/ctrl IRQs
    bios_hle_sync_mailbox(p);
    interconnect_tick(p);

    // Per-second stats
    if (p->debug_mode && p->frames % 30 == 0)
        pipeline_stats(&p->pipe);

    p->frames++;

    if (p->debug_mode && p->frames % 30 == 0)
        playdia_dump(p);
}

void playdia_set_button(Playdia *p, uint8_t btn, bool pressed) {
    if (pressed) p->controller |=  btn;
    else         p->controller &= ~btn;
}

void playdia_dump(Playdia *p) {
    printf("\n═══ Playdia State  [frame %u] ════════════════\n", p->frames);
    cpu_tlcs870_dump(&p->cpu);
    cpu_nec78k_dump (&p->io_cpu);
    printf("  CD-ROM: present=%d sector=%u ready=%d\n",
           p->cdrom.disc_present, p->cdrom.lba, p->cdrom.sector_ready);
    printf("  Video: active=%d frame=%u\n",
           p->video.video_active, p->video.frame_count);
    printf("  Controller: 0x%02X\n", p->controller);
    printf("════════════════════════════════════════════\n\n");
}
