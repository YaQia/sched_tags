/*===-- sched_tag.c - single-instance sched_hint support ------------------===*\
|*                                                                            *|
|* The per-thread slot cache, the per-thread prctl() registration, and the     *|
|* fork() hook live here, exactly once, and are linked in as libsched_tag.     *|
|*                                                                            *|
|* Why a library and not header-only: a constructor emitted from a header (or  *|
|* from the LLVM pass) is emitted once per translation unit, and ELF/LLVM      *|
|* constructors are appending, so the merged constructor still runs once per   *|
|* TU. In a large project (thousands of TUs) that means thousands of           *|
|* .init_array entries and thousands of pointless calls. Emitting this from a   *|
|* single TU makes it exactly one entry.                                      *|
|*                                                                            *|
|* The symbols talk to the kernel via the sched_hint ABI, hence the            *|
|* __sched_hint_* names; the library as a whole is the userspace "tag" layer,  *|
|* hence libsched_tag. A program that never references any of this does not    *|
|* pull this object in, so it pays nothing.                                    *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

#include "sched_tag.h"

#include <pthread.h>
#include <sys/prctl.h>

/* The one definition of the per-thread cache (declared extern in sched_tag.h). */
__thread struct sched_hint *__sched_hint_ptr
    __attribute__((tls_model("initial-exec")));

struct sched_hint *__sched_hint_register(void)
{
    unsigned long slot = 0;
    struct sched_hint *h;

    if (prctl(PR_SET_SCHED_HINT, (unsigned long)&slot, 0, 0, 0) != 0)
        goto fail;
    h = (struct sched_hint *)slot;
    if (h->magic != SCHED_HINT_MAGIC || h->version != SCHED_HINT_VERSION)
        goto fail;
    __sched_hint_ptr = h;
    return h;
fail:
    __sched_hint_ptr = __SCHED_HINT_DISABLED;
    return NULL;
}

/*
 * fork() copies this thread's TLS, so the child would inherit a pointer to the
 * parent's slot — but the slot mapping is VM_DONTCOPY, so the address is
 * unmapped in the child and the first tag write would fault. Clear the cache in
 * the child; it re-registers on first use and gets a slot in its own area.
 * (Raw clone(CLONE_VM) without pthread_atfork coverage shares the parent's TLS
 * wholesale — writes then land in the parent's slot, semantically wrong but
 * harmless.)
 */
static void __sched_hint_atfork_child(void)
{
    __sched_hint_ptr = NULL;
}

/* One definition, so exactly one .init_array entry and exactly one run. */
__attribute__((constructor))
static void __sched_hint_atfork_init(void)
{
    pthread_atfork(NULL, NULL, __sched_hint_atfork_child);
}
