/*
 * test/reader_obfuscated.c — Runtime verification with obfuscated symbol names.
 *
 * This test uses dynamic symbol lookup to find the sched_hint TLS variable
 * even when it has an obfuscated name with special characters.
 *
 * Build & run:
 *   # With custom symbol name (for testing):
 *   opt -load-pass-plugin=build/pass/libSchedTagPass.so \
 *       -passes=sched-tag -sched-hint-symbol="__sched_hint.test$123" \
 *       test/test_dense.ll -o /tmp/tagged.bc
 *
 *   # With random obfuscated name (production):
 *   opt -load-pass-plugin=build/pass/libSchedTagPass.so \
 *       -passes=sched-tag test/test_dense.ll -o /tmp/tagged.bc
 *
 *   llc /tmp/tagged.bc -filetype=obj -o /tmp/tagged.o
 *   clang /tmp/tagged.o test/reader_obfuscated.c -o /tmp/test_obfuscated
 *   /tmp/test_obfuscated
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "reader_dynamic.h"

/* Functions from the instrumented module. */
extern int workload(int n, double x);
extern int trivial(int a);

/*=========================================================================*/
/* Observation callback — called from inside dense regions                */
/*=========================================================================*/

/* Snapshot captured by the most recent observe_hint call. */
static uint8_t  observed_compute_dense;
static int      observed_tag_id;
static int      observe_count;
static struct sched_hint *g_hint;  /* Global pointer to TLS variable */

void observe_hint(int tag_id) {
    if (!g_hint) {
        fprintf(stderr, "ERROR: observe_hint called but g_hint is NULL\n");
        return;
    }
    observed_compute_dense = g_hint->compute_dense;
    observed_tag_id       = tag_id;
    observe_count++;
}

/*=========================================================================*/
/* Helpers                                                                 */
/*=========================================================================*/

static const char *compute_name(uint8_t t) {
    static char buf[32];
    if (t == SCHED_COMPUTE_NONE) return "NONE";
    buf[0] = '\0';
    if (t & SCHED_COMPUTE_INT) {
        if (buf[0]) strcat(buf, "|");
        strcat(buf, "INT");
    }
    if (t & SCHED_COMPUTE_FLOAT) {
        if (buf[0]) strcat(buf, "|");
        strcat(buf, "FLOAT");
    }
    if (t & SCHED_COMPUTE_SIMD) {
        if (buf[0]) strcat(buf, "|");
        strcat(buf, "SIMD");
    }
    return buf;
}

static int failures = 0;

static void check(const char *label, uint8_t expect_compute,
                  uint8_t actual_compute) {
    int ok = (actual_compute == expect_compute);
    printf("  [%-28s] compute_dense=%-5s  %s\n",
           label,
           compute_name(actual_compute),
           ok ? "OK" : "FAIL");
    if (!ok) failures++;
}

/*=========================================================================*/
/* main                                                                    */
/*=========================================================================*/

int main(void) {
    printf("=== Testing with obfuscated TLS symbol ===\n\n");
    
    /* Find the TLS variable dynamically */
    g_hint = find_sched_hint_var();
    if (!g_hint) {
        fprintf(stderr, "FATAL: Could not find sched_hint TLS variable\n");
        return 1;
    }

    printf("\n=== struct sched_hint ===\n");
    printf("magic:        0x%08x %s\n", g_hint->magic,
           g_hint->magic == SCHED_HINT_MAGIC ? "(OK)" : "(BAD!)");
    printf("version:      %u\n", g_hint->version);
    printf("sizeof:       %zu bytes\n\n", sizeof(*g_hint));

    if (g_hint->magic != SCHED_HINT_MAGIC) {
        printf("FATAL: bad magic — pass did not emit correct data?\n");
        return 1;
    }

    /* Before any instrumented code runs, compute_dense should be 0. */
    check("initial state", SCHED_COMPUTE_NONE, g_hint->compute_dense);

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
          SCHED_COMPUTE_INT, observed_compute_dense);

    /*
     * workload(5, 2.71): n <= 10 → float_work path → SET FLOAT.
     * observe_hint(2) fires inside float_work while SET is active.
     */
    printf("\n--- workload(5, 2.71) [float_work path] ---\n");
    observe_count = 0;
    int r2 = workload(5, 2.71);
    printf("  result = %d\n", r2);

    check("inside float_work (cb)",
          SCHED_COMPUTE_FLOAT, observed_compute_dense);

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
