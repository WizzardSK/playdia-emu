/* test_nec78k.c — NEC 78K/0 CPU test suite for Playdia sub-CPU */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "src/playdia.h"
#include "src/cpu_nec78k.h"

static int passed = 0, failed = 0;

#define EXPECT(name,cond) do { \
    if(cond){ printf("  PASS  %s\n",name); passed++; } \
    else    { printf("  FAIL  %s\n",name); failed++; } \
} while(0)

static void setup(CPU_NEC78K *cpu, uint8_t *mem, const uint8_t *prog, int len) {
    memset(mem, 0, 65536);
    memcpy(mem, prog, (size_t)len);
    /* reset vector → 0x0000 (already default start) */
    mem[0x0000] = 0x00; mem[0x0001] = 0x00; /* vector = 0x0000... */
    /* actually place program at 0x0100 and set vector there */
    memcpy(mem+0x0100, prog, (size_t)len);
    mem[0x0000] = 0x00; mem[0x0001] = 0x01; /* vector = 0x0100 */
    cpu_nec78k_init(cpu, mem);
    /* reset: PC should now be 0x0100 */
}

static void test_nop(void) {
    CPU_NEC78K cpu; uint8_t mem[65536];
    uint8_t prog[] = {0x00}; /* NOP */
    setup(&cpu,mem,prog,1);
    int clk = cpu_nec78k_step(&cpu);
    EXPECT("NOP advances PC", cpu.PC == 0x0101);
    EXPECT("NOP uses 2 clocks", clk == 2);
}

static void test_mov_r_imm(void) {
    CPU_NEC78K cpu; uint8_t mem[65536];
    /* MOV A,#0x42  -> B1 42 */
    uint8_t prog[] = {0xB1, 0x42};
    setup(&cpu,mem,prog,2);
    cpu_nec78k_step(&cpu);
    EXPECT("MOV A,#0x42", cpu.r[1] == 0x42);
    EXPECT("MOV A,# PC+2", cpu.PC == 0x0102);
}

static void test_mov_all_regs(void) {
    CPU_NEC78K cpu; uint8_t mem[65536];
    /* B0 11  B1 22  B2 33  B3 44  B4 55  B5 66  B6 77  B7 88 */
    uint8_t prog[] = {0xB0,0x11, 0xB1,0x22, 0xB2,0x33, 0xB3,0x44,
                      0xB4,0x55, 0xB5,0x66, 0xB6,0x77, 0xB7,0x88};
    setup(&cpu,mem,prog,sizeof(prog));
    for(int i=0;i<8;i++) cpu_nec78k_step(&cpu);
    EXPECT("MOV r0(X)=0x11", cpu.r[0] == 0x11);
    EXPECT("MOV r1(A)=0x22", cpu.r[1] == 0x22);
    EXPECT("MOV r2(C)=0x33", cpu.r[2] == 0x33);
    EXPECT("MOV r3(B)=0x44", cpu.r[3] == 0x44);
    EXPECT("MOV r4(E)=0x55", cpu.r[4] == 0x55);
    EXPECT("MOV r5(D)=0x66", cpu.r[5] == 0x66);
    EXPECT("MOV r6(L)=0x77", cpu.r[6] == 0x77);
    EXPECT("MOV r7(H)=0x88", cpu.r[7] == 0x88);
}

static void test_movw(void) {
    CPU_NEC78K cpu; uint8_t mem[65536];
    /* MOVW AX, #0x1234 → C0 34 12 */
    /* MOVW BC, #0xABCD → C2 CD AB */
    uint8_t prog[] = {0xC0,0x34,0x12, 0xC2,0xCD,0xAB};
    setup(&cpu,mem,prog,sizeof(prog));
    cpu_nec78k_step(&cpu);
    EXPECT("MOVW AX lo=0x34", cpu.r[0]==0x34);
    EXPECT("MOVW AX hi=0x12", cpu.r[1]==0x12);
    cpu_nec78k_step(&cpu);
    EXPECT("MOVW BC lo=0xCD", cpu.r[2]==0xCD);
    EXPECT("MOVW BC hi=0xAB", cpu.r[3]==0xAB);
}

static void test_add(void) {
    CPU_NEC78K cpu; uint8_t mem[65536];
    /* MOV A,#0x10; ADD A,#0x25 → result A=0x35, no carry */
    uint8_t prog[] = {0xB1,0x10, 0x02,0x25};
    setup(&cpu,mem,prog,sizeof(prog));
    cpu_nec78k_step(&cpu);
    cpu_nec78k_step(&cpu);
    EXPECT("ADD A,#0x25 result=0x35", cpu.r[1]==0x35);
    EXPECT("ADD no carry", !(cpu.PSW & NEC_CY));
    EXPECT("ADD no zero",  !(cpu.PSW & NEC_Z));
}

static void test_add_carry(void) {
    CPU_NEC78K cpu; uint8_t mem[65536];
    /* MOV A,#0xFF; ADD A,#0x01 → result=0x00 with carry */
    uint8_t prog[] = {0xB1,0xFF, 0x02,0x01};
    setup(&cpu,mem,prog,sizeof(prog));
    cpu_nec78k_step(&cpu); cpu_nec78k_step(&cpu);
    EXPECT("ADD overflow→0x00", cpu.r[1]==0x00);
    EXPECT("ADD carry set",     !!(cpu.PSW & NEC_CY));
    EXPECT("ADD zero flag",     !!(cpu.PSW & NEC_Z));
}

static void test_sub(void) {
    CPU_NEC78K cpu; uint8_t mem[65536];
    /* MOV A,#0x10; SUB A,#0x05 */
    uint8_t prog[] = {0xB1,0x10, 0x03,0x05};
    setup(&cpu,mem,prog,sizeof(prog));
    cpu_nec78k_step(&cpu); cpu_nec78k_step(&cpu);
    EXPECT("SUB result=0x0B", cpu.r[1]==0x0B);
    EXPECT("SUB no carry",    !(cpu.PSW & NEC_CY));
}

static void test_and_or_xor(void) {
    CPU_NEC78K cpu; uint8_t mem[65536];
    /* AND A,#0x0F then OR #0xA0 then XOR #0xFF */
    uint8_t prog[] = {0xB1,0x55, 0x04,0x0F, 0x05,0xA0, 0x40,0x41};
    /* note: 0x40 = XOR A,[HL], but [HL]=0 initially so XOR with 0 */
    /* let's do direct: MOV A,#0x55; AND #0x0F→0x05; OR #0xA0→0xA5 */
    uint8_t prog2[] = {0xB1,0x55, 0x04,0x0F, 0x05,0xA0};
    setup(&cpu,mem,prog2,sizeof(prog2));
    cpu_nec78k_step(&cpu);
    cpu_nec78k_step(&cpu);
    EXPECT("AND 0x55&0x0F=0x05", cpu.r[1]==0x05);
    cpu_nec78k_step(&cpu);
    EXPECT("OR  0x05|0xA0=0xA5", cpu.r[1]==0xA5);
}

static void test_cmp_z(void) {
    CPU_NEC78K cpu; uint8_t mem[65536];
    uint8_t prog[] = {0xB1,0x42, 0x01,0x42};
    setup(&cpu,mem,prog,sizeof(prog));
    cpu_nec78k_step(&cpu); cpu_nec78k_step(&cpu);
    EXPECT("CMP equal→Z set", !!(cpu.PSW & NEC_Z));
    EXPECT("CMP A unchanged",  cpu.r[1]==0x42);
}

static void test_inc_dec(void) {
    CPU_NEC78K cpu; uint8_t mem[65536];
    /* MOV B,#0x04  INC B  DEC B  DEC B */
    uint8_t prog[] = {0xB3,0x04, 0x5B, 0x53, 0x53};
    setup(&cpu,mem,prog,sizeof(prog));
    cpu_nec78k_step(&cpu);
    EXPECT("MOV B,#0x04", cpu.r[3]==0x04);
    cpu_nec78k_step(&cpu);
    EXPECT("INC B→0x05",  cpu.r[3]==0x05);
    cpu_nec78k_step(&cpu);
    EXPECT("DEC B→0x04",  cpu.r[3]==0x04);
    cpu_nec78k_step(&cpu);
    EXPECT("DEC B→0x03",  cpu.r[3]==0x03);
}

static void test_incw_decw(void) {
    CPU_NEC78K cpu; uint8_t mem[65536];
    /* MOVW HL,#0x00FF  INCW HL  INCW HL */
    uint8_t prog[] = {0xC6,0xFF,0x00, 0x86, 0x86};
    setup(&cpu,mem,prog,sizeof(prog));
    cpu_nec78k_step(&cpu);
    uint16_t hl = (uint16_t)(cpu.r[6]|(cpu.r[7]<<8));
    EXPECT("MOVW HL,#0x00FF", hl==0x00FF);
    cpu_nec78k_step(&cpu);
    hl = (uint16_t)(cpu.r[6]|(cpu.r[7]<<8));
    EXPECT("INCW HL→0x0100", hl==0x0100);
    cpu_nec78k_step(&cpu);
    hl = (uint16_t)(cpu.r[6]|(cpu.r[7]<<8));
    EXPECT("INCW HL→0x0101", hl==0x0101);
}

static void test_xch(void) {
    CPU_NEC78K cpu; uint8_t mem[65536];
    /* MOV A,#0xAA; MOV B,#0x55; XCH A,B (0x0B) */
    uint8_t prog[] = {0xB1,0xAA, 0xB3,0x55, 0x0B};
    setup(&cpu,mem,prog,sizeof(prog));
    cpu_nec78k_step(&cpu); cpu_nec78k_step(&cpu); cpu_nec78k_step(&cpu);
    EXPECT("XCH A,B → A=0x55", cpu.r[1]==0x55);
    EXPECT("XCH A,B → B=0xAA", cpu.r[3]==0xAA);
}

static void test_mov_hl_indirect(void) {
    CPU_NEC78K cpu; uint8_t mem[65536];
    /* MOVW HL,#0x0200  MOV [HL],A (with A=0 from init)
       then: MOV A,#0x99; MOV [HL],A; MOV A,#0; MOV A,[HL] */
    uint8_t prog[] = {
        0xC6,0x00,0x02,   /* MOVW HL,#0x0200 */
        0xB1,0x99,        /* MOV A,#0x99 */
        0x70,             /* MOV [HL],A */
        0xB1,0x00,        /* MOV A,#0 */
        0x60              /* MOV A,[HL] */
    };
    setup(&cpu,mem,prog,sizeof(prog));
    for(int i=0;i<5;i++) cpu_nec78k_step(&cpu);
    EXPECT("[HL] indirect round-trip", cpu.r[1]==0x99);
}

static void test_call_ret(void) {
    /* Program at 0x0100:
         CALL 0x0110    (9D 10 01)
         MOV A,#0x55    (B1 55)   ← should be skipped
       Label 0x0110:
         MOV A,#0x33   (B1 33)
         RET           (9E)
    */
    CPU_NEC78K cpu; uint8_t mem[65536];
    memset(mem,0,65536);
    mem[0x0000]=0x00; mem[0x0001]=0x01; /* reset → 0x0100 */
    /* at 0x0100: */
    mem[0x0100]=0x9D; mem[0x0101]=0x10; mem[0x0102]=0x01; /* CALL 0x0110 */
    mem[0x0103]=0xB1; mem[0x0104]=0x55; /* MOV A,#0x55 (should not run) */
    /* at 0x0110: */
    mem[0x0110]=0xB1; mem[0x0111]=0x33; /* MOV A,#0x33 */
    mem[0x0112]=0x9E;                   /* RET */
    cpu_nec78k_init(&cpu,mem);
    cpu_nec78k_step(&cpu); /* CALL */
    EXPECT("CALL jumps to 0x0110", cpu.PC==0x0110);
    cpu_nec78k_step(&cpu); /* MOV A,#0x33 */
    EXPECT("after CALL: MOV A=0x33", cpu.r[1]==0x33);
    cpu_nec78k_step(&cpu); /* RET */
    EXPECT("RET returns to 0x0103", cpu.PC==0x0103);
}

static void test_br_bz(void) {
    /* MOV A,#0; CMP A,#0 → Z=1; BZ +4 → skip 2 bytes; MOV A,#0xFF */
    CPU_NEC78K cpu; uint8_t mem[65536];
    memset(mem,0,65536);
    mem[0x0000]=0x00; mem[0x0001]=0x01;
    mem[0x0100]=0xB1; mem[0x0101]=0x00; /* MOV A,#0 */
    mem[0x0102]=0x01; mem[0x0103]=0x00; /* CMP A,#0 */
    mem[0x0104]=0xDE; mem[0x0105]=0x02; /* BZ +2 (skip next 2 bytes) */
    mem[0x0106]=0xB1; mem[0x0107]=0xFF; /* MOV A,#0xFF  (skipped) */
    mem[0x0108]=0xB1; mem[0x0109]=0x42; /* MOV A,#0x42 */
    cpu_nec78k_init(&cpu,mem);
    cpu_nec78k_step(&cpu); /* MOV A,0 */
    cpu_nec78k_step(&cpu); /* CMP A,0 */
    EXPECT("CMP A,0 → Z", !!(cpu.PSW & NEC_Z));
    cpu_nec78k_step(&cpu); /* BZ +2 */
    EXPECT("BZ taken → PC=0x0108", cpu.PC==0x0108);
    cpu_nec78k_step(&cpu); /* MOV A,#0x42 */
    EXPECT("executed instruction after jump = 0x42", cpu.r[1]==0x42);
}

static void test_di_ei(void) {
    CPU_NEC78K cpu; uint8_t mem[65536];
    uint8_t prog[] = {0xFC, 0xFB}; /* EI, DI */
    setup(&cpu,mem,prog,2);
    cpu_nec78k_step(&cpu); /* EI */
    EXPECT("EI sets IE flag", !!(cpu.PSW & NEC_IE));
    cpu_nec78k_step(&cpu); /* DI */
    EXPECT("DI clears IE flag", !(cpu.PSW & NEC_IE));
}

static void test_push_pop(void) {
    CPU_NEC78K cpu; uint8_t mem[65536];
    /* MOVW AX,#0x1234; PUSH AX; MOVW AX,#0; POP AX */
    uint8_t prog[] = {0xC0,0x34,0x12, 0xB8, 0xC0,0x00,0x00, 0xAC};
    /* AX=rp0, PUSH=0xB8, POP=0xAC (rp0) */
    setup(&cpu,mem,prog,sizeof(prog));
    cpu_nec78k_step(&cpu); /* MOVW AX,#0x1234 */
    uint16_t ax0 = (uint16_t)(cpu.r[0]|(cpu.r[1]<<8));
    EXPECT("MOVW AX=#0x1234", ax0==0x1234);
    uint16_t sp_before = cpu.SP;
    cpu_nec78k_step(&cpu); /* PUSH AX */
    EXPECT("PUSH decrements SP by 2", cpu.SP == sp_before-2);
    cpu_nec78k_step(&cpu); /* MOVW AX,#0 */
    cpu_nec78k_step(&cpu); /* POP AX */
    uint16_t ax1 = (uint16_t)(cpu.r[0]|(cpu.r[1]<<8));
    EXPECT("POP AX restored 0x1234", ax1==0x1234);
    EXPECT("POP restored SP", cpu.SP == sp_before);
}

static void test_halt(void) {
    CPU_NEC78K cpu; uint8_t mem[65536];
    uint8_t prog[] = {0xFF}; /* HALT */
    setup(&cpu,mem,prog,1);
    cpu_nec78k_step(&cpu);
    EXPECT("HALT sets halted", cpu.halted);
    cpu_nec78k_step(&cpu); /* should stay halted */
    EXPECT("HALT stays halted", cpu.halted);
    EXPECT("PC not advancing after halt", cpu.PC==0x0101);
}

static void test_interrupt(void) {
    CPU_NEC78K cpu; uint8_t mem[65536];
    memset(mem,0,65536);
    mem[0x0000]=0x00; mem[0x0001]=0x01; /* reset → 0x0100 */
    mem[0x0100]=0xFC; /* EI */
    mem[0x0101]=0x00; /* NOP */
    /* interrupt vector table at 0x0004 → handler at 0x0200 */
    mem[0x0004]=0x00; mem[0x0005]=0x02; /* vector 0x0200 */
    mem[0x0200]=0xB1; mem[0x0201]=0xBB; /* MOV A,#0xBB */
    mem[0x0202]=0x9F;                   /* RETI */
    cpu_nec78k_init(&cpu,mem);
    cpu_nec78k_step(&cpu); /* EI */
    EXPECT("EI before IRQ", !!(cpu.PSW & NEC_IE));
    uint16_t pc_before = cpu.PC;
    cpu_nec78k_irq(&cpu, 0x0004);
    EXPECT("IRQ→handler at 0x0200", cpu.PC==0x0200);
    EXPECT("IRQ disables IE",       !(cpu.PSW & NEC_IE));
    cpu_nec78k_step(&cpu); /* MOV A,#0xBB */
    EXPECT("IRQ handler runs", cpu.r[1]==0xBB);
    cpu_nec78k_step(&cpu); /* RETI */
    EXPECT("RETI returns", cpu.PC==pc_before);
}

int main(void) {
    printf("══ NEC 78K/0 CPU Test Suite ══════════════════════════\n");
    test_nop();
    test_mov_r_imm();
    test_mov_all_regs();
    test_movw();
    test_add();
    test_add_carry();
    test_sub();
    test_and_or_xor();
    test_cmp_z();
    test_inc_dec();
    test_incw_decw();
    test_xch();
    test_mov_hl_indirect();
    test_call_ret();
    test_br_bz();
    test_di_ei();
    test_push_pop();
    test_halt();
    test_interrupt();
    printf("══════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
