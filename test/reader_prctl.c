/*
 * test/reader_prctl.c — Verify the prctl constructor emitted by SchedTagPass.
 *
 * The pass generates a module constructor (__sched_hint_report) that calls
 *   prctl(PR_SET_SCHED_HINT_OFFSET, offset, vaddr)
 * before main().  We provide our own prctl() to intercept and verify the
 * arguments without needing a patched kernel.
 *
 * Build & run:
 *   opt -load-pass-plugin=build/pass/libSchedTagPass.so \
 *       -passes=sched-tag test/test_dense.ll -o /tmp/tagged.bc
 *   llc /tmp/tagged.bc -filetype=obj -o /tmp/tagged.o
 *   clang /tmp/tagged.o test/reader_prctl.c -o /tmp/test_prctl
 *   /tmp/test_prctl
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include "../include/sched_hint.h"

/* The pass emits this TLS global in the __sched_hint section. */
extern __thread struct sched_hint __sched_hint_data;

/* Stub — test_dense.ll calls this inside dense regions; not needed here. */
void observe_hint(int tag_id) { (void)tag_id; }

/* Functions from the instrumented module (unused, but must resolve). */
extern int workload(int n, double x);
extern int trivial(int a);

/*=========================================================================*/
/* Mock prctl — captures arguments from __sched_hint_report()              */
/*=========================================================================*/

static int  g_called;
static int  g_option;
static long g_offset;
static long g_vaddr;

int prctl(int option, ...) {
    va_list ap;
    va_start(ap, option);
    long a2 = va_arg(ap, long);
    long a3 = va_arg(ap, long);
    va_end(ap);

    g_called = 1;
    g_option = option;
    g_offset = a2;
    g_vaddr  = a3;
    return 0;
}

/*=========================================================================*/
/* main                                                                     */
/*=========================================================================*/

static int failures;

static void check(const char *label, int ok) {
    printf("  [%-34s] %s\n", label, ok ? "OK" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    printf("=== prctl constructor verification ===\n\n");

    /* 1. Constructor must have run before main. */
    check("constructor called before main", g_called);
    if (!g_called) {
        printf("\nFATAL: constructor did not run — remaining checks skipped.\n");
        return 1;
    }

    /* 2. option == PR_SET_SCHED_HINT_OFFSET (83) */
    printf("  option = %d\n", g_option);
    check("option == PR_SET_SCHED_HINT_OFFSET",
          g_option == PR_SET_SCHED_HINT_OFFSET);

    /* 3. vaddr == &__sched_hint_data */
    long want_vaddr = (long)&__sched_hint_data;
    printf("  vaddr      = 0x%lx\n", g_vaddr);
    printf("  &hint_data = 0x%lx\n", want_vaddr);
    check("vaddr == &__sched_hint_data",
          g_vaddr == want_vaddr);

    /* 4. offset == vaddr - thread_pointer */
    long tp = (long)__builtin_thread_pointer();
    long want_offset = want_vaddr - tp;
    printf("  offset     = %ld\n", g_offset);
    printf("  want       = %ld  (vaddr - tp)\n", want_offset);
    check("offset == vaddr - tp",
          g_offset == want_offset);

    printf("\n=== %s (%d failure(s)) ===\n",
           failures == 0 ? "ALL PASSED" : "SOME FAILED", failures);
    return failures != 0;
}
