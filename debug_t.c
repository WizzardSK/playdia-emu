#include "src/cpu_tlcs870.h"
#include <stdio.h>
#include <string.h>
static CPU_TLCS870 cpu; static uint8_t mem[0x10000];
static void run(uint8_t *prog,int len){memset(mem,0,sizeof(mem));cpu_tlcs870_init(&cpu,mem);memcpy(&mem[0x2000],prog,len);cpu.PC=0x2000;for(int i=0;i<200&&!cpu.halted;i++)cpu_tlcs870_step(&cpu);}
int main(void){
    // CALL/RET: call lands at 0x2008, which is index 8 from base 0x2000
    // prog[0]=CD 01 02  CALL 0x2008  -> but 0x2008 is byte offset 8 in prog
    // prog[3]=3E BB      LD A,0xBB (skip)
    // prog[5]=76         HALT
    // prog[6]=00         NOP (pad)
    // prog[7]=00         NOP (pad)  
    // prog[8]=3E AA      LD A,0xAA
    // prog[10]=C9        RET
    uint8_t p[]={0xCD,0x08,0x20, 0x3E,0xBB, 0x76, 0x00,0x00, 0x3E,0xAA, 0xC9};
    printf("Prog at 0x2000, CALL -> 0x2008\n");
    run(p,11);
    printf("A=%02X (expect AA)\n",cpu.A);
    cpu_tlcs870_dump(&cpu);

    // SRL A = CB 3F
    printf("\n=== SRL ===\n");
    uint8_t p2[]={0x3E,0x01,0xCB,0x3F,0x76};
    run(p2,5);
    printf("SRL A: A=%02X C=%d (expect A=0 C=1)\n",cpu.A,(cpu.F&FLAG_C)?1:0);
    // also test SRL opcode
    printf("op_srl test: 0x3F = %d (should be in SRL range)\n", 0x3F>>3);
}
