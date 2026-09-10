/*
 * test/reader_rust.c — Runtime verification of SchedTag on Rust-generated IR.
 *
 * Build & run (from project root):
 *
 *   # 1. Rust → LLVM IR (unoptimised, no overflow checks)
 *   rustc --emit=llvm-ir -C opt-level=1 -C overflow-checks=no \
 *         -C no-prepopulate-passes --crate-type=lib \
 *         test/test_rust_dense.rs -o /tmp/test_rust_dense.ll
 *
 *   # 2. Optimise + instrument
 *   opt -load-pass-plugin=build/pass/libSchedTagPass.so \
 *       -passes='default<O1>,sched-tag' /tmp/test_rust_dense.ll \
 *       -o /tmp/tagged_rust.bc
 *
 *   # 3. Compile to object
 *   llc /tmp/tagged_rust.bc -filetype=obj -o /tmp/tagged_rust.o
 *
 *   # 4. Link with this reader
 *   clang /tmp/tagged_rust.o test/reader_rust.c -lm -o /tmp/test_rust
 *
 *   # 5. Run
 *   /tmp/test_rust
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../include/sched_hint.h"

/* ---- TLS hint global emitted by the pass ---- */
extern __thread struct sched_hint __sched_hint_data __asm__("__sched_hint.data");

/* ---- Rust functions (extern "C", #[no_mangle]) ---- */
extern int32_t rust_int_work(int32_t n);
extern int32_t rust_float_work(double x);
extern double  rust_mixed_work(int32_t n, double x);
extern int32_t rust_int_loop(int32_t n);
extern int32_t rust_trivial(int32_t a);

/* ---- Rust EH personality stub (declared but never invoked) ---- */
int rust_eh_personality(void) { return 0; }

/*=========================================================================*/
/* Observation callback — called from inside dense regions in the Rust IR  */
/*=========================================================================*/

static uint8_t  observed_exec_dense;
static int      observed_tag_id;
static int      observe_count;

void observe_hint(int tag_id) {
    struct sched_hint *h = &__sched_hint_data;
    observed_exec_dense = h->exec_dense;
    observed_tag_id        = tag_id;
    observe_count++;
}

/*=========================================================================*/
/* Helpers                                                                 */
/*=========================================================================*/

static const char *exec_name(uint8_t t) {
    static char buf[32];
    if (t == SCHED_EXEC_NONE) return "NONE";
    buf[0] = '\0';
    if (t & SCHED_EXEC_INT) {
        if (buf[0]) strcat(buf, "|");
        strcat(buf, "INT");
    }
    if (t & SCHED_EXEC_FLOAT) {
        if (buf[0]) strcat(buf, "|");
        strcat(buf, "FLOAT");
    }
    if (t & SCHED_EXEC_SIMD) {
        if (buf[0]) strcat(buf, "|");
        strcat(buf, "SIMD");
    }
    if (t & SCHED_EXEC_CTRL) {
        if (buf[0]) strcat(buf, "|");
        strcat(buf, "CTRL");
    }
    return buf;
}

static int failures = 0;

static void check(const char *label, uint8_t expect_compute,
                  uint8_t actual_compute) {
    int ok = (actual_compute == expect_compute);
    printf("  [%-30s] exec_dense=%-5s  %s\n",
           label,
           exec_name(actual_compute),
           ok ? "OK" : "FAIL");
    if (!ok) failures++;
}

/*=========================================================================*/
/* main                                                                    */
/*=========================================================================*/

int main(void) {
    struct sched_hint *h = &__sched_hint_data;

    printf("=== Rust SchedTag test ===\n");
    printf("magic:        0x%08x %s\n", h->magic,
           h->magic == SCHED_HINT_MAGIC ? "(OK)" : "(BAD!)");
    printf("version:      %u\n", h->version);
    printf("sizeof:       %zu bytes\n\n", sizeof(*h));

    if (h->magic != SCHED_HINT_MAGIC) {
        printf("FATAL: bad magic — pass did not emit __sched_hint_data?\n");
        return 1;
    }

    /* Initial state: nothing active */
    check("initial state", SCHED_EXEC_NONE, h->exec_dense);

    /* ---- rust_int_work: BB-level INT ---- */
    printf("\n--- rust_int_work(20) [INT-dense BB] ---\n");
    observe_count = 0;
    int32_t r1 = rust_int_work(20);
    printf("  result = %d, observe_count = %d\n", r1, observe_count);

    check("inside int_work (cb)",
          SCHED_EXEC_INT, observed_exec_dense);

    /* ---- rust_float_work: BB-level FLOAT ---- */
    /* After -O1, fptosi becomes @llvm.fptosi.sat (a CallInst → NONE),
       so only FLOAT ops remain in the BB. */
    printf("\n--- rust_float_work(3.14) [FLOAT-dense BB] ---\n");
    observe_count = 0;
    int32_t r2 = rust_float_work(3.14);
    printf("  result = %d, observe_count = %d\n", r2, observe_count);

    check("inside float_work (cb)",
          SCHED_EXEC_FLOAT, observed_exec_dense);

    /* ---- rust_mixed_work: BB-level INT|FLOAT (bitmask replaces MIXED) ---- */
    printf("\n--- rust_mixed_work(42, 2.71) [INT|FLOAT-dense BB] ---\n");
    observe_count = 0;
    double r3 = rust_mixed_work(42, 2.71);
    printf("  result = %f, observe_count = %d\n", r3, observe_count);

    check("inside mixed_work (cb)",
          SCHED_EXEC_INT | SCHED_EXEC_FLOAT, observed_exec_dense);

    /* ---- rust_int_loop: loop-level INT|CTRL ---- */
    /* The loop body's conditional exit branch pushes branch density over
       the CTRL threshold, so the mask combines arithmetic and control-flow
       pressure (composable by design). */
    printf("\n--- rust_int_loop(100) [INT|CTRL-dense loop] ---\n");
    observe_count = 0;
    int32_t r4 = rust_int_loop(100);
    printf("  result = %d, observe_count = %d\n", r4, observe_count);

    check("inside int_loop (cb)",
          SCHED_EXEC_INT | SCHED_EXEC_CTRL, observed_exec_dense);

    /* ---- rust_trivial: no instrumentation ---- */
    printf("\n--- rust_trivial(42) [no dense regions] ---\n");
    observe_count = 0;
    int32_t r5 = rust_trivial(42);
    printf("  result = %d, observe_count = %d\n", r5, observe_count);

    printf("\n=== %s (%d failure(s)) ===\n",
           failures == 0 ? "ALL PASSED" : "SOME FAILED", failures);
    return failures != 0;
}
