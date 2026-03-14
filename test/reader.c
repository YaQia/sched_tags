/*
 * test/reader.c — Runtime verification of __sched_hint BB-level instrumentation.
 *
 * The .ll test files call observe_hint(tag_id) from INSIDE dense regions.
 * This callback captures the hint state while SET is active, so we can
 * verify that tags_active and compute_dense are correct during execution
 * — not just after the function returns (where CLR has already fired).
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
extern __thread struct sched_hint __sched_hint_data;

/* Functions from the instrumented module. */
extern int workload(int n, double x);
extern int trivial(int a);

/*=========================================================================*/
/* Observation callback — called from inside dense regions in the .ll      */
/*=========================================================================*/

/* Snapshot captured by the most recent observe_hint call. */
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

static void check(const char *label, uint64_t expect_tags, uint8_t expect_compute,
                  uint64_t actual_tags, uint8_t actual_compute) {
    int ok = (actual_tags == expect_tags && actual_compute == expect_compute);
    printf("  [%-28s] tags_active=0x%lx  compute_dense=%-5s  %s\n",
           label,
           (unsigned long)actual_tags,
           compute_name(actual_compute),
           ok ? "OK" : "FAIL");
    if (!ok) failures++;
}

static void dump_current(const char *label) {
    struct sched_hint *h = &__sched_hint_data;
    printf("  [%-28s] tags_active=0x%lx  compute_dense=%s\n",
           label,
           (unsigned long)h->tags_active,
           compute_name(h->compute_dense));
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
    printf("tags_present: 0x%lx\n", (unsigned long)h->tags_present);
    printf("sizeof:       %zu bytes\n\n", sizeof(*h));

    if (h->magic != SCHED_HINT_MAGIC) {
        printf("FATAL: bad magic — pass did not emit __sched_hint_data?\n");
        return 1;
    }

    /* Before any instrumented code runs, tags_active should be 0. */
    check("initial state", 0x0, SCHED_COMPUTE_NONE,
          h->tags_active, h->compute_dense);

    /*
     * workload(20, 3.14): n > 10 → int_work path → SET INT.
     * observe_hint(1) fires inside int_work while SET is active.
     * Then merge → CLR.
     */
    printf("\n--- workload(20, 3.14) [int_work path] ---\n");
    observe_count = 0;
    int r1 = workload(20, 3.14);
    printf("  result = %d\n", r1);

    /* Check what the callback saw DURING the dense region. */
    check("inside int_work (cb)",
          SCHED_TAG_COMPUTE_DENSE, SCHED_COMPUTE_INT,
          observed_tags_active, observed_compute_dense);

    /* After return, CLR should have cleared everything. */
    check("after return (main)",
          0x0, SCHED_COMPUTE_NONE,
          h->tags_active, h->compute_dense);

    /*
     * workload(5, 2.71): n <= 10 → float_work path → SET FLOAT.
     * observe_hint(2) fires inside float_work while SET is active.
     * Then merge → CLR.
     */
    printf("\n--- workload(5, 2.71) [float_work path] ---\n");
    observe_count = 0;
    int r2 = workload(5, 2.71);
    printf("  result = %d\n", r2);

    check("inside float_work (cb)",
          SCHED_TAG_COMPUTE_DENSE, SCHED_COMPUTE_FLOAT,
          observed_tags_active, observed_compute_dense);

    check("after return (main)",
          0x0, SCHED_COMPUTE_NONE,
          h->tags_active, h->compute_dense);

    /*
     * trivial(42): no dense BBs → no SET/CLR → no callback → hint unchanged.
     */
    printf("\n--- trivial(42) [no dense BBs] ---\n");
    observe_count = 0;
    int r3 = trivial(42);
    printf("  result = %d\n", r3);

    check("after trivial (main)",
          0x0, SCHED_COMPUTE_NONE,
          h->tags_active, h->compute_dense);

    printf("\n=== %s (%d failure(s)) ===\n",
           failures == 0 ? "ALL PASSED" : "SOME FAILED", failures);
    return failures != 0;
}
