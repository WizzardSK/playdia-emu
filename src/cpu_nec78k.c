/* ================================================================
   NEC µPD78214GC — 78K/0 series CPU core  (Playdia sub-CPU)
   12 MHz, handles CD-ROM control & I/O
   Based on: NEC 78K/0 Instructions User's Manual U12326EJ4V0UM

   Register map: X=r0  A=r1  C=r2  B=r3  E=r4  D=r5  L=r6  H=r7
   Pairs:        AX=rp0  BC=rp1  DE=rp2  HL=rp3
   SFRs:         0xFF00-0xFFFF  (mapped into mem[])
   General-purpose regs (4 banks): 0xFEE0-0xFEFF
   Reset vector: mem[0000..0001] (16-bit little-endian)
   ================================================================*/

#include "cpu_nec78k.h"
#include <string.h>
#include <stdio.h>

/* ── memory helpers ─────────────────────────────────────────── */
static inline uint8_t  m8 (CPU_NEC78K *c, uint16_t a) { return c->mem[a]; }
static inline uint16_t m16(CPU_NEC78K *c, uint16_t a) {
    return (uint16_t)(c->mem[a] | ((uint16_t)c->mem[(uint16_t)(a+1)] << 8)); }
static inline void wm8 (CPU_NEC78K *c, uint16_t a, uint8_t  v) { c->mem[a] = v; }
static inline void wm16(CPU_NEC78K *c, uint16_t a, uint16_t v) {
    c->mem[a] = v&0xFF; c->mem[(uint16_t)(a+1)] = v>>8; }

/* ── fetch ──────────────────────────────────────────────────── */
static inline uint8_t  f8 (CPU_NEC78K *c) { return c->mem[c->PC++]; }
static inline uint16_t f16(CPU_NEC78K *c) {
    uint16_t lo=c->mem[c->PC++]; return lo | ((uint16_t)c->mem[c->PC++]<<8); }

/* ── register bank ──────────────────────────────────────────── */
static inline uint8_t* RB(CPU_NEC78K *c) {
    return c->bank ? c->rb[c->bank-1] : c->r; }
static inline uint8_t  gr (CPU_NEC78K *c,int i) { return RB(c)[i]; }
static inline void     sr (CPU_NEC78K *c,int i,uint8_t v)  { RB(c)[i]=v; }
static inline uint16_t grp(CPU_NEC78K *c,int p) {
    uint8_t *r=RB(c); return (uint16_t)(r[p*2]|((uint16_t)r[p*2+1]<<8)); }
static inline void     srp(CPU_NEC78K *c,int p,uint16_t v) {
    uint8_t *r=RB(c); r[p*2]=v&0xFF; r[p*2+1]=v>>8; }

/* ── flags ──────────────────────────────────────────────────── */
static inline int  fcy(CPU_NEC78K *c)        { return c->PSW & NEC_CY; }
static inline void sz (CPU_NEC78K *c,uint8_t v)  { if(v) c->PSW&=~NEC_Z; else c->PSW|=NEC_Z; }
static inline void scy(CPU_NEC78K *c,int f)      { if(f) c->PSW|=NEC_CY; else c->PSW&=~NEC_CY; }
static inline void sac(CPU_NEC78K *c,int f)      { if(f) c->PSW|=NEC_AC; else c->PSW&=~NEC_AC; }

/* ── stack ──────────────────────────────────────────────────── */
static inline void ph8 (CPU_NEC78K *c,uint8_t v)  { wm8(c,--c->SP,v); }
static inline void ph16(CPU_NEC78K *c,uint16_t v) { ph8(c,v>>8); ph8(c,v&0xFF); }
static inline uint8_t  pp8 (CPU_NEC78K *c) { return m8(c,c->SP++); }
static inline uint16_t pp16(CPU_NEC78K *c) { uint16_t l=pp8(c); return l|((uint16_t)pp8(c)<<8); }

/* ── ALU ────────────────────────────────────────────────────── */
#define ALU(NAME,EXPR,CY_EXPR,AC_EXPR) \
static inline uint8_t NAME(CPU_NEC78K *c,uint8_t a,uint8_t b){ \
    uint16_t r=(EXPR); \
    scy(c,(CY_EXPR)); sac(c,(AC_EXPR)); sz(c,(uint8_t)r); return (uint8_t)r; }

ALU(add_,  (uint16_t)a+b,          r>0xFF, ((a&0xF)+(b&0xF))>0xF)
ALU(addc_, (uint16_t)a+b+fcy(c),   r>0xFF, ((a&0xF)+(b&0xF)+fcy(c))>0xF)
ALU(sub_,  (uint16_t)a-b,          r>0xFF, ((a&0xF)-(b&0xF))>0xF)
ALU(subc_, (uint16_t)a-b-fcy(c),   r>0xFF, ((a&0xF)-(b&0xF)-fcy(c))>0xF)

static inline uint8_t and_(CPU_NEC78K *c,uint8_t a,uint8_t b){uint8_t r=a&b;sz(c,r);return r;}
static inline uint8_t or_ (CPU_NEC78K *c,uint8_t a,uint8_t b){uint8_t r=a|b;sz(c,r);return r;}
static inline uint8_t xor_(CPU_NEC78K *c,uint8_t a,uint8_t b){uint8_t r=a^b;sz(c,r);return r;}
static inline void    cmp_(CPU_NEC78K *c,uint8_t a,uint8_t b){sub_(c,a,b);}

/* ────────────────────────────────────────────────────────────
   cpu_nec78k_step
   ──────────────────────────────────────────────────────────── */
int cpu_nec78k_step(CPU_NEC78K *cpu) {
    if (cpu->halted || cpu->stopped) { cpu->cycles+=2; return 2; }

    uint8_t op = f8(cpu);
    int clk = 4;

    /* shorthand macros (macros are always r[] of current bank) */
    #define A  gr(cpu,1)
    #define gHL grp(cpu,3)

    switch (op) {

    /* ── NOP ─────────────────────────────────── 0x00 ─────── */
    case 0x00: clk=2; break;

    /* ── CMP A, #byte ────────────────────────── 0x01 ─────── */
    case 0x01: cmp_(cpu,A,f8(cpu)); clk=2; break;

    /* ── ADD/ADDC/SUB/SUBC/AND/OR A, #byte ───── 0x02–0x07 ── */
    case 0x02: sr(cpu,1,add_ (cpu,A,f8(cpu))); clk=2; break;
    case 0x03: sr(cpu,1,sub_ (cpu,A,f8(cpu))); clk=2; break;
    case 0x04: sr(cpu,1,and_ (cpu,A,f8(cpu))); clk=2; break;
    case 0x05: sr(cpu,1,or_  (cpu,A,f8(cpu))); clk=2; break;
    case 0x06: sr(cpu,1,addc_(cpu,A,f8(cpu))); clk=2; break;
    case 0x07: sr(cpu,1,subc_(cpu,A,f8(cpu))); clk=2; break;

    /* ── XCH A, r  ────────────────────────────── 0x08–0x0F ── */
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x0E: {
        uint8_t t=A; sr(cpu,1,gr(cpu,op&7)); sr(cpu,op&7,t); clk=2; } break;

    /* ── ADD A, [HL]/r ───────────────────────── 0x10–0x17 ── */
    case 0x10: sr(cpu,1,add_(cpu,A,m8(cpu,(uint16_t)gHL))); clk=4; break;
    case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
        sr(cpu,1,add_(cpu,A,gr(cpu,op&7))); clk=2; break;

    /* ── SUB A, [HL]/r ───────────────────────── 0x18–0x1F ── */
    case 0x18: sr(cpu,1,sub_(cpu,A,m8(cpu,(uint16_t)gHL))); clk=4; break;
    case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        sr(cpu,1,sub_(cpu,A,gr(cpu,op&7))); clk=2; break;

    /* ── ADDC A, [HL]/r ──────────────────────── 0x20–0x27 ── */
    case 0x20: sr(cpu,1,addc_(cpu,A,m8(cpu,(uint16_t)gHL))); clk=4; break;
    case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
        sr(cpu,1,addc_(cpu,A,gr(cpu,op&7))); clk=2; break;

    /* ── SUBC A, [HL]/r ──────────────────────── 0x28–0x2F ── */
    case 0x28: sr(cpu,1,subc_(cpu,A,m8(cpu,(uint16_t)gHL))); clk=4; break;
    case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
        sr(cpu,1,subc_(cpu,A,gr(cpu,op&7))); clk=2; break;

    /* ── AND A, [HL]/r ───────────────────────── 0x30–0x37 ── */
    case 0x30: sr(cpu,1,and_(cpu,A,m8(cpu,(uint16_t)gHL))); clk=4; break;
    case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
        sr(cpu,1,and_(cpu,A,gr(cpu,op&7))); clk=2; break;

    /* ── OR A, [HL]/r ────────────────────────── 0x38–0x3F ── */
    case 0x38: sr(cpu,1,or_(cpu,A,m8(cpu,(uint16_t)gHL))); clk=4; break;
    case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        sr(cpu,1,or_(cpu,A,gr(cpu,op&7))); clk=2; break;

    /* ── XOR A, [HL]/r ───────────────────────── 0x40–0x47 ── */
    case 0x40: sr(cpu,1,xor_(cpu,A,m8(cpu,(uint16_t)gHL))); clk=4; break;
    case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        sr(cpu,1,xor_(cpu,A,gr(cpu,op&7))); clk=2; break;

    /* ── CMP A, [HL]/r ───────────────────────── 0x48–0x4F ── */
    case 0x48: cmp_(cpu,A,m8(cpu,(uint16_t)gHL)); clk=4; break;
    case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        cmp_(cpu,A,gr(cpu,op&7)); clk=2; break;

    /* ── MOV A, [HL] / r  ────────────────────── 0x60–0x67 ── */
    case 0x60: sr(cpu,1,m8(cpu,(uint16_t)gHL)); clk=4; break;
    case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
        sr(cpu,1,gr(cpu,op&7)); clk=2; break;

    /* ── MOV [HL]/r, A  ──────────────────────── 0x70–0x77 ── */
    case 0x70: wm8(cpu,(uint16_t)gHL,A); clk=4; break;
    case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        sr(cpu,op&7,A); clk=2; break;

    /* ── MOV r, #byte  ───────────────────────── 0xB0–0xB7 ── */
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        sr(cpu,op&7,f8(cpu)); clk=2; break;

    /* ── MOVW rp, #word ──────────────────────── 0xC0/2/4/6 ── */
    case 0xC0: case 0xC2: case 0xC4: case 0xC6:
        srp(cpu,(op>>1)&3,f16(cpu)); clk=4; break;

    /* ── MOVW rp, rp  (e.g. MOVW AX,rp2) ─────── 0x88/8A/8C/8E */
    case 0x88: case 0x8A: case 0x8C: case 0x8E: {
        int p=(op>>1)&3; srp(cpu,p,grp(cpu,0)); clk=2; } break;

    /* ── INCW rp ──────────────────────────────── 0x80/2/4/6 ── */
    case 0x80: case 0x82: case 0x84: case 0x86: {
        int p=(op>>1)&3; srp(cpu,p,(uint16_t)(grp(cpu,p)+1)); clk=4; } break;

    /* ── DECW rp ──────────────────────────────── 0x90/2/4/6 ── */
    case 0x90: case 0x92: case 0x94: case 0x96: {
        int p=(op>>1)&3; srp(cpu,p,(uint16_t)(grp(cpu,p)-1)); clk=4; } break;

    /* ── INC r  ───────────────────────────────── 0x58–0x5F ── */
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        uint8_t v=gr(cpu,op&7)+1; sr(cpu,op&7,v); sz(cpu,v); clk=2; } break;

    /* ── DEC r  ───────────────────────────────── 0x51–0x57 ── */
    case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: {
        uint8_t v=gr(cpu,op&7)-1; sr(cpu,op&7,v); sz(cpu,v); clk=2; } break;

    /* ── INC [HL] ─────────────────────────────── 0x50 ──────── */
    case 0x50: {
        uint8_t v=m8(cpu,(uint16_t)gHL)+1; wm8(cpu,(uint16_t)gHL,v); sz(cpu,v); clk=4; } break;

    /* ── ADDW AX, #word16 ─────────────────────── 0xCA ──────── */
    case 0xCA: { uint16_t w=f16(cpu);
                 uint32_t r=(uint32_t)grp(cpu,0)+w;
                 scy(cpu,r>0xFFFF); srp(cpu,0,(uint16_t)r); clk=4; } break;

    /* ── SUBW AX, #word16 ─────────────────────── 0xCB ──────── */
    case 0xCB: { uint16_t w=f16(cpu);
                 uint32_t r=(uint32_t)grp(cpu,0)-w;
                 scy(cpu,r>0xFFFF); srp(cpu,0,(uint16_t)r); clk=4; } break;

    /* ── MOV A, saddr   (short direct read) ──── 0xA8 ──────── */
    case 0xA8: { uint8_t s=f8(cpu); sr(cpu,1,m8(cpu,(uint16_t)(0xFE00|s))); clk=4; } break;
    /* ── MOV saddr, A   (short direct write) ─── 0xA9 ──────── */
    case 0xA9: { uint8_t s=f8(cpu); wm8(cpu,(uint16_t)(0xFE00|s),A); clk=4; } break;
    /* ── CMP A, saddr ────────────────────────── 0xA0 ──────── */
    case 0xA0: { uint8_t s=f8(cpu); cmp_(cpu,A,m8(cpu,(uint16_t)(0xFE00|s))); clk=4; } break;
    /* ── AND A, saddr ────────────────────────── 0xA1 ──────── */
    case 0xA1: { uint8_t s=f8(cpu); sr(cpu,1,and_(cpu,A,m8(cpu,(uint16_t)(0xFE00|s)))); clk=4; } break;
    /* ── OR  A, saddr ────────────────────────── 0xA2 ──────── */
    case 0xA2: { uint8_t s=f8(cpu); sr(cpu,1,or_ (cpu,A,m8(cpu,(uint16_t)(0xFE00|s)))); clk=4; } break;
    /* ── XOR A, saddr ────────────────────────── 0xA3 ──────── */
    case 0xA3: { uint8_t s=f8(cpu); sr(cpu,1,xor_(cpu,A,m8(cpu,(uint16_t)(0xFE00|s)))); clk=4; } break;

    /* ── MOV A, !addr16 ──────────────────────── 0xE8 ──────── */
    case 0xE8: { uint16_t a=f16(cpu); sr(cpu,1,m8(cpu,a)); clk=6; } break;
    /* ── MOV !addr16, A ──────────────────────── 0xE9 ──────── */
    case 0xE9: { uint16_t a=f16(cpu); wm8(cpu,a,A); clk=6; } break;

    /* ── MOV A, [DE] / [BC] ──────────────────── 0xE3/E5 ───── */
    case 0xE3: sr(cpu,1,m8(cpu,(uint16_t)grp(cpu,2))); clk=4; break; /* [DE] */
    case 0xE5: sr(cpu,1,m8(cpu,(uint16_t)grp(cpu,1))); clk=4; break; /* [BC] */
    /* ── MOV [DE]/[BC], A ────────────────────── 0xE4/E6 ───── */
    case 0xE4: wm8(cpu,(uint16_t)grp(cpu,2),A); clk=4; break;
    case 0xE6: wm8(cpu,(uint16_t)grp(cpu,1),A); clk=4; break;

    /* ── MOVW AX, saddrp ─────────────────────── 0xEA ──────── */
    case 0xEA: { uint8_t s=f8(cpu); srp(cpu,0,m16(cpu,(uint16_t)(0xFE00|s))); clk=4; } break;
    /* ── MOVW saddrp, AX ─────────────────────── 0xEB ──────── */
    case 0xEB: { uint8_t s=f8(cpu); wm16(cpu,(uint16_t)(0xFE00|s),grp(cpu,0)); clk=4; } break;

    /* ── PUSH / POP rp ───────────────────────── 0xB8/BA/BC/BE ─
         POP: 0xA4/A6/AC/AE  (approximate; varies by sub-variant) */
    case 0xB8: case 0xBA: case 0xBC: case 0xBE:
        ph16(cpu,grp(cpu,(op>>1)&3)); clk=4; break;
    /* POP AX=0xA4, BC=0xA6, DE=0xAC, HL=0xAE (explicit mapping) */
    case 0xA4: srp(cpu,0,pp16(cpu)); clk=4; break; /* POP AX */
    case 0xA6: srp(cpu,1,pp16(cpu)); clk=4; break; /* POP BC */
    case 0xAC: srp(cpu,0,pp16(cpu)); clk=4; break; /* POP AX (alt) */
    case 0xAE: srp(cpu,3,pp16(cpu)); clk=4; break; /* POP HL */

    /* ── PUSH/POP PSW ────────────────────────── 0xF8/F9 ───── */
    case 0xF8: ph8(cpu,cpu->PSW); clk=2; break;
    case 0xF9: cpu->PSW=pp8(cpu); cpu->bank=(cpu->PSW>>3)&3; clk=2; break;

    /* ── CALL !addr16 ────────────────────────── 0x9D ──────── */
    case 0x9D: { uint16_t a=f16(cpu); ph16(cpu,cpu->PC); cpu->PC=a; clk=8; } break;

    /* ── CALLF !fadr11 ───────────────────────── 0x0F ──────── */
    case 0x0F: { uint8_t lo=f8(cpu); uint8_t hi=f8(cpu);
                 uint16_t a=(uint16_t)(0x0800|(((uint16_t)(hi&7))<<8)|lo);
                 ph16(cpu,cpu->PC); cpu->PC=a; clk=8; } break;

    /* ── CALLT table-call ────────────────────── 0xC1+2n ───── */
    case 0xC1: case 0xC3: case 0xC5: case 0xC7:
    case 0xCD: case 0xCF:
    case 0xD1: case 0xD3: case 0xD5: case 0xD7: {
        uint16_t tbl=(uint16_t)(0x0040|((uint16_t)(op&0x1E)<<1));
        ph16(cpu,cpu->PC); cpu->PC=m16(cpu,tbl); clk=6; } break;

    /* ── RET ─────────────────────────────────── 0x9E ──────── */
    case 0x9E: cpu->PC=pp16(cpu); clk=6; break;

    /* ── RETI ────────────────────────────────── 0x9F ──────── */
    case 0x9F: cpu->PC=pp16(cpu); cpu->PSW=pp8(cpu);
               cpu->bank=(cpu->PSW>>3)&3; clk=8; break;

    /* ── BR !addr16 ──────────────────────────── 0x9C ──────── */
    case 0x9C: cpu->PC=f16(cpu); clk=6; break;

    /* ── BR $rel8 ────────────────────────────── 0xFA ──────── */
    case 0xFA: { int8_t rel=(int8_t)f8(cpu); cpu->PC=(uint16_t)(cpu->PC+rel); clk=4; } break;

    /* ── BC/BNC/BZ/BNZ ───────────────────────── 0xDC..0xDF ── */
    case 0xDC: { int8_t r=(int8_t)f8(cpu);
                 if( fcy(cpu)){cpu->PC=(uint16_t)(cpu->PC+r);clk=6;}else clk=4; } break;
    case 0xDD: { int8_t r=(int8_t)f8(cpu);
                 if(!fcy(cpu)){cpu->PC=(uint16_t)(cpu->PC+r);clk=6;}else clk=4; } break;
    case 0xDE: { int8_t r=(int8_t)f8(cpu);
                 if(cpu->PSW&NEC_Z){cpu->PC=(uint16_t)(cpu->PC+r);clk=6;}else clk=4; } break;
    case 0xDF: { int8_t r=(int8_t)f8(cpu);
                 if(!(cpu->PSW&NEC_Z)){cpu->PC=(uint16_t)(cpu->PC+r);clk=6;}else clk=4; } break;

    /* ── DBNZ r,$rel ─────────────────────────── 0xD8..0xDB ── */
    case 0xD8: case 0xDA: { /* DBNZ r, $rel (r=D8=r0, DA=r2) */
        uint8_t ri=(op==0xD8)?0:2; int8_t rel=(int8_t)f8(cpu);
        uint8_t v=gr(cpu,ri)-1; sr(cpu,ri,v);
        if(v){ cpu->PC=(uint16_t)(cpu->PC+rel); clk=6; } else clk=4; } break;

    /* ── BT / BF (bit test branch) ──────────────────────────── */
    case 0xE0: { /* BT bit, saddr, $rel */
        uint8_t sad=f8(cpu); uint8_t bit=f8(cpu)&7; int8_t rel=(int8_t)f8(cpu);
        if(m8(cpu,(uint16_t)(0xFE00|sad))&(1<<bit)) cpu->PC=(uint16_t)(cpu->PC+rel);
        clk=6; } break;
    case 0xE1: { /* BF bit, saddr, $rel */
        uint8_t sad=f8(cpu); uint8_t bit=f8(cpu)&7; int8_t rel=(int8_t)f8(cpu);
        if(!(m8(cpu,(uint16_t)(0xFE00|sad))&(1<<bit))) cpu->PC=(uint16_t)(cpu->PC+rel);
        clk=6; } break;

    /* ── SET1 / CLR1 bit in saddr ────────────────────────────── */
    case 0xE2: { /* SET1 bit, saddr */
        uint8_t sad=f8(cpu); uint8_t bit=f8(cpu)&7;
        uint16_t a=(uint16_t)(0xFE00|sad);
        wm8(cpu,a,m8(cpu,a)|(1<<bit)); clk=4; } break;
    case 0xF2: { /* CLR1 bit, saddr */
        uint8_t sad=f8(cpu); uint8_t bit=f8(cpu)&7;
        uint16_t a=(uint16_t)(0xFE00|sad);
        wm8(cpu,a,m8(cpu,a)&~(1<<bit)); clk=4; } break;

    /* ── NOT1 bit, saddr ─────────────────────────────────────── */
    case 0xF5: { uint8_t sad=f8(cpu); uint8_t bit=f8(cpu)&7;
                 uint16_t a=(uint16_t)(0xFE00|sad);
                 wm8(cpu,a,m8(cpu,a)^(1<<bit)); clk=4; } break;

    /* ── ROL / ROR / ROLC / RORC ─────────────────────────────── */
    case 0xED: { int cy=(A>>7)&1; sr(cpu,1,(uint8_t)((A<<1)|fcy(cpu)));
                 scy(cpu,cy); sz(cpu,A); clk=2; } break; /* ROLC A,1 */
    case 0xEE: { int cy=A&1;     sr(cpu,1,(uint8_t)((A>>1)|(fcy(cpu)<<7)));
                 scy(cpu,cy); sz(cpu,A); clk=2; } break; /* RORC A,1 */

    /* ── MOV SFR / MOV A,SFR ─────────────────────────────────── */
    case 0xF0: { uint8_t sfr=f8(cpu); wm8(cpu,(uint16_t)(0xFF00|sfr),A); clk=4; } break;
    case 0xF1: { uint8_t sfr=f8(cpu); sr(cpu,1,m8(cpu,(uint16_t)(0xFF00|sfr))); clk=4; } break;

    /* ── MOV saddrp ─────────────────────────────────────────── */
    case 0xE7: { uint8_t s=f8(cpu); /* INC [saddr] */
                 uint16_t a=(uint16_t)(0xFE00|s);
                 uint8_t v=m8(cpu,a)+1; wm8(cpu,a,v); sz(cpu,v); clk=4; } break;

    /* ── DI / EI ─────────────────────────────── 0xFB/FC ───── */
    case 0xFB: cpu->PSW &= ~NEC_IE; clk=2; break;
    case 0xFC: cpu->PSW |=  NEC_IE; clk=2; break;

    /* ── HALT / STOP ─────────────────────────── 0xFF/FE ───── */
    case 0xFF: cpu->halted  = true; clk=2; break;
    case 0xFE: cpu->stopped = true; clk=2; break;

    default: clk=2; break; /* unknown → skip */
    }

    #undef A
    #undef gHL

    cpu->cycles += (uint64_t)clk;
    return clk;
}

/* ── interrupt delivery ─────────────────────────────────────── */
void cpu_nec78k_irq(CPU_NEC78K *cpu, uint16_t vector_addr) {
    if (!(cpu->PSW & NEC_IE)) return;
    cpu->halted = cpu->stopped = false;
    ph8(cpu, cpu->PSW);
    ph16(cpu, cpu->PC);
    cpu->PSW &= ~NEC_IE;
    cpu->PC = m16(cpu, vector_addr);
}

/* ── init / reset ───────────────────────────────────────────── */
void cpu_nec78k_init(CPU_NEC78K *cpu, uint8_t *mem) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->mem = mem;
    cpu_nec78k_reset(cpu);
}

void cpu_nec78k_reset(CPU_NEC78K *cpu) {
    uint16_t vec = (uint16_t)(cpu->mem[0] | ((uint16_t)cpu->mem[1] << 8));
    cpu->PC      = vec;
    cpu->SP      = 0xFEDF;
    memset(cpu->r,  0, sizeof(cpu->r));
    memset(cpu->rb, 0, sizeof(cpu->rb));
    cpu->PSW     = 0;
    cpu->bank    = 0;
    cpu->halted  = false;
    cpu->stopped = false;
    cpu->cycles  = 0;
    memset(cpu->port, 0, sizeof(cpu->port));
}

/* ── debug dump ─────────────────────────────────────────────── */
void cpu_nec78k_dump(CPU_NEC78K *cpu) {
    static const char *rn[]={"X","A","C","B","E","D","L","H"};
    uint8_t *r = (cpu->bank==0) ? cpu->r : cpu->rb[cpu->bank-1];
    printf("─── NEC 78K/0 Sub-CPU ─────────────────────────────────\n");
    printf("  PC=%04X  SP=%04X  PSW=%02X  BANK=%d\n",
           cpu->PC, cpu->SP, cpu->PSW, cpu->bank);
    for (int i=0;i<8;i++) printf("  %s=%02X",rn[i],r[i]);
    printf("\n");
    printf("  AX=%04X BC=%04X DE=%04X HL=%04X\n",
           grp(cpu,0),grp(cpu,1),grp(cpu,2),grp(cpu,3));
    printf("  CY=%d AC=%d Z=%d IE=%d\n",
           !!(cpu->PSW&NEC_CY),!!(cpu->PSW&NEC_AC),
           !!(cpu->PSW&NEC_Z), !!(cpu->PSW&NEC_IE));
    printf("  CYCLES=%llu  HALTED=%d  STOPPED=%d\n",
           (unsigned long long)cpu->cycles, cpu->halted, cpu->stopped);
    printf("───────────────────────────────────────────────────────\n");
}
