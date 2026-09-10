/*
 * test/reader.c — Runtime verification of __sched_hint BB-level instrumentation.
 *
 * The .ll test files call observe_hint(tag_id) from INSIDE dense regions.
 * This callback captures the hint state while SET is active, so we can
 * verify that tags_active and exec_dense are correct during execution.
 *
 * Note: Tag clearing is now handled by the kernel scheduler on context switch,
 *       not by the instrumented code. Tests only verify SET behavior.
 *
 * Build & run:
 *   opt -load-pass-plugin=build/pass/libSchedTagPass.so \
 *       -passes=sched-tag test/test_dense.ll -o /tmp/tagged.bc
 *   llc /tmp/tagged.bc -filetype=obj -o /tmp/tagged.o
 *   clang /tmp/tagged.o test/reader.c -lm -o /tmp/test_reader
 *   /tmp/test_reader
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../include/sched_hint.h"

/* The pass emits this TLS global in the __sched_hint section. */
extern __thread struct sched_hint __sched_hint_data __asm__("__sched_hint.data");

/* Functions from the instrumented module. */
extern int workload(int n, double x);
extern int trivial(int a);

/*=========================================================================*/
/* Observation callback — called from inside dense regions in the .ll      */
/*=========================================================================*/

/* Snapshot captured by the most recent observe_hint call. */
static uint8_t  observed_exec_dense;
static int      observed_tag_id;
static int      observe_count;

void observe_hint(int tag_id) {
    struct sched_hint *h = &__sched_hint_data;
    observed_exec_dense = h->exec_dense;
    observed_tag_id       = tag_id;
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
    printf("  [%-28s] exec_dense=%-5s  %s\n",
           label,
           exec_name(actual_compute),
           ok ? "OK" : "FAIL");
    if (!ok) failures++;
}

static void dump_current(const char *label) {
    struct sched_hint *h = &__sched_hint_data;
    printf("  [%-28s] exec_dense=%s\n",
           label,
           exec_name(h->exec_dense));
}

/*=========================================================================*/
/* main                                                                    */
/*=========================================================================*/

int main(void) {
    struct sched_hint *h = &__sched_hint_data;

    printf("=== struct sched_hint ===\n");
    printf("magic:        0x%08x %s\n", h->magic,
           h->magic == SCHED_HINT_MAGIC ? "(OK)" : "(BAD!)");
    printf("version:      %u\n", h->version);
    printf("sizeof:       %zu bytes\n\n", sizeof(*h));

    if (h->magic != SCHED_HINT_MAGIC) {
        printf("FATAL: bad magic — pass did not emit __sched_hint_data?\n");
        return 1;
    }

    /* Before any instrumented code runs, exec_dense should be 0. */
    check("initial state", SCHED_EXEC_NONE, h->exec_dense);

    /*
     * workload(20, 3.14): n > 10 → int_work path → SET INT.
     * observe_hint(1) fires inside int_work while SET is active.
     */
    printf("\n--- workload(20, 3.14) [int_work path] ---\n");
    observe_count = 0;
    int r1 = workload(20, 3.14);
    printf("  result = %d\n", r1);

    /* Check what the callback saw DURING the dense region. */
    check("inside int_work (cb)",
          SCHED_EXEC_INT, observed_exec_dense);

    /*
     * workload(5, 2.71): n <= 10 → float_work path → SET FLOAT.
     * observe_hint(2) fires inside float_work while SET is active.
     */
    printf("\n--- workload(5, 2.71) [float_work path] ---\n");
    observe_count = 0;
    int r2 = workload(5, 2.71);
    printf("  result = %d\n", r2);

    check("inside float_work (cb)",
          SCHED_EXEC_FLOAT, observed_exec_dense);

    /*
     * trivial(42): no dense BBs → no SET → no callback.
     */
    printf("\n--- trivial(42) [no dense BBs] ---\n");
    observe_count = 0;
    int r3 = trivial(42);
    printf("  result = %d\n", r3);

    printf("\n=== %s (%d failure(s)) ===\n",
           failures == 0 ? "ALL PASSED" : "SOME FAILED", failures);
    return failures != 0;
}
