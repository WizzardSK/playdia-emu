#include "src/cpu_tlcs870.h"
#include <stdio.h>
#include <string.h>

static int passed=0, failed=0;
static void check(const char *name, bool cond) {
    if(cond){printf("  \xE2\x9C\x93 %s\n",name);passed++;}
    else    {printf("  \xE2\x9C\x97 %s  FAIL\n",name);failed++;}
}

static CPU_TLCS870 cpu;
static uint8_t mem[0x10000];

static void run(uint8_t *prog, int len) {
    memset(mem,0,sizeof(mem));
    cpu_tlcs870_init(&cpu,mem);
    memcpy(&mem[0x2000],prog,len);
    cpu.PC=0x2000;
    for(int i=0;i<200&&!cpu.halted;i++) cpu_tlcs870_step(&cpu);
}

int main(void){
    printf("\n=== Extended TLCS-870 Tests ===\n\n");

    printf("[ LD ]\n");
    {uint8_t p[]={0x06,0xAB,0x48,0x76};run(p,4);check("LD B,n/LD C,B",cpu.C==0xAB);}
    {uint8_t p[]={0x21,0x34,0x12,0x7C,0x76};run(p,5);check("LD HL,nn/LD A,H",cpu.A==0x12);}
    {uint8_t p[]={0x3E,0x55,0x32,0x00,0x30,0x3E,0x00,0x3A,0x00,0x30,0x76};
     run(p,11);check("LD (nn),A / LD A,(nn)",cpu.A==0x55);}

    printf("[ INC/DEC 16-bit ]\n");
    {uint8_t p[]={0x01,0xFF,0x00,0x03,0x76};run(p,5);
     check("INC BC",(cpu.B<<8|cpu.C)==0x0100);}
    {uint8_t p[]={0x11,0x00,0x01,0x1B,0x1B,0x76};run(p,6);
     check("DEC DE x2",(cpu.D<<8|cpu.E)==0x00FE);}

    printf("[ ALU ]\n");
    {uint8_t p[]={0x3E,0xF0,0xC6,0x20,0x76};run(p,5);
     check("ADD carry",cpu.A==0x10&&(cpu.F&FLAG_C));}
    {uint8_t p[]={0x3E,0x01,0x37,0xCE,0x01,0x76};run(p,6);
     check("ADC with carry",cpu.A==0x03);}
    {uint8_t p[]={0x3E,0x10,0xD6,0x10,0x76};run(p,5);
     check("SUB zero",cpu.A==0&&(cpu.F&FLAG_Z));}
    {uint8_t p[]={0x3E,0x05,0xFE,0x05,0x76};run(p,5);
     check("CP equal",cpu.A==5&&(cpu.F&FLAG_Z));}
    {uint8_t p[]={0x3E,0xFF,0xE6,0x0F,0x76};run(p,5);check("AND",cpu.A==0x0F);}
    {uint8_t p[]={0x3E,0xF0,0xF6,0x0F,0x76};run(p,5);check("OR",cpu.A==0xFF);}
    {uint8_t p[]={0x3E,0xFF,0xEE,0xFF,0x76};run(p,5);check("XOR->0",cpu.A==0);}

    printf("[ ADD HL,rr ]\n");
    {uint8_t p[]={0x21,0x00,0x10,0x01,0x00,0x01,0x09,0x76};run(p,8);
     check("ADD HL,BC",(cpu.H<<8|cpu.L)==0x1100);}

    printf("[ Jumps ]\n");
    {uint8_t p[]={0x06,0x03,0x10,0xFE,0x76};run(p,5);check("DJNZ",cpu.B==0);}
    {uint8_t p[]={0x3E,0x00,0xFE,0x01,0xC2,0x0A,0x20,0x3E,0x99,0x00,0x76};
     run(p,11);check("JP NZ skips",cpu.A==0x00);}
    {uint8_t p[]={0x18,0x02,0x3E,0x99,0x76};run(p,5);check("JR skips",cpu.A!=0x99);}

    printf("[ CALL/RET ]\n");
    {uint8_t p[]={0xCD,0x07,0x20, 0x76, 0x00,0x00,0x00, 0x3E,0xAA,0xC9};
     run(p,10);check("CALL/RET",cpu.A==0xAA);}

    printf("[ EX/EXX ]\n");
    {uint8_t p[]={0x3E,0x11,0x08,0x3E,0x22,0x08,0x76};
     run(p,7);check("EX AF,AF'",cpu.A==0x11);}
    {uint8_t p[]={0x01,0xBB,0xAA,0xD9,0x01,0x00,0x00,0xD9,0x76};
     run(p,9);check("EXX BC",(cpu.B<<8|cpu.C)==0xAABB);}

    printf("[ PUSH/POP ]\n");
    {uint8_t p[]={0x01,0xBE,0xEF,0xC5,0x01,0x00,0x00,0xC1,0x76};
     run(p,9);check("PUSH/POP BC",cpu.B==0xEF&&cpu.C==0xBE);}

    printf("[ CB prefix ]\n");
    {uint8_t p[]={0x3E,0x80,0xCB,0x07,0x76};run(p,5);
     check("RLC A",cpu.A==0x01&&(cpu.F&FLAG_C));}
    {uint8_t p[]={0x3E,0x01,0xCB,0x6F,0x76};run(p,5);
     check("BIT 5,A Z=1",(cpu.F&FLAG_Z)!=0);}
    {uint8_t p[]={0x3E,0x00,0xCB,0xC7,0x76};run(p,5);check("SET 0,A",cpu.A==0x01);}
    {uint8_t p[]={0x3E,0xFF,0xCB,0x87,0x76};run(p,5);check("RES 0,A",cpu.A==0xFE);}
    {uint8_t p[]={0x3E,0x01,0xCB,0x38,0x76};run(p,5);
     check("SRL A 0x02->0x01",cpu.A==0x01&&!(cpu.F&FLAG_C));}

    printf("[ Misc ]\n");
    {uint8_t p[]={0x3E,0xAA,0x2F,0x76};run(p,4);check("CPL",cpu.A==0x55);}
    {uint8_t p[]={0x37,0x3F,0x76};run(p,3);check("SCF+CCF",!(cpu.F&FLAG_C));}
    {uint8_t p[]={0x11,0x34,0x12,0x21,0xCD,0xAB,0xEB,0x76};run(p,8);
     check("EX DE,HL",cpu.H==0x12&&cpu.L==0x34&&cpu.D==0xAB&&cpu.E==0xCD);}

    printf("\n\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\n");
    printf("  Passed: %d / %d\n", passed, passed+failed);
    printf("\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\xE2\x95\x90\n");
    return failed?1:0;
}
