#pragma once
#include "playdia.h"

#define FLAG_C  0x01
#define FLAG_N  0x02
#define FLAG_V  0x04
#define FLAG_H  0x10
#define FLAG_Z  0x40
#define FLAG_S  0x80

typedef struct CPU_TLCS870 {
    uint8_t  A, F;
    uint8_t  B, C, D, E, H, L;
    uint16_t SP, PC;
    // Shadow registers
    uint8_t  Ap, Fp, Bp, Cp, Dp, Ep, Hp, Lp;
    // Index registers (DD/FD prefix)
    uint16_t IX, IY;
    // Special
    uint8_t  I, R;
    bool     ime, halted;
    uint8_t  irq_pending;
    uint64_t cycles;
    uint8_t *mem;
} CPU_TLCS870;

#define REG_BC(c)  ((uint16_t)((c)->B << 8 | (c)->C))
#define REG_DE(c)  ((uint16_t)((c)->D << 8 | (c)->E))
#define REG_HL(c)  ((uint16_t)((c)->H << 8 | (c)->L))
#define REG_IX(c)  ((c)->IX)
#define REG_IY(c)  ((c)->IY)

void     cpu_tlcs870_init   (CPU_TLCS870 *cpu, uint8_t *mem);
void     cpu_tlcs870_reset  (CPU_TLCS870 *cpu);
int      cpu_tlcs870_step   (CPU_TLCS870 *cpu);
void     cpu_tlcs870_irq    (CPU_TLCS870 *cpu, uint8_t vector);
void     cpu_tlcs870_dump   (CPU_TLCS870 *cpu);
