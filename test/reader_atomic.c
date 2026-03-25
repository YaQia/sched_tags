/*
 * test/reader_atomic.c — Runtime verification of atomic-dense __sched_hint
 *                        instrumentation, including bloom-filter atomic_magic.
 *
 * The .ll test file calls observe_hint(tag_id) from INSIDE atomic-dense
 * regions. This callback captures the hint state while SET is active,
 * so we can verify that atomic_dense and atomic_magic are correct during
 * execution.
 *
 * Note: Tag clearing (atomic_dense) is now handled by the kernel scheduler
 *       on context switch, not by the instrumented code. Tests only verify
 *       SET behavior. atomic_magic is never cleared (performance optimization).
 *
 * Build & run:
 *   opt -load-pass-plugin=build/pass/libSchedTagPass.so \
 *       -passes=sched-tag test/test_atomic.ll -o /tmp/tagged_atomic.bc
 *   llc /tmp/tagged_atomic.bc -filetype=obj -o /tmp/tagged_atomic.o
 *   clang /tmp/tagged_atomic.o test/reader_atomic.c -lm -o /tmp/test_atomic
 *   /tmp/test_atomic
 */

#include <stdio.h>
#include <stdint.h>
#include "../include/sched_hint.h"

/* The pass emits this TLS global in the __sched_hint section. */
extern __thread struct sched_hint __sched_hint_data __asm__("__sched_hint.data");

/* Functions from the instrumented module. */
extern int atomic_bb_work(int *p);
extern int atomic_loop_work(int n, int *p);
extern int mixed_atomic_bb(int n, int *p);
extern int trivial(int a);
extern void cas_retry_loop(int *p);

/*=========================================================================*/
/* Bloom filter reference implementation — must match the LLVM pass.       */
/*                                                                         */
/*   h     = (uint64_t)ptr * BLOOM_HASH_PRIME                             */
/*   bloom = (1 << (h & 63))                                              */
/*         | (1 << ((h >> 16) & 63))                                      */
/*         | (1 << ((h >> 32) & 63))                                      */
/*         | (1 << ((h >> 48) & 63))                                      */
/*                                                                         */
/* k=4 bits per pointer; multiple pointers OR'd together.                  */
/*=========================================================================*/

#define BLOOM_HASH_PRIME  0x9E3779B97F4A7C15ULL
#define K_BLOOM_BITS      4

static uint64_t bloom_hash_reference(const void *ptr) {
    /* Mask off lower 6 bits (align to 64-byte cache line) */
    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    uint64_t aligned_addr = addr & ~63ULL;

    uint64_t h = aligned_addr * BLOOM_HASH_PRIME;
    uint64_t bloom = 0;
    bloom |= (uint64_t)1 << (h         & 63);
    bloom |= (uint64_t)1 << ((h >> 16) & 63);
    bloom |= (uint64_t)1 << ((h >> 32) & 63);
    bloom |= (uint64_t)1 << ((h >> 48) & 63);
    return bloom;
}

static int popcount64(uint64_t v) {
    return __builtin_popcountll(v);
}

/*=========================================================================*/
/* Observation callback — called from inside dense regions in the .ll      */
/*=========================================================================*/

static uint8_t  observed_atomic_dense;
static uint64_t observed_atomic_magic;
static int      observed_tag_id;
static int      observe_count;

void observe_hint(int tag_id) {
    struct sched_hint *h = &__sched_hint_data;
    observed_atomic_dense = h->atomic_dense;
    observed_atomic_magic = h->atomic_magic;
    observed_tag_id       = tag_id;
    observe_count++;
}

/*=========================================================================*/
/* Helpers                                                                 */
/*=========================================================================*/

static int failures = 0;

static void check(const char *label, uint8_t expect_atomic,
                  uint8_t actual_atomic) {
    int ok = (actual_atomic == expect_atomic);
    printf("  [%-34s] atomic_dense=%u  %s\n",
           label,
           actual_atomic,
           ok ? "OK" : "FAIL");
    if (!ok) failures++;
}

static void check_magic(const char *label, uint64_t actual_magic,
                         int expect_nonzero) {
    if (expect_nonzero) {
        /* Inside a dense region: magic should be non-zero with >= K bits set */
        int pc = popcount64(actual_magic);
        int ok = (actual_magic != 0) && (pc >= K_BLOOM_BITS);
        printf("  [%-34s] atomic_magic=0x%016lx (popcount=%d)  %s\n",
               label, (unsigned long)actual_magic, pc,
               ok ? "OK" : "FAIL");
        if (!ok) failures++;
    } else {
        /* Initial state: magic should be 0 */
        printf("  [%-34s] atomic_magic=0x%016lx\n",
               label, (unsigned long)actual_magic);
    }
}

static void check_magic_exact(const char *label, uint64_t actual_magic,
                               uint64_t expected_magic) {
    int ok = (actual_magic == expected_magic);
    printf("  [%-34s] magic=0x%016lx expect=0x%016lx  %s\n",
           label, (unsigned long)actual_magic, (unsigned long)expected_magic,
           ok ? "OK" : "FAIL");
    if (!ok) failures++;
}

/*=========================================================================*/
/* main                                                                    */
/*=========================================================================*/

int main(void) {
    struct sched_hint *h = &__sched_hint_data;
    int scratch = 0;  /* shared variable for atomic operations */

    printf("=== Atomic-dense SchedTag test (with bloom filter) ===\n");
    printf("magic:        0x%08x %s\n", h->magic,
           h->magic == SCHED_HINT_MAGIC ? "(OK)" : "(BAD!)");
    printf("version:      %u\n", h->version);
    printf("sizeof:       %zu bytes\n\n", sizeof(*h));

    if (h->magic != SCHED_HINT_MAGIC) {
        printf("FATAL: bad magic — pass did not emit __sched_hint_data?\n");
        return 1;
    }

    /* Compute the expected bloom hash for &scratch — all 3 functions use
     * the same pointer, so their bloom magic should be identical. */
    uint64_t expected_magic = bloom_hash_reference(&scratch);
    printf("bloom reference for &scratch (%p): 0x%016lx (popcount=%d)\n\n",
           (void *)&scratch, (unsigned long)expected_magic,
           popcount64(expected_magic));

    /* Before any instrumented code runs, both fields should be 0. */
    check("initial state", 0, h->atomic_dense);
    check_magic("initial state (magic)", h->atomic_magic, 0);

    /*
     * atomic_bb_work(&scratch): BB with 8 atomicrmw ops → atomic-dense.
     * observe_hint(1) fires inside the BB while SET is active.
     */
    printf("\n--- atomic_bb_work [BB-level atomic dense] ---\n");
    observe_count = 0;
    int r1 = atomic_bb_work(&scratch);
    printf("  result = %d, observe_count = %d\n", r1, observe_count);

    check("inside atomic_bb (cb)", 1, observed_atomic_dense);
    check_magic("inside atomic_bb magic (cb)", observed_atomic_magic, 1);
    check_magic_exact("atomic_bb magic vs ref", observed_atomic_magic,
                      expected_magic);

    /*
     * atomic_loop_work(10, &scratch): loop with 6 atomicrmw ops → dense.
     * observe_hint(10) fires every iteration inside the loop.
     * SET is in preheader.
     */
    printf("\n--- atomic_loop_work(10) [loop-level atomic dense] ---\n");
    observe_count = 0;
    scratch = 0;
    int r2 = atomic_loop_work(10, &scratch);
    printf("  result = %d, observe_count = %d\n", r2, observe_count);

    check("inside atomic loop (cb)", 1, observed_atomic_dense);
    check_magic("inside atomic loop magic (cb)", observed_atomic_magic, 1);
    check_magic_exact("atomic_loop magic vs ref", observed_atomic_magic,
                      expected_magic);

    /*
     * mixed_atomic_bb(200, &scratch): n > 100 → enters atomic_bb.
     * The standalone BB has 6 atomicrmw ops → atomic-dense BB-level SET.
     * observe_hint(30) fires inside the atomic BB.
     */
    printf("\n--- mixed_atomic_bb(200) [atomic BB path] ---\n");
    observe_count = 0;
    scratch = 0;
    int r3 = mixed_atomic_bb(200, &scratch);
    printf("  result = %d, observe_count = %d\n", r3, observe_count);

    check("inside atomic_bb (cb)", 1, observed_atomic_dense);
    check_magic("inside mixed_bb magic (cb)", observed_atomic_magic, 1);
    check_magic_exact("mixed_bb magic vs ref", observed_atomic_magic,
                      expected_magic);

    /*
     * mixed_atomic_bb(50, &scratch): n <= 100 → skips atomic_bb.
     * No observe_hint call → observe_count stays 0.
     */
    printf("\n--- mixed_atomic_bb(50) [skip atomic BB] ---\n");
    observe_count = 0;
    scratch = 0;
    int r4 = mixed_atomic_bb(50, &scratch);
    printf("  result = %d, observe_count = %d (expect 0)\n", r4, observe_count);

    /*
     * trivial(42): no atomic ops → no instrumentation.
     */
    printf("\n--- trivial(42) [no atomic ops] ---\n");
    observe_count = 0;
    int r5 = trivial(42);
    printf("  result = %d, observe_count = %d (expect 0)\n", r5, observe_count);

    /*
     * cas_retry_loop(&scratch): short loop, 1 cmpxchg, back-edge on success.
     * observe_hint(40) fires inside the loop.
     */
    printf("\n--- cas_retry_loop [CAS retry loop-level atomic dense] ---\n");
    observe_count = 0;
    scratch = 0;
    cas_retry_loop(&scratch);
    printf("  observe_count = %d\n", observe_count);

    check("inside cas_retry loop (cb)", 1, observed_atomic_dense);
    check_magic("inside cas_retry magic (cb)", observed_atomic_magic, 1);
    check_magic_exact("cas_retry magic vs ref", observed_atomic_magic,
                      expected_magic);

    printf("\n=== %s (%d failure(s)) ===\n",
           failures == 0 ? "ALL PASSED" : "SOME FAILED", failures);
    return failures != 0;
}
