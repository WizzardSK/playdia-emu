#pragma once
#include "playdia.h"

// NEC µPD78214GC — 78K/0 compatible, 12MHz
// r[0]=X r[1]=A r[2]=C r[3]=B r[4]=E r[5]=D r[6]=L r[7]=H
// pairs: rp0=AX rp1=BC rp2=DE rp3=HL

#define NEC_CY  0x01
#define NEC_AC  0x04
#define NEC_Z   0x40
#define NEC_IE  0x80

typedef struct CPU_NEC78K {
    uint8_t  r[8];      // r[0]=X r[1]=A r[2]=C r[3]=B r[4]=E r[5]=D r[6]=L r[7]=H
    uint8_t  rb[3][8];  // register banks 1-3
    uint16_t PC, SP;
    uint8_t  PSW;
    bool     halted, stopped;
    uint8_t  bank;
    uint8_t  port[16];
    uint8_t *mem;
    uint64_t cycles;
} CPU_NEC78K;

void cpu_nec78k_init  (CPU_NEC78K *cpu, uint8_t *mem);
void cpu_nec78k_reset (CPU_NEC78K *cpu);
int  cpu_nec78k_step  (CPU_NEC78K *cpu);
void cpu_nec78k_irq   (CPU_NEC78K *cpu, uint16_t vector_addr);
void cpu_nec78k_dump  (CPU_NEC78K *cpu);
