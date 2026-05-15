#pragma once
#include "playdia.h"

/* ============================================================
 *  Toshiba TLCS-870 — Playdia main CPU (8 MHz)
 *
 *  This is NOT a Z80.  The TLCS-870 has its own instruction set:
 *    - No A'/F' shadow register file
 *    - No CB / DD / FD prefix instructions
 *    - Register file lives in RAM, banked by PSW.RBS
 *    - 8-bit GPRs are paired as WA, BC, DE, HL
 *    - Flags: JF (Jump Flag), ZF (Zero), CF (Carry), HF (Half-carry)
 *
 *  Reference implementation: MAME `src/devices/cpu/tlcs870/`
 *  Toshiba documentation:    TLCS-870 / TLCS-870/C series
 *                            Hardware/Software User's Manual
 *
 *  Status: WORK IN PROGRESS skeleton with the correct
 *  TLCS-870 register architecture.  Opcode coverage is partial;
 *  unknown opcodes consume one cycle and pass through.  Replaces
 *  the earlier Z80-flavoured implementation, which was a wrong
 *  architecture entirely (see issue #4).
 * ============================================================ */

/* PSW flag bits */
#define FLAG_RBS 0x0F      /* register-bank select (bits 0..3)  */
#define FLAG_H   0x10      /* half-carry                        */
#define FLAG_C   0x20      /* carry                             */
#define FLAG_Z   0x40      /* zero                              */
#define FLAG_J   0x80      /* jump flag (compare/test result)   */

/* Legacy aliases retained for callers that haven't migrated yet
 * (interconnect.c, bios_hle.c, main.c).  Map onto TLCS-870 flags. */
#define FLAG_N   FLAG_H    /* no negative flag on TLCS-870      */
#define FLAG_V   FLAG_J    /* approximate ‘V’ ↔ jump-flag       */
#define FLAG_S   FLAG_J

/* Register-file indices within the active RBS bank (8 GPRs).
 *   TLCS-870 register ordering (low → high address):
 *     E, D, L, H, A, W, C, B
 *   Pair registers: WA (A+W), BC, DE, HL */
enum {
    TLCS870_REG_E = 0,
    TLCS870_REG_D = 1,
    TLCS870_REG_L = 2,
    TLCS870_REG_H = 3,
    TLCS870_REG_A = 4,
    TLCS870_REG_W = 5,
    TLCS870_REG_C = 6,
    TLCS870_REG_B = 7,
};

typedef struct CPU_TLCS870 {
    /* PSW with JF/ZF/CF/HF and RBS bank-select */
    uint8_t  PSW;

    /* "Convenience" mirror of the active bank's GPRs.  The
     *  bank is also kept in RAM at (0x40 + RBS*8) so saddr-style
     *  accesses still work transparently. */
    uint8_t  A, W, B, C, D, E, H, L;

    /* 16-bit address registers */
    uint16_t PC, SP;

    /* TLCS-870 has no Z80-style IX/IY index registers; if extension
     * variants need them they live in the GPR pair file (HL).
     * The fields below are kept only as a compatibility holdover. */
    uint16_t IX, IY;

    /* Compatibility aliases for callers that still use Z80 names */
    uint8_t  F;             /* alias for PSW (read-only mirror)  */
    uint8_t  I, R;          /* no real equivalents on TLCS-870   */

    bool     ime;           /* interrupt master enable           */
    bool     halted;
    uint8_t  irq_pending;
    uint64_t cycles;
    uint8_t *mem;
} CPU_TLCS870;

#define REG_BC(c)  ((uint16_t)((c)->B << 8 | (c)->C))
#define REG_DE(c)  ((uint16_t)((c)->D << 8 | (c)->E))
#define REG_HL(c)  ((uint16_t)((c)->H << 8 | (c)->L))
#define REG_WA(c)  ((uint16_t)((c)->W << 8 | (c)->A))

void cpu_tlcs870_init  (CPU_TLCS870 *cpu, uint8_t *mem);
void cpu_tlcs870_reset (CPU_TLCS870 *cpu);
int  cpu_tlcs870_step  (CPU_TLCS870 *cpu);
void cpu_tlcs870_irq   (CPU_TLCS870 *cpu, uint8_t vector);
void cpu_tlcs870_dump  (CPU_TLCS870 *cpu);
