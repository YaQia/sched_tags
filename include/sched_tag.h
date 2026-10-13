/*===-- sched_tag.h - Source-level Scheduling Instrumentation --------------===*\
|*                                                                            *|
|* This header provides the `sched_tag_set` function/macro for directly       *|
|* instrumenting source code with scheduling tags. It is a lightweight        *|
|* alternative or complement to the LLVM-based `sched_tags.json` config.      *|
|*                                                                            *|
|* Data model: each thread's tags live in a 64-byte slot inside kernel-owned  *|
|* pages. The first use on a thread registers it via                          *|
|* prctl(PR_SET_SCHED_HINT, &word) and caches the backfilled slot address in  *|
|* TLS. The kernel stamps the slot header; this side only writes payloads.    *|
|*                                                                            *|
|* Usage Examples:                                                            *|
|*   1. Basic tag assignment:                                                 *|
|*      sched_tag_set(UNSHARED, SCHED_UNSHARED_HELD);                         *|
|*      critical_section();                                                   *|
|*      sched_tag_set(UNSHARED, SCHED_UNSHARED_NONE);                         *|
|*                                                                            *|
|*   2. Co-scheduling with a lock variable (Bloom Filter magic):              *|
|*      sched_tag_set(UNSHARED, SCHED_UNSHARED_HELD, &my_mutex);              *|
|*      ...                                                                   *|
|*      sched_tag_set(UNSHARED, SCHED_UNSHARED_NONE);                         *|
|*                                                                            *|
|*   3. Cross-process co-scheduling with a Static Magic Number:               *|
|*      sched_tag_set(UNSHARED, SCHED_UNSHARED_HELD, 0xDEADBEEFCAFEULL);      *|
|*      ...                                                                   *|
|*      sched_tag_set(UNSHARED, SCHED_UNSHARED_NONE);                         *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

#ifndef SCHED_TAG_H
#define SCHED_TAG_H

#include "sched_hint.h"
#include <stdint.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-thread slot cache. Weak so that GCC/Clang can include this header
 * from many translation units and the linker merges the TLS instances.
 *
 * NULL                   — not yet registered (first use registers)
 * __SCHED_HINT_DISABLED  — registration failed; give up for this thread
 * otherwise              — this thread's live slot
 */
__attribute__((weak, tls_model("initial-exec")))
__thread struct sched_hint *__sched_hint_ptr;

/* Cache value for "registration failed, stop trying". Real slots are
 * 64-byte aligned userspace addresses, so 1 can never be one. */
#define __SCHED_HINT_DISABLED ((struct sched_hint *)1)

/*
 * Register this thread and return its slot, or NULL on failure.
 * Idempotent kernel-side, but we call it at most once per thread: on
 * failure the cache settles on the disabled sentinel — permanent for
 * this thread, since retrying would only win back a rare ENOMEM and
 * would cost a failed prctl per tag write on kernels without support.
 *
 * The magic/version read doubles as the pre-touch: it faults the slot's
 * page in once, here in the cold path, instead of inside the first tagged
 * region — and it doubles as an ABI check: a mismatched header means the
 * kernel's struct layout may differ from this build's, so refuse to write.
 */
static __attribute__((unused, noinline, cold))
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
 * Internal: this thread's slot for tag WRITES (the sched_tag_set()
 * implementation path). Same lazy-registration and NULL semantics as
 * the public reader below. A cache value <= 1 is never a slot (they
 * are 64-byte aligned): NULL = first use, __SCHED_HINT_DISABLED = gave up.
 */
static inline struct sched_hint *__sched_hint_slot(void)
{
    struct sched_hint *h = __sched_hint_ptr;
    if ((uintptr_t)h <= 1)
        return h ? NULL : __sched_hint_register();
    return h;
}

/*
 * The current thread's live slot, READ-ONLY view for readers and
 * diagnostics. Lazily registers on first use, returns NULL when the
 * mechanism is unavailable (kernel without sched_hint support, or ABI
 * mismatch). The const return puts the contract in the type system:
 * the header (magic/version) is kernel-stamped, and all payload writes
 * go through sched_tag_set() — writing through this pointer must not
 * compile.
 */
static inline const struct sched_hint *sched_hint_current(void)
{
    return __sched_hint_slot();
}

/*
 * fork() copies this thread's TLS, so the child would inherit a pointer to
 * the parent's slot — but the slot mapping is VM_DONTCOPY, so the address
 * is unmapped in the child and the first tag write would fault. Clear the
 * cache in the child; it re-registers on first use and gets a slot in its
 * own area. (Raw clone(CLONE_VM) without pthread_atfork coverage shares the
 * parent's TLS wholesale — writes then land in the parent's slot, which is
 * semantically wrong but harmless.)
 */
static void __sched_hint_atfork_child(void)
{
    __sched_hint_ptr = NULL;
}

__attribute__((constructor, weak))
void __sched_hint_atfork_init(void)
{
    pthread_atfork(NULL, NULL, __sched_hint_atfork_child);
}

/* Tag types corresponding to sched_hint fields */
typedef enum {
    EXEC_DENSE,
    MEMORY_DENSE,
    ATOMIC_DENSE,
    UNSHARED,
    LOAD_TREND
} sched_tag_type_t;

/* Bloom filter hash calculation (identical to LLVM pass logic).
 * Takes __u64* to match the struct sched_hint magic fields exactly (the uapi
 * header uses __u64 == unsigned long long, which is a distinct pointer type
 * from uint64_t on LP64 even though the width matches). */
static inline void __sched_tag_bloom_add(__u64 *bloom, const void *ptr) {
    if (!ptr) return;
    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    addr &= ~63ULL; /* align to 64 bytes */
    uint64_t h = addr * SCHED_HINT_BLOOM_PRIME; /* single-source hash constant */
    *bloom |= (1ULL << (h & 63))
           |  (1ULL << ((h >> 16) & 63))
           |  (1ULL << ((h >> 32) & 63))
           |  (1ULL << ((h >> 48) & 63));
}

/* Base setter (no magic var). Returns the slot (NULL when unavailable)
 * so the layered setters can reuse it without a second TLS load. */
static inline struct sched_hint *__sched_tag_set_base(sched_tag_type_t type, uint8_t value) {
    struct sched_hint *hint = __sched_hint_slot();
    if (!hint)
        return NULL;
    switch (type) {
        case EXEC_DENSE:    hint->exec_dense    = value; break;
        case MEMORY_DENSE:  hint->memory_dense  = value; break;
        case ATOMIC_DENSE:  hint->atomic_dense  = value; if (!value) hint->atomic_magic = 0; break;
        case UNSHARED:      hint->unshared      = value; if (!value) hint->unshared_magic = 0; break;
        case LOAD_TREND:    hint->load_trend    = value; break;
    }
    return hint;
}

/* Setter for pointer-based bloom filter calculation */
static inline void __sched_tag_set_ptr(sched_tag_type_t type, uint8_t value, const void *ptr) {
    struct sched_hint *hint = __sched_tag_set_base(type, value);
    if (hint && value && ptr) {
        if (type == ATOMIC_DENSE) __sched_tag_bloom_add(&hint->atomic_magic, ptr);
        else if (type == UNSHARED) __sched_tag_bloom_add(&hint->unshared_magic, ptr);
    }
}

/* Setter for 64-bit static magic ID */
static inline void __sched_tag_set_id(sched_tag_type_t type, uint8_t value, uint64_t id) {
    struct sched_hint *hint = __sched_tag_set_base(type, value);
    if (hint && value && id) {
        if (type == ATOMIC_DENSE) hint->atomic_magic = id;
        else if (type == UNSHARED) hint->unshared_magic = id;
    }
}

#ifdef __cplusplus
} /* extern "C" */

/* C++ Overloads */
static inline void sched_tag_set(sched_tag_type_t type, uint8_t value) {
    __sched_tag_set_base(type, value);
}

static inline void sched_tag_set(sched_tag_type_t type, uint8_t value, const void *ptr) {
    __sched_tag_set_ptr(type, value, ptr);
}

static inline void sched_tag_set(sched_tag_type_t type, uint8_t value, uint64_t id) {
    __sched_tag_set_id(type, value, id);
}

#else /* __cplusplus */

/* C macros with _Generic for C11 (allows static magic IDs vs pointers) */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define __sched_tag_3(type, value, magic) _Generic((magic), \
    int: __sched_tag_set_id, \
    long: __sched_tag_set_id, \
    long long: __sched_tag_set_id, \
    unsigned int: __sched_tag_set_id, \
    unsigned long: __sched_tag_set_id, \
    unsigned long long: __sched_tag_set_id, \
    default: __sched_tag_set_ptr \
)(type, value, magic)
#else
/* Fallback for pre-C11: assume magic is a pointer, unless manually cast. */
#define __sched_tag_3(type, value, magic) __sched_tag_set_ptr(type, value, (const void*)(uintptr_t)(magic))
#endif

/* Macro dispatcher for optional 3rd argument */
#define __SCHED_TAG_MACRO(_1, _2, _3, NAME, ...) NAME
#define sched_tag_set(...) __SCHED_TAG_MACRO(__VA_ARGS__, __sched_tag_3, __sched_tag_set_base)(__VA_ARGS__)

#endif /* __cplusplus */

#endif /* SCHED_TAG_H */
