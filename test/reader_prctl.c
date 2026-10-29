/*
 * test/reader_prctl.c — Verify per-thread slot registration.
 *
 * A thread registers lazily on its first tag write: the library calls
 * prctl(PR_SET_SCHED_HINT, &slot), the kernel back-fills the slot's user
 * address, and the address is cached in @__sched_hint_ptr. This test
 * interposes prctl() to hand back a fake slot, so registration and the tag
 * stores can be verified without a patched kernel.
 *
 * Build & run:
 *   opt -load-pass-plugin=build/pass/libSchedTagPass.so \
 *       -passes=sched-tag test/test_dense.ll -o /tmp/tagged.bc
 *   llc /tmp/tagged.bc -filetype=obj -o /tmp/tagged.o
 *   clang /tmp/tagged.o test/reader_prctl.c -I <kernel>/usr/include \
 *         -o /tmp/test_prctl
 *   /tmp/test_prctl
 */

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../include/sched_tag.h"

/* Stub — test_dense.ll calls this inside dense regions; not needed here. */
void observe_hint(int tag_id) { (void)tag_id; }

/* Functions from the instrumented module. */
extern int workload(int n, double x);
extern int trivial(int a);

/* Fake kernel slot handed back through the prctl back-fill. */
static struct sched_hint g_fake_slot;

static int   g_called;
static int   g_option;
static void *g_uptr;

int prctl(int option, ...) {
    va_list ap;
    va_start(ap, option);
    void *uptr = va_arg(ap, void *);
    va_end(ap);

    g_called = 1;
    g_option = option;
    g_uptr   = uptr;
    if (option == PR_SET_SCHED_HINT && uptr)
        *(unsigned long *)uptr = (unsigned long)&g_fake_slot; /* back-fill */
    return 0;
}

static int failures;

static void check(const char *label, int ok) {
    printf("  [%-44s] %s\n", label, ok ? "OK" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    memset(&g_fake_slot, 0, sizeof g_fake_slot);
    g_fake_slot.magic   = SCHED_HINT_MAGIC;
    g_fake_slot.version = SCHED_HINT_VERSION;

    printf("=== slot registration via prctl ===\n\n");

    /* The first instrumented tag store lazily registers this thread. */
    int r = workload(20, 3.14);
    printf("  workload(20, 3.14) = %d\n", r);

    check("prctl(PR_SET_SCHED_HINT) called",
          g_called && g_option == PR_SET_SCHED_HINT);
    check("back-fill pointer non-NULL", g_uptr != NULL);
    check("funit_dense=INT stored into slot",
          g_fake_slot.funit_dense == SCHED_FUNIT_INT);
    check("sched_tag_get() reads it back",
          sched_tag_get(FUNIT_DENSE) == SCHED_FUNIT_INT);

    /* Registration is once per thread: a later tag must not re-register. */
    g_called = 0;
    int r2 = workload(5, 2.71);
    printf("  workload(5, 2.71) = %d\n", r2);
    check("no second prctl once cached", !g_called);
    check("funit_dense=FLOAT stored into slot",
          g_fake_slot.funit_dense == SCHED_FUNIT_FLOAT);

    (void)trivial(42);

    printf("\n=== %s (%d failure(s)) ===\n",
           failures == 0 ? "ALL PASSED" : "SOME FAILED", failures);
    return failures != 0;
}
