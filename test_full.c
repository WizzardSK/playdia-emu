/* ================================================================
 *  test_full.c — sanity tests for the rewritten CPU cores.
 *
 *  The previous version of this file tested Z80 opcodes (LD/EX
 *  AF,AF'/EXX/CB-prefix etc.) against the old, incorrect Z80-based
 *  TLCS-870 implementation.  Issue #4 documented that the chip is
 *  not a Z80, so both the cores and these tests had to be redone.
 *
 *  Only a small smoke-test set is here for now — it confirms the
 *  new cores boot, fetch, decode the handful of opcodes that are
 *  actually implemented, and produce the expected register state.
 *  Until the opcode coverage in cpu_tlcs870.c / cpu_nec78k.c is
 *  filled out from the official manuals, broader test vectors are
 *  not meaningful.
 * ================================================================ */

#include "src/cpu_tlcs870.h"
#include "src/cpu_nec78k.h"
#include <stdio.h>
#include <string.h>

static int passed = 0, failed = 0;
static void check(const char *name, bool cond) {
    if (cond) { printf("  \xE2\x9C\x93 %s\n", name);       passed++; }
    else      { printf("  \xE2\x9C\x97 %s  FAIL\n", name); failed++; }
}

/* ── TLCS-870 ─────────────────────────────────────────────── */
static CPU_TLCS870 t_cpu;
static uint8_t     t_mem[0x10000];

static void t_run(const uint8_t *prog, int len) {
    memset(t_mem, 0, sizeof(t_mem));
    /* preload reset vector so init() picks 0x2000 as entry */
    t_mem[0xFFFE] = 0x00; t_mem[0xFFFF] = 0x20;
    cpu_tlcs870_init(&t_cpu, t_mem);
    memcpy(&t_mem[0x2000], prog, len);
    t_cpu.PC = 0x2000;
    for (int i = 0; i < 200 && !t_cpu.halted; i++)
        cpu_tlcs870_step(&t_cpu);
}

static void test_tlcs870(void) {
    printf("\n=== TLCS-870 sanity tests ===\n\n");

    /* NOP×4 — every NOP burns 2 cycles, expect ≥ 8 cycles after run */
    { uint8_t p[] = { 0x00, 0x00, 0x00, 0x00 }; t_run(p, 4);
      check("NOP×4 cycles", t_cpu.cycles >= 8); }

    /* SWAP A : A=0xAB → 0xBA */
    { uint8_t p[] = { 0xD0, 0xAB, 0x01 }; t_run(p, 3);
      check("SWAP A", t_cpu.A == 0xBA); }

    /* LD A,#0x55 */
    { uint8_t p[] = { 0xD0, 0x55 }; t_run(p, 2);
      check("LD A,#imm", t_cpu.A == 0x55); }

    /* INC A : reg index for A = TLCS870_REG_A = 4 → opcode 0x14 */
    { uint8_t p[] = { 0xD0, 0x0F, 0x14 }; t_run(p, 3);
      check("INC A", t_cpu.A == 0x10); }

    /* MUL W,A : A=4, W=3 → WA=12 → A=12, W=0 */
    { uint8_t p[] = { 0xD0, 0x04, 0xD5, 0x03, 0x02 }; t_run(p, 5);
      check("MUL W,A", t_cpu.A == 12 && t_cpu.W == 0); }

    /* CALL/RET round-trip */
    { uint8_t p[] = {
        0xFC, 0x08, 0x20,    /* CALL 0x2008                       */
        0x00, 0x00, 0x00, 0x00, 0x00,
        0xD0, 0x77,          /* @0x2008: LD A,#0x77                */
        0x05                 /* RET                                */
      }; t_run(p, 11);
      check("CALL/RET", t_cpu.A == 0x77); }

    /* JR rel : skip a byte */
    { uint8_t p[] = { 0xC0, 0x02, 0xD0, 0xFF, 0xD0, 0x33 }; t_run(p, 6);
      check("JR skips LD", t_cpu.A == 0x33); }
}

/* ── NEC 78K/II ────────────────────────────────────────────── */
static CPU_NEC78K  n_cpu;
static uint8_t     n_mem[0x10000];

static void n_run(const uint8_t *prog, int len) {
    memset(n_mem, 0, sizeof(n_mem));
    n_mem[0] = 0x00; n_mem[1] = 0x20;        /* reset vector → 0x2000 */
    cpu_nec78k_init(&n_cpu, n_mem);
    memcpy(&n_mem[0x2000], prog, len);
    n_cpu.PC = 0x2000;
    for (int i = 0; i < 200 && !n_cpu.halted; i++)
        cpu_nec78k_step(&n_cpu);
}

static void test_nec78k(void) {
    printf("\n=== NEC 78K/II sanity tests ===\n\n");

    /* NOP×3 then HALT (0xFF) */
    { uint8_t p[] = { 0x00, 0x00, 0x00, 0xFF }; n_run(p, 4);
      check("NOP+HALT", n_cpu.halted); }

    /* MOV A,#imm   (opcode 0x10, imm) */
    { uint8_t p[] = { 0x10, 0x42, 0xFF }; n_run(p, 3);
      check("MOV A,#0x42", n_cpu.r[1] == 0x42); }

    /* MOVW AX,#0xABCD */
    { uint8_t p[] = { 0xC0, 0xCD, 0xAB, 0xFF }; n_run(p, 4);
      check("MOVW AX,#0xABCD",
            n_cpu.r[0] == 0xCD && n_cpu.r[1] == 0xAB); }

    /* MOV A,#5 ; CMP A,#5 → Z=1 */
    { uint8_t p[] = { 0x10, 0x05, 0xCF, 0x05, 0xFF }; n_run(p, 5);
      check("CMP A,#5 Z=1", (n_cpu.PSW & NEC_Z) != 0); }

    /* DI then EI toggles IE */
    { uint8_t p[] = { 0xFB, 0xFF }; n_run(p, 2);
      check("DI clears IE", (n_cpu.PSW & NEC_IE) == 0); }
    { uint8_t p[] = { 0xFC, 0xFF }; n_run(p, 2);
      check("EI sets IE",   (n_cpu.PSW & NEC_IE) != 0); }
}

int main(void) {
    test_tlcs870();
    test_nec78k();

    printf("\n══════════════════════\n");
    printf("  Passed: %d / %d\n", passed, passed + failed);
    printf("══════════════════════\n\n");
    return failed ? 1 : 0;
}
