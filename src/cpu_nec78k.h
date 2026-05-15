#pragma once
#include "playdia.h"

/* ============================================================
 *  NEC µPD78214GC — 78K/II series CPU core (Playdia sub-CPU)
 *  12 MHz, handles CD-ROM control & I/O
 *
 *  Reference: NEC 78K/II Instruction User's Manual
 *             https://docs.alexrp.com/78k/78k_ii.pdf
 *
 *  78K/II is NOT 78K/0.  Key differences:
 *    - 1 MB segmented address space (20-bit physical)
 *      Code accesses use PC + CS-segment-equivalent (PMC[4:0])
 *    - Extended instruction set: MULU, DIVUW, BR/CALL !!addr20,
 *      MOV PSW,#byte, MOV ES,A, etc.
 *    - 4 register banks at 0xFEEx–0xFEFx (was 1 in 78K/S, 2 in 78K/0)
 *    - PSW adds RBS2 (3 bank-select bits total) and ISP fields
 *
 *  Register file (per bank, mapped at 0xFEE0–0xFEFF):
 *    r[0]=X r[1]=A r[2]=C r[3]=B r[4]=E r[5]=D r[6]=L r[7]=H
 *  Pair registers:
 *    rp0=AX  rp1=BC  rp2=DE  rp3=HL
 *
 *  This implementation is a WORK-IN-PROGRESS skeleton with the
 *  correct 78K/II architecture.  Many opcodes are still TODO and
 *  fall through to a NOP-equivalent.  See cpu_nec78k.c.
 * ============================================================ */

/* PSW bits (78K/II) */
#define NEC_CY   0x01      /* carry                          */
#define NEC_ISP  0x02      /* in-service priority            */
#define NEC_AC   0x04      /* auxiliary carry                */
#define NEC_RBS0 0x08      /* register bank select bit 0     */
#define NEC_RBS1 0x10      /* register bank select bit 1     */
#define NEC_RBS2 0x20      /* register bank select bit 2     */
#define NEC_Z    0x40      /* zero                           */
#define NEC_IE   0x80      /* interrupt enable               */

typedef struct CPU_NEC78K {
    /* register file – 8 banks of 8 GPRs, mapped at 0xFEE0..0xFEFF */
    uint8_t  r[8];            /* current bank (mirrored from RAM) */

    /* program counter is 16-bit within the current 64KB segment.
     * The 78K/II adds a 4-bit code segment (CS / "PMC[3:0]") for
     * 1 MB physical addressing.  Most Playdia code stays in CS=0. */
    uint16_t PC;
    uint8_t  CS;              /* code segment (0..15)             */

    uint16_t SP;
    uint8_t  PSW;

    /* extra-data segment for `MOV A,ES:[addr]` style accesses */
    uint8_t  ES;

    bool     halted, stopped;
    uint8_t  bank;            /* 0..7  (decoded from PSW.RBS2..0)  */
    uint8_t  port[16];

    uint8_t *mem;             /* 64KB current segment view         */
    uint64_t cycles;
} CPU_NEC78K;

void cpu_nec78k_init  (CPU_NEC78K *cpu, uint8_t *mem);
void cpu_nec78k_reset (CPU_NEC78K *cpu);
int  cpu_nec78k_step  (CPU_NEC78K *cpu);
void cpu_nec78k_irq   (CPU_NEC78K *cpu, uint16_t vector_addr);
void cpu_nec78k_dump  (CPU_NEC78K *cpu);
