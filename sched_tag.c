/*===-- sched_tag.c - single-instance sched_hint support ------------------===*\
|*                                                                            *|
|* The per-thread slot cache, the per-thread prctl() registration, the        *|
|* process-wide key salt, and the fork() hook live here, exactly once, and    *|
|* are linked in as libsched_tag.                                             *|
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
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

/* The one definition of the per-thread cache (declared extern in sched_tag.h). */
__thread struct sched_hint *__sched_hint_ptr
    __attribute__((tls_model("initial-exec")));

/*
 * Process-wide salt for pointer identity keys. Every thread shares it, so the
 * same lock address maps to the same key inside one process. It is deliberately
 * not the raw virtual address, so the scheduler never sees user VAs.
 */
uint64_t __sched_tag_process_salt;

static uint64_t __sched_tag_make_salt(void)
{
    uint64_t secret = 0;
    uint64_t fallback;
    struct timespec ts;

    /*
     * Prefer the kernel CSPRNG. It is initialized before userspace runs in
     * normal boots, so this is a one-time cold-path call.
     */
    if (getrandom(&secret, sizeof(secret), 0) == (ssize_t)sizeof(secret) &&
        secret)
        return secret;

    /*
     * Fallback: mix time, pid and a stack address. This is only a privacy
     * barrier, not a security boundary; the scheduler must not rely on the
     * key being unpredictable.
     */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fallback = ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)ts.tv_nsec ^
               ((uint64_t)getpid() << 16) ^ (uint64_t)(uintptr_t)&secret;
    fallback = sched_tag_hash64(fallback);
    return fallback ? fallback : 0x9E3779B97F4A7C15ULL;
}

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

/*
 * Priority 101 runs before ordinary user constructors (default priority
 * 65535), so the salt is ready before any application constructor can tag.
 */
__attribute__((constructor(101)))
static void __sched_hint_init(void)
{
    __sched_tag_process_salt = __sched_tag_make_salt();
    /* One definition, so exactly one .init_array entry and exactly one run. */
    pthread_atfork(NULL, NULL, __sched_hint_atfork_child);
}
