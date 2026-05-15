/* ================================================================
 *  NEC µPD78214GC — 78K/II series CPU core (Playdia sub-CPU)
 *  12 MHz, handles CD-ROM control & I/O.
 *
 *  Cross-checked against MAME `src/devices/cpu/upd78k/upd78k2d.cpp`
 *  for opcode encoding (MAME provides a 78K/II disassembler but no
 *  execution core); execution semantics derived from the NEC 78K/II
 *  Instructions User's Manual.
 *
 *  Status: substantial subset of the 78K/II ISA — all common 1-byte
 *  opcodes, the major prefix groups (0x05, 0x06, 0x08, 0x09, 0x0A,
 *  0x16, 0x25, 0x29, 0x38/0x39, 0x43, 0x78-0x7F) and the bulk of
 *  MOV/MOVW/ALU/BIT/BRANCH/RET families.  Unknown opcodes fall
 *  through to a 1-cycle no-op; the source is annotated where work
 *  remains.
 *
 *  Register file (per RBS bank, 8 bytes):
 *     +0=X +1=A +2=C +3=B +4=E +5=D +6=L +7=H
 *  Pair registers (little-endian within slot):
 *     AX = (A<<8)|X    BC = (B<<8)|C    DE = (D<<8)|E    HL = (H<<8)|L
 *  IRAM bank-base address (µPD78214, 256-byte IRAM at 0xFE00..0xFEFF):
 *     bank 0  → 0xFEF8 (PSW.5=0, PSW.3=0)
 *     bank 1  → 0xFEF0 (PSW.5=0, PSW.3=1)
 *     bank 2  → 0xFEE8 (PSW.5=1, PSW.3=0)
 *     bank 3  → 0xFEE0 (PSW.5=1, PSW.3=1)
 *  saddr space:   0xFE20..0xFEFF, with idx<0x20 aliasing SFR 0xFF00..0xFF1F
 *  SFR space:     0xFF00..0xFFFF
 *  Reset vector:  mem[0x0000..0x0001] (little-endian)
 *  Vector table:  0x0000..0x003F (16× 2-byte entries)
 *  CALLT table:   0x0040..0x007E  (32× 2-byte entries, indexed by 0xE0..0xFF)
 * ================================================================ */

#include "cpu_nec78k.h"
#include <string.h>
#include <stdio.h>

/* ── memory helpers ─────────────────────────────────────── */
static inline uint8_t  m8 (CPU_NEC78K *c, uint16_t a) { return c->mem[a]; }
static inline uint16_t m16(CPU_NEC78K *c, uint16_t a) {
    return (uint16_t)(c->mem[a] | ((uint16_t)c->mem[(uint16_t)(a + 1)] << 8));
}
static inline void wm8 (CPU_NEC78K *c, uint16_t a, uint8_t  v) { c->mem[a] = v; }
static inline void wm16(CPU_NEC78K *c, uint16_t a, uint16_t v) {
    c->mem[a] = (uint8_t)(v & 0xFF);
    c->mem[(uint16_t)(a + 1)] = (uint8_t)(v >> 8);
}

/* ── instruction fetch ──────────────────────────────────── */
static inline uint8_t  f8 (CPU_NEC78K *c) { return c->mem[c->PC++]; }
static inline uint16_t f16(CPU_NEC78K *c) {
    uint16_t lo = c->mem[c->PC++];
    uint16_t hi = c->mem[c->PC++];
    return (uint16_t)(lo | (hi << 8));
}

/* ── bank-base computation (per MAME register_base()) ────── */
static inline uint16_t bank_base(uint8_t psw) {
    uint16_t hi  = (psw & NEC_RBS1) ? 0xE0 : 0xF0;
    uint16_t lo  = (~psw & NEC_RBS0);          /* gives 0x00 or 0x08 */
    return (uint16_t)(0xFE00 | hi | lo);
}
static inline void psw_decode_bank(CPU_NEC78K *c) {
    c->bank = (uint8_t)(((c->PSW & NEC_RBS1) ? 2 : 0) |
                       ((c->PSW & NEC_RBS0) ? 1 : 0));
}

/* Pull the active bank into the scalar mirror cpu->r[].         */
static void sync_cache(CPU_NEC78K *c) {
    psw_decode_bank(c);
    uint16_t b = bank_base(c->PSW);
    for (int i = 0; i < 8; i++) c->r[i] = c->mem[b + i];
}
/* Push the scalar mirror back to RAM (captures external writes  */
/* via cpu->r[]).                                                 */
static void flush_cache(CPU_NEC78K *c) {
    uint16_t b = bank_base(c->PSW);
    for (int i = 0; i < 8; i++) c->mem[b + i] = c->r[i];
}

/* ── register-file accessors (always go through RAM) ─────── */
static inline uint8_t gr(CPU_NEC78K *c, int i) {
    return c->mem[bank_base(c->PSW) + (i & 7)];
}
static inline void sr(CPU_NEC78K *c, int i, uint8_t v) {
    c->mem[bank_base(c->PSW) + (i & 7)] = v;
}
static inline uint16_t grp(CPU_NEC78K *c, int p) {
    uint16_t b = bank_base(c->PSW);
    int idx = (p & 3) * 2;
    return (uint16_t)(c->mem[b + idx] | ((uint16_t)c->mem[b + idx + 1] << 8));
}
static inline void srp(CPU_NEC78K *c, int p, uint16_t v) {
    uint16_t b = bank_base(c->PSW);
    int idx = (p & 3) * 2;
    c->mem[b + idx]     = (uint8_t)(v & 0xFF);
    c->mem[b + idx + 1] = (uint8_t)(v >> 8);
}

/* ── flag helpers ───────────────────────────────────────── */
static inline int  fcy(CPU_NEC78K *c)             { return (c->PSW & NEC_CY) != 0; }
static inline void sz (CPU_NEC78K *c, uint8_t v)  { if (v) c->PSW &= ~NEC_Z;  else c->PSW |= NEC_Z;  }
static inline void szw(CPU_NEC78K *c, uint16_t v) { if (v) c->PSW &= ~NEC_Z;  else c->PSW |= NEC_Z;  }
static inline void scy(CPU_NEC78K *c, int f)      { if (f) c->PSW |=  NEC_CY; else c->PSW &= ~NEC_CY; }
static inline void sac(CPU_NEC78K *c, int f)      { if (f) c->PSW |=  NEC_AC; else c->PSW &= ~NEC_AC; }

/* ── stack ───────────────────────────────────────────────
 *  78K/II convention: predecrement on push, post-increment on
 *  pop.  SP points to the topmost occupied byte.                */
static inline void    ph8 (CPU_NEC78K *c, uint8_t  v)  { wm8(c, --c->SP, v); }
static inline void    ph16(CPU_NEC78K *c, uint16_t v)  { ph8(c, (uint8_t)(v >> 8)); ph8(c, (uint8_t)(v & 0xFF)); }
static inline uint8_t  pp8 (CPU_NEC78K *c)             { return m8(c, c->SP++); }
static inline uint16_t pp16(CPU_NEC78K *c)             { uint16_t l = pp8(c); return (uint16_t)(l | ((uint16_t)pp8(c) << 8)); }

/* ── saddr / sfr resolution ──────────────────────────────
 *  saddr index byte: idx < 0x20 → SFR alias (0xFF00 + idx),
 *  idx >= 0x20 → 0xFE00 + idx.                                  */
static inline uint16_t saddr_to_phys(uint8_t idx) {
    return (idx < 0x20) ? (uint16_t)(0xFF00 + idx) : (uint16_t)(0xFE00 + idx);
}
static inline uint16_t sfr_to_phys(uint8_t idx) {
    return (uint16_t)(0xFF00 + idx);
}

/* ── 8-bit ALU primitives (78K-style flag updates) ───────── */
#define ALU8(NAME, EXPR, CY_EXPR, AC_EXPR)                                  \
static inline uint8_t NAME(CPU_NEC78K *c, uint8_t a, uint8_t b) {           \
    uint16_t r = (EXPR);                                                    \
    scy(c, (CY_EXPR)); sac(c, (AC_EXPR)); sz(c, (uint8_t)r);                \
    return (uint8_t)r;                                                      \
}
ALU8(add_,  (uint16_t)a + b,            r > 0xFF,
            ((a & 0xF) + (b & 0xF)) > 0xF)
ALU8(addc_, (uint16_t)a + b + fcy(c),   r > 0xFF,
            ((a & 0xF) + (b & 0xF) + fcy(c)) > 0xF)
ALU8(sub_,  (uint16_t)a - b,            (a < b),
            ((a & 0xF) < (b & 0xF)))
ALU8(subc_, (uint16_t)a - b - fcy(c),   ((uint16_t)a < ((uint16_t)b + fcy(c))),
            ((a & 0xF) < ((b & 0xF) + fcy(c))))
#undef ALU8

static inline uint8_t and_(CPU_NEC78K *c, uint8_t a, uint8_t b) { uint8_t r = a & b; sz(c, r); return r; }
static inline uint8_t or_ (CPU_NEC78K *c, uint8_t a, uint8_t b) { uint8_t r = a | b; sz(c, r); return r; }
static inline uint8_t xor_(CPU_NEC78K *c, uint8_t a, uint8_t b) { uint8_t r = a ^ b; sz(c, r); return r; }
static inline void    cmp_(CPU_NEC78K *c, uint8_t a, uint8_t b) { sub_(c, a, b); }

/* ── 16-bit ALU (ADDW / SUBW / CMPW only on 78K/II) ───────── */
static inline uint16_t addw_(CPU_NEC78K *c, uint16_t a, uint16_t b) {
    uint32_t r = (uint32_t)a + b;
    scy(c, r > 0xFFFF);
    sac(c, ((uint32_t)(a & 0xFFF) + (b & 0xFFF)) > 0xFFF);
    szw(c, (uint16_t)r);
    return (uint16_t)r;
}
static inline uint16_t subw_(CPU_NEC78K *c, uint16_t a, uint16_t b) {
    uint32_t r = (uint32_t)a - b;
    scy(c, a < b);
    sac(c, (a & 0xFFF) < (b & 0xFFF));
    szw(c, (uint16_t)r);
    return (uint16_t)r;
}
static inline void cmpw_(CPU_NEC78K *c, uint16_t a, uint16_t b) { subw_(c, a, b); }

/* ── shift / rotate primitives ───────────────────────────── */
static inline uint8_t rolc8(CPU_NEC78K *c, uint8_t v, int n) {
    for (int i = 0; i < n; i++) {
        int newcy = (v >> 7) & 1;
        v = (uint8_t)((v << 1) | fcy(c));
        scy(c, newcy);
    }
    sz(c, v);
    return v;
}
static inline uint8_t rorc8(CPU_NEC78K *c, uint8_t v, int n) {
    for (int i = 0; i < n; i++) {
        int newcy = v & 1;
        v = (uint8_t)((v >> 1) | (fcy(c) << 7));
        scy(c, newcy);
    }
    sz(c, v);
    return v;
}
static inline uint8_t rol8(CPU_NEC78K *c, uint8_t v, int n) {
    for (int i = 0; i < n; i++) {
        int b = (v >> 7) & 1;
        v = (uint8_t)((v << 1) | b);
        scy(c, b);
    }
    sz(c, v);
    return v;
}
static inline uint8_t ror8(CPU_NEC78K *c, uint8_t v, int n) {
    for (int i = 0; i < n; i++) {
        int b = v & 1;
        v = (uint8_t)((v >> 1) | (b << 7));
        scy(c, b);
    }
    sz(c, v);
    return v;
}
static inline uint8_t shl8(CPU_NEC78K *c, uint8_t v, int n) {
    for (int i = 0; i < n; i++) {
        scy(c, (v >> 7) & 1);
        v = (uint8_t)(v << 1);
    }
    sz(c, v); return v;
}
static inline uint8_t shr8(CPU_NEC78K *c, uint8_t v, int n) {
    for (int i = 0; i < n; i++) {
        scy(c, v & 1);
        v = (uint8_t)(v >> 1);
    }
    sz(c, v); return v;
}
static inline uint16_t shlw(CPU_NEC78K *c, uint16_t v, int n) {
    for (int i = 0; i < n; i++) {
        scy(c, (v >> 15) & 1);
        v = (uint16_t)(v << 1);
    }
    szw(c, v); return v;
}
static inline uint16_t shrw(CPU_NEC78K *c, uint16_t v, int n) {
    for (int i = 0; i < n; i++) {
        scy(c, v & 1);
        v = (uint16_t)(v >> 1);
    }
    szw(c, v); return v;
}

/* ── BCD adjust (ADJBA / ADJBS) ──────────────────────────── */
static inline uint8_t adjba(CPU_NEC78K *c, uint8_t a) {
    if ((a & 0xF) > 9 || (c->PSW & NEC_AC)) { a = (uint8_t)(a + 0x06); sac(c, 1); }
    if (a > 0x9F           || (c->PSW & NEC_CY)) { a = (uint8_t)(a + 0x60); scy(c, 1); }
    sz(c, a); return a;
}
static inline uint8_t adjbs(CPU_NEC78K *c, uint8_t a) {
    if ((a & 0xF) > 9 || (c->PSW & NEC_AC)) { a = (uint8_t)(a - 0x06); sac(c, 1); }
    if (a > 0x9F           || (c->PSW & NEC_CY)) { a = (uint8_t)(a - 0x60); scy(c, 1); }
    sz(c, a); return a;
}

/* ── ALU dispatch by op-bit (0..7 = ADD/ADDC/SUB/SUBC/AND/XOR/OR/CMP)
 *  Matches the 78K/II `s_alu_ops[]` order; note XOR is index 5 (NOT 6). */
static uint8_t alu_dispatch(CPU_NEC78K *c, int op, uint8_t a, uint8_t b) {
    switch (op & 7) {
    case 0: return add_ (c, a, b);
    case 1: return addc_(c, a, b);
    case 2: return sub_ (c, a, b);
    case 3: return subc_(c, a, b);
    case 4: return and_ (c, a, b);
    case 5: return xor_ (c, a, b);
    case 6: return or_  (c, a, b);
    case 7: cmp_(c, a, b); return a;     /* CMP doesn't write back */
    }
    return a;
}

/* ── forward decls for prefix decoders ───────────────────── */
static int decode_01(CPU_NEC78K *c);
static int decode_02_03(CPU_NEC78K *c, uint8_t op1);
static int decode_05(CPU_NEC78K *c);
static int decode_06(CPU_NEC78K *c);
static int decode_08(CPU_NEC78K *c);
static int decode_09(CPU_NEC78K *c);
static int decode_0A(CPU_NEC78K *c);
static int decode_16(CPU_NEC78K *c);
static int decode_24(CPU_NEC78K *c);
static int decode_25(CPU_NEC78K *c);
static int decode_30(CPU_NEC78K *c, int dir);
static int decode_38_39(CPU_NEC78K *c, uint8_t op1);
static int decode_88(CPU_NEC78K *c, uint8_t op1);

/* ====================================================================
 *  Main step()
 * ==================================================================== */
int cpu_nec78k_step(CPU_NEC78K *cpu) {
    if (cpu->halted || cpu->stopped) { cpu->cycles += 2; return 2; }

    /* Pick up any external scalar writes into RAM */
    flush_cache(cpu);

    uint8_t op = f8(cpu);
    int clk = 4;

    switch (op) {

    case 0x00: clk = 2; break;                                      /* NOP */
    case 0x01: clk = decode_01(cpu); break;                         /* prefix */
    case 0x02: clk = decode_02_03(cpu, op); break;                  /* bit on PSW */
    case 0x03: clk = decode_02_03(cpu, op); break;                  /* bit on r */
    case 0x04: clk = 1; break;                                      /* illegal */
    case 0x05: clk = decode_05(cpu); break;                         /* prefix */
    case 0x06: clk = decode_06(cpu); break;                         /* prefix */
    case 0x07: clk = 1; break;                                      /* illegal */
    case 0x08: clk = decode_08(cpu); break;                         /* prefix */
    case 0x09: clk = decode_09(cpu); break;                         /* prefix */
    case 0x0A: clk = decode_0A(cpu); break;                         /* prefix */

    case 0x0B: { /* MOVW sfrp,#word16  (sfrp=0xFC → SP) */
        uint8_t s = f8(cpu); uint16_t v = f16(cpu);
        if (s == 0xFC) cpu->SP = v;
        else           wm16(cpu, sfr_to_phys(s), v);
        clk = 10;
    } break;

    case 0x0C: { /* MOVW saddrp,#word16 */
        uint8_t s = f8(cpu); uint16_t v = f16(cpu);
        wm16(cpu, saddr_to_phys(s), v);
        clk = 10;
    } break;

    case 0x0D: clk = 1; break;
    case 0x0E: sr(cpu, NEC78K_REG_A, adjba(cpu, gr(cpu, NEC78K_REG_A))); clk = 4; break;
    case 0x0F: sr(cpu, NEC78K_REG_A, adjbs(cpu, gr(cpu, NEC78K_REG_A))); clk = 4; break;

    case 0x10: { /* MOV A,sfr */
        uint8_t s = f8(cpu);
        sr(cpu, NEC78K_REG_A, m8(cpu, sfr_to_phys(s)));
        clk = 6;
    } break;
    case 0x11: { /* MOVW AX,sfrp (or AX,SP) */
        uint8_t s = f8(cpu);
        uint16_t v = (s == 0xFC) ? cpu->SP : m16(cpu, sfr_to_phys(s));
        srp(cpu, NEC78K_PAIR_AX, v);
        clk = 8;
    } break;
    case 0x12: { /* MOV sfr,A  (sfr=0xFE → MOV PSW,A) */
        uint8_t s = f8(cpu);
        uint8_t a = gr(cpu, NEC78K_REG_A);
        if (s == 0xFE) { cpu->PSW = a; psw_decode_bank(cpu); sync_cache(cpu); }
        else           wm8(cpu, sfr_to_phys(s), a);
        clk = 6;
    } break;
    case 0x13: { /* MOVW sfrp,AX */
        uint8_t s = f8(cpu);
        uint16_t v = grp(cpu, NEC78K_PAIR_AX);
        if (s == 0xFC) cpu->SP = v;
        else           wm16(cpu, sfr_to_phys(s), v);
        clk = 8;
    } break;

    case 0x14: { /* BR $rel8 — unconditional short branch */
        int8_t d = (int8_t)f8(cpu);
        cpu->PC = (uint16_t)(cpu->PC + d);
        clk = 6;
    } break;

    case 0x15: clk = 1; break;
    case 0x16: clk = decode_16(cpu); break;
    case 0x17: clk = 1; break;
    case 0x18: clk = 1; break;
    case 0x19: clk = 1; break;

    case 0x1A: { /* MOVW saddrp,AX */
        uint8_t s = f8(cpu);
        wm16(cpu, saddr_to_phys(s), grp(cpu, NEC78K_PAIR_AX));
        clk = 8;
    } break;
    case 0x1B: clk = 1; break;
    case 0x1C: { /* MOVW AX,saddrp */
        uint8_t s = f8(cpu);
        srp(cpu, NEC78K_PAIR_AX, m16(cpu, saddr_to_phys(s)));
        clk = 8;
    } break;
    case 0x1D: { /* ADDW AX,saddrp */
        uint8_t s = f8(cpu);
        uint16_t v = addw_(cpu, grp(cpu, NEC78K_PAIR_AX), m16(cpu, saddr_to_phys(s)));
        srp(cpu, NEC78K_PAIR_AX, v); clk = 8;
    } break;
    case 0x1E: { /* SUBW AX,saddrp */
        uint8_t s = f8(cpu);
        uint16_t v = subw_(cpu, grp(cpu, NEC78K_PAIR_AX), m16(cpu, saddr_to_phys(s)));
        srp(cpu, NEC78K_PAIR_AX, v); clk = 8;
    } break;
    case 0x1F: { /* CMPW AX,saddrp */
        uint8_t s = f8(cpu);
        cmpw_(cpu, grp(cpu, NEC78K_PAIR_AX), m16(cpu, saddr_to_phys(s)));
        clk = 8;
    } break;

    case 0x20: { /* MOV A,saddr */
        uint8_t s = f8(cpu);
        sr(cpu, NEC78K_REG_A, m8(cpu, saddr_to_phys(s)));
        clk = 6;
    } break;
    case 0x21: { /* XCH A,saddr */
        uint8_t s = f8(cpu); uint16_t a = saddr_to_phys(s);
        uint8_t t = gr(cpu, NEC78K_REG_A);
        sr(cpu, NEC78K_REG_A, m8(cpu, a));
        wm8(cpu, a, t); clk = 8;
    } break;
    case 0x22: { /* MOV saddr,A */
        uint8_t s = f8(cpu);
        wm8(cpu, saddr_to_phys(s), gr(cpu, NEC78K_REG_A));
        clk = 6;
    } break;
    case 0x23: clk = 1; break;
    case 0x24: clk = decode_24(cpu); break;
    case 0x25: clk = decode_25(cpu); break;
    case 0x26: { /* INC saddr */
        uint8_t s = f8(cpu); uint16_t a = saddr_to_phys(s);
        uint8_t v = (uint8_t)(m8(cpu, a) + 1);
        wm8(cpu, a, v);
        sac(cpu, (v & 0xF) == 0); sz(cpu, v);
        clk = 8;
    } break;
    case 0x27: { /* DEC saddr */
        uint8_t s = f8(cpu); uint16_t a = saddr_to_phys(s);
        uint8_t v = (uint8_t)(m8(cpu, a) - 1);
        wm8(cpu, a, v);
        sac(cpu, (v & 0xF) == 0xF); sz(cpu, v);
        clk = 8;
    } break;

    case 0x28: { /* CALL !addr16 */
        uint16_t addr = f16(cpu);
        ph16(cpu, cpu->PC);
        cpu->PC = addr;
        clk = 16;
    } break;
    case 0x29: { /* PUSH sfr */
        uint8_t s = f8(cpu); ph8(cpu, m8(cpu, sfr_to_phys(s))); clk = 8;
    } break;
    case 0x2A: clk = 1; break;
    case 0x2B: { /* MOV sfr,#imm8 (sfr=0xFE → MOV PSW,#imm8) */
        uint8_t s = f8(cpu); uint8_t v = f8(cpu);
        if (s == 0xFE) { cpu->PSW = v; psw_decode_bank(cpu); sync_cache(cpu); }
        else           wm8(cpu, sfr_to_phys(s), v);
        clk = 10;
    } break;
    case 0x2C: { /* BR !addr16 */
        cpu->PC = f16(cpu); clk = 10;
    } break;
    case 0x2D: { /* ADDW AX,#word16 */
        uint16_t v = addw_(cpu, grp(cpu, NEC78K_PAIR_AX), f16(cpu));
        srp(cpu, NEC78K_PAIR_AX, v); clk = 10;
    } break;
    case 0x2E: { /* SUBW AX,#word16 */
        uint16_t v = subw_(cpu, grp(cpu, NEC78K_PAIR_AX), f16(cpu));
        srp(cpu, NEC78K_PAIR_AX, v); clk = 10;
    } break;
    case 0x2F: { /* CMPW AX,#word16 */
        cmpw_(cpu, grp(cpu, NEC78K_PAIR_AX), f16(cpu)); clk = 10;
    } break;

    case 0x30: clk = decode_30(cpu, 0); break;          /* RORC/ROR/SHR/SHRW r,n */
    case 0x31: clk = decode_30(cpu, 1); break;          /* ROLC/ROL/SHL/SHLW r,n */

    case 0x32: case 0x33: { /* DBNZ r,$rel8  (r=C if op=0x32, B if 0x33) */
        int r = (op & 1) ? NEC78K_REG_B : NEC78K_REG_C;
        int8_t d = (int8_t)f8(cpu);
        uint8_t v = (uint8_t)(gr(cpu, r) - 1);
        sr(cpu, r, v);
        if (v) { cpu->PC = (uint16_t)(cpu->PC + d); clk = 10; }
        else   { clk = 6; }
    } break;

    case 0x34: srp(cpu, NEC78K_PAIR_AX, pp16(cpu)); clk = 8;  break;  /* POP AX */
    case 0x35: srp(cpu, NEC78K_PAIR_BC, pp16(cpu)); clk = 8;  break;  /* POP BC */
    case 0x36: srp(cpu, NEC78K_PAIR_DE, pp16(cpu)); clk = 8;  break;  /* POP DE */
    case 0x37: srp(cpu, NEC78K_PAIR_HL, pp16(cpu)); clk = 8;  break;  /* POP HL */

    case 0x38: clk = decode_38_39(cpu, op); break;            /* MOV saddr,saddr */
    case 0x39: clk = decode_38_39(cpu, op); break;            /* XCH saddr,saddr */

    case 0x3A: { /* MOV saddr,#imm8 */
        uint8_t s = f8(cpu); uint8_t v = f8(cpu);
        wm8(cpu, saddr_to_phys(s), v);
        clk = 8;
    } break;
    case 0x3B: { /* DBNZ saddr,$rel8 */
        uint8_t s = f8(cpu); int8_t d = (int8_t)f8(cpu);
        uint16_t a = saddr_to_phys(s);
        uint8_t v = (uint8_t)(m8(cpu, a) - 1);
        wm8(cpu, a, v);
        if (v) { cpu->PC = (uint16_t)(cpu->PC + d); clk = 12; }
        else   { clk = 8; }
    } break;
    case 0x3C: ph16(cpu, grp(cpu, NEC78K_PAIR_AX)); clk = 8;  break; /* PUSH AX */
    case 0x3D: ph16(cpu, grp(cpu, NEC78K_PAIR_BC)); clk = 8;  break; /* PUSH BC */
    case 0x3E: ph16(cpu, grp(cpu, NEC78K_PAIR_DE)); clk = 8;  break; /* PUSH DE */
    case 0x3F: ph16(cpu, grp(cpu, NEC78K_PAIR_HL)); clk = 8;  break; /* PUSH HL */

    case 0x40: scy(cpu, 0); clk = 4; break;    /* CLR1 CY */
    case 0x41: scy(cpu, 1); clk = 4; break;    /* SET1 CY */
    case 0x42: scy(cpu, !fcy(cpu)); clk = 4; break;  /* NOT1 CY */
    case 0x43: { /* POP sfr */
        uint8_t s = f8(cpu); wm8(cpu, sfr_to_phys(s), pp8(cpu)); clk = 8;
    } break;

    case 0x44: srp(cpu, NEC78K_PAIR_AX, (uint16_t)(grp(cpu, NEC78K_PAIR_AX) + 1)); clk = 4; break; /* INCW AX */
    case 0x45: srp(cpu, NEC78K_PAIR_BC, (uint16_t)(grp(cpu, NEC78K_PAIR_BC) + 1)); clk = 4; break;
    case 0x46: srp(cpu, NEC78K_PAIR_DE, (uint16_t)(grp(cpu, NEC78K_PAIR_DE) + 1)); clk = 4; break;
    case 0x47: srp(cpu, NEC78K_PAIR_HL, (uint16_t)(grp(cpu, NEC78K_PAIR_HL) + 1)); clk = 4; break;

    case 0x48: cpu->PSW = pp8(cpu); psw_decode_bank(cpu); sync_cache(cpu); clk = 6; break;  /* POP PSW */
    case 0x49: ph8(cpu, cpu->PSW); clk = 4; break;                                          /* PUSH PSW */
    case 0x4A: cpu->PSW &= ~NEC_IE; clk = 4; break;     /* DI */
    case 0x4B: cpu->PSW |=  NEC_IE; clk = 4; break;     /* EI */

    case 0x4C: srp(cpu, NEC78K_PAIR_AX, (uint16_t)(grp(cpu, NEC78K_PAIR_AX) - 1)); clk = 4; break;
    case 0x4D: srp(cpu, NEC78K_PAIR_BC, (uint16_t)(grp(cpu, NEC78K_PAIR_BC) - 1)); clk = 4; break;
    case 0x4E: srp(cpu, NEC78K_PAIR_DE, (uint16_t)(grp(cpu, NEC78K_PAIR_DE) - 1)); clk = 4; break;
    case 0x4F: srp(cpu, NEC78K_PAIR_HL, (uint16_t)(grp(cpu, NEC78K_PAIR_HL) - 1)); clk = 4; break;

    /* 0x50..0x5F: MOV A,[DE/HL +/-/=] / MOV [DE/HL +/-/=],A / RET / RETI / BRK / RETB */
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x58: case 0x59:
    case 0x5A: case 0x5B: case 0x5C: case 0x5D: {
        int dst_is_a = (op & 0x08) != 0;
        int use_hl   = (op & 0x01) != 0;
        int mode     = (op >> 1) & 3;       /* 00=+ 01=- 10=plain 11=invalid */
        int pair     = use_hl ? NEC78K_PAIR_HL : NEC78K_PAIR_DE;
        uint16_t a   = grp(cpu, pair);

        if (dst_is_a) {
            sr(cpu, NEC78K_REG_A, m8(cpu, a));
        } else {
            wm8(cpu, a, gr(cpu, NEC78K_REG_A));
        }
        if (mode == 0)      srp(cpu, pair, (uint16_t)(a + 1));    /* [xx+] */
        else if (mode == 1) srp(cpu, pair, (uint16_t)(a - 1));    /* [xx-] */
        clk = 6;
    } break;

    case 0x56: cpu->PC = pp16(cpu); clk = 12; break;                        /* RET */
    case 0x57: cpu->PC = pp16(cpu); cpu->PSW = pp8(cpu);                    /* RETI */
               psw_decode_bank(cpu); sync_cache(cpu); clk = 18; break;
    case 0x5E: { /* BRK — software interrupt, vector at 0x003E */
        ph8(cpu, cpu->PSW);
        ph16(cpu, cpu->PC);
        cpu->PSW &= ~NEC_IE;
        cpu->PC = m16(cpu, 0x003E);
        clk = 22;
    } break;
    case 0x5F: cpu->PC = pp16(cpu); cpu->PSW = pp8(cpu);                    /* RETB */
               psw_decode_bank(cpu); sync_cache(cpu); clk = 18; break;

    case 0x60: srp(cpu, NEC78K_PAIR_AX, f16(cpu)); clk = 10; break;  /* MOVW AX,#word */
    case 0x61: clk = 1; break;
    case 0x62: srp(cpu, NEC78K_PAIR_BC, f16(cpu)); clk = 10; break;  /* MOVW BC,#word */
    case 0x63: clk = 1; break;
    case 0x64: srp(cpu, NEC78K_PAIR_DE, f16(cpu)); clk = 10; break;  /* MOVW DE,#word */
    case 0x65: clk = 1; break;
    case 0x66: srp(cpu, NEC78K_PAIR_HL, f16(cpu)); clk = 10; break;  /* MOVW HL,#word */
    case 0x67: clk = 1; break;

    /* 0x68..0x6F: <ALU> saddr,#imm8 */
    case 0x68: case 0x69: case 0x6A: case 0x6B:
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
        uint8_t s = f8(cpu); uint8_t imm = f8(cpu);
        uint16_t a = saddr_to_phys(s);
        uint8_t v  = alu_dispatch(cpu, op & 7, m8(cpu, a), imm);
        if ((op & 7) != 7) wm8(cpu, a, v);     /* CMP doesn't store */
        clk = 8;
    } break;

    /* 0x70..0x77: BT saddr.bit,$rel8 */
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77: {
        int bit = op & 7;
        uint8_t s = f8(cpu); int8_t d = (int8_t)f8(cpu);
        uint8_t v = m8(cpu, saddr_to_phys(s));
        if (v & (1u << bit)) { cpu->PC = (uint16_t)(cpu->PC + d); clk = 12; }
        else                  clk = 8;
    } break;

    /* 0x78..0x7F: <ALU> saddr,saddr  (78K/II) — src first, dst second */
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        uint8_t src_idx = f8(cpu); uint8_t dst_idx = f8(cpu);
        uint16_t sa = saddr_to_phys(src_idx);
        uint16_t da = saddr_to_phys(dst_idx);
        uint8_t v = alu_dispatch(cpu, op & 7, m8(cpu, da), m8(cpu, sa));
        if ((op & 7) != 7) wm8(cpu, da, v);
        clk = 10;
    } break;

    /* 0x80..0x83: conditional branches (Z, NZ, NC, C) */
    case 0x80: { int8_t d = (int8_t)f8(cpu); if (!(cpu->PSW & NEC_Z)) { cpu->PC = (uint16_t)(cpu->PC + d); clk = 6; } else clk = 4; } break;
    case 0x81: { int8_t d = (int8_t)f8(cpu); if ( (cpu->PSW & NEC_Z)) { cpu->PC = (uint16_t)(cpu->PC + d); clk = 6; } else clk = 4; } break;
    case 0x82: { int8_t d = (int8_t)f8(cpu); if (!fcy(cpu))          { cpu->PC = (uint16_t)(cpu->PC + d); clk = 6; } else clk = 4; } break;
    case 0x83: { int8_t d = (int8_t)f8(cpu); if ( fcy(cpu))          { cpu->PC = (uint16_t)(cpu->PC + d); clk = 6; } else clk = 4; } break;
    case 0x84: case 0x85: case 0x86: case 0x87: clk = 1; break;

    /* 0x88..0x8F: ALU r,r' / ADDW/SUBW/CMPW AX,rp */
    case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        clk = decode_88(cpu, op); break;

    /* 0x90..0x97: CALLF !addr16 — target = 0x0800 | ((op&7)<<8) | nextbyte */
    case 0x90: case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97: {
        uint8_t lo = f8(cpu);
        uint16_t target = (uint16_t)(0x0800 | ((op & 7) << 8) | lo);
        ph16(cpu, cpu->PC);
        cpu->PC = target;
        clk = 14;
    } break;

    /* 0x98..0x9F: <ALU> A,saddr */
    case 0x98: case 0x99: case 0x9A: case 0x9B:
    case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
        uint8_t s = f8(cpu);
        uint8_t a = gr(cpu, NEC78K_REG_A);
        uint8_t b = m8(cpu, saddr_to_phys(s));
        uint8_t r = alu_dispatch(cpu, op & 7, a, b);
        if ((op & 7) != 7) sr(cpu, NEC78K_REG_A, r);
        clk = 6;
    } break;

    /* 0xA0..0xA7: CLR1 saddr.bit */
    case 0xA0: case 0xA1: case 0xA2: case 0xA3:
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: {
        int bit = op & 7;
        uint8_t s = f8(cpu); uint16_t a = saddr_to_phys(s);
        wm8(cpu, a, (uint8_t)(m8(cpu, a) & ~(1u << bit)));
        clk = 6;
    } break;

    /* 0xA8..0xAF: <ALU> A,#imm8 */
    case 0xA8: case 0xA9: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF: {
        uint8_t imm = f8(cpu);
        uint8_t a = gr(cpu, NEC78K_REG_A);
        uint8_t r = alu_dispatch(cpu, op & 7, a, imm);
        if ((op & 7) != 7) sr(cpu, NEC78K_REG_A, r);
        clk = 4;
    } break;

    /* 0xB0..0xB7: SET1 saddr.bit */
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
        int bit = op & 7;
        uint8_t s = f8(cpu); uint16_t a = saddr_to_phys(s);
        wm8(cpu, a, (uint8_t)(m8(cpu, a) | (1u << bit)));
        clk = 6;
    } break;

    /* 0xB8..0xBF: MOV r,#imm8 */
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        sr(cpu, op & 7, f8(cpu));
        clk = 4;
    } break;

    /* 0xC0..0xC7: INC r */
    case 0xC0: case 0xC1: case 0xC2: case 0xC3:
    case 0xC4: case 0xC5: case 0xC6: case 0xC7: {
        int r = op & 7;
        uint8_t v = (uint8_t)(gr(cpu, r) + 1);
        sr(cpu, r, v);
        sac(cpu, (v & 0xF) == 0); sz(cpu, v);
        clk = 2;
    } break;
    /* 0xC8..0xCF: DEC r */
    case 0xC8: case 0xC9: case 0xCA: case 0xCB:
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
        int r = op & 7;
        uint8_t v = (uint8_t)(gr(cpu, r) - 1);
        sr(cpu, r, v);
        sac(cpu, (v & 0xF) == 0xF); sz(cpu, v);
        clk = 2;
    } break;

    /* 0xD0..0xD7: MOV A,r */
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
    case 0xD4: case 0xD5: case 0xD6: case 0xD7:
        sr(cpu, NEC78K_REG_A, gr(cpu, op & 7));
        clk = 2; break;

    /* 0xD8..0xDF: XCH A,r */
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
        int r = op & 7;
        uint8_t t = gr(cpu, NEC78K_REG_A);
        sr(cpu, NEC78K_REG_A, gr(cpu, r));
        sr(cpu, r, t);
        clk = 2;
    } break;

    /* 0xE0..0xFF: CALLT [addr] — addr at 0x0040 + (op&0x3F)<<1 */
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7:
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xED: case 0xEE: case 0xEF:
    case 0xF0: case 0xF1: case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF7:
    case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD: case 0xFE: case 0xFF: {
        uint16_t tbl = (uint16_t)(0x0040 + ((op & 0x3F) << 1));
        ph16(cpu, cpu->PC);
        cpu->PC = m16(cpu, tbl);
        clk = 16;
    } break;

    default: clk = 1; break;
    }

    sync_cache(cpu);
    cpu->cycles += (uint64_t)clk;
    return clk;
}

/* ====================================================================
 *  Prefix 0x01  —  mixed: ALU/MOV/XCH with sfr or & (alternate bank)
 * ==================================================================== */
static int decode_01(CPU_NEC78K *c) {
    uint8_t op2 = f8(c);

    /* MOV/XCH/ALU A, sfr  (78K/II "sfr" alias of saddr<0x20 form) */
    if (op2 == 0x21) {                /* XCH A,sfr */
        uint8_t s = f8(c); uint16_t a = sfr_to_phys(s);
        uint8_t t = gr(c, NEC78K_REG_A);
        sr(c, NEC78K_REG_A, m8(c, a)); wm8(c, a, t);
        return 10;
    }
    if (op2 == 0x1D || op2 == 0x1E || op2 == 0x1F) {  /* ADDW/SUBW/CMPW AX,sfrp */
        uint8_t s = f8(c);
        uint16_t v = m16(c, sfr_to_phys(s));
        if (op2 == 0x1D) srp(c, NEC78K_PAIR_AX, addw_(c, grp(c, NEC78K_PAIR_AX), v));
        else if (op2 == 0x1E) srp(c, NEC78K_PAIR_AX, subw_(c, grp(c, NEC78K_PAIR_AX), v));
        else cmpw_(c, grp(c, NEC78K_PAIR_AX), v);
        return 10;
    }
    if ((op2 & 0xF8) == 0x68) {       /* <ALU> sfr,#imm8 */
        uint8_t s = f8(c); uint8_t imm = f8(c);
        uint16_t a = sfr_to_phys(s);
        uint8_t v = alu_dispatch(c, op2 & 7, m8(c, a), imm);
        if ((op2 & 7) != 7) wm8(c, a, v);
        return 12;
    }
    if ((op2 & 0xF8) == 0x98) {       /* <ALU> A,sfr */
        uint8_t s = f8(c);
        uint8_t b = m8(c, sfr_to_phys(s));
        uint8_t a = gr(c, NEC78K_REG_A);
        uint8_t r = alu_dispatch(c, op2 & 7, a, b);
        if ((op2 & 7) != 7) sr(c, NEC78K_REG_A, r);
        return 8;
    }

    /* TODO: sub-prefixes 0x01 0x05 / 0x06 / 0x09 / 0x0A / 0x16 / 0x5x
     * (the `&` alternate-bank addressing forms).  These access the
     * expansion memory bank via the MM SFR; needs host cooperation.
     * For now, skip the extra immediates the disassembler would
     * have consumed.  Caller will see PC drifting if these are hit. */
    return 4;
}

/* ====================================================================
 *  Prefix 0x02 (PSW.bit) / 0x03 (r.bit)  — bit-wise booleans + BT/BF/BTCLR
 * ==================================================================== */
static int decode_02_03(CPU_NEC78K *c, uint8_t op1) {
    uint8_t op2 = f8(c);
    int bit_idx = op2 & 7;
    int complement = (op2 & 0x10) != 0;

    /* Read source bit */
    int b;
    if (op1 == 0x02) {                /* PSW.bit */
        b = (cpu_extract_psw_bit(c, bit_idx) != 0);   /* see helper */
    } else {                          /* r.bit  (r = X or A only) */
        int r = (op2 >> 3) & 1 ? NEC78K_REG_A : NEC78K_REG_X;
        b = (gr(c, r) >> bit_idx) & 1;
    }
    if (complement) b ^= 1;

    /* Dispatch by op2 ranges:
     *   0x00..0x0F:  AND1/OR1/XOR1/MOV1 CY,<bit>  (and complements)
     *   0x10..0x1F:  MOV1 <bit>,CY
     *   0x70..0x7F:  NOT1 <bit>
     *   0x80..0x8F:  SET1 <bit>
     *   0x90..0x9F:  CLR1 <bit>
     *   0xA0..0xAF:  BF  <bit>,$rel8
     *   0xB0..0xBF:  BT  <bit>,$rel8
     *   0xD0..0xDF:  BTCLR <bit>,$rel8
     */

    if (op2 < 0x70) {
        int kind = (op2 >> 5) & 3;
        if ((op2 & 0xF0) == 0x10) {   /* MOV1 <bit>,CY  (write bit) */
            int cy = fcy(c);
            if (op1 == 0x02) cpu_inject_psw_bit(c, bit_idx, cy);
            else {
                int r = (op2 >> 3) & 1 ? NEC78K_REG_A : NEC78K_REG_X;
                uint8_t v = gr(c, r);
                v = (uint8_t)(cy ? (v | (1u << bit_idx)) : (v & ~(1u << bit_idx)));
                sr(c, r, v);
            }
            return 6;
        }
        /* bit-bool ops on CY */
        switch (kind) {
        case 0: /* MOV1 CY,<bit> */ scy(c, b);                     break;
        case 1: /* AND1 CY,<bit> */ scy(c, fcy(c) && b);          break;
        case 2: /* OR1  CY,<bit> */ scy(c, fcy(c) ||  b);          break;
        case 3: /* XOR1 CY,<bit> */ scy(c, fcy(c) ^   b);          break;
        }
        return 6;
    }
    if (op2 < 0xA0) {
        if (op2 < 0x80) {             /* NOT1 <bit> */
            if (op1 == 0x02) cpu_inject_psw_bit(c, bit_idx, !((cpu_extract_psw_bit(c, bit_idx)) != 0));
            else {
                int r = (op2 >> 3) & 1 ? NEC78K_REG_A : NEC78K_REG_X;
                sr(c, r, (uint8_t)(gr(c, r) ^ (1u << bit_idx)));
            }
        } else if (op2 < 0x90) {      /* SET1 <bit> */
            if (op1 == 0x02) cpu_inject_psw_bit(c, bit_idx, 1);
            else {
                int r = (op2 >> 3) & 1 ? NEC78K_REG_A : NEC78K_REG_X;
                sr(c, r, (uint8_t)(gr(c, r) | (1u << bit_idx)));
            }
        } else {                       /* CLR1 <bit> */
            if (op1 == 0x02) cpu_inject_psw_bit(c, bit_idx, 0);
            else {
                int r = (op2 >> 3) & 1 ? NEC78K_REG_A : NEC78K_REG_X;
                sr(c, r, (uint8_t)(gr(c, r) & ~(1u << bit_idx)));
            }
        }
        return 6;
    }
    /* 0xA0..0xBF, 0xD0..0xDF: BT/BF/BTCLR with $rel8 */
    int8_t d = (int8_t)f8(c);
    int take = 0;
    if ((op2 & 0xE0) == 0xA0)        take = (op2 & 0x10) ? b : !b;     /* BT/BF */
    else if ((op2 & 0xF0) == 0xD0) { take = b;                          /* BTCLR */
        if (take) {
            if (op1 == 0x02) cpu_inject_psw_bit(c, bit_idx, 0);
            else {
                int r = (op2 >> 3) & 1 ? NEC78K_REG_A : NEC78K_REG_X;
                sr(c, r, (uint8_t)(gr(c, r) & ~(1u << bit_idx)));
            }
        }
    }
    if (take) { c->PC = (uint16_t)(c->PC + d); return 12; }
    return 8;
}

/* helpers used by decode_02_03 — extract/inject PSW bit by index */
int cpu_extract_psw_bit(CPU_NEC78K *c, int b);
void cpu_inject_psw_bit(CPU_NEC78K *c, int b, int val);
int cpu_extract_psw_bit(CPU_NEC78K *c, int b) {
    static const uint8_t bit_mask[8] = { NEC_CY, NEC_ISP, 0x04, NEC_RBS0, NEC_AC, NEC_RBS1, NEC_Z, NEC_IE };
    return (c->PSW & bit_mask[b & 7]) != 0;
}
void cpu_inject_psw_bit(CPU_NEC78K *c, int b, int val) {
    static const uint8_t bit_mask[8] = { NEC_CY, NEC_ISP, 0x04, NEC_RBS0, NEC_AC, NEC_RBS1, NEC_Z, NEC_IE };
    uint8_t m = bit_mask[b & 7];
    if (val) c->PSW |= m; else c->PSW &= ~m;
    psw_decode_bank(c);
    sync_cache(c);
}

/* ====================================================================
 *  Prefix 0x05  —  MULU / DIVUW / BR rp / CALL rp / ROL4 / ROR4 /
 *                  SEL RBn / INCW SP / DECW SP / MOVW AX,[DE/HL]
 * ==================================================================== */
static int decode_05(CPU_NEC78K *c) {
    uint8_t op2 = f8(c);

    /* MULU r:    op2 = 0x08 | r  (r=2..7) */
    if ((op2 & 0xE8) == 0x08 && !(op2 & 0x10)) {
        int r = op2 & 7;
        uint16_t prod = (uint16_t)gr(c, NEC78K_REG_A) * (uint16_t)gr(c, r);
        srp(c, NEC78K_PAIR_AX, prod);
        szw(c, prod);
        return 22;
    }
    /* DIVUW r:   op2 = 0x18 | r  (r=2..7) */
    if ((op2 & 0xE8) == 0x08 && (op2 & 0x10)) {
        int r = op2 & 7;
        uint16_t ax = grp(c, NEC78K_PAIR_AX);
        uint8_t  d  = gr(c, r);
        if (d == 0) { scy(c, 1); return 25; }
        uint16_t q = ax / d;
        uint8_t  rm= (uint8_t)(ax % d);
        srp(c, NEC78K_PAIR_AX, q);
        sr(c, r, rm);
        scy(c, 0); szw(c, q);
        return 25;
    }
    /* BR rp / CALL rp:   op2 = 0x48 | (rp<<1)   /   0x58 | (rp<<1) */
    if ((op2 & 0xE9) == 0x48) {
        int rp = (op2 >> 1) & 3;
        uint16_t target = grp(c, rp);
        if (op2 & 0x10) {              /* CALL rp */
            ph16(c, c->PC); c->PC = target; return 12;
        }
        c->PC = target;                /* BR rp */
        return 8;
    }
    /* ROR4 / ROL4 [DE]/[HL]:    op2 = 0x88 | (dir<<4) | (use_hl<<1) */
    if ((op2 & 0xED) == 0x88) {
        int use_hl = (op2 & 0x02) != 0;
        int is_rol = (op2 & 0x10) != 0;
        int pair   = use_hl ? NEC78K_PAIR_HL : NEC78K_PAIR_DE;
        uint16_t a = grp(c, pair);
        uint8_t  m = m8(c, a);
        uint8_t  A_ = gr(c, NEC78K_REG_A);
        if (is_rol) {
            /* ROL4: A_low ← mem_high, mem_high ← mem_low, mem_low ← A_low */
            uint8_t newA = (uint8_t)((A_ & 0xF0) | (m >> 4));
            uint8_t newM = (uint8_t)((m << 4) | (A_ & 0x0F));
            sr(c, NEC78K_REG_A, newA);
            wm8(c, a, newM);
        } else {
            uint8_t newA = (uint8_t)((A_ & 0xF0) | (m & 0x0F));
            uint8_t newM = (uint8_t)((A_ << 4) | (m >> 4));
            sr(c, NEC78K_REG_A, newA);
            wm8(c, a, newM);
        }
        return 16;
    }
    /* SEL RBn:   op2 = 0xA8 | n  (n=0..3) */
    if ((op2 & 0xFC) == 0xA8) {
        int n = op2 & 3;
        c->PSW = (uint8_t)((c->PSW & ~(NEC_RBS0 | NEC_RBS1)) |
                          ((n & 1) ? NEC_RBS0 : 0) |
                          ((n & 2) ? NEC_RBS1 : 0));
        psw_decode_bank(c);
        sync_cache(c);
        return 4;
    }
    /* INCW SP / DECW SP:    op2 = 0xC8 or 0xC9 */
    if (op2 == 0xC8) { c->SP++; return 4; }
    if (op2 == 0xC9) { c->SP--; return 4; }

    /* MOVW AX,[DE]/[HL] and reverse:   op2 = 0xE2/0xE3/0xE6/0xE7 */
    if ((op2 & 0xFA) == 0xE2) {
        int use_hl = (op2 & 0x01) != 0;
        int store  = (op2 & 0x04) != 0;
        int pair   = use_hl ? NEC78K_PAIR_HL : NEC78K_PAIR_DE;
        uint16_t a = grp(c, pair);
        if (store) wm16(c, a, grp(c, NEC78K_PAIR_AX));
        else       srp(c, NEC78K_PAIR_AX, m16(c, a));
        return 12;
    }
    return 4;
}

/* ====================================================================
 *  Prefix 0x06  —  [base+disp8] addressing  (DE/SP/HL + disp8)
 *  Encoding: 06, op2, disp8
 *    op2 bit 7  : direction (0 = A,[base+d]; 1 = [base+d],A)
 *    op2 bit 5  : base reg high bit
 *    op2 bit 4  : base reg low  bit  (00=DE 01=SP 10=HL)
 *    op2 bit 3  : 0 ⇒ MOV (bit 2 ⇒ XCH)  /  1 ⇒ ALU op (bits 2:0)
 *    op2 bit 2  : 0=MOV, 1=XCH (when bit 3=0)
 * ==================================================================== */
static int decode_06(CPU_NEC78K *c) {
    uint8_t op2 = f8(c);
    int8_t  disp = (int8_t)f8(c);
    int     base_sel = (op2 >> 4) & 3;
    uint16_t base;
    switch (base_sel) {
    case 0: base = grp(c, NEC78K_PAIR_DE); break;
    case 1: base = c->SP;                  break;
    case 2: base = grp(c, NEC78K_PAIR_HL); break;
    default: return 1;                     /* illegal */
    }
    uint16_t addr = (uint16_t)(base + (uint16_t)disp);
    int dir_store = (op2 & 0x80) != 0;
    int is_alu    = (op2 & 0x08) != 0;
    int is_xch    = (op2 & 0x04) != 0;

    if (is_alu) {
        int aluop = op2 & 7;
        uint8_t a = gr(c, NEC78K_REG_A);
        uint8_t b = m8(c, addr);
        if (dir_store) {                 /* <op> [addr],A ? — rare; treat as ALU on memory */
            uint8_t r = alu_dispatch(c, aluop, b, a);
            if (aluop != 7) wm8(c, addr, r);
        } else {                         /* <op> A,[addr] */
            uint8_t r = alu_dispatch(c, aluop, a, b);
            if (aluop != 7) sr(c, NEC78K_REG_A, r);
        }
        return 10;
    }
    if (is_xch) {
        uint8_t t = gr(c, NEC78K_REG_A);
        sr(c, NEC78K_REG_A, m8(c, addr));
        wm8(c, addr, t);
        return 10;
    }
    /* MOV */
    if (dir_store) wm8(c, addr, gr(c, NEC78K_REG_A));
    else           sr(c, NEC78K_REG_A, m8(c, addr));
    return 8;
}

/* ====================================================================
 *  Prefix 0x08  —  bit ops on saddr.bit / sfr.bit
 *  Encoding: 08, op2, addr_idx [, disp8 for BT/BF/BTCLR]
 *    op2 bit 3 = 1 ⇒ sfr_idx; bit 3 = 0 ⇒ saddr_idx
 *    op2 bits 7:4 select sub-op (see decoder)
 * ==================================================================== */
static int decode_08(CPU_NEC78K *c) {
    uint8_t op2 = f8(c);
    uint8_t addr_idx = f8(c);
    int is_sfr = (op2 & 0x08) != 0;
    int bit_idx = op2 & 7;
    uint16_t a = is_sfr ? sfr_to_phys(addr_idx) : saddr_to_phys(addr_idx);

    if ((op2 & 0xF0) == 0x10) {           /* MOV1 <bit>,CY */
        uint8_t v = m8(c, a);
        v = (uint8_t)(fcy(c) ? (v | (1u << bit_idx)) : (v & ~(1u << bit_idx)));
        wm8(c, a, v);
        return 8;
    }
    if (op2 < 0x70) {                     /* bool ops on CY */
        int b = (m8(c, a) >> bit_idx) & 1;
        if (op2 & 0x10) b ^= 1;
        int kind = (op2 >> 5) & 3;
        switch (kind) {
        case 0: scy(c, b);                break;   /* MOV1 CY,<bit> */
        case 1: scy(c, fcy(c) && b);     break;   /* AND1 */
        case 2: scy(c, fcy(c) ||  b);     break;   /* OR1  */
        case 3: scy(c, fcy(c) ^   b);     break;   /* XOR1 */
        }
        return 8;
    }
    if (op2 < (is_sfr ? 0xA0 : 0x80)) {   /* NOT1/SET1/CLR1 */
        uint8_t v = m8(c, a);
        if (op2 < 0x80)              v = (uint8_t)(v ^ (1u << bit_idx));   /* NOT1 */
        else if (!(op2 & 0x10))      v = (uint8_t)(v | (1u << bit_idx));   /* SET1 */
        else                          v = (uint8_t)(v & ~(1u << bit_idx)); /* CLR1 */
        wm8(c, a, v);
        return 8;
    }
    /* BT / BF / BTCLR */
    int8_t d = (int8_t)f8(c);
    int b = (m8(c, a) >> bit_idx) & 1;
    int take = 0;
    if ((op2 & 0xE0) == 0xA0)        take = (op2 & 0x10) ? b : !b;
    else if ((op2 & 0xF0) == 0xD0) {
        take = b;
        if (take) wm8(c, a, (uint8_t)(m8(c, a) & ~(1u << bit_idx)));
    }
    if (take) { c->PC = (uint16_t)(c->PC + d); return 14; }
    return 10;
}

/* ====================================================================
 *  Prefix 0x09  —  MOV STBC,#imm8 / MOV A,!addr16 / MOV !addr16,A
 * ==================================================================== */
static int decode_09(CPU_NEC78K *c) {
    uint8_t op2 = f8(c);
    if (op2 == 0xC0) {                 /* MOV STBC,#imm  (4 bytes, protected) */
        uint8_t nimm = f8(c);
        uint8_t imm  = f8(c);
        if ((uint8_t)(~nimm) == imm) {
            /* STBC = SFR at 0xFFB0 (typical for µPD78214 family) */
            wm8(c, 0xFFB0, imm);
            /* HALT/STOP triggered by bits in STBC — set flags here */
            if (imm & 0x01) c->halted  = true;
            if (imm & 0x02) c->stopped = true;
        }
        return 12;
    }
    if (op2 == 0xF0) {                 /* MOV A,!addr16 */
        uint16_t a = f16(c);
        sr(c, NEC78K_REG_A, m8(c, a));
        return 12;
    }
    if (op2 == 0xF1) {                 /* MOV !addr16,A */
        uint16_t a = f16(c);
        wm8(c, a, gr(c, NEC78K_REG_A));
        return 12;
    }
    return 4;
}

/* ====================================================================
 *  Prefix 0x0A  —  word[A] / word[B] / word[DE] / word[HL] addressing
 *  Encoding: 0A, op2, lo, hi
 *    op2 bit 7  : direction
 *    op2 bit 5  : 1 ⇒ HL/B (else DE/A)
 *    op2 bit 4  : 1 ⇒ index by register (A or B); 0 ⇒ 16-bit displacement
 *    op2 bit 3  : 1 ⇒ ALU; 0 ⇒ MOV (bit 2 picks XCH)
 *    op2 bit 2  : (when bit 3=0) 0=MOV, 1=XCH
 * ==================================================================== */
static int decode_0A(CPU_NEC78K *c) {
    uint8_t op2 = f8(c);
    uint16_t imm = f16(c);
    int use_hl  = (op2 & 0x20) != 0;
    int is_idx  = (op2 & 0x10) != 0;
    uint16_t addr;
    if (is_idx) {
        uint8_t idx = use_hl ? gr(c, NEC78K_REG_B) : gr(c, NEC78K_REG_A);
        addr = (uint16_t)(imm + idx);
    } else {
        uint16_t base = use_hl ? grp(c, NEC78K_PAIR_HL) : grp(c, NEC78K_PAIR_DE);
        addr = (uint16_t)(imm + base);
    }
    int dir_store = (op2 & 0x80) != 0;
    int is_alu    = (op2 & 0x08) != 0;
    int is_xch    = (op2 & 0x04) != 0;

    if (is_alu) {
        int aluop = op2 & 7;
        uint8_t a = gr(c, NEC78K_REG_A);
        uint8_t b = m8(c, addr);
        if (dir_store) {
            uint8_t r = alu_dispatch(c, aluop, b, a);
            if (aluop != 7) wm8(c, addr, r);
        } else {
            uint8_t r = alu_dispatch(c, aluop, a, b);
            if (aluop != 7) sr(c, NEC78K_REG_A, r);
        }
        return 12;
    }
    if (is_xch) {
        uint8_t t = gr(c, NEC78K_REG_A);
        sr(c, NEC78K_REG_A, m8(c, addr));
        wm8(c, addr, t);
        return 12;
    }
    if (dir_store) wm8(c, addr, gr(c, NEC78K_REG_A));
    else           sr(c, NEC78K_REG_A, m8(c, addr));
    return 10;
}

/* ====================================================================
 *  Prefix 0x16  —  pre/post-modify indirect via [DE]/[HL] for ALU/MOV/XCH
 * ==================================================================== */
static int decode_16(CPU_NEC78K *c) {
    uint8_t op2 = f8(c);
    int use_hl    = (op2 & 0x10) != 0;
    int mode      = (op2 >> 5) & 3;       /* 00=+ 01=- 10=plain */
    int pair      = use_hl ? NEC78K_PAIR_HL : NEC78K_PAIR_DE;
    uint16_t addr = grp(c, pair);
    int dir_store = (op2 & 0x80) != 0;
    int is_alu    = (op2 & 0x08) != 0;
    int is_xch    = (op2 & 0x04) != 0;

    if (is_alu) {
        int aluop = op2 & 7;
        uint8_t a = gr(c, NEC78K_REG_A);
        uint8_t b = m8(c, addr);
        if (dir_store) {
            uint8_t r = alu_dispatch(c, aluop, b, a);
            if (aluop != 7) wm8(c, addr, r);
        } else {
            uint8_t r = alu_dispatch(c, aluop, a, b);
            if (aluop != 7) sr(c, NEC78K_REG_A, r);
        }
    } else if (is_xch) {
        uint8_t t = gr(c, NEC78K_REG_A);
        sr(c, NEC78K_REG_A, m8(c, addr)); wm8(c, addr, t);
    } else {
        if (dir_store) wm8(c, addr, gr(c, NEC78K_REG_A));
        else           sr(c, NEC78K_REG_A, m8(c, addr));
    }
    if (mode == 0)      srp(c, pair, (uint16_t)(addr + 1));
    else if (mode == 1) srp(c, pair, (uint16_t)(addr - 1));
    return 8;
}

/* ====================================================================
 *  Prefix 0x24  —  MOV r,r' / MOVW rp,rp'
 * ==================================================================== */
static int decode_24(CPU_NEC78K *c) {
    uint8_t rr = f8(c);
    if ((rr & 0x88) == 0x00) {            /* MOV r1,r2 */
        int r1 = (rr >> 4) & 7;
        int r2 = rr & 7;
        sr(c, r1, gr(c, r2));
        return 4;
    }
    if ((rr & 0x99) == 0x08) {            /* MOVW rp1,rp2 */
        int rp1 = (rr >> 5) & 3;
        int rp2 = (rr >> 1) & 3;
        srp(c, rp1, grp(c, rp2));
        return 6;
    }
    return 1;
}

/* ====================================================================
 *  Prefix 0x25  —  XCH r,r'
 * ==================================================================== */
static int decode_25(CPU_NEC78K *c) {
    uint8_t rr = f8(c);
    if ((rr & 0x88) == 0x00) {
        int r1 = (rr >> 4) & 7;
        int r2 = rr & 7;
        uint8_t t = gr(c, r1);
        sr(c, r1, gr(c, r2));
        sr(c, r2, t);
        return 6;
    }
    return 1;
}

/* ====================================================================
 *  Shift dispatch (0x30 = right, 0x31 = left)
 *    n byte: (n>>6)&3 = type, (n>>3)&7 = count, (n>=0xC0)? rp : r
 * ==================================================================== */
static int decode_30(CPU_NEC78K *c, int dir) {
    uint8_t n = f8(c);
    int kind  = (n >> 6) & 3;
    int count = (n >> 3) & 7;
    int is_rp = (n >= 0xC0);

    if (is_rp) {
        int rp = (n & 0x06) >> 1;
        uint16_t v = grp(c, rp);
        if (kind == 3) {                  /* SHRW / SHLW */
            v = dir ? shlw(c, v, count) : shrw(c, v, count);
            srp(c, rp, v);
        }
        return 6 + count;
    }
    int r = n & 7;
    uint8_t v = gr(c, r);
    switch (kind) {
    case 0: v = dir ? rolc8(c, v, count) : rorc8(c, v, count); break;
    case 1: v = dir ? rol8 (c, v, count) : ror8 (c, v, count); break;
    case 2: v = dir ? shl8 (c, v, count) : shr8 (c, v, count); break;
    }
    sr(c, r, v);
    return 4 + count;
}

/* ====================================================================
 *  Prefix 0x38 (MOV) / 0x39 (XCH) saddr,saddr
 * ==================================================================== */
static int decode_38_39(CPU_NEC78K *c, uint8_t op1) {
    uint8_t src_idx = f8(c);
    uint8_t dst_idx = f8(c);
    uint16_t sa = saddr_to_phys(src_idx);
    uint16_t da = saddr_to_phys(dst_idx);
    if (op1 == 0x38) {
        wm8(c, da, m8(c, sa));
    } else {
        uint8_t t = m8(c, da);
        wm8(c, da, m8(c, sa));
        wm8(c, sa, t);
    }
    return 10;
}

/* ====================================================================
 *  ALU r,r'  (0x88..0x8F) or ADDW/SUBW/CMPW AX,rp
 * ==================================================================== */
static int decode_88(CPU_NEC78K *c, uint8_t op1) {
    uint8_t rr = f8(c);
    int aluop = op1 & 7;
    if ((rr & 0x88) == 0x00) {            /* <ALU> r1,r2 */
        int r1 = (rr >> 4) & 7;
        int r2 = rr & 7;
        uint8_t v = alu_dispatch(c, aluop, gr(c, r1), gr(c, r2));
        if (aluop != 7) sr(c, r1, v);
        return 6;
    }
    /* ADDW/SUBW/CMPW AX,rp  — only for ALU index 0 (ADD), 2 (SUB), 7 (CMP) */
    if ((rr & 0xF9) == 0x08) {
        int rp = (rr >> 1) & 3;
        uint16_t a = grp(c, NEC78K_PAIR_AX);
        uint16_t b = grp(c, rp);
        if (aluop == 0)      srp(c, NEC78K_PAIR_AX, addw_(c, a, b));
        else if (aluop == 2) srp(c, NEC78K_PAIR_AX, subw_(c, a, b));
        else if (aluop == 7) cmpw_(c, a, b);
        return 8;
    }
    return 1;
}

/* ====================================================================
 *  Interrupt delivery
 *  vector_addr is a physical address (e.g. 0x0004..0x003E).
 * ==================================================================== */
void cpu_nec78k_irq(CPU_NEC78K *cpu, uint16_t vector_addr) {
    if (!(cpu->PSW & NEC_IE)) return;
    cpu->halted = cpu->stopped = false;
    ph8(cpu, cpu->PSW);
    ph16(cpu, cpu->PC);
    cpu->PSW &= ~NEC_IE;
    cpu->PC = m16(cpu, vector_addr);
}

/* ====================================================================
 *  init / reset / dump
 * ==================================================================== */
void cpu_nec78k_init(CPU_NEC78K *cpu, uint8_t *mem) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->mem = mem;
    cpu_nec78k_reset(cpu);
}

void cpu_nec78k_reset(CPU_NEC78K *cpu) {
    /* 78K/II reset: PC = m16(0x0000), PSW = 0x02, SP = 0xFEDF */
    uint16_t vec = (uint16_t)(cpu->mem[0] | ((uint16_t)cpu->mem[1] << 8));
    cpu->PC      = vec;
    cpu->SP      = 0xFEDF;
    cpu->PSW     = 0x02;
    cpu->bank    = 0;
    cpu->halted  = false;
    cpu->stopped = false;
    cpu->cycles  = 0;
    memset(cpu->port, 0, sizeof(cpu->port));
    sync_cache(cpu);
}

void cpu_nec78k_dump(CPU_NEC78K *cpu) {
    static const char *rn[] = { "X", "A", "C", "B", "E", "D", "L", "H" };
    psw_decode_bank(cpu);
    printf("─── NEC µPD78214 (78K/II) Sub-CPU ────────────────────\n");
    printf("  PC=%04X  SP=%04X  PSW=%02X  BANK=%d\n",
           cpu->PC, cpu->SP, cpu->PSW, cpu->bank);
    for (int i = 0; i < 8; i++) printf("  %s=%02X", rn[i], gr(cpu, i));
    printf("\n");
    printf("  AX=%04X BC=%04X DE=%04X HL=%04X\n",
           grp(cpu, NEC78K_PAIR_AX), grp(cpu, NEC78K_PAIR_BC),
           grp(cpu, NEC78K_PAIR_DE), grp(cpu, NEC78K_PAIR_HL));
    printf("  CY=%d AC=%d Z=%d IE=%d  RBS=%d%d  ISP=%d\n",
           !!(cpu->PSW & NEC_CY), !!(cpu->PSW & NEC_AC),
           !!(cpu->PSW & NEC_Z),  !!(cpu->PSW & NEC_IE),
           !!(cpu->PSW & NEC_RBS1), !!(cpu->PSW & NEC_RBS0),
           !!(cpu->PSW & NEC_ISP));
    printf("  CYCLES=%llu  HALTED=%d  STOPPED=%d\n",
           (unsigned long long)cpu->cycles, cpu->halted, cpu->stopped);
    printf("──────────────────────────────────────────────────────\n");
}
