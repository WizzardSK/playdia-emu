#include "src/cpu_tlcs870.h"
#include <stdio.h>
#include <string.h>
static CPU_TLCS870 cpu; static uint8_t mem[0x10000];
int main(void){
    memset(mem,0,sizeof(mem));cpu_tlcs870_init(&cpu,mem);
    uint8_t p[]={0x3E,0x02,0xCB,0x3F,0x76};
    memcpy(&mem[0x2000],p,5); cpu.PC=0x2000;
    for(int i=0;i<20&&!cpu.halted;i++){
        printf("PC=%04X op=%02X\n",cpu.PC,mem[cpu.PC]);
        cpu_tlcs870_step(&cpu);
        printf("  A=%02X F=%02X\n",cpu.A,cpu.F);
    }
    printf("Final: A=%02X C=%d (expect A=01 C=0)\n",cpu.A,(cpu.F&FLAG_C)?1:0);
}
