#pragma once
#include "playdia.h"

/* ============================================================
 *  NEC µPD78214GC — 78K/II series CPU core (Playdia sub-CPU)
 *  12 MHz, handles CD-ROM control & I/O.
 *
 *  Reference: NEC 78K/II Instructions User's Manual
 *             (U10905EJ et al.); cross-checked against MAME
 *             `src/devices/cpu/upd78k/upd78k2d.cpp` (disassembler
 *             only — MAME has no 78K/II execution core).
 *
 *  Key architectural facts about 78K/II that differ from
 *  78K/0:
 *    - 1 MB physical address space, but the ISA is still 16-bit
 *      addressed: pages outside the current 64 KB window are
 *      reached via the external MM (Memory Mapping) SFR at
 *      0xFFC4.  Code does NOT carry segment registers like
 *      CS/ES — that was the 78K/III, IV.
 *    - **4 register banks** (RB0..RB3), selected by PSW.RBS1
 *      (bit 5) and PSW.RBS0 (bit 3).  RB0 lives at the HIGHEST
 *      IRAM offset (0xFEF8) and RB3 at the lowest (0xFEE0).
 *    - Extended instruction set vs 78K/0: MULU, DIVUW, BRK/RETB,
 *      [SP+disp8] / [HL+disp8] / [DE+disp8] addressing,
 *      `word[A]` / `word[B]` indexed forms, MOV saddr,saddr.
 *
 *  Register file (per RBS bank, 8 bytes):
 *    offset +0 = X   +1 = A   +2 = C   +3 = B
 *    offset +4 = E   +5 = D   +6 = L   +7 = H
 *  Pair registers (little-endian within the pair):
 *    AX = X:A   BC = C:B   DE = E:D   HL = L:H
 *
 *  PSW byte (bit 7..0):
 *    bit 7 = IE   (interrupt enable)
 *    bit 6 = Z    (zero)
 *    bit 5 = RBS1 (register-bank select bit 1)
 *    bit 4 = AC   (auxiliary carry)
 *    bit 3 = RBS0 (register-bank select bit 0)
 *    bit 2 = reserved (reads 0)
 *    bit 1 = ISP  (in-service priority)
 *    bit 0 = CY   (carry)
 * ============================================================ */

/* PSW bit masks (matches the on-chip PSW layout) */
#define NEC_CY    0x01
#define NEC_ISP   0x02
#define NEC_RBS0  0x08
#define NEC_AC    0x10
#define NEC_RBS1  0x20
#define NEC_Z     0x40
#define NEC_IE    0x80

/* Register indices within a bank (X=0..H=7, MAME 78K/0 order) */
enum {
    NEC78K_REG_X = 0,
    NEC78K_REG_A = 1,
    NEC78K_REG_C = 2,
    NEC78K_REG_B = 3,
    NEC78K_REG_E = 4,
    NEC78K_REG_D = 5,
    NEC78K_REG_L = 6,
    NEC78K_REG_H = 7,
};
/* Pair indices (low/high in the 2-byte slot) */
enum {
    NEC78K_PAIR_AX = 0,   /* X:A  → low=X, high=A — careful  */
    NEC78K_PAIR_BC = 1,
    NEC78K_PAIR_DE = 2,
    NEC78K_PAIR_HL = 3,
};

typedef struct CPU_NEC78K {
    /* PSW + scalar mirror of the current bank's GPRs.  Kept in
     * sync with RAM at the start of every step() so external
     * writes through cpu->r[i] are visible to opcode handlers,
     * and after every step() so external readers see the new
     * register state.  Banked RAM mirror lives at 0xFEE0..0xFEFF
     * within mem[].  */
    uint8_t  r[8];          /* active bank: X,A,C,B,E,D,L,H        */
    uint16_t PC, SP;
    uint8_t  PSW;
    bool     halted, stopped;
    uint8_t  bank;          /* 0..3, decoded from PSW.RBS{0,1}     */
    uint8_t  port[16];

    /* Memory window currently visible to the core. Always 64 KB
     * — the host (interconnect.c) decides what physical page is
     * mapped via the MM SFR at 0xFFC4.                            */
    uint8_t *mem;
    uint64_t cycles;
} CPU_NEC78K;

void cpu_nec78k_init  (CPU_NEC78K *cpu, uint8_t *mem);
void cpu_nec78k_reset (CPU_NEC78K *cpu);
int  cpu_nec78k_step  (CPU_NEC78K *cpu);
void cpu_nec78k_irq   (CPU_NEC78K *cpu, uint16_t vector_addr);
void cpu_nec78k_dump  (CPU_NEC78K *cpu);
