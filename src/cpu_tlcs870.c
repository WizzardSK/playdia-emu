#include "cpu_tlcs870.h"
#include <stdio.h>
#include <string.h>

// ── Memory ────────────────────────────────────────────────────
static inline uint8_t  mr8 (CPU_TLCS870 *c, uint16_t a)          { return c->mem[a]; }
static inline uint16_t mr16(CPU_TLCS870 *c, uint16_t a)          { return (uint16_t)(c->mem[a]|(c->mem[(uint16_t)(a+1)]<<8)); }
static inline void     mw8 (CPU_TLCS870 *c, uint16_t a, uint8_t v) { if(a>=0x2000) c->mem[a]=v; }
static inline void     mw16(CPU_TLCS870 *c, uint16_t a, uint16_t v){ mw8(c,a,v&0xFF); mw8(c,(uint16_t)(a+1),v>>8); }

// ── Flags ─────────────────────────────────────────────────────
static inline void sf(CPU_TLCS870 *c,uint8_t f,bool v){ if(v)c->F|=f; else c->F&=~f; }
static inline bool gf(CPU_TLCS870 *c,uint8_t f){ return (c->F&f)!=0; }
static inline void uzs(CPU_TLCS870 *c,uint8_t r){ sf(c,FLAG_Z,r==0); sf(c,FLAG_S,(r&0x80)!=0); }
static inline bool par(uint8_t v){ v^=v>>4;v^=v>>2;v^=v>>1;return(v&1)==0; }

// ── Stack ─────────────────────────────────────────────────────
static inline void    push(CPU_TLCS870 *c,uint16_t v){ c->SP-=2; mw16(c,c->SP,v); }
static inline uint16_t pop(CPU_TLCS870 *c)           { uint16_t v=mr16(c,c->SP); c->SP+=2; return v; }

// ── Register index (Z80: B=0 C=1 D=2 E=3 H=4 L=5 (HL)=6 A=7) ─
static uint8_t rg(CPU_TLCS870 *c,int r){
    switch(r&7){case 0:return c->B;case 1:return c->C;case 2:return c->D;case 3:return c->E;
                case 4:return c->H;case 5:return c->L;case 6:return mr8(c,REG_HL(c));default:return c->A;}
}
static void rs(CPU_TLCS870 *c,int r,uint8_t v){
    switch(r&7){case 0:c->B=v;break;case 1:c->C=v;break;case 2:c->D=v;break;case 3:c->E=v;break;
                case 4:c->H=v;break;case 5:c->L=v;break;case 6:mw8(c,REG_HL(c),v);break;default:c->A=v;break;}
}
static int rc(int r){ return (r&7)==6?8:4; }

// ── ALU ───────────────────────────────────────────────────────
static void add8(CPU_TLCS870 *c,uint8_t v,bool adc){
    uint8_t cin=adc&&gf(c,FLAG_C)?1:0;
    uint16_t r=(uint16_t)c->A+v+cin;
    sf(c,FLAG_H,((c->A&0xF)+(v&0xF)+cin)>0xF);
    sf(c,FLAG_C,r>0xFF); sf(c,FLAG_N,false);
    sf(c,FLAG_V,(~(c->A^v)&(c->A^(uint8_t)r)&0x80)!=0);
    c->A=(uint8_t)r; uzs(c,c->A);
}
static void sub8(CPU_TLCS870 *c,uint8_t v,bool sbc,bool store){
    uint8_t cin=sbc&&gf(c,FLAG_C)?1:0;
    uint16_t r=(uint16_t)c->A-v-cin;
    sf(c,FLAG_H,(c->A&0xF)<(v&0xF)+cin);
    sf(c,FLAG_C,r>0xFF); sf(c,FLAG_N,true);
    sf(c,FLAG_V,((c->A^v)&(c->A^(uint8_t)r)&0x80)!=0);
    if(store){c->A=(uint8_t)r;} uzs(c,(uint8_t)r);
}
static void and8(CPU_TLCS870 *c,uint8_t v){ c->A&=v; sf(c,FLAG_H,true);  sf(c,FLAG_N,false);sf(c,FLAG_C,false);sf(c,FLAG_V,par(c->A));uzs(c,c->A); }
static void or8 (CPU_TLCS870 *c,uint8_t v){ c->A|=v; sf(c,FLAG_H,false); sf(c,FLAG_N,false);sf(c,FLAG_C,false);sf(c,FLAG_V,par(c->A));uzs(c,c->A); }
static void xor8(CPU_TLCS870 *c,uint8_t v){ c->A^=v; sf(c,FLAG_H,false); sf(c,FLAG_N,false);sf(c,FLAG_C,false);sf(c,FLAG_V,par(c->A));uzs(c,c->A); }
static void addhl(CPU_TLCS870 *c,uint16_t v){
    uint16_t hl=REG_HL(c); uint32_t r=(uint32_t)hl+v;
    sf(c,FLAG_H,((hl&0xFFF)+(v&0xFFF))>0xFFF); sf(c,FLAG_N,false); sf(c,FLAG_C,r>0xFFFF);
    c->H=(r>>8)&0xFF; c->L=r&0xFF;
}

// ── INC / DEC (preserve C) ────────────────────────────────────
static uint8_t inc8(CPU_TLCS870 *c,uint8_t v){ sf(c,FLAG_V,v==0x7F);sf(c,FLAG_H,(v&0xF)==0xF);sf(c,FLAG_N,false);v++;uzs(c,v);return v; }
static uint8_t dec8(CPU_TLCS870 *c,uint8_t v){ sf(c,FLAG_V,v==0x80);sf(c,FLAG_H,(v&0xF)==0);sf(c,FLAG_N,true);v--;uzs(c,v);return v; }

// ── Rotate / Shift ────────────────────────────────────────────
static uint8_t rlc(CPU_TLCS870 *c,uint8_t v){ uint8_t r=(v<<1)|(v>>7); sf(c,FLAG_C,(v&0x80)!=0);sf(c,FLAG_N,false);sf(c,FLAG_H,false);sf(c,FLAG_V,par(r));uzs(c,r);return r; }
static uint8_t rrc(CPU_TLCS870 *c,uint8_t v){ uint8_t r=(v>>1)|(v<<7); sf(c,FLAG_C,(v&1)!=0);  sf(c,FLAG_N,false);sf(c,FLAG_H,false);sf(c,FLAG_V,par(r));uzs(c,r);return r; }
static uint8_t rl (CPU_TLCS870 *c,uint8_t v){ uint8_t r=(v<<1)|(gf(c,FLAG_C)?1:0); sf(c,FLAG_C,(v&0x80)!=0);sf(c,FLAG_N,false);sf(c,FLAG_H,false);sf(c,FLAG_V,par(r));uzs(c,r);return r; }
static uint8_t rr (CPU_TLCS870 *c,uint8_t v){ uint8_t r=(v>>1)|(gf(c,FLAG_C)?0x80:0); sf(c,FLAG_C,(v&1)!=0);sf(c,FLAG_N,false);sf(c,FLAG_H,false);sf(c,FLAG_V,par(r));uzs(c,r);return r; }
static uint8_t sla(CPU_TLCS870 *c,uint8_t v){ sf(c,FLAG_C,(v&0x80)!=0);uint8_t r=v<<1; sf(c,FLAG_N,false);sf(c,FLAG_H,false);sf(c,FLAG_V,par(r));uzs(c,r);return r; }
static uint8_t sra(CPU_TLCS870 *c,uint8_t v){ sf(c,FLAG_C,(v&1)!=0);uint8_t r=(v>>1)|(v&0x80);sf(c,FLAG_N,false);sf(c,FLAG_H,false);sf(c,FLAG_V,par(r));uzs(c,r);return r; }
static uint8_t sll(CPU_TLCS870 *c,uint8_t v){ sf(c,FLAG_C,(v&0x80)!=0);uint8_t r=(v<<1)|1;    sf(c,FLAG_N,false);sf(c,FLAG_H,false);sf(c,FLAG_V,par(r));uzs(c,r);return r; }
static uint8_t srl(CPU_TLCS870 *c,uint8_t v){ sf(c,FLAG_C,(v&1)!=0);uint8_t r=v>>1;           sf(c,FLAG_N,false);sf(c,FLAG_H,false);sf(c,FLAG_V,par(r));uzs(c,r);return r; }

// ── CB prefix ─────────────────────────────────────────────────
static int exec_cb(CPU_TLCS870 *c){
    uint8_t op=mr8(c,c->PC++); int reg=op&7; int grp=op>>3;
    int cyc=(reg==6)?16:8;
    uint8_t v=rg(c,reg),r=v;
    if(grp<8){
        switch(grp){case 0:r=rlc(c,v);break;case 1:r=rrc(c,v);break;case 2:r=rl(c,v);break;case 3:r=rr(c,v);break;
                    case 4:r=sla(c,v);break;case 5:r=sra(c,v);break;case 6:r=sll(c,v);break;case 7:r=srl(c,v);break;}
        rs(c,reg,r);
    } else if(grp<16){
        sf(c,FLAG_Z,(v&(1<<(grp-8)))==0); sf(c,FLAG_H,true); sf(c,FLAG_N,false);
        return (reg==6)?12:8;
    } else if(grp<24){
        rs(c,reg,v&~(1<<(grp-16)));
    } else {
        rs(c,reg,v|(1<<(grp-24)));
    }
    return cyc;
}

// ── DD/FD prefix (IX/IY) ──────────────────────────────────────
// In DD/FD mode, HL→IX/IY, (HL)→(IX+d)/(IY+d), H→IXH/IYH, L→IXL/IYL
static int exec_ddfd(CPU_TLCS870 *c, uint16_t *ir) {
    uint8_t op = mr8(c, c->PC++);
    int cyc = 4;

    // Chained prefixes: FD FD → last prefix wins, DD FD → switch to IY, etc.
    while (op == 0xDD || op == 0xFD) {
        ir = (op == 0xDD) ? &c->IX : &c->IY;
        op = mr8(c, c->PC++);
        cyc += 4;
    }
    uint8_t irh = (*ir >> 8) & 0xFF;
    uint8_t irl = *ir & 0xFF;

    // Helper: read register, replacing H/L with IRH/IRL and (HL) with (IR+d)
    #define DDFD_RG(r, d_used) \
        ((r)==4 ? irh : (r)==5 ? irl : (r)==6 ? (d_used=1, mr8(c, (uint16_t)((int16_t)*ir + (int8_t)mr8(c, c->PC)))) : rg(c, r))
    #define DDFD_RS(r, v, d_used) do { \
        if ((r)==4) { irh = (v); *ir = ((uint16_t)irh << 8) | irl; } \
        else if ((r)==5) { irl = (v); *ir = ((uint16_t)irh << 8) | irl; } \
        else if ((r)==6) { d_used = 1; mw8(c, (uint16_t)((int16_t)*ir + (int8_t)mr8(c, c->PC)), (v)); } \
        else rs(c, r, v); \
    } while(0)

    switch (op) {
    // LD IR, nn
    case 0x21: { uint16_t nn = mr16(c, c->PC); c->PC += 2; *ir = nn; cyc = 14; break; }
    // LD (nn), IR
    case 0x22: { uint16_t nn = mr16(c, c->PC); c->PC += 2; mw16(c, nn, *ir); cyc = 20; break; }
    // LD IR, (nn)
    case 0x2A: { uint16_t nn = mr16(c, c->PC); c->PC += 2; *ir = mr16(c, nn); cyc = 20; break; }
    // INC IR
    case 0x23: (*ir)++; cyc = 10; break;
    // DEC IR
    case 0x2B: (*ir)--; cyc = 10; break;
    // ADD IR, rr
    case 0x09: { uint32_t r = (uint32_t)*ir + REG_BC(c); sf(c, FLAG_H, ((*ir & 0xFFF) + (REG_BC(c) & 0xFFF)) > 0xFFF); sf(c, FLAG_N, false); sf(c, FLAG_C, r > 0xFFFF); *ir = (uint16_t)r; cyc = 15; break; }
    case 0x19: { uint32_t r = (uint32_t)*ir + REG_DE(c); sf(c, FLAG_H, ((*ir & 0xFFF) + (REG_DE(c) & 0xFFF)) > 0xFFF); sf(c, FLAG_N, false); sf(c, FLAG_C, r > 0xFFFF); *ir = (uint16_t)r; cyc = 15; break; }
    case 0x29: { uint32_t r = (uint32_t)*ir + *ir;        sf(c, FLAG_H, ((*ir & 0xFFF) + (*ir & 0xFFF)) > 0xFFF);        sf(c, FLAG_N, false); sf(c, FLAG_C, r > 0xFFFF); *ir = (uint16_t)r; cyc = 15; break; }
    case 0x39: { uint32_t r = (uint32_t)*ir + c->SP;      sf(c, FLAG_H, ((*ir & 0xFFF) + (c->SP & 0xFFF)) > 0xFFF);      sf(c, FLAG_N, false); sf(c, FLAG_C, r > 0xFFFF); *ir = (uint16_t)r; cyc = 15; break; }
    // PUSH IR
    case 0xE5: push(c, *ir); cyc = 15; break;
    // POP IR
    case 0xE1: *ir = pop(c); cyc = 14; break;
    // JP (IR)
    case 0xE9: c->PC = *ir; cyc = 8; break;
    // LD SP, IR
    case 0xF9: c->SP = *ir; cyc = 10; break;
    // EX (SP), IR
    case 0xE3: { uint16_t v = mr16(c, c->SP); mw16(c, c->SP, *ir); *ir = v; cyc = 23; break; }
    // INC IRH
    case 0x24: irh = inc8(c, irh); *ir = ((uint16_t)irh << 8) | irl; cyc = 8; break;
    // DEC IRH
    case 0x25: irh = dec8(c, irh); *ir = ((uint16_t)irh << 8) | irl; cyc = 8; break;
    // LD IRH, n
    case 0x26: irh = mr8(c, c->PC++); *ir = ((uint16_t)irh << 8) | irl; cyc = 11; break;
    // INC IRL
    case 0x2C: irl = inc8(c, irl); *ir = ((uint16_t)irh << 8) | irl; cyc = 8; break;
    // DEC IRL
    case 0x2D: irl = dec8(c, irl); *ir = ((uint16_t)irh << 8) | irl; cyc = 8; break;
    // LD IRL, n
    case 0x2E: irl = mr8(c, c->PC++); *ir = ((uint16_t)irh << 8) | irl; cyc = 11; break;
    // INC (IR+d)
    case 0x34: { int8_t d = (int8_t)mr8(c, c->PC++); uint16_t a = (uint16_t)((int16_t)*ir + d); uint8_t v = inc8(c, mr8(c, a)); mw8(c, a, v); cyc = 23; break; }
    // DEC (IR+d)
    case 0x35: { int8_t d = (int8_t)mr8(c, c->PC++); uint16_t a = (uint16_t)((int16_t)*ir + d); uint8_t v = dec8(c, mr8(c, a)); mw8(c, a, v); cyc = 23; break; }
    // LD (IR+d), n
    case 0x36: { int8_t d = (int8_t)mr8(c, c->PC++); uint8_t n = mr8(c, c->PC++); mw8(c, (uint16_t)((int16_t)*ir + d), n); cyc = 19; break; }

    // LD r, (IR+d) — 0x46,0x4E,0x56,0x5E,0x66,0x6E,0x7E
    case 0x46: case 0x4E: case 0x56: case 0x5E: case 0x66: case 0x6E: case 0x7E: {
        int8_t d = (int8_t)mr8(c, c->PC++);
        uint8_t v = mr8(c, (uint16_t)((int16_t)*ir + d));
        rs(c, (op >> 3) & 7, v);
        cyc = 19;
        break;
    }
    // LD (IR+d), r — 0x70-0x75, 0x77
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x77: {
        int8_t d = (int8_t)mr8(c, c->PC++);
        mw8(c, (uint16_t)((int16_t)*ir + d), rg(c, op & 7));
        cyc = 19;
        break;
    }

    // LD between IRH/IRL and registers (0x44,0x45,0x4C,0x4D,0x54,0x55,0x5C,0x5D,0x60-0x65,0x67,0x68-0x6D,0x6F)
    case 0x44: c->B = irh; cyc = 8; break;
    case 0x45: c->B = irl; cyc = 8; break;
    case 0x4C: c->C = irh; cyc = 8; break;
    case 0x4D: c->C = irl; cyc = 8; break;
    case 0x54: c->D = irh; cyc = 8; break;
    case 0x55: c->D = irl; cyc = 8; break;
    case 0x5C: c->E = irh; cyc = 8; break;
    case 0x5D: c->E = irl; cyc = 8; break;
    case 0x7C: c->A = irh; cyc = 8; break;
    case 0x7D: c->A = irl; cyc = 8; break;
    case 0x60: irh = c->B; *ir = ((uint16_t)irh << 8) | irl; cyc = 8; break;
    case 0x61: irh = c->C; *ir = ((uint16_t)irh << 8) | irl; cyc = 8; break;
    case 0x62: irh = c->D; *ir = ((uint16_t)irh << 8) | irl; cyc = 8; break;
    case 0x63: irh = c->E; *ir = ((uint16_t)irh << 8) | irl; cyc = 8; break;
    case 0x64: cyc = 8; break; // LD IRH, IRH (nop)
    case 0x65: irh = irl; *ir = ((uint16_t)irh << 8) | irl; cyc = 8; break;
    case 0x67: irh = c->A; *ir = ((uint16_t)irh << 8) | irl; cyc = 8; break;
    case 0x68: irl = c->B; *ir = ((uint16_t)irh << 8) | irl; cyc = 8; break;
    case 0x69: irl = c->C; *ir = ((uint16_t)irh << 8) | irl; cyc = 8; break;
    case 0x6A: irl = c->D; *ir = ((uint16_t)irh << 8) | irl; cyc = 8; break;
    case 0x6B: irl = c->E; *ir = ((uint16_t)irh << 8) | irl; cyc = 8; break;
    case 0x6C: irl = irh; *ir = ((uint16_t)irh << 8) | irl; cyc = 8; break;
    case 0x6D: cyc = 8; break; // LD IRL, IRL (nop)
    case 0x6F: irl = c->A; *ir = ((uint16_t)irh << 8) | irl; cyc = 8; break;

    // ALU A, (IR+d)
    case 0x86: { int8_t d = (int8_t)mr8(c, c->PC++); add8(c, mr8(c, (uint16_t)((int16_t)*ir + d)), false); cyc = 19; break; }
    case 0x8E: { int8_t d = (int8_t)mr8(c, c->PC++); add8(c, mr8(c, (uint16_t)((int16_t)*ir + d)), true);  cyc = 19; break; }
    case 0x96: { int8_t d = (int8_t)mr8(c, c->PC++); sub8(c, mr8(c, (uint16_t)((int16_t)*ir + d)), false, true); cyc = 19; break; }
    case 0x9E: { int8_t d = (int8_t)mr8(c, c->PC++); sub8(c, mr8(c, (uint16_t)((int16_t)*ir + d)), true, true);  cyc = 19; break; }
    case 0xA6: { int8_t d = (int8_t)mr8(c, c->PC++); and8(c, mr8(c, (uint16_t)((int16_t)*ir + d))); cyc = 19; break; }
    case 0xAE: { int8_t d = (int8_t)mr8(c, c->PC++); xor8(c, mr8(c, (uint16_t)((int16_t)*ir + d))); cyc = 19; break; }
    case 0xB6: { int8_t d = (int8_t)mr8(c, c->PC++); or8 (c, mr8(c, (uint16_t)((int16_t)*ir + d))); cyc = 19; break; }
    case 0xBE: { int8_t d = (int8_t)mr8(c, c->PC++); sub8(c, mr8(c, (uint16_t)((int16_t)*ir + d)), false, false); cyc = 19; break; }

    // ALU A, IRH/IRL
    case 0x84: add8(c, irh, false); cyc = 8; break;
    case 0x85: add8(c, irl, false); cyc = 8; break;
    case 0x8C: add8(c, irh, true);  cyc = 8; break;
    case 0x8D: add8(c, irl, true);  cyc = 8; break;
    case 0x94: sub8(c, irh, false, true); cyc = 8; break;
    case 0x95: sub8(c, irl, false, true); cyc = 8; break;
    case 0x9C: sub8(c, irh, true, true);  cyc = 8; break;
    case 0x9D: sub8(c, irl, true, true);  cyc = 8; break;
    case 0xA4: and8(c, irh); cyc = 8; break;
    case 0xA5: and8(c, irl); cyc = 8; break;
    case 0xAC: xor8(c, irh); cyc = 8; break;
    case 0xAD: xor8(c, irl); cyc = 8; break;
    case 0xB4: or8 (c, irh); cyc = 8; break;
    case 0xB5: or8 (c, irl); cyc = 8; break;
    case 0xBC: sub8(c, irh, false, false); cyc = 8; break;
    case 0xBD: sub8(c, irl, false, false); cyc = 8; break;

    // DDCB / FDCB prefix — bit ops on (IR+d)
    case 0xCB: {
        int8_t d = (int8_t)mr8(c, c->PC++);
        uint8_t cb_op = mr8(c, c->PC++);
        uint16_t a = (uint16_t)((int16_t)*ir + d);
        uint8_t v = mr8(c, a);
        int grp = cb_op >> 3;
        uint8_t r = v;
        if (grp < 8) {
            switch (grp) {
                case 0: r = rlc(c, v); break; case 1: r = rrc(c, v); break;
                case 2: r = rl(c, v);  break; case 3: r = rr(c, v);  break;
                case 4: r = sla(c, v); break; case 5: r = sra(c, v); break;
                case 6: r = sll(c, v); break; case 7: r = srl(c, v); break;
            }
            mw8(c, a, r);
            // Undocumented: also store in register (if not (HL))
            if ((cb_op & 7) != 6) rs(c, cb_op & 7, r);
            cyc = 23;
        } else if (grp < 16) {
            // BIT
            sf(c, FLAG_Z, (v & (1 << (grp - 8))) == 0);
            sf(c, FLAG_H, true); sf(c, FLAG_N, false);
            cyc = 20;
        } else if (grp < 24) {
            // RES
            r = v & ~(1 << (grp - 16));
            mw8(c, a, r);
            if ((cb_op & 7) != 6) rs(c, cb_op & 7, r);
            cyc = 23;
        } else {
            // SET
            r = v | (1 << (grp - 24));
            mw8(c, a, r);
            if ((cb_op & 7) != 6) rs(c, cb_op & 7, r);
            cyc = 23;
        }
        break;
    }

    default:
        printf("[CPU] Unknown DD/FD 0x%02X at PC=0x%04X\n", op, c->PC - 2);
        cyc = 4;
        break;
    }

    #undef DDFD_RG
    #undef DDFD_RS
    return cyc;
}

// ── ED prefix ─────────────────────────────────────────────────
static int exec_ed(CPU_TLCS870 *c){
    uint8_t op=mr8(c,c->PC++);
    switch(op){
    // NEG
    case 0x44:case 0x4C:case 0x54:case 0x5C:case 0x64:case 0x6C:case 0x74:case 0x7C:{
        uint8_t a=c->A; c->A=0; sub8(c,a,false,true);
        sf(c,FLAG_V,a==0x80); sf(c,FLAG_C,a!=0); return 8;
    }
    case 0x45:case 0x55:case 0x65:case 0x75: c->ime=true; c->PC=pop(c); return 16; // RETN
    case 0x4D:                                c->ime=true; c->PC=pop(c); return 16; // RETI
    case 0x46:case 0x4E:case 0x56:case 0x5E:case 0x66:case 0x6E:case 0x76:case 0x7E: return 8; // IM
    case 0x47: c->I=c->A; return 8;
    case 0x4F: c->R=c->A; return 8;
    case 0x57: c->A=c->I; uzs(c,c->A); sf(c,FLAG_N,false);sf(c,FLAG_H,false);sf(c,FLAG_V,c->ime); return 8;
    case 0x5F: c->A=c->R; uzs(c,c->A); sf(c,FLAG_N,false);sf(c,FLAG_H,false);sf(c,FLAG_V,c->ime); return 8;
    // ADC HL, rr
    case 0x4A:{uint8_t cy=gf(c,FLAG_C)?1:0;uint32_t r=(uint32_t)REG_HL(c)+REG_BC(c)+cy;sf(c,FLAG_C,r>0xFFFF);sf(c,FLAG_N,false);c->H=(r>>8)&0xFF;c->L=r&0xFF;sf(c,FLAG_Z,(uint16_t)r==0);sf(c,FLAG_S,((uint16_t)r&0x8000)!=0);return 16;}
    case 0x5A:{uint8_t cy=gf(c,FLAG_C)?1:0;uint32_t r=(uint32_t)REG_HL(c)+REG_DE(c)+cy;sf(c,FLAG_C,r>0xFFFF);sf(c,FLAG_N,false);c->H=(r>>8)&0xFF;c->L=r&0xFF;sf(c,FLAG_Z,(uint16_t)r==0);sf(c,FLAG_S,((uint16_t)r&0x8000)!=0);return 16;}
    case 0x6A:{uint8_t cy=gf(c,FLAG_C)?1:0;uint32_t r=(uint32_t)REG_HL(c)+REG_HL(c)+cy;sf(c,FLAG_C,r>0xFFFF);sf(c,FLAG_N,false);c->H=(r>>8)&0xFF;c->L=r&0xFF;sf(c,FLAG_Z,(uint16_t)r==0);return 16;}
    case 0x7A:{uint8_t cy=gf(c,FLAG_C)?1:0;uint32_t r=(uint32_t)REG_HL(c)+c->SP+cy;    sf(c,FLAG_C,r>0xFFFF);sf(c,FLAG_N,false);c->H=(r>>8)&0xFF;c->L=r&0xFF;sf(c,FLAG_Z,(uint16_t)r==0);return 16;}
    // SBC HL, rr
    case 0x42:{uint8_t cy=gf(c,FLAG_C)?1:0;uint16_t hl=REG_HL(c),rr=REG_BC(c);uint32_t r=(uint32_t)hl-rr-cy;sf(c,FLAG_C,r>0xFFFF);sf(c,FLAG_N,true);sf(c,FLAG_V,((hl^rr)&(hl^(uint16_t)r)&0x8000)!=0);c->H=((uint16_t)r>>8)&0xFF;c->L=(uint16_t)r&0xFF;sf(c,FLAG_Z,(uint16_t)r==0);sf(c,FLAG_S,((uint16_t)r&0x8000)!=0);return 16;}
    case 0x52:{uint8_t cy=gf(c,FLAG_C)?1:0;uint16_t hl=REG_HL(c),rr=REG_DE(c);uint32_t r=(uint32_t)hl-rr-cy;sf(c,FLAG_C,r>0xFFFF);sf(c,FLAG_N,true);c->H=((uint16_t)r>>8)&0xFF;c->L=(uint16_t)r&0xFF;sf(c,FLAG_Z,(uint16_t)r==0);return 16;}
    case 0x62:{uint8_t cy=gf(c,FLAG_C)?1:0;uint16_t hl=REG_HL(c);uint32_t r=(uint32_t)hl-hl-cy;sf(c,FLAG_C,r>0xFFFF);sf(c,FLAG_N,true);c->H=((uint16_t)r>>8)&0xFF;c->L=(uint16_t)r&0xFF;sf(c,FLAG_Z,(uint16_t)r==0);return 16;}
    case 0x72:{uint8_t cy=gf(c,FLAG_C)?1:0;uint16_t hl=REG_HL(c);uint32_t r=(uint32_t)hl-c->SP-cy;sf(c,FLAG_C,r>0xFFFF);sf(c,FLAG_N,true);c->H=((uint16_t)r>>8)&0xFF;c->L=(uint16_t)r&0xFF;sf(c,FLAG_Z,(uint16_t)r==0);return 16;}
    // LD (nn), rr
    case 0x43:{uint16_t nn=mr16(c,c->PC);c->PC+=2;mw16(c,nn,REG_BC(c));return 20;}
    case 0x53:{uint16_t nn=mr16(c,c->PC);c->PC+=2;mw16(c,nn,REG_DE(c));return 20;}
    case 0x63:{uint16_t nn=mr16(c,c->PC);c->PC+=2;mw16(c,nn,REG_HL(c));return 20;}
    case 0x73:{uint16_t nn=mr16(c,c->PC);c->PC+=2;mw16(c,nn,c->SP);    return 20;}
    // LD rr, (nn)
    case 0x4B:{uint16_t nn=mr16(c,c->PC);c->PC+=2;uint16_t v=mr16(c,nn);c->B=v>>8;c->C=v&0xFF;return 20;}
    case 0x5B:{uint16_t nn=mr16(c,c->PC);c->PC+=2;uint16_t v=mr16(c,nn);c->D=v>>8;c->E=v&0xFF;return 20;}
    case 0x6B:{uint16_t nn=mr16(c,c->PC);c->PC+=2;uint16_t v=mr16(c,nn);c->H=v>>8;c->L=v&0xFF;return 20;}
    case 0x7B:{uint16_t nn=mr16(c,c->PC);c->PC+=2;c->SP=mr16(c,nn);return 20;}
    // LDI
    case 0xA0:{mw8(c,REG_DE(c),mr8(c,REG_HL(c)));uint16_t de=REG_DE(c)+1;c->D=de>>8;c->E=de&0xFF;uint16_t hl=REG_HL(c)+1;c->H=hl>>8;c->L=hl&0xFF;uint16_t bc=REG_BC(c)-1;c->B=bc>>8;c->C=bc&0xFF;sf(c,FLAG_V,bc!=0);sf(c,FLAG_H,false);sf(c,FLAG_N,false);return 16;}
    // LDD
    case 0xA8:{mw8(c,REG_DE(c),mr8(c,REG_HL(c)));uint16_t de=REG_DE(c)-1;c->D=de>>8;c->E=de&0xFF;uint16_t hl=REG_HL(c)-1;c->H=hl>>8;c->L=hl&0xFF;uint16_t bc=REG_BC(c)-1;c->B=bc>>8;c->C=bc&0xFF;sf(c,FLAG_V,bc!=0);sf(c,FLAG_H,false);sf(c,FLAG_N,false);return 16;}
    // LDIR
    case 0xB0:{int t=0;do{mw8(c,REG_DE(c),mr8(c,REG_HL(c)));uint16_t de=REG_DE(c)+1;c->D=de>>8;c->E=de&0xFF;uint16_t hl=REG_HL(c)+1;c->H=hl>>8;c->L=hl&0xFF;uint16_t bc=REG_BC(c)-1;c->B=bc>>8;c->C=bc&0xFF;t+=21;}while(REG_BC(c)!=0);sf(c,FLAG_V,false);sf(c,FLAG_H,false);sf(c,FLAG_N,false);return t+16;}
    // LDDR
    case 0xB8:{int t=0;do{mw8(c,REG_DE(c),mr8(c,REG_HL(c)));uint16_t de=REG_DE(c)-1;c->D=de>>8;c->E=de&0xFF;uint16_t hl=REG_HL(c)-1;c->H=hl>>8;c->L=hl&0xFF;uint16_t bc=REG_BC(c)-1;c->B=bc>>8;c->C=bc&0xFF;t+=21;}while(REG_BC(c)!=0);sf(c,FLAG_V,false);sf(c,FLAG_H,false);sf(c,FLAG_N,false);return t+16;}
    // CPI
    case 0xA1:{uint8_t v=mr8(c,REG_HL(c));uint8_t r=c->A-v;uint16_t hl=REG_HL(c)+1;c->H=hl>>8;c->L=hl&0xFF;uint16_t bc=REG_BC(c)-1;c->B=bc>>8;c->C=bc&0xFF;uzs(c,r);sf(c,FLAG_H,(c->A&0xF)<(v&0xF));sf(c,FLAG_N,true);sf(c,FLAG_V,bc!=0);return 16;}
    // CPD
    case 0xA9:{uint8_t v=mr8(c,REG_HL(c));uint8_t r=c->A-v;uint16_t hl=REG_HL(c)-1;c->H=hl>>8;c->L=hl&0xFF;uint16_t bc=REG_BC(c)-1;c->B=bc>>8;c->C=bc&0xFF;uzs(c,r);sf(c,FLAG_H,(c->A&0xF)<(v&0xF));sf(c,FLAG_N,true);sf(c,FLAG_V,bc!=0);return 16;}
    // CPIR
    case 0xB1:{int t=0;do{uint8_t v=mr8(c,REG_HL(c));uint8_t r=c->A-v;uint16_t hl=REG_HL(c)+1;c->H=hl>>8;c->L=hl&0xFF;uint16_t bc=REG_BC(c)-1;c->B=bc>>8;c->C=bc&0xFF;t+=21;sf(c,FLAG_Z,r==0);if(r==0||REG_BC(c)==0)break;}while(1);sf(c,FLAG_N,true);sf(c,FLAG_V,REG_BC(c)!=0);return t;}
    // CPDR
    case 0xB9:{int t=0;do{uint8_t v=mr8(c,REG_HL(c));uint8_t r=c->A-v;uint16_t hl=REG_HL(c)-1;c->H=hl>>8;c->L=hl&0xFF;uint16_t bc=REG_BC(c)-1;c->B=bc>>8;c->C=bc&0xFF;t+=21;sf(c,FLAG_Z,r==0);if(r==0||REG_BC(c)==0)break;}while(1);sf(c,FLAG_N,true);sf(c,FLAG_V,REG_BC(c)!=0);return t;}
    default: printf("[CPU] Unknown ED 0x%02X at %04X\n",op,c->PC-2); return 8;
    }
}

// ─────────────────────────────────────────────────────────────
void cpu_tlcs870_init(CPU_TLCS870 *cpu,uint8_t *mem){ memset(cpu,0,sizeof(*cpu)); cpu->mem=mem; cpu_tlcs870_reset(cpu); }
void cpu_tlcs870_reset(CPU_TLCS870 *cpu){
    cpu->A=0xFF;cpu->F=0;cpu->B=cpu->C=cpu->D=cpu->E=cpu->H=cpu->L=0;
    cpu->Ap=cpu->Fp=cpu->Bp=cpu->Cp=cpu->Dp=cpu->Ep=cpu->Hp=cpu->Lp=0;
    cpu->IX=cpu->IY=0; cpu->I=cpu->R=0; cpu->SP=0xFFFF; cpu->PC=0x0000;
    cpu->ime=false; cpu->halted=false; cpu->irq_pending=0; cpu->cycles=0;
}

int cpu_tlcs870_step(CPU_TLCS870 *cpu){
    if(cpu->halted) return 4;
    cpu->R=(cpu->R&0x80)|((cpu->R+1)&0x7F);
    uint8_t op=mr8(cpu,cpu->PC++); int cyc=4;

    switch(op){
    case 0x00: cyc=4; break;       // NOP
    case 0x76: cpu->halted=true; cyc=4; break; // HALT

    // LD rr, nn
    case 0x01:{uint16_t n=mr16(cpu,cpu->PC);cpu->PC+=2;cpu->B=n>>8;cpu->C=n&0xFF;cyc=12;break;}
    case 0x11:{uint16_t n=mr16(cpu,cpu->PC);cpu->PC+=2;cpu->D=n>>8;cpu->E=n&0xFF;cyc=12;break;}
    case 0x21:{uint16_t n=mr16(cpu,cpu->PC);cpu->PC+=2;cpu->H=n>>8;cpu->L=n&0xFF;cyc=12;break;}
    case 0x31:{cpu->SP=mr16(cpu,cpu->PC);cpu->PC+=2;cyc=12;break;}
    // LD (rr),A / LD A,(rr)
    case 0x02: mw8(cpu,REG_BC(cpu),cpu->A); cyc=8; break;
    case 0x12: mw8(cpu,REG_DE(cpu),cpu->A); cyc=8; break;
    case 0x0A: cpu->A=mr8(cpu,REG_BC(cpu)); cyc=8; break;
    case 0x1A: cpu->A=mr8(cpu,REG_DE(cpu)); cyc=8; break;
    // LD (nn),A / LD A,(nn)
    case 0x32:{uint16_t a=mr16(cpu,cpu->PC);cpu->PC+=2;mw8(cpu,a,cpu->A);cyc=16;break;}
    case 0x3A:{uint16_t a=mr16(cpu,cpu->PC);cpu->PC+=2;cpu->A=mr8(cpu,a);cyc=16;break;}
    // LD (nn),HL / LD HL,(nn)
    case 0x22:{uint16_t a=mr16(cpu,cpu->PC);cpu->PC+=2;mw16(cpu,a,REG_HL(cpu));cyc=20;break;}
    case 0x2A:{uint16_t a=mr16(cpu,cpu->PC);cpu->PC+=2;uint16_t v=mr16(cpu,a);cpu->H=v>>8;cpu->L=v&0xFF;cyc=20;break;}
    // LD SP,HL
    case 0xF9: cpu->SP=REG_HL(cpu); cyc=8; break;
    // INC rr
    case 0x03:{uint16_t v=REG_BC(cpu)+1;cpu->B=v>>8;cpu->C=v&0xFF;cyc=8;break;}
    case 0x13:{uint16_t v=REG_DE(cpu)+1;cpu->D=v>>8;cpu->E=v&0xFF;cyc=8;break;}
    case 0x23:{uint16_t v=REG_HL(cpu)+1;cpu->H=v>>8;cpu->L=v&0xFF;cyc=8;break;}
    case 0x33: cpu->SP++; cyc=8; break;
    // DEC rr
    case 0x0B:{uint16_t v=REG_BC(cpu)-1;cpu->B=v>>8;cpu->C=v&0xFF;cyc=8;break;}
    case 0x1B:{uint16_t v=REG_DE(cpu)-1;cpu->D=v>>8;cpu->E=v&0xFF;cyc=8;break;}
    case 0x2B:{uint16_t v=REG_HL(cpu)-1;cpu->H=v>>8;cpu->L=v&0xFF;cyc=8;break;}
    case 0x3B: cpu->SP--; cyc=8; break;
    // INC r
    case 0x04: cpu->B=inc8(cpu,cpu->B);cyc=4;break;
    case 0x0C: cpu->C=inc8(cpu,cpu->C);cyc=4;break;
    case 0x14: cpu->D=inc8(cpu,cpu->D);cyc=4;break;
    case 0x1C: cpu->E=inc8(cpu,cpu->E);cyc=4;break;
    case 0x24: cpu->H=inc8(cpu,cpu->H);cyc=4;break;
    case 0x2C: cpu->L=inc8(cpu,cpu->L);cyc=4;break;
    case 0x34:{uint8_t v=inc8(cpu,mr8(cpu,REG_HL(cpu)));mw8(cpu,REG_HL(cpu),v);cyc=12;break;}
    case 0x3C: cpu->A=inc8(cpu,cpu->A);cyc=4;break;
    // DEC r
    case 0x05: cpu->B=dec8(cpu,cpu->B);cyc=4;break;
    case 0x0D: cpu->C=dec8(cpu,cpu->C);cyc=4;break;
    case 0x15: cpu->D=dec8(cpu,cpu->D);cyc=4;break;
    case 0x1D: cpu->E=dec8(cpu,cpu->E);cyc=4;break;
    case 0x25: cpu->H=dec8(cpu,cpu->H);cyc=4;break;
    case 0x2D: cpu->L=dec8(cpu,cpu->L);cyc=4;break;
    case 0x35:{uint8_t v=dec8(cpu,mr8(cpu,REG_HL(cpu)));mw8(cpu,REG_HL(cpu),v);cyc=12;break;}
    case 0x3D: cpu->A=dec8(cpu,cpu->A);cyc=4;break;
    // LD r, n
    case 0x06: cpu->B=mr8(cpu,cpu->PC++);cyc=8;break;
    case 0x0E: cpu->C=mr8(cpu,cpu->PC++);cyc=8;break;
    case 0x16: cpu->D=mr8(cpu,cpu->PC++);cyc=8;break;
    case 0x1E: cpu->E=mr8(cpu,cpu->PC++);cyc=8;break;
    case 0x26: cpu->H=mr8(cpu,cpu->PC++);cyc=8;break;
    case 0x2E: cpu->L=mr8(cpu,cpu->PC++);cyc=8;break;
    case 0x36:{uint8_t n=mr8(cpu,cpu->PC++);mw8(cpu,REG_HL(cpu),n);cyc=12;break;}
    case 0x3E: cpu->A=mr8(cpu,cpu->PC++);cyc=8;break;
    // RLCA/RRCA/RLA/RRA
    case 0x07:{uint8_t b=cpu->A>>7;cpu->A=(cpu->A<<1)|b;sf(cpu,FLAG_C,b);sf(cpu,FLAG_H,false);sf(cpu,FLAG_N,false);cyc=4;break;}
    case 0x0F:{uint8_t b=cpu->A&1;cpu->A=(cpu->A>>1)|(b<<7);sf(cpu,FLAG_C,b);sf(cpu,FLAG_H,false);sf(cpu,FLAG_N,false);cyc=4;break;}
    case 0x17:{uint8_t oc=gf(cpu,FLAG_C)?1:0;sf(cpu,FLAG_C,(cpu->A&0x80)!=0);cpu->A=(cpu->A<<1)|oc;sf(cpu,FLAG_H,false);sf(cpu,FLAG_N,false);cyc=4;break;}
    case 0x1F:{uint8_t oc=gf(cpu,FLAG_C)?0x80:0;sf(cpu,FLAG_C,(cpu->A&1)!=0);cpu->A=(cpu->A>>1)|oc;sf(cpu,FLAG_H,false);sf(cpu,FLAG_N,false);cyc=4;break;}
    // ADD HL, rr
    case 0x09: addhl(cpu,REG_BC(cpu));cyc=12;break;
    case 0x19: addhl(cpu,REG_DE(cpu));cyc=12;break;
    case 0x29: addhl(cpu,REG_HL(cpu));cyc=12;break;
    case 0x39: addhl(cpu,cpu->SP);    cyc=12;break;
    // JR
    case 0x18:{int8_t e=(int8_t)mr8(cpu,cpu->PC++);cpu->PC+=e;cyc=12;break;}
    case 0x20:{int8_t e=(int8_t)mr8(cpu,cpu->PC++);if(!gf(cpu,FLAG_Z)){cpu->PC+=e;cyc=12;}else cyc=8;break;}
    case 0x28:{int8_t e=(int8_t)mr8(cpu,cpu->PC++);if( gf(cpu,FLAG_Z)){cpu->PC+=e;cyc=12;}else cyc=8;break;}
    case 0x30:{int8_t e=(int8_t)mr8(cpu,cpu->PC++);if(!gf(cpu,FLAG_C)){cpu->PC+=e;cyc=12;}else cyc=8;break;}
    case 0x38:{int8_t e=(int8_t)mr8(cpu,cpu->PC++);if( gf(cpu,FLAG_C)){cpu->PC+=e;cyc=12;}else cyc=8;break;}
    // DJNZ
    case 0x10:{int8_t e=(int8_t)mr8(cpu,cpu->PC++);cpu->B--;if(cpu->B!=0){cpu->PC+=e;cyc=13;}else cyc=8;break;}
    // DAA
    case 0x27:{uint8_t a=cpu->A;uint16_t adj=0;if(gf(cpu,FLAG_H)||(!gf(cpu,FLAG_N)&&(a&0xF)>9))adj|=0x06;if(gf(cpu,FLAG_C)||(!gf(cpu,FLAG_N)&&a>0x99)){adj|=0x60;sf(cpu,FLAG_C,true);}cpu->A=gf(cpu,FLAG_N)?(a-(uint8_t)adj):(a+(uint8_t)adj);uzs(cpu,cpu->A);sf(cpu,FLAG_H,false);sf(cpu,FLAG_V,par(cpu->A));cyc=4;break;}
    case 0x2F:{cpu->A=~cpu->A;sf(cpu,FLAG_H,true);sf(cpu,FLAG_N,true);cyc=4;break;}  // CPL
    case 0x37:{sf(cpu,FLAG_C,true);sf(cpu,FLAG_N,false);sf(cpu,FLAG_H,false);cyc=4;break;}   // SCF
    case 0x3F:{sf(cpu,FLAG_H,gf(cpu,FLAG_C));sf(cpu,FLAG_C,!gf(cpu,FLAG_C));sf(cpu,FLAG_N,false);cyc=4;break;} // CCF
    // EX AF, AF'
    case 0x08:{uint8_t ta=cpu->A,tf=cpu->F;cpu->A=cpu->Ap;cpu->F=cpu->Fp;cpu->Ap=ta;cpu->Fp=tf;cyc=4;break;}

    // LD r, r'  (0x40–0x7F, except 0x76=HALT)
    case 0x40:case 0x41:case 0x42:case 0x43:case 0x44:case 0x45:case 0x46:case 0x47:
    case 0x48:case 0x49:case 0x4A:case 0x4B:case 0x4C:case 0x4D:case 0x4E:case 0x4F:
    case 0x50:case 0x51:case 0x52:case 0x53:case 0x54:case 0x55:case 0x56:case 0x57:
    case 0x58:case 0x59:case 0x5A:case 0x5B:case 0x5C:case 0x5D:case 0x5E:case 0x5F:
    case 0x60:case 0x61:case 0x62:case 0x63:case 0x64:case 0x65:case 0x66:case 0x67:
    case 0x68:case 0x69:case 0x6A:case 0x6B:case 0x6C:case 0x6D:case 0x6E:case 0x6F:
    case 0x70:case 0x71:case 0x72:case 0x73:case 0x74:case 0x75:          case 0x77:
    case 0x78:case 0x79:case 0x7A:case 0x7B:case 0x7C:case 0x7D:case 0x7E:case 0x7F:
        {int d=(op>>3)&7,s=op&7;rs(cpu,d,rg(cpu,s));cyc=(s==6||d==6)?8:4;break;}

    // ALU A, r  (0x80–0xBF)
    case 0x80:case 0x81:case 0x82:case 0x83:case 0x84:case 0x85:case 0x86:case 0x87:
        {add8(cpu,rg(cpu,op&7),false);cyc=rc(op&7);break;}  // ADD
    case 0x88:case 0x89:case 0x8A:case 0x8B:case 0x8C:case 0x8D:case 0x8E:case 0x8F:
        {add8(cpu,rg(cpu,op&7),true); cyc=rc(op&7);break;}  // ADC
    case 0x90:case 0x91:case 0x92:case 0x93:case 0x94:case 0x95:case 0x96:case 0x97:
        {sub8(cpu,rg(cpu,op&7),false,true); cyc=rc(op&7);break;}  // SUB
    case 0x98:case 0x99:case 0x9A:case 0x9B:case 0x9C:case 0x9D:case 0x9E:case 0x9F:
        {sub8(cpu,rg(cpu,op&7),true, true); cyc=rc(op&7);break;}  // SBC
    case 0xA0:case 0xA1:case 0xA2:case 0xA3:case 0xA4:case 0xA5:case 0xA6:case 0xA7:
        {and8(cpu,rg(cpu,op&7));cyc=rc(op&7);break;}  // AND
    case 0xA8:case 0xA9:case 0xAA:case 0xAB:case 0xAC:case 0xAD:case 0xAE:case 0xAF:
        {xor8(cpu,rg(cpu,op&7));cyc=rc(op&7);break;}  // XOR
    case 0xB0:case 0xB1:case 0xB2:case 0xB3:case 0xB4:case 0xB5:case 0xB6:case 0xB7:
        {or8 (cpu,rg(cpu,op&7));cyc=rc(op&7);break;}  // OR
    case 0xB8:case 0xB9:case 0xBA:case 0xBB:case 0xBC:case 0xBD:case 0xBE:case 0xBF:
        {sub8(cpu,rg(cpu,op&7),false,false);cyc=rc(op&7);break;}  // CP

    // RET cc / RET
    case 0xC0: if(!gf(cpu,FLAG_Z)){cpu->PC=pop(cpu);cyc=20;}else cyc=8;break;
    case 0xC8: if( gf(cpu,FLAG_Z)){cpu->PC=pop(cpu);cyc=20;}else cyc=8;break;
    case 0xD0: if(!gf(cpu,FLAG_C)){cpu->PC=pop(cpu);cyc=20;}else cyc=8;break;
    case 0xD8: if( gf(cpu,FLAG_C)){cpu->PC=pop(cpu);cyc=20;}else cyc=8;break;
    case 0xE0: if(!gf(cpu,FLAG_V)){cpu->PC=pop(cpu);cyc=20;}else cyc=8;break;
    case 0xE8: if( gf(cpu,FLAG_V)){cpu->PC=pop(cpu);cyc=20;}else cyc=8;break;
    case 0xF0: if(!gf(cpu,FLAG_S)){cpu->PC=pop(cpu);cyc=20;}else cyc=8;break;
    case 0xF8: if( gf(cpu,FLAG_S)){cpu->PC=pop(cpu);cyc=20;}else cyc=8;break;
    case 0xC9: cpu->PC=pop(cpu);cyc=16;break;
    // POP rr
    case 0xC1:{uint16_t v=pop(cpu);cpu->B=v>>8;cpu->C=v&0xFF;cyc=12;break;}
    case 0xD1:{uint16_t v=pop(cpu);cpu->D=v>>8;cpu->E=v&0xFF;cyc=12;break;}
    case 0xE1:{uint16_t v=pop(cpu);cpu->H=v>>8;cpu->L=v&0xFF;cyc=12;break;}
    case 0xF1:{uint16_t v=pop(cpu);cpu->A=v>>8;cpu->F=v&0xFF;cyc=12;break;}
    // PUSH rr
    case 0xC5: push(cpu,REG_BC(cpu));cyc=16;break;
    case 0xD5: push(cpu,REG_DE(cpu));cyc=16;break;
    case 0xE5: push(cpu,REG_HL(cpu));cyc=16;break;
    case 0xF5: push(cpu,(uint16_t)(cpu->A<<8|cpu->F));cyc=16;break;
    // JP cc, nn
    case 0xC2:{uint16_t nn=mr16(cpu,cpu->PC);cpu->PC+=2;if(!gf(cpu,FLAG_Z)){cpu->PC=nn;cyc=16;}else cyc=12;break;}
    case 0xCA:{uint16_t nn=mr16(cpu,cpu->PC);cpu->PC+=2;if( gf(cpu,FLAG_Z)){cpu->PC=nn;cyc=16;}else cyc=12;break;}
    case 0xD2:{uint16_t nn=mr16(cpu,cpu->PC);cpu->PC+=2;if(!gf(cpu,FLAG_C)){cpu->PC=nn;cyc=16;}else cyc=12;break;}
    case 0xDA:{uint16_t nn=mr16(cpu,cpu->PC);cpu->PC+=2;if( gf(cpu,FLAG_C)){cpu->PC=nn;cyc=16;}else cyc=12;break;}
    case 0xE2:{uint16_t nn=mr16(cpu,cpu->PC);cpu->PC+=2;if(!gf(cpu,FLAG_V)){cpu->PC=nn;cyc=16;}else cyc=12;break;}
    case 0xEA:{uint16_t nn=mr16(cpu,cpu->PC);cpu->PC+=2;if( gf(cpu,FLAG_V)){cpu->PC=nn;cyc=16;}else cyc=12;break;}
    case 0xF2:{uint16_t nn=mr16(cpu,cpu->PC);cpu->PC+=2;if(!gf(cpu,FLAG_S)){cpu->PC=nn;cyc=16;}else cyc=12;break;}
    case 0xFA:{uint16_t nn=mr16(cpu,cpu->PC);cpu->PC+=2;if( gf(cpu,FLAG_S)){cpu->PC=nn;cyc=16;}else cyc=12;break;}
    case 0xC3:{uint16_t nn=mr16(cpu,cpu->PC);cpu->PC=nn;cyc=16;break;}
    case 0xE9: cpu->PC=REG_HL(cpu);cyc=4;break;
    // CALL cc, nn
    case 0xC4:{uint16_t nn=mr16(cpu,cpu->PC);cpu->PC+=2;if(!gf(cpu,FLAG_Z)){push(cpu,cpu->PC);cpu->PC=nn;cyc=24;}else cyc=12;break;}
    case 0xCC:{uint16_t nn=mr16(cpu,cpu->PC);cpu->PC+=2;if( gf(cpu,FLAG_Z)){push(cpu,cpu->PC);cpu->PC=nn;cyc=24;}else cyc=12;break;}
    case 0xD4:{uint16_t nn=mr16(cpu,cpu->PC);cpu->PC+=2;if(!gf(cpu,FLAG_C)){push(cpu,cpu->PC);cpu->PC=nn;cyc=24;}else cyc=12;break;}
    case 0xDC:{uint16_t nn=mr16(cpu,cpu->PC);cpu->PC+=2;if( gf(cpu,FLAG_C)){push(cpu,cpu->PC);cpu->PC=nn;cyc=24;}else cyc=12;break;}
    case 0xE4:{uint16_t nn=mr16(cpu,cpu->PC);cpu->PC+=2;if(!gf(cpu,FLAG_V)){push(cpu,cpu->PC);cpu->PC=nn;cyc=24;}else cyc=12;break;}
    case 0xEC:{uint16_t nn=mr16(cpu,cpu->PC);cpu->PC+=2;if( gf(cpu,FLAG_V)){push(cpu,cpu->PC);cpu->PC=nn;cyc=24;}else cyc=12;break;}
    case 0xF4:{uint16_t nn=mr16(cpu,cpu->PC);cpu->PC+=2;if(!gf(cpu,FLAG_S)){push(cpu,cpu->PC);cpu->PC=nn;cyc=24;}else cyc=12;break;}
    case 0xFC:{uint16_t nn=mr16(cpu,cpu->PC);cpu->PC+=2;if( gf(cpu,FLAG_S)){push(cpu,cpu->PC);cpu->PC=nn;cyc=24;}else cyc=12;break;}
    case 0xCD:{uint16_t nn=mr16(cpu,cpu->PC);cpu->PC+=2;push(cpu,cpu->PC);cpu->PC=nn;cyc=24;break;}
    // RST
    case 0xC7: push(cpu,cpu->PC);cpu->PC=0x00;cyc=16;break;
    case 0xCF: push(cpu,cpu->PC);cpu->PC=0x08;cyc=16;break;
    case 0xD7: push(cpu,cpu->PC);cpu->PC=0x10;cyc=16;break;
    case 0xDF: push(cpu,cpu->PC);cpu->PC=0x18;cyc=16;break;
    case 0xE7: push(cpu,cpu->PC);cpu->PC=0x20;cyc=16;break;
    case 0xEF: push(cpu,cpu->PC);cpu->PC=0x28;cyc=16;break;
    case 0xF7: push(cpu,cpu->PC);cpu->PC=0x30;cyc=16;break;
    case 0xFF: push(cpu,cpu->PC);cpu->PC=0x38;cyc=16;break;
    // ALU A, n (immediate)
    case 0xC6: add8(cpu,mr8(cpu,cpu->PC++),false);cyc=8;break;
    case 0xCE: add8(cpu,mr8(cpu,cpu->PC++),true); cyc=8;break;
    case 0xD6: sub8(cpu,mr8(cpu,cpu->PC++),false,true); cyc=8;break;
    case 0xDE: sub8(cpu,mr8(cpu,cpu->PC++),true, true); cyc=8;break;
    case 0xE6: and8(cpu,mr8(cpu,cpu->PC++));cyc=8;break;
    case 0xEE: xor8(cpu,mr8(cpu,cpu->PC++));cyc=8;break;
    case 0xF6: or8 (cpu,mr8(cpu,cpu->PC++));cyc=8;break;
    case 0xFE: sub8(cpu,mr8(cpu,cpu->PC++),false,false);cyc=8;break;
    // EX, EXX
    case 0xEB:{uint8_t t;t=cpu->D;cpu->D=cpu->H;cpu->H=t;t=cpu->E;cpu->E=cpu->L;cpu->L=t;cyc=4;break;}
    case 0xE3:{uint16_t v=mr16(cpu,cpu->SP);mw16(cpu,cpu->SP,REG_HL(cpu));cpu->H=v>>8;cpu->L=v&0xFF;cyc=20;break;}
    case 0xD9:{uint8_t t;t=cpu->B;cpu->B=cpu->Bp;cpu->Bp=t;t=cpu->C;cpu->C=cpu->Cp;cpu->Cp=t;t=cpu->D;cpu->D=cpu->Dp;cpu->Dp=t;t=cpu->E;cpu->E=cpu->Ep;cpu->Ep=t;t=cpu->H;cpu->H=cpu->Hp;cpu->Hp=t;t=cpu->L;cpu->L=cpu->Lp;cpu->Lp=t;cyc=4;break;}
    // EI / DI
    case 0xF3: cpu->ime=false;cyc=4;break;
    case 0xFB: cpu->ime=true; cyc=4;break;
    // ADD SP, e
    // 0xE8 = RET PE (handled above)
    // LD HL, SP+e
    // (0xF8 conflicts with RET M — RET M takes priority in the RET cc block above)

    // Prefixes
    case 0xCB: cyc=exec_cb(cpu); break;
    case 0xDD: cyc=exec_ddfd(cpu, &cpu->IX); break;
    case 0xFD: cyc=exec_ddfd(cpu, &cpu->IY); break;
    case 0xED: cyc=exec_ed(cpu); break;

    default:
        printf("[CPU] Unknown opcode 0x%02X at PC=0x%04X\n",op,cpu->PC-1);
        cyc=4; break;
    }

    cpu->cycles+=cyc;
    return cyc;
}

void cpu_tlcs870_irq(CPU_TLCS870 *cpu,uint8_t vector){
    if(!cpu->ime) return;
    cpu->halted=false; cpu->ime=false;
    push(cpu,cpu->PC);
    cpu->PC=mr16(cpu,vector);
    cpu->cycles+=24;
}

void cpu_tlcs870_dump(CPU_TLCS870 *cpu){
    printf("─── TLCS-870 CPU State ───────────────────────\n");
    printf("  PC=%04X  SP=%04X\n",cpu->PC,cpu->SP);
    printf("  A=%02X  F=%02X  [%c%c%c%c%c%c]\n",cpu->A,cpu->F,
        (cpu->F&FLAG_S)?'S':'.',(cpu->F&FLAG_Z)?'Z':'.',
        (cpu->F&FLAG_H)?'H':'.',(cpu->F&FLAG_V)?'V':'.',
        (cpu->F&FLAG_N)?'N':'.',(cpu->F&FLAG_C)?'C':'.');
    printf("  BC=%04X  DE=%04X  HL=%04X  I=%02X  R=%02X\n",REG_BC(cpu),REG_DE(cpu),REG_HL(cpu),cpu->I,cpu->R);
    printf("  IME=%d  HALTED=%d  CYCLES=%llu\n",cpu->ime,cpu->halted,(unsigned long long)cpu->cycles);
    printf("──────────────────────────────────────────────\n");
}
