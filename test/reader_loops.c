/*
 * test/reader_loops.c — Runtime verification of loop-level __sched_hint
 *                       instrumentation.
 *
 * The .ll test files call observe_hint(tag_id) from INSIDE dense loops and
 * dense BBs. This callback captures the hint state while SET is active,
 * so we can verify that tags_active / compute_dense are correct during
 * execution — not just after the function returns (where CLR has fired).
 *
 * Build & run:
 *   opt -load-pass-plugin=build/pass/libSchedTagPass.so \
 *       -passes=sched-tag test/test_loops.ll -o /tmp/tagged_loops.bc
 *   llc /tmp/tagged_loops.bc -filetype=obj -o /tmp/tagged_loops.o
 *   clang /tmp/tagged_loops.o test/reader_loops.c -lm -o /tmp/test_loops
 *   /tmp/test_loops
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../include/sched_hint.h"

/* The pass emits this TLS global in the __sched_hint section. */
extern __thread struct sched_hint __sched_hint_data;

/* Functions from the instrumented module. */
extern int    int_loop_dense(int n);
extern double float_loop_dense(int n, double seed);
/* simd_loop_dense takes vector args — skip calling from C for simplicity. */
extern int    mixed_with_dense_bb(int n);
extern int    trivial(int a);

/*=========================================================================*/
/* Observation callback — called from inside dense regions in the .ll       */
/*=========================================================================*/

static uint64_t observed_tags_active;
static uint8_t  observed_compute_dense;
static int      observed_tag_id;
static int      observe_count;

void observe_hint(int tag_id) {
    struct sched_hint *h = &__sched_hint_data;
    observed_tags_active  = h->tags_active;
    observed_compute_dense = h->compute_dense;
    observed_tag_id       = tag_id;
    observe_count++;
}

/*=========================================================================*/
/* Helpers                                                                  */
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

static void check(const char *label, uint64_t expect_tags, uint8_t expect_compute,
                  uint64_t actual_tags, uint8_t actual_compute) {
    int ok = (actual_tags == expect_tags && actual_compute == expect_compute);
    printf("  [%-34s] tags_active=0x%lx  compute_dense=%-5s  %s\n",
           label,
           (unsigned long)actual_tags,
           compute_name(actual_compute),
           ok ? "OK" : "FAIL");
    if (!ok) failures++;
}

/*=========================================================================*/
/* main                                                                     */
/*=========================================================================*/

int main(void) {
    struct sched_hint *h = &__sched_hint_data;

    printf("=== struct sched_hint ===\n");
    printf("magic:        0x%08x %s\n", h->magic,
           h->magic == SCHED_HINT_MAGIC ? "(OK)" : "(BAD!)");
    printf("version:      %u\n", h->version);
    printf("tags_present: 0x%lx\n", (unsigned long)h->tags_present);
    printf("sizeof:       %zu bytes\n\n", sizeof(*h));

    if (h->magic != SCHED_HINT_MAGIC) {
        printf("FATAL: bad magic — pass did not emit __sched_hint_data?\n");
        return 1;
    }

    check("initial state", 0x0, SCHED_COMPUTE_NONE,
          h->tags_active, h->compute_dense);

    /*
     * int_loop_dense(100): loop body is INT-dense.
     * observe_hint(10) fires on every iteration inside the loop.
     * SET is in the preheader (before the loop), CLR in exit block.
     */
    printf("\n--- int_loop_dense(100) ---\n");
    observe_count = 0;
    int r1 = int_loop_dense(100);
    printf("  result = %d,  observe_count = %d\n", r1, observe_count);

    check("inside int loop (cb)",
          SCHED_TAG_COMPUTE_DENSE, SCHED_COMPUTE_INT,
          observed_tags_active, observed_compute_dense);

    check("after int_loop_dense (main)",
          0x0, SCHED_COMPUTE_NONE,
          h->tags_active, h->compute_dense);

    /*
     * float_loop_dense(50, 1.5): loop body is FLOAT-dense.
     * observe_hint(20) fires on every iteration.
     */
    printf("\n--- float_loop_dense(50, 1.5) ---\n");
    observe_count = 0;
    double r2 = float_loop_dense(50, 1.5);
    printf("  result = %f,  observe_count = %d\n", r2, observe_count);

    check("inside float loop (cb)",
          SCHED_TAG_COMPUTE_DENSE, SCHED_COMPUTE_FLOAT,
          observed_tags_active, observed_compute_dense);

    check("after float_loop_dense (main)",
          0x0, SCHED_COMPUTE_NONE,
          h->tags_active, h->compute_dense);

    /*
     * mixed_with_dense_bb(200): n > 100 → enters dense_bb.
     * dense_bb has BB-level SET+CLR with observe_hint(30) inside.
     */
    printf("\n--- mixed_with_dense_bb(200) [dense_bb path] ---\n");
    observe_count = 0;
    int r3 = mixed_with_dense_bb(200);
    printf("  result = %d,  observe_count = %d\n", r3, observe_count);

    check("inside dense_bb (cb)",
          SCHED_TAG_COMPUTE_DENSE, SCHED_COMPUTE_INT,
          observed_tags_active, observed_compute_dense);

    check("after mixed_dense_bb(200) (main)",
          0x0, SCHED_COMPUTE_NONE,
          h->tags_active, h->compute_dense);

    /*
     * mixed_with_dense_bb(50): n <= 100 → skips dense_bb.
     * No observe_hint call → observe_count stays 0.
     */
    printf("\n--- mixed_with_dense_bb(50) [skip dense_bb] ---\n");
    observe_count = 0;
    int r4 = mixed_with_dense_bb(50);
    printf("  result = %d,  observe_count = %d (expect 0)\n", r4, observe_count);

    check("after mixed_dense_bb(50) (main)",
          0x0, SCHED_COMPUTE_NONE,
          h->tags_active, h->compute_dense);

    /*
     * trivial(42): no instrumentation at all.
     */
    printf("\n--- trivial(42) ---\n");
    observe_count = 0;
    int r5 = trivial(42);
    printf("  result = %d,  observe_count = %d (expect 0)\n", r5, observe_count);

    check("after trivial (main)",
          0x0, SCHED_COMPUTE_NONE,
          h->tags_active, h->compute_dense);

    printf("\n=== %s (%d failure(s)) ===\n",
           failures == 0 ? "ALL PASSED" : "SOME FAILED", failures);
    return failures != 0;
}
