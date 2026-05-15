/* ================================================================
 *  Toshiba TLCS-870 — Playdia main CPU (8 MHz)
 *
 *  C port of the MAME TLCS-870 core, originally written by
 *  David Haywood as part of MAME (BSD-3-Clause). Translated from
 *  the C++ sources in src/devices/cpu/tlcs870/ (tlcs870.cpp +
 *  tlcs870_ops*.cpp) with MAME framework dependencies removed.
 *  See LICENSE / README for full attribution.
 *
 *  Original copyright-holders: David Haywood (MAME, BSD-3-Clause).
 *  C port adaptations: WizzardSK (issue #4 follow-up).
 *
 *  The Playdia main CPU is a TLCS-870 variant; this core is the
 *  generic TLCS-870 ISA with no on-chip-peripheral wiring. SFR
 *  reads/writes go through the host memory[] array, so the host
 *  side is free to intercept them as needed.
 *
 *  Register file lives in the CPU's RAM mirror (interconnect.c
 *  provides one 64 KB block); for any given RBS bank, the eight
 *  GPRs map to 0x40 + RBS*8 + {A=0,W=1,C=2,B=3,E=4,D=5,L=6,H=7}.
 *  Register pairs (low/high): WA=A/W, BC=C/B, DE=E/D, HL=L/H.
 *  PSW byte layout: F bits 7..4 = JF,ZF,CF,HF ; bits 3..0 = RBS.
 * ================================================================ */

#include "cpu_tlcs870.h"
#include <stdio.h>
#include <string.h>

/* ── flag aliases internal to this file ───────────────────── */
#define JF_BIT   FLAG_J   /* 0x80 */
#define ZF_BIT   FLAG_Z   /* 0x40 */
#define CF_BIT   FLAG_C   /* 0x20 */
#define HF_BIT   FLAG_H   /* 0x10 */

/* ── register indices (MAME REG_A..REG_H) ─────────────────── */
#define REG_A  TLCS870_REG_A   /* 4 */
#define REG_W  TLCS870_REG_W   /* 5 */
#define REG_C  TLCS870_REG_C   /* 6 */
#define REG_B  TLCS870_REG_B   /* 7 */
#define REG_E  TLCS870_REG_E   /* 0 */
#define REG_D  TLCS870_REG_D   /* 1 */
#define REG_L  TLCS870_REG_L   /* 2 */
#define REG_H  TLCS870_REG_H   /* 3 */

/* 16-bit pair indices (encoded in low 2 bits of opcode):
 *  0=WA  1=BC  2=DE  3=HL                                       */
#define PAIR_WA 0
#define PAIR_BC 1
#define PAIR_DE 2
#define PAIR_HL 3

/* ── address-mode enum (src/dst prefix decode) ───────────── */
enum {
    AM_IMM_X        = 0,   /* (x)     direct byte address       */
    AM_PC_PLUS_A    = 1,   /* (PC+A)  ROM relative              */
    AM_DE           = 2,   /* (DE)                              */
    AM_HL           = 3,   /* (HL)                              */
    AM_HL_PLUS_D    = 4,   /* (HL+d)  signed disp byte          */
    AM_HL_PLUS_C    = 5,   /* (HL+C)                            */
    AM_HL_INC       = 6,   /* (HL+)   post-increment            */
    AM_DEC_HL       = 7,   /* (-HL)   pre-decrement             */
};

/* condition codes for JR cc, used by JR cc,a (D0..D7)         */
enum {
    COND_EQ_Z = 0,
    COND_NE_NZ,
    COND_LT_CS,
    COND_GE_CC,
    COND_LE,
    COND_GT,
    COND_T,
    COND_F,
};

/* ── basic memory helpers (operate on the host's flat 64KB) ─ */
static inline uint8_t  rm8 (CPU_TLCS870 *c, uint16_t a) { return c->mem[a]; }
static inline uint16_t rm16(CPU_TLCS870 *c, uint16_t a) {
    return (uint16_t)(c->mem[a] | ((uint16_t)c->mem[(uint16_t)(a + 1)] << 8));
}
static inline void wm8(CPU_TLCS870 *c, uint16_t a, uint8_t v) {
    /* Internal ROM (0x0000..0x1FFF on Playdia) is read-only.    */
    if (a >= 0x2000) c->mem[a] = v;
}
static inline void wm16(CPU_TLCS870 *c, uint16_t a, uint16_t v) {
    wm8(c, a, v & 0xFF);
    wm8(c, (uint16_t)(a + 1), v >> 8);
}

/* ── instruction-stream fetch ───────────────────────────────
 *  MAME models a separate `m_addr` cursor that advances
 *  through reads.  In this port we just bump PC directly. */
static inline uint8_t  read8 (CPU_TLCS870 *c) { return c->mem[c->PC++]; }
static inline uint16_t read16(CPU_TLCS870 *c) {
    uint16_t lo = c->mem[c->PC++];
    uint16_t hi = c->mem[c->PC++];
    return (uint16_t)(lo | (hi << 8));
}

/* ── stack (TLCS-870 convention)
 *  Push: WM at SP, then SP--.  Pop: SP++, then RM at SP.
 *  16-bit push: WM16 at SP-1, then SP -= 2.  Pop16: SP += 2,
 *  RM16 at SP-1.  SP points one byte BELOW the topmost item. */
static inline void push8(CPU_TLCS870 *c, uint8_t v) {
    wm8(c, c->SP, v); c->SP -= 1;
}
static inline uint8_t pop8(CPU_TLCS870 *c) {
    c->SP += 1; return rm8(c, c->SP);
}
static inline void push16(CPU_TLCS870 *c, uint16_t v) {
    wm16(c, (uint16_t)(c->SP - 1), v); c->SP -= 2;
}
static inline uint16_t pop16(CPU_TLCS870 *c) {
    c->SP += 2; return rm16(c, (uint16_t)(c->SP - 1));
}

/* ── PSW / flags ──────────────────────────────────────────── */
static inline uint8_t rbs_get(const CPU_TLCS870 *c) { return c->PSW & 0x0F; }
static inline void rbs_set(CPU_TLCS870 *c, uint8_t v) {
    c->PSW = (c->PSW & 0xF0) | (v & 0x0F);
}
static inline uint8_t psw_get(const CPU_TLCS870 *c) { return c->PSW; }
static inline void    psw_set(CPU_TLCS870 *c, uint8_t v) {
    c->PSW = v;
    c->F   = v;             /* keep legacy alias in sync           */
}
static inline void set_flag  (CPU_TLCS870 *c, uint8_t m) { psw_set(c, c->PSW |  m); }
static inline void clear_flag(CPU_TLCS870 *c, uint8_t m) { psw_set(c, c->PSW & ~m); }
static inline void put_flag  (CPU_TLCS870 *c, uint8_t m, int cond) {
    if (cond) psw_set(c, c->PSW |  m);
    else      psw_set(c, c->PSW & ~m);
}
static inline int  is_flag(const CPU_TLCS870 *c, uint8_t m) { return (c->PSW & m) != 0; }

#define set_JF(c)    set_flag  ((c), JF_BIT)
#define clear_JF(c)  clear_flag((c), JF_BIT)
#define set_ZF(c)    set_flag  ((c), ZF_BIT)
#define clear_ZF(c)  clear_flag((c), ZF_BIT)
#define set_CF(c)    set_flag  ((c), CF_BIT)
#define clear_CF(c)  clear_flag((c), CF_BIT)
#define set_HF(c)    set_flag  ((c), HF_BIT)
#define clear_HF(c)  clear_flag((c), HF_BIT)
#define is_JF(c)     is_flag   ((c), JF_BIT)
#define is_ZF(c)     is_flag   ((c), ZF_BIT)
#define is_CF(c)     is_flag   ((c), CF_BIT)
#define is_HF(c)     is_flag   ((c), HF_BIT)

/* ── register-file accessors ────────────────────────────────
 *  Bank base = 0x40 + RBS*8.  8-bit indices: A=0 W=1 C=2 B=3
 *  E=4 D=5 L=6 H=7 (this differs from the on-disk order; MAME's
 *  REG_A..REG_H map to (0,1,2,3,4,5,6,7) using the same scheme). */
static inline uint16_t bank_base(const CPU_TLCS870 *c) {
    return (uint16_t)(0x40 + rbs_get(c) * 8);
}
static inline uint8_t get_reg8(CPU_TLCS870 *c, int idx) {
    return c->mem[bank_base(c) + (idx & 7)];
}
static inline void set_reg8(CPU_TLCS870 *c, int idx, uint8_t v) {
    c->mem[bank_base(c) + (idx & 7)] = v;
}
/* 16-bit pair: low byte at index*2, high byte at index*2+1 */
static inline uint16_t get_reg16(CPU_TLCS870 *c, int pair) {
    uint16_t base = bank_base(c);
    return (uint16_t)(c->mem[base + (pair & 3) * 2] |
                      ((uint16_t)c->mem[base + (pair & 3) * 2 + 1] << 8));
}
static inline void set_reg16(CPU_TLCS870 *c, int pair, uint16_t v) {
    uint16_t base = bank_base(c);
    c->mem[base + (pair & 3) * 2]     = v & 0xFF;
    c->mem[base + (pair & 3) * 2 + 1] = v >> 8;
}

/* Refresh the cached A/W/B/C/D/E/H/L scalar fields after a
 * bank switch or memory-side update.  Cheap: 8 byte reads.   */
static void sync_cache(CPU_TLCS870 *c) {
    uint16_t b = bank_base(c);
    c->A = c->mem[b + 0];
    c->W = c->mem[b + 1];
    c->C = c->mem[b + 2];
    c->B = c->mem[b + 3];
    c->E = c->mem[b + 4];
    c->D = c->mem[b + 5];
    c->L = c->mem[b + 6];
    c->H = c->mem[b + 7];
}
/* Inverse: push the scalar fields into the bank's RAM mirror. */
static void flush_cache(CPU_TLCS870 *c) {
    uint16_t b = bank_base(c);
    c->mem[b + 0] = c->A;
    c->mem[b + 1] = c->W;
    c->mem[b + 2] = c->C;
    c->mem[b + 3] = c->B;
    c->mem[b + 4] = c->E;
    c->mem[b + 5] = c->D;
    c->mem[b + 6] = c->L;
    c->mem[b + 7] = c->H;
}

/* The opcode handlers mutate the register file via set_reg8/16
 *  which writes RAM directly.  At the end of each step we resync
 *  the scalar mirror so external callers (bios_hle.c) see the
 *  same A/B/C/... they used to.  This costs ~8 byte reads per
 *  instruction step. */

/* ── ALU primitives (MAME do_add_8bit etc., with the JF rule
 *    matched to the spec's flag-effect table: JF = CF for
 *    ADD/SUB/ADDC/SUBB, JF = ZF for AND/XOR/OR/CMP/INC/DEC) ── */
static uint8_t do_add_8bit(CPU_TLCS870 *c, uint16_t p1, uint16_t p2) {
    uint16_t r = (uint16_t)(p1 + p2);
    put_flag(c, CF_BIT, r & 0x100);
    put_flag(c, ZF_BIT, (r & 0xFF) == 0);
    put_flag(c, HF_BIT, ((p1 & 0xF) + (p2 & 0xF)) & 0x10);
    put_flag(c, JF_BIT, is_CF(c));      /* JF = CF (spec)         */
    return (uint8_t)r;
}
static uint8_t do_sub_8bit(CPU_TLCS870 *c, uint16_t p1, uint16_t p2) {
    uint16_t r = (uint16_t)(p1 - p2);
    put_flag(c, CF_BIT, p1 < p2);
    put_flag(c, HF_BIT, (p1 & 0xF) < (p2 & 0xF));
    put_flag(c, ZF_BIT, (r & 0xFF) == 0);
    put_flag(c, JF_BIT, is_CF(c));      /* JF = CF (spec)         */
    return (uint8_t)r;
}
static void do_cmp_8bit(CPU_TLCS870 *c, uint16_t p1, uint16_t p2) {
    put_flag(c, CF_BIT, p1 < p2);
    put_flag(c, HF_BIT, (p1 & 0xF) < (p2 & 0xF));
    put_flag(c, ZF_BIT, p1 == p2);
    put_flag(c, JF_BIT, is_ZF(c));      /* JF = ZF for CMP        */
}
static uint16_t do_add_16bit(CPU_TLCS870 *c, uint32_t p1, uint32_t p2) {
    uint32_t r = p1 + p2;
    put_flag(c, CF_BIT, r & 0x10000);
    put_flag(c, ZF_BIT, (r & 0xFFFF) == 0);
    /* HF on 16-bit ADD is documented "undefined"; MAME uses byte
     * 7→8 carry. */
    put_flag(c, HF_BIT, ((p1 & 0xFF) + (p2 & 0xFF)) & 0x100);
    put_flag(c, JF_BIT, is_CF(c));
    return (uint16_t)r;
}
static uint16_t do_sub_16bit(CPU_TLCS870 *c, uint32_t p1, uint32_t p2) {
    uint32_t r = p1 - p2;
    put_flag(c, CF_BIT, p1 < p2);
    put_flag(c, HF_BIT, (p1 & 0xFF) < (p2 & 0xFF));
    put_flag(c, ZF_BIT, (r & 0xFFFF) == 0);
    put_flag(c, JF_BIT, is_CF(c));
    return (uint16_t)r;
}
static void do_cmp_16bit(CPU_TLCS870 *c, uint32_t p1, uint32_t p2) {
    put_flag(c, CF_BIT, p1 < p2);
    put_flag(c, HF_BIT, (p1 & 0xFF) < (p2 & 0xFF));
    put_flag(c, ZF_BIT, p1 == p2);
    put_flag(c, JF_BIT, is_ZF(c));
}

static uint16_t do_and(CPU_TLCS870 *c, uint16_t p1, uint16_t p2) {
    uint16_t r = p1 & p2;
    put_flag(c, ZF_BIT, r == 0);
    put_flag(c, JF_BIT, is_ZF(c));
    return r;
}
static uint16_t do_or(CPU_TLCS870 *c, uint16_t p1, uint16_t p2) {
    uint16_t r = p1 | p2;
    put_flag(c, ZF_BIT, r == 0);
    put_flag(c, JF_BIT, is_ZF(c));
    return r;
}
static uint16_t do_xor(CPU_TLCS870 *c, uint16_t p1, uint16_t p2) {
    uint16_t r = p1 ^ p2;
    put_flag(c, ZF_BIT, r == 0);
    put_flag(c, JF_BIT, is_ZF(c));
    return r;
}

/* 8-bit ALU dispatch (op 0..7) */
static uint8_t do_alu_8bit(CPU_TLCS870 *c, int op, uint16_t p1, uint16_t p2) {
    uint8_t r = 0;
    switch (op & 7) {
    case 0: p2 = (uint16_t)(p2 + (is_CF(c) ? 1 : 0));
            r = do_add_8bit(c, p1, p2); break;             /* ADDC */
    case 1: r = do_add_8bit(c, p1, p2); break;             /* ADD  */
    case 2: p2 = (uint16_t)(p2 + (is_CF(c) ? 1 : 0));
            r = do_sub_8bit(c, p1, p2); break;             /* SUBB */
    case 3: r = do_sub_8bit(c, p1, p2); break;             /* SUB  */
    case 4: r = (uint8_t)do_and(c, p1, p2); break;         /* AND  */
    case 5: r = (uint8_t)do_xor(c, p1, p2); break;         /* XOR  */
    case 6: r = (uint8_t)do_or (c, p1, p2); break;         /* OR   */
    case 7: do_cmp_8bit(c, p1, p2); break;                 /* CMP  */
    }
    return r;
}
static uint16_t do_alu_16bit(CPU_TLCS870 *c, int op, uint32_t p1, uint32_t p2) {
    uint16_t r = 0;
    switch (op & 7) {
    case 0: p2 = p2 + (is_CF(c) ? 1 : 0);
            r = do_add_16bit(c, p1, p2); break;
    case 1: r = do_add_16bit(c, p1, p2); break;
    case 2: p2 = p2 + (is_CF(c) ? 1 : 0);
            r = do_sub_16bit(c, p1, p2); break;
    case 3: r = do_sub_16bit(c, p1, p2); break;
    case 4: r = do_and(c, (uint16_t)p1, (uint16_t)p2); break;
    case 5: r = do_xor(c, (uint16_t)p1, (uint16_t)p2); break;
    case 6: r = do_or (c, (uint16_t)p1, (uint16_t)p2); break;
    case 7: do_cmp_16bit(c, p1, p2); break;
    }
    return r;
}

/* shifts / rotates */
static uint8_t handle_SHLC(CPU_TLCS870 *c, uint8_t v) {
    put_flag(c, CF_BIT, v & 0x80);
    v = (uint8_t)(v << 1);
    put_flag(c, JF_BIT, is_CF(c));
    put_flag(c, ZF_BIT, v == 0);
    return v;
}
static uint8_t handle_SHRC(CPU_TLCS870 *c, uint8_t v) {
    put_flag(c, CF_BIT, v & 0x01);
    v = (uint8_t)(v >> 1);
    put_flag(c, JF_BIT, is_CF(c));
    put_flag(c, ZF_BIT, v == 0);
    return v;
}
static uint8_t handle_ROLC(CPU_TLCS870 *c, uint8_t v) {
    int tmp = is_CF(c) ? 1 : 0;
    put_flag(c, CF_BIT, v & 0x80);
    v = (uint8_t)((v << 1) | tmp);
    put_flag(c, JF_BIT, is_CF(c));
    put_flag(c, ZF_BIT, v == 0);
    return v;
}
static uint8_t handle_RORC(CPU_TLCS870 *c, uint8_t v) {
    int tmp = is_CF(c) ? 0x80 : 0;
    put_flag(c, CF_BIT, v & 0x01);
    v = (uint8_t)((v >> 1) | tmp);
    put_flag(c, JF_BIT, is_CF(c));
    put_flag(c, ZF_BIT, v == 0);
    return v;
}
static uint8_t handle_DAA(CPU_TLCS870 *c, uint8_t v) {
    if ((v & 0xF) > 9 || is_HF(c)) { v = (uint8_t)(v + 0x06); set_HF(c); } else clear_HF(c);
    if (v > 0x9F           || is_CF(c)) { v = (uint8_t)(v + 0x60); set_CF(c); } else clear_CF(c);
    put_flag(c, ZF_BIT, v == 0);
    put_flag(c, JF_BIT, is_CF(c));
    return v;
}
static uint8_t handle_DAS(CPU_TLCS870 *c, uint8_t v) {
    if ((v & 0xF) > 9 || is_HF(c)) { v = (uint8_t)(v - 0x06); set_HF(c); } else clear_HF(c);
    if (v > 0x9F           || is_CF(c)) { v = (uint8_t)(v - 0x60); set_CF(c); } else clear_CF(c);
    put_flag(c, ZF_BIT, v == 0);
    put_flag(c, JF_BIT, is_CF(c));
    return v;
}
static void handle_swap(CPU_TLCS870 *c, int reg) {
    uint8_t v = get_reg8(c, reg);
    v = (uint8_t)((v << 4) | (v >> 4));
    set_reg8(c, reg, v);
    set_JF(c);
}
static void handle_mul(CPU_TLCS870 *c, int pair) {
    uint16_t v = get_reg16(c, pair);
    uint16_t r = (uint16_t)((v & 0xFF) * ((v >> 8) & 0xFF));
    set_reg16(c, pair, r);
    put_flag(c, ZF_BIT, (r & 0xFF00) == 0);
    put_flag(c, JF_BIT, is_ZF(c));
}
static void handle_div(CPU_TLCS870 *c, int pair) {
    uint16_t num = get_reg16(c, pair);
    uint8_t  den = get_reg8(c, REG_C);
    if (!den) {
        set_CF(c);                      /* divide by zero        */
        return;
    }
    uint16_t q = (uint16_t)(num / den);
    uint8_t  rm= (uint8_t)(num % den);
    set_reg16(c, pair, (uint16_t)((rm << 8) | (q & 0xFF)));
    put_flag(c, CF_BIT, q & 0xFF00);
    put_flag(c, ZF_BIT, rm == 0);
    put_flag(c, JF_BIT, is_ZF(c));
}

/* condition-code evaluator (D0..D7 / JR cc,a) */
static int check_jr_cond(CPU_TLCS870 *c, int cc) {
    switch (cc & 7) {
    case COND_EQ_Z:  return  is_ZF(c);
    case COND_NE_NZ: return !is_ZF(c);
    case COND_LT_CS: return  is_CF(c);
    case COND_GE_CC: return !is_CF(c);
    case COND_LE:    return  is_CF(c) || is_ZF(c);
    case COND_GT:    return !(is_CF(c) || is_ZF(c));
    case COND_T:     return  is_JF(c);
    case COND_F:     return !is_JF(c);
    }
    return 0;
}

/* ── src/dst address-mode resolution (E0..E7 / F0..F7) ─────
 *  Resolves to a 16-bit effective address.  For modes that
 *  read an immediate byte after the prefix opcode the caller
 *  must supply `imm`; for register-pair modes `imm` is ignored.
 *  The base-cycle adjustment table is `cyc_base[]` below.       */
static const uint8_t cyc_base_src[] = { 1, 2, 0, 0, 2, 2, 1, 1 };
static const uint8_t cyc_base_dst[] = { 1, 0, 0, 0, 2, 0, 1, 1 };

static uint16_t get_addr(CPU_TLCS870 *c, int mode, uint8_t imm,
                         uint16_t op_start_pc) {
    switch (mode & 7) {
    case AM_IMM_X:      return imm;
    case AM_PC_PLUS_A:  return (uint16_t)(op_start_pc + 2 + get_reg8(c, REG_A));
    case AM_DE:         return get_reg16(c, PAIR_DE);
    case AM_HL:         return get_reg16(c, PAIR_HL);
    case AM_HL_PLUS_D:  return (uint16_t)(get_reg16(c, PAIR_HL) + (int8_t)imm);
    case AM_HL_PLUS_C:  return (uint16_t)(get_reg16(c, PAIR_HL) + get_reg8(c, REG_C));
    case AM_HL_INC: {
        uint16_t hl = get_reg16(c, PAIR_HL);
        set_reg16(c, PAIR_HL, (uint16_t)(hl + 1));
        return hl;
    }
    case AM_DEC_HL: {
        uint16_t hl = (uint16_t)(get_reg16(c, PAIR_HL) - 1);
        set_reg16(c, PAIR_HL, hl);
        return hl;
    }
    }
    return 0;
}

/* ─── interrupt vector table accessor ────────────────────── */
static uint16_t irq_vector_addr(int priority) {
    return (uint16_t)(0xFFE0 + (15 - priority) * 2);
}

/* ── forward decls for prefix sub-decoders ─────────────── */
static int decode_e0_e7(CPU_TLCS870 *c, uint8_t op0, uint16_t op_start_pc);
static int decode_e8_ef(CPU_TLCS870 *c, uint8_t op0);
static int decode_f0_f7(CPU_TLCS870 *c, uint8_t op0, uint16_t op_start_pc);

/* ====================================================================
 *  Main decode (primary opcode byte)
 * ==================================================================== */
int cpu_tlcs870_step(CPU_TLCS870 *cpu) {
    if (cpu->halted) { cpu->cycles += 2; return 2; }
    cpu->F = cpu->PSW;     /* keep alias in sync                   */

    /* Capture any scalar writes done by external code (bios_hle,
     * interconnect) into the RAM register-file mirror before this
     * instruction reads from it. */
    flush_cache(cpu);

    uint16_t op_pc = cpu->PC;
    uint8_t op = read8(cpu);
    int clk = 1;

    switch (op) {

    case 0x00: clk = 1; break;                                /* NOP   */

    case 0x01: handle_swap(cpu, REG_A); clk = 3; break;       /* SWAP A */

    case 0x02: handle_mul(cpu, PAIR_WA); clk = 7; break;      /* MUL W,A */

    case 0x03: handle_div(cpu, PAIR_WA); clk = 7; break;      /* DIV WA,C */

    /* RETI: pop PC, then pop PSW; re-enable IRQs.                       */
    case 0x04: {
        cpu->PC  = pop16(cpu);
        psw_set(cpu, pop8(cpu));
        sync_cache(cpu);
        cpu->ime = true;
        clk = 6;
    } break;

    /* RET: pop PC.                                                       */
    case 0x05: cpu->PC = pop16(cpu); clk = 6; break;

    case 0x06: psw_set(cpu, pop8(cpu)); sync_cache(cpu); clk = 3; break;   /* POP PSW */
    case 0x07: push8(cpu, psw_get(cpu));                       clk = 2; break;  /* PUSH PSW */

    case 0x0A: set_reg8(cpu, REG_A, handle_DAA(cpu, get_reg8(cpu, REG_A))); clk = 2; break;
    case 0x0B: set_reg8(cpu, REG_A, handle_DAS(cpu, get_reg8(cpu, REG_A))); clk = 2; break;

    case 0x0C: clear_CF(cpu); set_JF(cpu);   clk = 1; break;  /* CLR CF */
    case 0x0D: set_CF(cpu);   clear_JF(cpu); clk = 1; break;  /* SET CF */
    case 0x0E:                                                 /* CPL CF */
        if (is_CF(cpu)) { set_JF(cpu);   clear_CF(cpu); }
        else            { clear_JF(cpu); set_CF(cpu);   }
        clk = 1; break;

    case 0x0F: { uint8_t n = read8(cpu); rbs_set(cpu, n);      /* LD RBS,n */
                  sync_cache(cpu); set_JF(cpu); clk = 4; } break;

    /* 0x10..0x13 INC rr (WA,BC,DE,HL) */
    case 0x10: case 0x11: case 0x12: case 0x13: {
        int p = op & 3;
        uint16_t v = (uint16_t)(get_reg16(cpu, p) + 1);
        set_reg16(cpu, p, v);
        put_flag(cpu, ZF_BIT, v == 0);
        put_flag(cpu, JF_BIT, v == 0);
        clk = 2;
    } break;

    /* 0x14..0x17 LD rr,mn */
    case 0x14: case 0x15: case 0x16: case 0x17: {
        uint16_t v = read16(cpu);
        set_reg16(cpu, op & 3, v);
        set_JF(cpu);
        clk = 3;
    } break;

    /* 0x18..0x1B DEC rr */
    case 0x18: case 0x19: case 0x1A: case 0x1B: {
        int p = op & 3;
        uint16_t v = (uint16_t)(get_reg16(cpu, p) - 1);
        set_reg16(cpu, p, v);
        put_flag(cpu, JF_BIT, v == 0xFFFF);
        put_flag(cpu, ZF_BIT, v == 0);
        clk = 2;
    } break;

    case 0x1C: set_reg8(cpu, REG_A, handle_SHLC(cpu, get_reg8(cpu, REG_A))); clk = 1; break;
    case 0x1D: set_reg8(cpu, REG_A, handle_SHRC(cpu, get_reg8(cpu, REG_A))); clk = 1; break;
    case 0x1E: set_reg8(cpu, REG_A, handle_ROLC(cpu, get_reg8(cpu, REG_A))); clk = 1; break;
    case 0x1F: set_reg8(cpu, REG_A, handle_RORC(cpu, get_reg8(cpu, REG_A))); clk = 1; break;

    /* 0x20 INC (x) */
    case 0x20: {
        uint16_t a = read8(cpu);
        uint8_t  v = (uint8_t)(rm8(cpu, a) + 1);
        wm8(cpu, a, v);
        put_flag(cpu, ZF_BIT, v == 0);
        put_flag(cpu, JF_BIT, v == 0);
        clk = 5;
    } break;
    /* 0x21 INC (HL) */
    case 0x21: {
        uint16_t a = get_reg16(cpu, PAIR_HL);
        uint8_t  v = (uint8_t)(rm8(cpu, a) + 1);
        wm8(cpu, a, v);
        put_flag(cpu, ZF_BIT, v == 0);
        put_flag(cpu, JF_BIT, v == 0);
        clk = 4;
    } break;

    /* 0x22 LD A,(x) */
    case 0x22: {
        uint16_t a = read8(cpu);
        uint8_t  v = rm8(cpu, a);
        set_reg8(cpu, REG_A, v);
        set_JF(cpu);
        put_flag(cpu, ZF_BIT, v == 0);
        clk = 3;
    } break;
    /* 0x23 LD A,(HL) */
    case 0x23: {
        uint16_t a = get_reg16(cpu, PAIR_HL);
        uint8_t  v = rm8(cpu, a);
        set_reg8(cpu, REG_A, v);
        set_JF(cpu);
        put_flag(cpu, ZF_BIT, v == 0);
        clk = 2;
    } break;

    /* 0x24 LDW (x),mn  — 16-bit immediate to direct byte addr */
    case 0x24: {
        uint16_t a = read8(cpu);
        uint16_t v = read16(cpu);
        wm16(cpu, a, v);
        set_JF(cpu);
        clk = 6;
    } break;
    /* 0x25 LDW (HL),mn */
    case 0x25: {
        uint16_t a = get_reg16(cpu, PAIR_HL);
        uint16_t v = read16(cpu);
        wm16(cpu, a, v);
        set_JF(cpu);
        clk = 5;
    } break;
    /* 0x26 LD (x),(y) — note MAME reads dst then src; here we follow */
    case 0x26: {
        uint16_t src = read8(cpu);
        uint16_t dst = read8(cpu);
        uint8_t  v   = rm8(cpu, src);
        wm8(cpu, dst, v);
        set_JF(cpu);
        put_flag(cpu, ZF_BIT, v == 0);
        clk = 5;
    } break;

    /* 0x28 DEC (x) */
    case 0x28: {
        uint16_t a = read8(cpu);
        uint8_t  v = (uint8_t)(rm8(cpu, a) - 1);
        wm8(cpu, a, v);
        put_flag(cpu, JF_BIT, v == 0xFF);
        put_flag(cpu, ZF_BIT, v == 0);
        clk = 5;
    } break;
    /* 0x29 DEC (HL) */
    case 0x29: {
        uint16_t a = get_reg16(cpu, PAIR_HL);
        uint8_t  v = (uint8_t)(rm8(cpu, a) - 1);
        wm8(cpu, a, v);
        put_flag(cpu, JF_BIT, v == 0xFF);
        put_flag(cpu, ZF_BIT, v == 0);
        clk = 4;
    } break;

    /* 0x2A LD (x),A */
    case 0x2A: {
        uint16_t a = read8(cpu);
        uint8_t  v = get_reg8(cpu, REG_A);
        wm8(cpu, a, v);
        set_JF(cpu);
        put_flag(cpu, ZF_BIT, v == 0);
        clk = 3;
    } break;
    /* 0x2B LD (HL),A */
    case 0x2B: {
        uint16_t a = get_reg16(cpu, PAIR_HL);
        uint8_t  v = get_reg8(cpu, REG_A);
        wm8(cpu, a, v);
        set_JF(cpu);
        clk = 2;
    } break;

    /* 0x2C LD (x),n */
    case 0x2C: {
        uint16_t a = read8(cpu);
        uint8_t  v = read8(cpu);
        wm8(cpu, a, v);
        set_JF(cpu);
        clk = 4;
    } break;
    /* 0x2D LD (HL),n */
    case 0x2D: {
        uint16_t a = get_reg16(cpu, PAIR_HL);
        uint8_t  v = read8(cpu);
        wm8(cpu, a, v);
        set_JF(cpu);
        clk = 3;
    } break;

    /* 0x2E CLR (x) */
    case 0x2E: {
        uint16_t a = read8(cpu);
        wm8(cpu, a, 0);
        set_JF(cpu);
        clk = 4;
    } break;
    /* 0x2F CLR (HL) */
    case 0x2F: {
        uint16_t a = get_reg16(cpu, PAIR_HL);
        wm8(cpu, a, 0);
        set_JF(cpu);
        clk = 2;
    } break;

    /* 0x30..0x37 LD r,n  (r index in low 3 bits; A=0..H=7 in MAME enum) */
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x34: case 0x35: case 0x36: case 0x37: {
        uint8_t v = read8(cpu);
        set_reg8(cpu, op & 7, v);
        set_JF(cpu);
        clk = 2;
    } break;

    /* 0x40..0x47 SET (x).b — high 3 bits in opcode select bit index */
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47: {
        uint16_t a = read8(cpu);
        int bit = op & 7;
        uint8_t v = rm8(cpu, a);
        put_flag(cpu, ZF_BIT, (v & (1u << bit)) == 0);
        put_flag(cpu, JF_BIT, is_ZF(cpu));
        v = (uint8_t)(v | (1u << bit));
        wm8(cpu, a, v);
        clk = 5;
    } break;
    /* 0x48..0x4F CLR (x).b */
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
        uint16_t a = read8(cpu);
        int bit = op & 7;
        uint8_t v = rm8(cpu, a);
        put_flag(cpu, ZF_BIT, (v & (1u << bit)) == 0);
        put_flag(cpu, JF_BIT, is_ZF(cpu));
        v = (uint8_t)(v & ~(1u << bit));
        wm8(cpu, a, v);
        clk = 5;
    } break;

    /* 0x50..0x57 LD A,r */
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57: {
        uint8_t v = get_reg8(cpu, op & 7);
        set_reg8(cpu, REG_A, v);
        set_JF(cpu);
        put_flag(cpu, ZF_BIT, v == 0);
        clk = 1;
    } break;
    /* 0x58..0x5F LD r,A */
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        uint8_t v = get_reg8(cpu, REG_A);
        set_reg8(cpu, op & 7, v);
        set_JF(cpu);
        put_flag(cpu, ZF_BIT, v == 0);
        clk = 1;
    } break;

    /* 0x60..0x67 INC r */
    case 0x60: case 0x61: case 0x62: case 0x63:
    case 0x64: case 0x65: case 0x66: case 0x67: {
        int r = op & 7;
        uint8_t v = (uint8_t)(get_reg8(cpu, r) + 1);
        set_reg8(cpu, r, v);
        put_flag(cpu, ZF_BIT, v == 0);
        put_flag(cpu, JF_BIT, v == 0);
        clk = 1;
    } break;
    /* 0x68..0x6F DEC r */
    case 0x68: case 0x69: case 0x6A: case 0x6B:
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
        int r = op & 7;
        uint8_t v = (uint8_t)(get_reg8(cpu, r) - 1);
        set_reg8(cpu, r, v);
        put_flag(cpu, JF_BIT, v == 0xFF);
        put_flag(cpu, ZF_BIT, v == 0);
        clk = 1;
    } break;

    /* 0x70..0x77 ALUOP A,n */
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77: {
        uint8_t n = read8(cpu);
        int aluop = op & 7;
        uint8_t a = get_reg8(cpu, REG_A);
        uint8_t r = do_alu_8bit(cpu, aluop, a, n);
        if (aluop != 7) set_reg8(cpu, REG_A, r);   /* CMP doesn't write back */
        clk = 2;
    } break;
    /* 0x78..0x7F ALUOP A,(x) */
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        uint16_t addr = read8(cpu);
        uint8_t  n    = rm8(cpu, addr);
        int aluop = op & 7;
        uint8_t a = get_reg8(cpu, REG_A);
        uint8_t r = do_alu_8bit(cpu, aluop, a, n);
        if (aluop != 7) set_reg8(cpu, REG_A, r);
        clk = 4;
    } break;

    /* 0x80..0x9F JRS T,a  — short relative jump on JF==1
     *  Displacement encoded in the low 5 bits of the opcode
     *  (signed: 0x80=-32, 0x9F=-1, 0x80+16=0..15 etc.).
     *  Specifically MAME interprets bits 4..0 as signed 5-bit
     *  via: rel = (opcode - 0x80) - 32 if (opcode>=0xa0)
     *  No, looking again: 0x80..0x9F = JRS T, encoding 32 dests.
     *  Treating bits[4:0] as twos-complement 5-bit:
     *    range -16..+15, with +1 offset (relative to next PC).
     */
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
    case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8E: case 0x8F:
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95:
    case 0x96: case 0x97: case 0x98: case 0x99: case 0x9A: case 0x9B:
    case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
        int8_t d = (int8_t)((op & 0x1F) | ((op & 0x10) ? 0xE0 : 0));
        if (is_JF(cpu)) { cpu->PC = (uint16_t)(cpu->PC + d); clk = 4; }
        else            { clk = 2; }
        set_JF(cpu);
    } break;
    /* 0xA0..0xBF JRS F,a  — short relative jump on JF==0 */
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5:
    case 0xA6: case 0xA7: case 0xA8: case 0xA9: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5:
    case 0xB6: case 0xB7: case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        int8_t d = (int8_t)((op & 0x1F) | ((op & 0x10) ? 0xE0 : 0));
        if (!is_JF(cpu)) { cpu->PC = (uint16_t)(cpu->PC + d); clk = 4; }
        else             { clk = 2; }
        set_JF(cpu);
    } break;

    /* 0xC0..0xCF CALLV n — call via vector at 0xFFC0 + 2*n */
    case 0xC0: case 0xC1: case 0xC2: case 0xC3:
    case 0xC4: case 0xC5: case 0xC6: case 0xC7:
    case 0xC8: case 0xC9: case 0xCA: case 0xCB:
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
        push16(cpu, cpu->PC);
        cpu->PC = rm16(cpu, (uint16_t)(0xFFC0 + (op & 0x0F) * 2));
        clk = 7;
    } break;

    /* 0xD0..0xD7 JR cc,a — signed 8-bit relative jump on condition */
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
    case 0xD4: case 0xD5: case 0xD6: case 0xD7: {
        int8_t d = (int8_t)read8(cpu);
        if (check_jr_cond(cpu, op & 7)) {
            cpu->PC = (uint16_t)(cpu->PC + d); clk = 4;
        } else clk = 2;
        set_JF(cpu);
    } break;

    /* 0xD8..0xDF LD CF,(x).b */
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
        uint16_t a = read8(cpu);
        int bit = op & 7;
        int cf  = (rm8(cpu, a) >> bit) & 1;
        put_flag(cpu, CF_BIT, cf);
        put_flag(cpu, JF_BIT, !cf);   /* JF = ~CF (TEST semantics)   */
        clk = 4;
    } break;

    /* 0xE0..0xE7 src prefix */
    case 0xE0: case 0xE1: case 0xE2: case 0xE3:
    case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        clk = decode_e0_e7(cpu, op, op_pc);
        break;

    /* 0xE8..0xEF reg prefix */
    case 0xE8: case 0xE9: case 0xEA: case 0xEB:
    case 0xEC: case 0xED: case 0xEE: case 0xEF:
        clk = decode_e8_ef(cpu, op);
        break;

    /* 0xF0..0xF7 dst prefix */
    case 0xF0: case 0xF1: case 0xF2: case 0xF3:
    case 0xF4: case 0xF5: case 0xF6: case 0xF7:
        clk = decode_f0_f7(cpu, op, op_pc);
        break;

    case 0xFA: cpu->SP = read16(cpu); set_JF(cpu); clk = 3; break;  /* LD SP,mn */

    case 0xFB: {                                     /* JR a   (signed 8-bit) */
        int8_t d = (int8_t)read8(cpu);
        cpu->PC = (uint16_t)(cpu->PC + d);
        set_JF(cpu);
        clk = 4;
    } break;

    case 0xFC: {                                     /* CALL mn */
        uint16_t a = read16(cpu);
        push16(cpu, cpu->PC);
        cpu->PC = a;
        clk = 6;
    } break;

    case 0xFD: {                                     /* CALLP n  → 0xFF00 | n */
        uint8_t n = read8(cpu);
        push16(cpu, cpu->PC);
        cpu->PC = (uint16_t)(0xFF00 | n);
        clk = 6;
    } break;

    case 0xFE: cpu->PC = read16(cpu); set_JF(cpu); clk = 4; break;  /* JP mn */

    case 0xFF: {                                     /* SWI (software irq) */
        push8 (cpu, psw_get(cpu));
        push16(cpu, cpu->PC);
        cpu->PC = rm16(cpu, irq_vector_addr(14)); /* INTSW = priority 14 */
        clk = 9;
    } break;

    default:
        /* Illegal opcode — MAME logs and consumes 1 cycle.    */
        clk = 1;
        break;
    }

    /* Refresh the scalar A/W/B/C/D/E/H/L fields from the RAM
     * mirror so external callers see the post-step state. */
    sync_cache(cpu);
    cpu->F = cpu->PSW;
    cpu->cycles += (uint64_t)clk;
    return clk;
}

/* ====================================================================
 *  Src-prefix decode (E0..E7)
 *  First byte selects addressing mode; second byte selects operation.
 *  Cycle counts here include the mode's base cost.
 * ==================================================================== */
static int decode_e0_e7(CPU_TLCS870 *c, uint8_t op0, uint16_t op_start_pc) {
    int mode = op0 & 7;
    uint8_t imm = 0;
    /* (x) and (HL+d) need an immediate byte before opbyte1. */
    if (mode == AM_IMM_X || mode == AM_HL_PLUS_D)
        imm = read8(c);
    uint16_t srcaddr = get_addr(c, mode, imm, op_start_pc);

    uint8_t op1 = read8(c);
    int base = cyc_base_src[mode];

    switch (op1) {

    case 0x08: {                                  /* ROLD A,(src) */
        uint8_t lo  = (uint8_t)(get_reg8(c, REG_A) & 0x0F);
        uint8_t mem = rm8(c, srcaddr);
        uint8_t new_a = (uint8_t)((get_reg8(c, REG_A) & 0xF0) | ((mem >> 4) & 0x0F));
        uint8_t new_m = (uint8_t)((mem << 4) | lo);
        set_reg8(c, REG_A, new_a);
        wm8(c, srcaddr, new_m);
        set_JF(c);
        return 7 + base;
    }
    case 0x09: {                                  /* RORD A,(src) */
        uint8_t lo  = (uint8_t)(get_reg8(c, REG_A) & 0x0F);
        uint8_t mem = rm8(c, srcaddr);
        uint8_t new_a = (uint8_t)((get_reg8(c, REG_A) & 0xF0) | (mem & 0x0F));
        uint8_t new_m = (uint8_t)((lo << 4) | (mem >> 4));
        set_reg8(c, REG_A, new_a);
        wm8(c, srcaddr, new_m);
        set_JF(c);
        return 7 + base;
    }

    case 0x14: case 0x15: case 0x16: case 0x17: { /* LD rr,(src..src+1) */
        uint16_t v = rm16(c, srcaddr);
        set_reg16(c, op1 & 3, v);
        set_JF(c);
        return 4 + base;
    }

    case 0x20: {                                  /* INC (src) */
        uint8_t v = (uint8_t)(rm8(c, srcaddr) + 1);
        wm8(c, srcaddr, v);
        put_flag(c, ZF_BIT, v == 0);
        put_flag(c, JF_BIT, v == 0);
        return 4 + base;
    }
    case 0x26: {                                  /* LD (x),(src) — third byte = x */
        uint8_t  v = rm8(c, srcaddr);
        uint16_t x = read8(c);
        wm8(c, x, v);
        set_JF(c);
        return 5 + base;
    }
    case 0x27: {                                  /* LD (HL),(src) */
        uint8_t  v = rm8(c, srcaddr);
        wm8(c, get_reg16(c, PAIR_HL), v);
        set_JF(c);
        put_flag(c, ZF_BIT, v == 0);
        return 4 + base;
    }
    case 0x28: {                                  /* DEC (src) */
        uint8_t v = (uint8_t)(rm8(c, srcaddr) - 1);
        wm8(c, srcaddr, v);
        put_flag(c, JF_BIT, v == 0xFF);
        put_flag(c, ZF_BIT, v == 0);
        return 4 + base;
    }
    case 0x2F: {                                  /* MCMP (src),n */
        uint8_t n = read8(c);
        uint8_t v = (uint8_t)(rm8(c, srcaddr) & n);
        do_cmp_8bit(c, get_reg8(c, REG_A), v);
        return 5 + base;
    }

    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47: { /* SET (src).b */
        int bit = op1 & 7;
        uint8_t v = rm8(c, srcaddr);
        put_flag(c, ZF_BIT, (v & (1u << bit)) == 0);
        put_flag(c, JF_BIT, is_ZF(c));
        v = (uint8_t)(v | (1u << bit));
        wm8(c, srcaddr, v);
        return 4 + base;
    }
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F: { /* CLR (src).b */
        int bit = op1 & 7;
        uint8_t v = rm8(c, srcaddr);
        put_flag(c, ZF_BIT, (v & (1u << bit)) == 0);
        put_flag(c, JF_BIT, is_ZF(c));
        v = (uint8_t)(v & ~(1u << bit));
        wm8(c, srcaddr, v);
        return 4 + base;
    }

    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: { /* LD r,(src) */
        uint8_t v = rm8(c, srcaddr);
        set_reg8(c, op1 & 7, v);
        set_JF(c);
        put_flag(c, ZF_BIT, v == 0);
        return 3 + base;
    }

    case 0x60: case 0x61: case 0x62: case 0x63:
    case 0x64: case 0x65: case 0x66: case 0x67: { /* ALUOP (src),(HL) */
        int aluop = op1 & 7;
        uint8_t p1 = rm8(c, srcaddr);
        uint8_t p2 = rm8(c, get_reg16(c, PAIR_HL));
        uint8_t r  = do_alu_8bit(c, aluop, p1, p2);
        if (aluop != 7) wm8(c, srcaddr, r);
        return 6 + base;
    }

    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77: { /* ALUOP (src),n */
        int aluop = op1 & 7;
        uint8_t p1 = rm8(c, srcaddr);
        uint8_t p2 = read8(c);
        uint8_t r  = do_alu_8bit(c, aluop, p1, p2);
        if (aluop != 7) wm8(c, srcaddr, r);
        return 5 + base;
    }

    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: { /* ALUOP A,(src) */
        int aluop = op1 & 7;
        uint8_t a = get_reg8(c, REG_A);
        uint8_t p = rm8(c, srcaddr);
        uint8_t r = do_alu_8bit(c, aluop, a, p);
        if (aluop != 7) set_reg8(c, REG_A, r);
        return 3 + base;
    }

    case 0xA8: case 0xA9: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF: { /* XCH r,(src) */
        int r = op1 & 7;
        uint8_t rv = get_reg8(c, r);
        uint8_t mv = rm8(c, srcaddr);
        set_reg8(c, r, mv);
        wm8(c, srcaddr, rv);
        set_JF(c);
        put_flag(c, ZF_BIT, mv == 0);
        return 4 + base;
    }

    case 0xC0: case 0xC1: case 0xC2: case 0xC3:
    case 0xC4: case 0xC5: case 0xC6: case 0xC7: { /* CPL (src).b */
        int bit = op1 & 7;
        uint8_t v = rm8(c, srcaddr);
        put_flag(c, ZF_BIT, (v & (1u << bit)) == 0);
        put_flag(c, JF_BIT, is_ZF(c));
        v = (uint8_t)(v ^ (1u << bit));
        wm8(c, srcaddr, v);
        return 4 + base;
    }
    case 0xC8: case 0xC9: case 0xCA: case 0xCB:
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: { /* LD (src).b, CF */
        int bit = op1 & 7;
        uint8_t v = rm8(c, srcaddr);
        if (is_CF(c)) v = (uint8_t)(v |  (1u << bit));
        else          v = (uint8_t)(v & ~(1u << bit));
        wm8(c, srcaddr, v);
        set_JF(c);
        return 4 + base;
    }
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
    case 0xD4: case 0xD5: case 0xD6: case 0xD7: { /* XOR CF,(src).b */
        int bit = op1 & 7;
        int b   = (rm8(c, srcaddr) >> bit) & 1;
        int nc  = is_CF(c) ^ b;
        put_flag(c, CF_BIT, nc);
        put_flag(c, JF_BIT, !nc);
        return 3 + base;
    }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF: { /* LD CF,(src).b */
        int bit = op1 & 7;
        int b   = (rm8(c, srcaddr) >> bit) & 1;
        put_flag(c, CF_BIT, b);
        put_flag(c, JF_BIT, !b);
        return 3 + base;
    }

    case 0xFC: {                                  /* CALL (src) */
        uint16_t a = rm16(c, srcaddr);
        push16(c, c->PC);
        c->PC = a;
        return 8 + base;
    }
    case 0xFE: {                                  /* JP (src) */
        c->PC = rm16(c, srcaddr);
        set_JF(c);
        return 5 + base;
    }

    default:
        return 1 + base;  /* illegal — log-and-pass */
    }
}

/* ====================================================================
 *  Reg-prefix decode (E8..EF)
 *  op0 low 3 bits select an 8-bit register `g`, low 2 bits select a
 *  16-bit pair `gg`.  Second byte selects operation.
 * ==================================================================== */
static int decode_e8_ef(CPU_TLCS870 *c, uint8_t op0) {
    int g  = op0 & 7;
    int gg = op0 & 3;
    uint8_t op1 = read8(c);

    switch (op1) {

    case 0x01: handle_swap(c, g); return 4;
    case 0x02: handle_mul(c, gg); return 8;
    case 0x03: handle_div(c, gg); return 8;

    case 0x04:                              /* RETN (only E8 form)  */
        if (op0 == 0xE8) {
            c->PC  = pop16(c);
            psw_set(c, pop8(c));
            sync_cache(c);
            /* RETN does NOT auto-re-enable IRQs */
            return 7;
        }
        return 1;

    case 0x06: set_reg16(c, gg, pop16(c)); return 5;             /* POP gg */
    case 0x07: push16(c, get_reg16(c, gg)); return 4;            /* PUSH gg */

    case 0x0A: set_reg8(c, g, handle_DAA(c, get_reg8(c, g))); return 3;
    case 0x0B: set_reg8(c, g, handle_DAS(c, get_reg8(c, g))); return 3;

    case 0x10: case 0x11: case 0x12: case 0x13: {                /* XCH rr,gg */
        int rr = op1 & 3;
        uint16_t a = get_reg16(c, rr);
        uint16_t b = get_reg16(c, gg);
        set_reg16(c, rr, b);
        set_reg16(c, gg, a);
        set_JF(c);
        return 3;
    }
    case 0x14: case 0x15: case 0x16: case 0x17: {                /* LD rr,gg */
        set_reg16(c, op1 & 3, get_reg16(c, gg));
        set_JF(c);
        return 2;
    }

    case 0x1C: set_reg8(c, g, handle_SHLC(c, get_reg8(c, g))); return 2;
    case 0x1D: set_reg8(c, g, handle_SHRC(c, get_reg8(c, g))); return 2;
    case 0x1E: set_reg8(c, g, handle_ROLC(c, get_reg8(c, g))); return 2;
    case 0x1F: set_reg8(c, g, handle_RORC(c, get_reg8(c, g))); return 2;

    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x34: case 0x35: case 0x36: case 0x37: {                /* ALUOP WA,gg */
        int aluop = op1 & 7;
        uint16_t wa = get_reg16(c, PAIR_WA);
        uint16_t g16 = get_reg16(c, gg);
        uint16_t r = do_alu_16bit(c, aluop, wa, g16);
        if (aluop != 7) set_reg16(c, PAIR_WA, r);
        return 4;
    }
    case 0x38: case 0x39: case 0x3A: case 0x3B:
    case 0x3C: case 0x3D: case 0x3E: case 0x3F: {                /* ALUOP gg,mn */
        int aluop = op1 & 7;
        uint16_t a = get_reg16(c, gg);
        uint16_t b = read16(c);
        uint16_t r = do_alu_16bit(c, aluop, a, b);
        if (aluop != 7) set_reg16(c, gg, r);
        return 4;
    }

    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47: {                /* SET g.b */
        int bit = op1 & 7;
        uint8_t v = get_reg8(c, g);
        put_flag(c, ZF_BIT, (v & (1u << bit)) == 0);
        put_flag(c, JF_BIT, is_ZF(c));
        v = (uint8_t)(v | (1u << bit));
        set_reg8(c, g, v);
        return 3;
    }
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F: {                /* CLR g.b */
        int bit = op1 & 7;
        uint8_t v = get_reg8(c, g);
        put_flag(c, ZF_BIT, (v & (1u << bit)) == 0);
        put_flag(c, JF_BIT, is_ZF(c));
        v = (uint8_t)(v & ~(1u << bit));
        set_reg8(c, g, v);
        return 3;
    }

    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: {                /* LD r,g */
        uint8_t v = get_reg8(c, g);
        set_reg8(c, op1 & 7, v);
        set_JF(c);
        put_flag(c, ZF_BIT, v == 0);
        return 2;
    }

    case 0x60: case 0x61: case 0x62: case 0x63:
    case 0x64: case 0x65: case 0x66: case 0x67: {                /* ALUOP A,g */
        int aluop = op1 & 7;
        uint8_t a = get_reg8(c, REG_A);
        uint8_t b = get_reg8(c, g);
        uint8_t r = do_alu_8bit(c, aluop, a, b);
        if (aluop != 7) set_reg8(c, REG_A, r);
        return 2;
    }
    case 0x68: case 0x69: case 0x6A: case 0x6B:
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: {                /* ALUOP g,A */
        int aluop = op1 & 7;
        uint8_t a = get_reg8(c, g);
        uint8_t b = get_reg8(c, REG_A);
        uint8_t r = do_alu_8bit(c, aluop, a, b);
        if (aluop != 7) set_reg8(c, g, r);
        return 3;
    }
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77: {                /* ALUOP g,n */
        int aluop = op1 & 7;
        uint8_t a = get_reg8(c, g);
        uint8_t b = read8(c);
        uint8_t r = do_alu_8bit(c, aluop, a, b);
        if (aluop != 7) set_reg8(c, g, r);
        return 3;
    }

    /* 0x82..0x9F: bit ops on (DE/HL).g — `g & 7` selects the bit. */
    case 0x82: {                                                 /* SET (DE).g */
        int bit = g & 7;
        uint16_t a = get_reg16(c, PAIR_DE);
        uint8_t v = rm8(c, a);
        put_flag(c, ZF_BIT, (v & (1u << bit)) == 0);
        put_flag(c, JF_BIT, is_ZF(c));
        v = (uint8_t)(v | (1u << bit));
        wm8(c, a, v);
        return 5;
    }
    case 0x83: {                                                 /* SET (HL).g */
        int bit = g & 7;
        uint16_t a = get_reg16(c, PAIR_HL);
        uint8_t v = rm8(c, a);
        put_flag(c, ZF_BIT, (v & (1u << bit)) == 0);
        put_flag(c, JF_BIT, is_ZF(c));
        v = (uint8_t)(v | (1u << bit));
        wm8(c, a, v);
        return 5;
    }
    case 0x8A: {                                                 /* CLR (DE).g */
        int bit = g & 7;
        uint16_t a = get_reg16(c, PAIR_DE);
        uint8_t v = rm8(c, a);
        put_flag(c, ZF_BIT, (v & (1u << bit)) == 0);
        put_flag(c, JF_BIT, is_ZF(c));
        v = (uint8_t)(v & ~(1u << bit));
        wm8(c, a, v);
        return 5;
    }
    case 0x8B: {                                                 /* CLR (HL).g */
        int bit = g & 7;
        uint16_t a = get_reg16(c, PAIR_HL);
        uint8_t v = rm8(c, a);
        put_flag(c, ZF_BIT, (v & (1u << bit)) == 0);
        put_flag(c, JF_BIT, is_ZF(c));
        v = (uint8_t)(v & ~(1u << bit));
        wm8(c, a, v);
        return 5;
    }
    case 0x92: {                                                 /* CPL (DE).g */
        int bit = g & 7;
        uint16_t a = get_reg16(c, PAIR_DE);
        uint8_t v = rm8(c, a);
        put_flag(c, ZF_BIT, (v & (1u << bit)) == 0);
        put_flag(c, JF_BIT, is_ZF(c));
        v = (uint8_t)(v ^ (1u << bit));
        wm8(c, a, v);
        return 5;
    }
    case 0x93: {                                                 /* CPL (HL).g */
        int bit = g & 7;
        uint16_t a = get_reg16(c, PAIR_HL);
        uint8_t v = rm8(c, a);
        put_flag(c, ZF_BIT, (v & (1u << bit)) == 0);
        put_flag(c, JF_BIT, is_ZF(c));
        v = (uint8_t)(v ^ (1u << bit));
        wm8(c, a, v);
        return 5;
    }
    case 0x9A: {                                                 /* LD (DE).g, CF */
        int bit = g & 7;
        uint16_t a = get_reg16(c, PAIR_DE);
        uint8_t v = rm8(c, a);
        if (is_CF(c)) v = (uint8_t)(v |  (1u << bit));
        else          v = (uint8_t)(v & ~(1u << bit));
        wm8(c, a, v);
        set_JF(c);
        return 5;
    }
    case 0x9B: {                                                 /* LD (HL).g, CF */
        int bit = g & 7;
        uint16_t a = get_reg16(c, PAIR_HL);
        uint8_t v = rm8(c, a);
        if (is_CF(c)) v = (uint8_t)(v |  (1u << bit));
        else          v = (uint8_t)(v & ~(1u << bit));
        wm8(c, a, v);
        set_JF(c);
        return 5;
    }
    case 0x9E: {                                                 /* LD CF,(DE).g */
        int bit = g & 7;
        int b = (rm8(c, get_reg16(c, PAIR_DE)) >> bit) & 1;
        put_flag(c, CF_BIT, b);
        put_flag(c, JF_BIT, !b);
        return 4;
    }
    case 0x9F: {                                                 /* LD CF,(HL).g */
        int bit = g & 7;
        int b = (rm8(c, get_reg16(c, PAIR_HL)) >> bit) & 1;
        put_flag(c, CF_BIT, b);
        put_flag(c, JF_BIT, !b);
        return 4;
    }

    case 0xA8: case 0xA9: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF: {                /* XCH r,g */
        int r = op1 & 7;
        uint8_t rv = get_reg8(c, r);
        uint8_t gv = get_reg8(c, g);
        set_reg8(c, r, gv);
        set_reg8(c, g, rv);
        set_JF(c);
        put_flag(c, ZF_BIT, gv == 0);
        return 3;
    }

    case 0xC0: case 0xC1: case 0xC2: case 0xC3:
    case 0xC4: case 0xC5: case 0xC6: case 0xC7: {                /* CPL g.b */
        int bit = op1 & 7;
        uint8_t v = get_reg8(c, g);
        put_flag(c, ZF_BIT, (v & (1u << bit)) == 0);
        put_flag(c, JF_BIT, is_ZF(c));
        v = (uint8_t)(v ^ (1u << bit));
        set_reg8(c, g, v);
        return 3;
    }
    case 0xC8: case 0xC9: case 0xCA: case 0xCB:
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: {                /* LD g.b, CF */
        int bit = op1 & 7;
        uint8_t v = get_reg8(c, g);
        if (is_CF(c)) v = (uint8_t)(v |  (1u << bit));
        else          v = (uint8_t)(v & ~(1u << bit));
        set_reg8(c, g, v);
        set_JF(c);
        return 2;
    }
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
    case 0xD4: case 0xD5: case 0xD6: case 0xD7: {                /* XOR CF, g.b */
        int bit = op1 & 7;
        int b = (get_reg8(c, g) >> bit) & 1;
        int nc = is_CF(c) ^ b;
        put_flag(c, CF_BIT, nc);
        put_flag(c, JF_BIT, !nc);
        return 2;
    }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF: {                /* LD CF, g.b */
        int bit = op1 & 7;
        int b = (get_reg8(c, g) >> bit) & 1;
        put_flag(c, CF_BIT, b);
        put_flag(c, JF_BIT, !b);
        return 2;
    }

    case 0xFA: c->SP = get_reg16(c, gg); set_JF(c); return 3;     /* LD SP,gg */
    case 0xFB: set_reg16(c, gg, c->SP); set_JF(c); return 3;      /* LD gg,SP */
    case 0xFC: {                                                  /* CALL gg */
        uint16_t a = get_reg16(c, gg);
        push16(c, c->PC);
        c->PC = a;
        return 6;
    }
    case 0xFE: c->PC = get_reg16(c, gg); set_JF(c); return 3;     /* JP gg */

    default:
        return 1;
    }
}

/* ====================================================================
 *  Dst-prefix decode (F0..F7) — write-only address modes.
 *  F1 / F5 are illegal.
 * ==================================================================== */
static int decode_f0_f7(CPU_TLCS870 *c, uint8_t op0, uint16_t op_start_pc) {
    int mode = op0 & 7;
    if (mode == AM_PC_PLUS_A || mode == AM_HL_PLUS_C) return 1; /* illegal */

    uint8_t imm = 0;
    if (mode == AM_IMM_X || mode == AM_HL_PLUS_D)
        imm = read8(c);
    uint16_t dst = get_addr(c, mode, imm, op_start_pc);

    uint8_t op1 = read8(c);
    int base = cyc_base_dst[mode];

    switch (op1) {

    case 0x10: case 0x11: case 0x12: case 0x13: {                /* LD (dst),rr */
        uint16_t v = get_reg16(c, op1 & 3);
        wm16(c, dst, v);
        set_JF(c);
        return 4 + base;
    }
    case 0x2C: {                                                 /* LD (dst),n */
        uint8_t v = read8(c);
        wm8(c, dst, v);
        set_JF(c);
        return 4 + base;
    }
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57: {                /* LD (dst),r */
        uint8_t v = get_reg8(c, op1 & 7);
        wm8(c, dst, v);
        set_JF(c);
        return 3 + base;
    }

    default:
        return 1 + base;
    }
}

/* ── interrupt acceptance ────────────────────────────────────
 *  Pushes PSW, then PC, then jumps to the vector. Disables
 *  further maskable IRQs (matches MAME).                       */
void cpu_tlcs870_irq(CPU_TLCS870 *cpu, uint8_t vector) {
    if (!cpu->ime) return;
    cpu->halted = false;
    push8 (cpu, psw_get(cpu));
    push16(cpu, cpu->PC);
    cpu->ime = false;
    cpu->PC = rm16(cpu, irq_vector_addr(vector & 0x0F));
    cpu->F  = cpu->PSW;
}

/* ── init / reset ───────────────────────────────────────────── */
void cpu_tlcs870_init(CPU_TLCS870 *cpu, uint8_t *mem) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->mem = mem;
    cpu_tlcs870_reset(cpu);
}

void cpu_tlcs870_reset(CPU_TLCS870 *cpu) {
    /* Reset vector at 0xFFFE..0xFFFF.  Fall back to 0x0000 if it
     * looks unprogrammed (typical for empty ROMs). */
    if (cpu->mem) {
        uint16_t v = (uint16_t)(cpu->mem[0xFFFE] | ((uint16_t)cpu->mem[0xFFFF] << 8));
        cpu->PC = (v == 0xFFFF || v == 0x0000) ? 0x0000 : v;
    } else {
        cpu->PC = 0x0000;
    }
    cpu->SP  = 0xDFFF;
    cpu->PSW = 0;
    cpu->F   = 0;
    cpu->A = cpu->W = cpu->B = cpu->C = 0;
    cpu->D = cpu->E = cpu->H = cpu->L = 0;
    cpu->IX = cpu->IY = 0;
    cpu->ime    = false;
    cpu->halted = false;
    cpu->irq_pending = 0;
    cpu->cycles = 0;
    flush_cache(cpu);
}

/* ── debug dump ─────────────────────────────────────────────── */
void cpu_tlcs870_dump(CPU_TLCS870 *cpu) {
    printf("─── Toshiba TLCS-870 Main CPU ────────────────────────\n");
    printf("  PC=%04X  SP=%04X  PSW=%02X  RBS=%d  IME=%d\n",
           cpu->PC, cpu->SP, cpu->PSW, rbs_get(cpu), cpu->ime);
    printf("  A=%02X  W=%02X  B=%02X  C=%02X  D=%02X  E=%02X  H=%02X  L=%02X\n",
           cpu->A, cpu->W, cpu->B, cpu->C, cpu->D, cpu->E, cpu->H, cpu->L);
    printf("  WA=%04X BC=%04X DE=%04X HL=%04X\n",
           REG_WA(cpu), REG_BC(cpu), REG_DE(cpu), REG_HL(cpu));
    printf("  JF=%d ZF=%d CF=%d HF=%d\n",
           !!(cpu->PSW & JF_BIT), !!(cpu->PSW & ZF_BIT),
           !!(cpu->PSW & CF_BIT), !!(cpu->PSW & HF_BIT));
    printf("  CYCLES=%llu  HALTED=%d\n",
           (unsigned long long)cpu->cycles, cpu->halted);
    printf("──────────────────────────────────────────────────────\n");
}
