/*
 * test/reader_source_labels.c — Runtime verification of source labels
 *                               from sched_tags.json configuration.
 *
 * This tests:
 *   1. Single query labels (compute-dense, atomic-dense, io-dense, branch-dense)
 *   2. Ranged query labels (unshared with start/end)
 *   3. Proper SET/CLR behavior for ranged labels
 *
 * Build & run:
 *   opt -load-pass-plugin=build/pass/libSchedTagPass.so \
 *       -passes=sched-tag \
 *       -sched-tags-file=test/sched_tags_test.json \
 *       -sched-auto-analysis=false \
 *       test/test_source_labels.ll -o /tmp/tagged_source.bc
 *   llc /tmp/tagged_source.bc -filetype=obj -o /tmp/tagged_source.o
 *   clang /tmp/tagged_source.o test/reader_source_labels.c -o /tmp/test_source
 *   /tmp/test_source
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../include/sched_hint.h"

/* The pass emits this TLS global in the __sched_hint section. */
extern __thread struct sched_hint __sched_hint_data __asm__("__sched_hint.data");

/* Functions from the instrumented module. */
extern int labeled_compute(int n);
extern int labeled_atomic_loop(int n, int *counter);
extern int critical_section(int x);
extern int another_critical(int a, int b);
extern int unlabeled_function(int n);
extern void io_operation(void *buf, int size);
extern int branchy_code(int x, int y);

/* Mutex stubs - just track calls for testing */
static int mutex_lock_count = 0;
static int mutex_unlock_count = 0;

void mutex_lock(void *m) {
    mutex_lock_count++;
}

void mutex_unlock(void *m) {
    mutex_unlock_count++;
}

/*=========================================================================*/
/* Observation state - captured inside regions                             */
/*=========================================================================*/

struct hint_snapshot {
    uint8_t compute_dense;
    uint8_t branch_dense;
    uint8_t memory_dense;
    uint8_t atomic_dense;
    uint8_t io_dense;
    uint8_t unshared;
    uint64_t atomic_magic;
    uint64_t unshared_magic;
};

static struct hint_snapshot snapshots[100];
static int snapshot_count = 0;

void observe_hint(int tag_id) {
    struct sched_hint *h = &__sched_hint_data;
    if (tag_id < 100) {
        snapshots[tag_id].compute_dense = h->compute_dense;
        snapshots[tag_id].branch_dense = h->branch_dense;
        snapshots[tag_id].memory_dense = h->memory_dense;
        snapshots[tag_id].atomic_dense = h->atomic_dense;
        snapshots[tag_id].io_dense = h->io_dense;
        snapshots[tag_id].unshared = h->unshared;
        snapshots[tag_id].atomic_magic = h->atomic_magic;
        snapshots[tag_id].unshared_magic = h->unshared_magic;
    }
    snapshot_count++;
}

/*=========================================================================*/
/* Helpers                                                                 */
/*=========================================================================*/

static int failures = 0;

static const char *compute_type_name(uint8_t mask) {
    if (mask == 0) return "NONE";
    static char buf[32];
    buf[0] = '\0';
    if (mask & SCHED_COMPUTE_INT) strcat(buf, "INT");
    if (mask & SCHED_COMPUTE_FLOAT) {
        if (buf[0]) strcat(buf, "|");
        strcat(buf, "FLOAT");
    }
    if (mask & SCHED_COMPUTE_SIMD) {
        if (buf[0]) strcat(buf, "|");
        strcat(buf, "SIMD");
    }
    return buf;
}

static void check_u8(const char *label, const char *field, 
                     uint8_t expect, uint8_t actual) {
    int ok = (actual == expect);
    printf("  [%-40s] %s=%u  %s\n", label, field, actual, ok ? "OK" : "FAIL");
    if (!ok) {
        printf("    expected %u, got %u\n", expect, actual);
        failures++;
    }
}

static void check_compute(const char *label, uint8_t expect, uint8_t actual) {
    int ok = (actual == expect);
    printf("  [%-40s] compute=%s  %s\n", label, compute_type_name(actual), 
           ok ? "OK" : "FAIL");
    if (!ok) {
        printf("    expected %s, got %s\n", 
               compute_type_name(expect), compute_type_name(actual));
        failures++;
    }
}

/*=========================================================================*/
/* Main test                                                               */
/*=========================================================================*/

int main(void) {
    struct sched_hint *hint = &__sched_hint_data;
    
    printf("=== Source Labels Test (sched_tags.json) ===\n");
    printf("magic:        0x%08x %s\n", hint->magic,
           hint->magic == SCHED_HINT_MAGIC ? "(OK)" : "(BAD!)");
    printf("version:      %u\n", hint->version);
    printf("sizeof:       %zu bytes\n\n", sizeof(struct sched_hint));
    
    if (hint->magic != SCHED_HINT_MAGIC) {
        printf("ERROR: Invalid magic number!\n");
        return 1;
    }
    
    /* Clear snapshots */
    memset(snapshots, 0, sizeof(snapshots));
    snapshot_count = 0;
    
    /*---------------------------------------------------------------------*/
    printf("--- Test 1: labeled_compute (compute-dense=INT via source label) ---\n");
    /*---------------------------------------------------------------------*/
    int r1 = labeled_compute(10);
    printf("  result = %d\n", r1);
    check_compute("inside labeled_compute (tag 1)", SCHED_COMPUTE_INT, 
                  snapshots[1].compute_dense);
    printf("\n");
    
    /*---------------------------------------------------------------------*/
    printf("--- Test 2: labeled_atomic_loop (atomic-dense via source label) ---\n");
    /*---------------------------------------------------------------------*/
    int counter = 0;
    int r2 = labeled_atomic_loop(3, &counter);
    printf("  result = %d, counter = %d\n", r2, counter);
    check_u8("inside atomic loop (tag 10)", "atomic_dense", 1, 
             snapshots[10].atomic_dense);
    printf("\n");
    
    /*---------------------------------------------------------------------*/
    printf("--- Test 3: critical_section (unshared ranged label) ---\n");
    /*---------------------------------------------------------------------*/
    mutex_lock_count = mutex_unlock_count = 0;
    int r3 = critical_section(21);
    printf("  result = %d, lock_count=%d, unlock_count=%d\n", 
           r3, mutex_lock_count, mutex_unlock_count);
    check_u8("inside critical section (tag 20)", "unshared", 1, 
             snapshots[20].unshared);
    check_u8("after unlock (tag 21)", "unshared", 0, 
             snapshots[21].unshared);
    printf("\n");
    
    /*---------------------------------------------------------------------*/
    printf("--- Test 4: another_critical (unshared ranged label) ---\n");
    /*---------------------------------------------------------------------*/
    mutex_lock_count = mutex_unlock_count = 0;
    int r4 = another_critical(5, 7);
    printf("  result = %d, lock_count=%d, unlock_count=%d\n",
           r4, mutex_lock_count, mutex_unlock_count);
    check_u8("inside another_critical (tag 30)", "unshared", 1, 
             snapshots[30].unshared);
    check_u8("after unlock (tag 31)", "unshared", 0, 
             snapshots[31].unshared);
    printf("\n");
    
    /*---------------------------------------------------------------------*/
    printf("--- Test 5: unlabeled_function (no source labels for this func) ---\n");
    /*---------------------------------------------------------------------*/
    /*
     * NOTE: Non-ranged labels (compute-dense, atomic-dense, etc.) persist
     * until quiescent (handled by scheduler), NOT until function return.
     * So previous labels may still be visible here. This is expected behavior.
     * 
     * Only unshared (ranged) labels have explicit CLR at end of range.
     */
    int r5 = unlabeled_function(100);
    printf("  result = %d\n", r5);
    printf("  [note: non-ranged labels persist until quiescent]\n");
    printf("  compute_dense=%u (may be non-zero from previous calls)\n",
           snapshots[99].compute_dense);
    printf("  atomic_dense=%u (may be non-zero from previous calls)\n",
           snapshots[99].atomic_dense);
    /* Only unshared should be 0 because it was explicitly cleared */
    check_u8("unlabeled function (tag 99)", "unshared", 0, 
             snapshots[99].unshared);
    printf("\n");
    
    /*---------------------------------------------------------------------*/
    printf("--- Test 6: io_operation (io-dense via source label) ---\n");
    /*---------------------------------------------------------------------*/
    char buf[64];
    io_operation(buf, sizeof(buf));
    check_u8("inside io_operation (tag 40)", "io_dense", 1, 
             snapshots[40].io_dense);
    printf("\n");
    
    /*---------------------------------------------------------------------*/
    printf("--- Test 7: branchy_code (branch-dense via source label) ---\n");
    /*---------------------------------------------------------------------*/
    int r7 = branchy_code(5, 3);
    printf("  result = %d\n", r7);
    check_u8("inside branchy_code (tag 50)", "branch_dense", 1, 
             snapshots[50].branch_dense);
    printf("\n");
    
    /*---------------------------------------------------------------------*/
    printf("=== %s (%d failure(s)) ===\n",
           failures == 0 ? "ALL PASSED" : "SOME FAILED", failures);
    
    return failures;
}
