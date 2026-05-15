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

    /* MAME opcode encoding (reg index: A=0, W=1, C=2, B=3, E=4, D=5, L=6, H=7):
     *   0x30..0x37  LD r,n   (0x30 = LD A,n; 0x31 = LD W,n; …)
     *   0x60..0x67  INC r    (0x60 = INC A; 0x61 = INC W; …)
     *   0x01        SWAP A
     *   0x02        MUL W,A
     *   0xFB        JR a     (signed 8-bit displacement)
     *   0xFC        CALL mn
     *   0x05        RET                                                       */

    /* NOP×4 — every NOP is 1 cycle, expect ≥ 4 cycles after run */
    { uint8_t p[] = { 0x00, 0x00, 0x00, 0x00 }; t_run(p, 4);
      check("NOP×4 cycles", t_cpu.cycles >= 4); }

    /* SWAP A : A=0xAB → 0xBA  (LD A,#0xAB = 0x30 0xAB; SWAP A = 0x01) */
    { uint8_t p[] = { 0x30, 0xAB, 0x01 }; t_run(p, 3);
      check("SWAP A", t_cpu.A == 0xBA); }

    /* LD A,#0x55 */
    { uint8_t p[] = { 0x30, 0x55 }; t_run(p, 2);
      check("LD A,#imm", t_cpu.A == 0x55); }

    /* INC A : LD A,#0x0F + INC A */
    { uint8_t p[] = { 0x30, 0x0F, 0x60 }; t_run(p, 3);
      check("INC A", t_cpu.A == 0x10); }

    /* MUL W,A : A=4, W=3 → WA=12 → A=12, W=0 */
    { uint8_t p[] = { 0x30, 0x04, 0x31, 0x03, 0x02 }; t_run(p, 5);
      check("MUL W,A", t_cpu.A == 12 && t_cpu.W == 0); }

    /* CALL/RET round-trip */
    { uint8_t p[] = {
        0xFC, 0x08, 0x20,    /* CALL 0x2008                                  */
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x30, 0x77,          /* @0x2008: LD A,#0x77                          */
        0x05                 /* RET                                          */
      }; t_run(p, 11);
      check("CALL/RET", t_cpu.A == 0x77); }

    /* JR a +2 : skip two bytes (an LD A,#0xFF that would otherwise execute) */
    { uint8_t p[] = { 0xFB, 0x02, 0x30, 0xFF, 0x30, 0x33 }; t_run(p, 6);
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

    /* 78K/II opcode encoding (per MAME upd78k2d):
     *   MOV r,#imm:  0xB8+r   (r=X=0,A=1,C=2,B=3,E=4,D=5,L=6,H=7)
     *   MOVW rp,#w:  0x60 / 0x62 / 0x64 / 0x66  for AX / BC / DE / HL
     *   <ALU> A,#imm:0xA8+op  (CMP = op 7 → 0xAF)
     *   DI:          0x4A     EI: 0x4B
     *   HALT (via STBC SFR): MOV STBC,#imm  = 0x09 0xC0 ~imm imm
     *                        with imm bit 0 = HALT.                     */

    /* For HALT, run a MOV STBC,#0x01 (HALT bit) which sets cpu->halted. */
    { uint8_t p[] = { 0x09, 0xC0, 0xFE, 0x01 }; n_run(p, 4);
      check("MOV STBC,#1 → HALT", n_cpu.halted); }

    /* MOV A,#0x42 — A is reg index 1, so 0xB9 0x42 */
    { uint8_t p[] = { 0xB9, 0x42 }; n_run(p, 2);
      check("MOV A,#0x42", n_cpu.r[NEC78K_REG_A] == 0x42); }

    /* MOVW AX,#0xABCD — 0x60 lo hi */
    { uint8_t p[] = { 0x60, 0xCD, 0xAB }; n_run(p, 3);
      check("MOVW AX,#0xABCD",
            n_cpu.r[NEC78K_REG_X] == 0xCD &&
            n_cpu.r[NEC78K_REG_A] == 0xAB); }

    /* MOV A,#5 then CMP A,#5  → Z=1.  STBC=2 stops the run after. */
    { uint8_t p[] = { 0xB9, 0x05, 0xAF, 0x05,
                       0x09, 0xC0, 0xFD, 0x02 }; n_run(p, 8);
      check("CMP A,#5 Z=1", (n_cpu.PSW & NEC_Z) != 0); }

    /* DI then HALT via STBC */
    { uint8_t p[] = { 0x4A, 0x09, 0xC0, 0xFE, 0x01 }; n_run(p, 5);
      check("DI clears IE", (n_cpu.PSW & NEC_IE) == 0); }
    /* EI then HALT */
    { uint8_t p[] = { 0x4B, 0x09, 0xC0, 0xFE, 0x01 }; n_run(p, 5);
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
