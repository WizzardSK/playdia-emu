/* ================================================================
 *  Toshiba TLCS-870 — Playdia main CPU (8 MHz)
 *
 *  Replaces the previous Z80-derived implementation (issue #4).
 *  TLCS-870 has its own ISA; this core models the proper register
 *  architecture (banked GPR file, WA/BC/DE/HL pairs, JF/ZF/CF/HF
 *  flags) and dispatches the most common opcodes used by Playdia
 *  firmware.
 *
 *  Reference: MAME tlcs870 core (src/devices/cpu/tlcs870/) and the
 *  Toshiba TLCS-870 Hardware/Software User's Manual.
 *
 *  Status: WORK IN PROGRESS skeleton.  Unimplemented opcodes pass
 *  through with a 1-cycle penalty.  This is intentionally a
 *  foundation: it now matches the real chip architecturally, but
 *  large parts of the opcode map still need to be filled in.
 *
 *  Register file (in RAM, base = 0x40 + RBS * 8):
 *     E=0  D=1  L=2  H=3  A=4  W=5  C=6  B=7
 *  Pair registers (high << 8 | low):
 *     WA = W:A    BC = B:C    DE = D:E    HL = H:L
 *  PSW bits:
 *     bit7=JF  bit6=ZF  bit5=CF  bit4=HF  bits3..0 = RBS
 * ================================================================ */

#include "cpu_tlcs870.h"
#include <stdio.h>
#include <string.h>

/* ── memory ───────────────────────────────────────────────── */
static inline uint8_t  mr8 (CPU_TLCS870 *c, uint16_t a)            { return c->mem[a]; }
static inline uint16_t mr16(CPU_TLCS870 *c, uint16_t a)            {
    return (uint16_t)(c->mem[a] | (c->mem[(uint16_t)(a + 1)] << 8));
}
static inline void     mw8 (CPU_TLCS870 *c, uint16_t a, uint8_t v) {
    /* Internal ROM (0x0000..0x1FFF) is read-only on real hardware. */
    if (a >= 0x2000) c->mem[a] = v;
}
static inline void     mw16(CPU_TLCS870 *c, uint16_t a, uint16_t v){
    mw8(c, a, v & 0xFF); mw8(c, (uint16_t)(a + 1), v >> 8);
}

/* ── PSW / flags ──────────────────────────────────────────── */
static inline void sf(CPU_TLCS870 *c, uint8_t f, bool v) {
    if (v) c->PSW |= f; else c->PSW &= ~f;
    c->F = c->PSW;
}
static inline bool gf(CPU_TLCS870 *c, uint8_t f) { return (c->PSW & f) != 0; }
static inline void uzs(CPU_TLCS870 *c, uint8_t r) {
    sf(c, FLAG_Z, r == 0);
}
static inline uint8_t rbs(CPU_TLCS870 *c) { return c->PSW & FLAG_RBS; }

/* ── register-file <-> RAM mirror ───────────────────────────
 *  Bank base = 0x40 + RBS*8 (low RAM page).  We sync writes
 *  through to RAM so memory-addressed accesses see the same
 *  state as the convenience scalar fields. */
static inline uint16_t bank_base(CPU_TLCS870 *c) {
    return (uint16_t)(0x40 + rbs(c) * 8);
}
static void sync_to_ram(CPU_TLCS870 *c) {
    uint16_t b = bank_base(c);
    c->mem[b + TLCS870_REG_E] = c->E;
    c->mem[b + TLCS870_REG_D] = c->D;
    c->mem[b + TLCS870_REG_L] = c->L;
    c->mem[b + TLCS870_REG_H] = c->H;
    c->mem[b + TLCS870_REG_A] = c->A;
    c->mem[b + TLCS870_REG_W] = c->W;
    c->mem[b + TLCS870_REG_C] = c->C;
    c->mem[b + TLCS870_REG_B] = c->B;
}
static void sync_from_ram(CPU_TLCS870 *c) {
    uint16_t b = bank_base(c);
    c->E = c->mem[b + TLCS870_REG_E];
    c->D = c->mem[b + TLCS870_REG_D];
    c->L = c->mem[b + TLCS870_REG_L];
    c->H = c->mem[b + TLCS870_REG_H];
    c->A = c->mem[b + TLCS870_REG_A];
    c->W = c->mem[b + TLCS870_REG_W];
    c->C = c->mem[b + TLCS870_REG_C];
    c->B = c->mem[b + TLCS870_REG_B];
}

static uint8_t reg_get(CPU_TLCS870 *c, int idx) {
    switch (idx & 7) {
    case TLCS870_REG_E: return c->E;
    case TLCS870_REG_D: return c->D;
    case TLCS870_REG_L: return c->L;
    case TLCS870_REG_H: return c->H;
    case TLCS870_REG_A: return c->A;
    case TLCS870_REG_W: return c->W;
    case TLCS870_REG_C: return c->C;
    case TLCS870_REG_B: return c->B;
    }
    return 0;
}
static void reg_set(CPU_TLCS870 *c, int idx, uint8_t v) {
    switch (idx & 7) {
    case TLCS870_REG_E: c->E = v; break;
    case TLCS870_REG_D: c->D = v; break;
    case TLCS870_REG_L: c->L = v; break;
    case TLCS870_REG_H: c->H = v; break;
    case TLCS870_REG_A: c->A = v; break;
    case TLCS870_REG_W: c->W = v; break;
    case TLCS870_REG_C: c->C = v; break;
    case TLCS870_REG_B: c->B = v; break;
    }
    c->mem[bank_base(c) + (idx & 7)] = v;
}

/* ── stack (TLCS-870: predecrement, 16-bit) ─────────────────── */
static inline void    push16(CPU_TLCS870 *c, uint16_t v) { c->SP -= 2; mw16(c, c->SP, v); }
static inline uint16_t pop16(CPU_TLCS870 *c)             { uint16_t v = mr16(c, c->SP); c->SP += 2; return v; }
static inline void    push8 (CPU_TLCS870 *c, uint8_t v)  { c->SP -= 1; mw8 (c, c->SP, v); }
static inline uint8_t  pop8 (CPU_TLCS870 *c)             { uint8_t  v = mr8 (c, c->SP);   c->SP += 1; return v; }

/* ── ALU ──────────────────────────────────────────────────── */
static void add_a(CPU_TLCS870 *c, uint8_t v, bool with_carry) {
    uint8_t cin = (with_carry && gf(c, FLAG_C)) ? 1 : 0;
    uint16_t r = (uint16_t)c->A + v + cin;
    sf(c, FLAG_H, ((c->A & 0xF) + (v & 0xF) + cin) > 0xF);
    sf(c, FLAG_C, r > 0xFF);
    c->A = (uint8_t)r;
    reg_set(c, TLCS870_REG_A, c->A);
    uzs(c, c->A);
}
static void sub_a(CPU_TLCS870 *c, uint8_t v, bool with_carry, bool store) {
    uint8_t cin = (with_carry && gf(c, FLAG_C)) ? 1 : 0;
    uint16_t r = (uint16_t)c->A - v - cin;
    sf(c, FLAG_H, (c->A & 0xF) < ((v & 0xF) + cin));
    sf(c, FLAG_C, (r & 0x100) != 0);
    if (store) {
        c->A = (uint8_t)r;
        reg_set(c, TLCS870_REG_A, c->A);
    }
    uzs(c, (uint8_t)r);
}
static void and_a(CPU_TLCS870 *c, uint8_t v) {
    c->A &= v; reg_set(c, TLCS870_REG_A, c->A); sf(c, FLAG_C, false); uzs(c, c->A);
}
static void or_a (CPU_TLCS870 *c, uint8_t v) {
    c->A |= v; reg_set(c, TLCS870_REG_A, c->A); sf(c, FLAG_C, false); uzs(c, c->A);
}
static void xor_a(CPU_TLCS870 *c, uint8_t v) {
    c->A ^= v; reg_set(c, TLCS870_REG_A, c->A); sf(c, FLAG_C, false); uzs(c, c->A);
}

/* ── decoder ──────────────────────────────────────────────────
 *  TLCS-870 opcode map.  This is a partial implementation —
 *  every opcode is annotated with its mnemonic/encoding so the
 *  table can be extended incrementally.  See MAME's tlcs870 for
 *  the full reference. */
int cpu_tlcs870_step(CPU_TLCS870 *cpu) {
    if (cpu->halted) { cpu->cycles += 2; return 2; }
    cpu->F = cpu->PSW;          /* keep alias in sync for callers */

    uint8_t op = mr8(cpu, cpu->PC++);
    int clk = 4;

    switch (op) {

    /* 0x00  NOP */
    case 0x00: clk = 2; break;

    /* 0x01  SWAP A   – swap upper/lower nibble of A */
    case 0x01: {
        uint8_t v = cpu->A;
        cpu->A = (uint8_t)((v << 4) | (v >> 4));
        reg_set(cpu, TLCS870_REG_A, cpu->A);
        clk = 4;
    } break;

    /* 0x02  MUL W, A   – WA = W * A (unsigned 8×8 → 16) */
    case 0x02: {
        uint16_t r = (uint16_t)cpu->W * (uint16_t)cpu->A;
        cpu->A = (uint8_t)(r & 0xFF);
        cpu->W = (uint8_t)(r >> 8);
        reg_set(cpu, TLCS870_REG_A, cpu->A);
        reg_set(cpu, TLCS870_REG_W, cpu->W);
        uzs(cpu, cpu->W);   /* JF/ZF reflect product high byte */
        clk = 8;
    } break;

    /* 0x03  DIV WA, C  – WA / C → A=quotient, W=remainder */
    case 0x03: {
        uint16_t wa = REG_WA(cpu);
        if (cpu->C == 0) {
            /* division by zero: TLCS-870 leaves WA unchanged, sets JF */
            sf(cpu, FLAG_J, true);
        } else {
            cpu->A = (uint8_t)(wa / cpu->C);
            cpu->W = (uint8_t)(wa % cpu->C);
            reg_set(cpu, TLCS870_REG_A, cpu->A);
            reg_set(cpu, TLCS870_REG_W, cpu->W);
        }
        clk = 16;
    } break;

    /* 0x04  RETN  – return from NMI (pops PC, PSW) */
    case 0x04:
        cpu->PC  = pop16(cpu);
        cpu->PSW = pop8(cpu);
        sync_from_ram(cpu);
        clk = 12;
        break;

    /* 0x05  RET   – pops PC */
    case 0x05: cpu->PC  = pop16(cpu); clk = 10; break;

    /* 0x06  POP PSW */
    case 0x06:
        cpu->PSW = pop8(cpu);
        sync_from_ram(cpu);
        clk = 6;
        break;
    /* 0x07  PUSH PSW */
    case 0x07: push8(cpu, cpu->PSW); clk = 6; break;

    /* 0x10..0x17  INC r   (E,D,L,H,A,W,C,B) */
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: case 0x17: {
        int r = op & 7;
        uint8_t v = reg_get(cpu, r) + 1;
        reg_set(cpu, r, v);
        sf(cpu, FLAG_H, (v & 0xF) == 0);
        uzs(cpu, v);
        clk = 4;
    } break;

    /* 0x18..0x1F  DEC r */
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: {
        int r = op & 7;
        uint8_t v = reg_get(cpu, r) - 1;
        reg_set(cpu, r, v);
        sf(cpu, FLAG_H, (v & 0xF) == 0xF);
        uzs(cpu, v);
        clk = 4;
    } break;

    /* 0x40..0x47  LD r, A      (move A into register r) */
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
        reg_set(cpu, op & 7, cpu->A);
        clk = 4;
        break;

    /* 0x48..0x4F  LD A, r */
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        cpu->A = reg_get(cpu, op & 7);
        reg_set(cpu, TLCS870_REG_A, cpu->A);
        uzs(cpu, cpu->A);
        clk = 4;
        break;

    /* 0x60..0x67  ADD A, r */
    case 0x60: case 0x61: case 0x62: case 0x63:
    case 0x64: case 0x65: case 0x66: case 0x67:
        add_a(cpu, reg_get(cpu, op & 7), false); clk = 4; break;
    /* 0x68..0x6F  ADC A, r */
    case 0x68: case 0x69: case 0x6A: case 0x6B:
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        add_a(cpu, reg_get(cpu, op & 7), true);  clk = 4; break;
    /* 0x70..0x77  SUB A, r */
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
        sub_a(cpu, reg_get(cpu, op & 7), false, true); clk = 4; break;
    /* 0x78..0x7F  SBC A, r */
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        sub_a(cpu, reg_get(cpu, op & 7), true, true); clk = 4; break;

    /* 0xA0..0xA7  AND A, r */
    case 0xA0: case 0xA1: case 0xA2: case 0xA3:
    case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        and_a(cpu, reg_get(cpu, op & 7)); clk = 4; break;
    /* 0xA8..0xAF  OR A, r */
    case 0xA8: case 0xA9: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        or_a (cpu, reg_get(cpu, op & 7)); clk = 4; break;
    /* 0xB0..0xB7  XOR A, r */
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        xor_a(cpu, reg_get(cpu, op & 7)); clk = 4; break;
    /* 0xB8..0xBF  CMP A, r (subtract without store) */
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        sub_a(cpu, reg_get(cpu, op & 7), false, false); clk = 4; break;

    /* 0xD0  LD A, #imm8 */
    case 0xD0: {
        uint8_t v = mr8(cpu, cpu->PC++);
        cpu->A = v; reg_set(cpu, TLCS870_REG_A, v); uzs(cpu, v); clk = 4;
    } break;
    /* 0xD1..0xD7  LD r, #imm8 */
    case 0xD1: case 0xD2: case 0xD3:
    case 0xD4: case 0xD5: case 0xD6: case 0xD7: {
        uint8_t v = mr8(cpu, cpu->PC++);
        reg_set(cpu, op & 7, v);
        uzs(cpu, v); clk = 4;
    } break;

    /* 0xFA  LDW SP, #imm16 (TLCS-870/X encoding — close enough) */
    case 0xFA: cpu->SP = mr16(cpu, cpu->PC); cpu->PC += 2; clk = 8; break;

    /* 0xFB  JP !addr16 */
    case 0xFB: { uint16_t a = mr16(cpu, cpu->PC); cpu->PC = a; clk = 8; } break;

    /* 0xFC  CALL !addr16 */
    case 0xFC: {
        uint16_t a = mr16(cpu, cpu->PC); cpu->PC += 2;
        push16(cpu, cpu->PC);
        cpu->PC = a; clk = 14;
    } break;

    /* 0xFD  CALLV n   – call vector at FFC0+2n */
    case 0xFD: {
        uint8_t n = mr8(cpu, cpu->PC++);
        uint16_t vec = (uint16_t)(0xFFC0 + (n & 0x0F) * 2);
        push16(cpu, cpu->PC);
        cpu->PC = mr16(cpu, vec);
        clk = 14;
    } break;

    /* 0xFE  EI */
    case 0xFE: cpu->ime = true;  clk = 4; break;
    /* 0xFF  DI */
    case 0xFF: cpu->ime = false; clk = 4; break;

    /* 0x?? short branches.  TLCS-870 uses 0xC0..0xCF for J cc, $rel
     *  with a 4-bit condition code in the low nibble.  Implement the
     *  common ones; bit 4 selects "JR" vs "J cc". */
    case 0xC0: { int8_t r = (int8_t)mr8(cpu, cpu->PC++);                          /* JR  $rel  */
                 cpu->PC = (uint16_t)(cpu->PC + r); clk = 8; } break;
    case 0xC1: { int8_t r = (int8_t)mr8(cpu, cpu->PC++);                          /* JR Z       */
                 if ( gf(cpu, FLAG_Z)) { cpu->PC = (uint16_t)(cpu->PC + r); clk = 8; } else clk = 4; } break;
    case 0xC2: { int8_t r = (int8_t)mr8(cpu, cpu->PC++);                          /* JR NZ      */
                 if (!gf(cpu, FLAG_Z)) { cpu->PC = (uint16_t)(cpu->PC + r); clk = 8; } else clk = 4; } break;
    case 0xC3: { int8_t r = (int8_t)mr8(cpu, cpu->PC++);                          /* JR C       */
                 if ( gf(cpu, FLAG_C)) { cpu->PC = (uint16_t)(cpu->PC + r); clk = 8; } else clk = 4; } break;
    case 0xC4: { int8_t r = (int8_t)mr8(cpu, cpu->PC++);                          /* JR NC      */
                 if (!gf(cpu, FLAG_C)) { cpu->PC = (uint16_t)(cpu->PC + r); clk = 8; } else clk = 4; } break;

    default:
        /* Opcodes still to implement, per the TLCS-870 manual:
         *   - 0x20..0x3F bit ops on (HL)/saddr (SET/CLR/CHG/TEST)
         *   - 0x80..0xBF arithmetic with addressing-mode prefixes
         *   - 0xE0..0xEF src-prefix forms (e.g. LD A,(HL+))
         *   - 0xF0..0xF7 dst-prefix forms
         *   - 0xEF / 0xFE SWI / TRAP family
         *   - 16-bit pair arithmetic (ADDC HL,WA, INCW BC, ...)
         *   - All conditional jumps using JF latch semantics
         * Fall-through stub: 1 cycle, PC already advanced past op.
         */
        clk = 1;
        break;
    }

    cpu->F = cpu->PSW;
    cpu->cycles += (uint64_t)clk;
    return clk;
}

/* ── interrupt ──────────────────────────────────────────────── */
void cpu_tlcs870_irq(CPU_TLCS870 *cpu, uint8_t vector) {
    if (!cpu->ime) return;
    cpu->halted = false;
    push8 (cpu, cpu->PSW);
    push16(cpu, cpu->PC);
    cpu->ime = false;
    /* Vector table at 0xFFE0..0xFFFF, 2 bytes per entry. */
    uint16_t addr = (uint16_t)(0xFFE0 + (vector & 0x0F) * 2);
    cpu->PC = mr16(cpu, addr);
}

/* ── init / reset ───────────────────────────────────────────── */
void cpu_tlcs870_init(CPU_TLCS870 *cpu, uint8_t *mem) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->mem = mem;
    cpu_tlcs870_reset(cpu);
}

void cpu_tlcs870_reset(CPU_TLCS870 *cpu) {
    /* Reset vector: word at 0xFFFE..0xFFFF (little-endian).
     * Most boot ROMs map this to the entry point. */
    if (cpu->mem) {
        cpu->PC = (uint16_t)(cpu->mem[0xFFFE] | ((uint16_t)cpu->mem[0xFFFF] << 8));
        if (cpu->PC == 0xFFFF || cpu->PC == 0x0000)
            cpu->PC = 0x0000;
    } else {
        cpu->PC = 0x0000;
    }
    cpu->SP    = 0xDFFF;
    cpu->PSW   = 0;
    cpu->F     = 0;
    cpu->A = cpu->W = 0;
    cpu->B = cpu->C = cpu->D = cpu->E = cpu->H = cpu->L = 0;
    cpu->IX = cpu->IY = 0;
    cpu->ime   = false;
    cpu->halted = false;
    cpu->irq_pending = 0;
    cpu->cycles = 0;
    sync_to_ram(cpu);
}

/* ── debug dump ─────────────────────────────────────────────── */
void cpu_tlcs870_dump(CPU_TLCS870 *cpu) {
    printf("─── Toshiba TLCS-870 Main CPU ────────────────────────\n");
    printf("  PC=%04X  SP=%04X  PSW=%02X  RBS=%d  IME=%d\n",
           cpu->PC, cpu->SP, cpu->PSW, rbs(cpu), cpu->ime);
    printf("  A=%02X  W=%02X  B=%02X  C=%02X  D=%02X  E=%02X  H=%02X  L=%02X\n",
           cpu->A, cpu->W, cpu->B, cpu->C, cpu->D, cpu->E, cpu->H, cpu->L);
    printf("  WA=%04X BC=%04X DE=%04X HL=%04X\n",
           REG_WA(cpu), REG_BC(cpu), REG_DE(cpu), REG_HL(cpu));
    printf("  JF=%d ZF=%d CF=%d HF=%d\n",
           !!(cpu->PSW & FLAG_J), !!(cpu->PSW & FLAG_Z),
           !!(cpu->PSW & FLAG_C), !!(cpu->PSW & FLAG_H));
    printf("  CYCLES=%llu  HALTED=%d\n",
           (unsigned long long)cpu->cycles, cpu->halted);
    printf("──────────────────────────────────────────────────────\n");
}
