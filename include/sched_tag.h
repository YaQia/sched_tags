/*===-- sched_tag.h - Source-level Scheduling Instrumentation --------------===*\
|*                                                                            *|
|* This header provides the `sched_tag_set` writer plus the `sched_tag_get`   *|
|* and `sched_tag_test_magic` readers, for directly instrumenting source      *|
|* code with scheduling tags. It is a lightweight alternative or complement   *|
|* to the LLVM-based `sched_tags.json` config.                                *|
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
|*   4. Read back / probe (the raw struct is never exposed):                  *|
|*      uint8_t v      = sched_tag_get(UNSHARED);                             *|
|*      bool    shared = sched_tag_test_magic(UNSHARED, &my_mutex);           *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

/*
 * Link users of this header with the library `libsched_tag.a`: it owns the
 * single per-thread registration and slot cache, and this header is the inline
 * API over it. A program that never uses tags never references it, so the
 * linker does not pull it in.
 */

#ifndef SCHED_TAG_H
#define SCHED_TAG_H

#include "sched_hint.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-thread slot cache, defined once in the library (libsched_tag).
 * Shared by every translation unit and by the LLVM pass, so all writers and
 * readers hit the same cell for the current thread.
 *
 * NULL                   — not yet registered (first use registers)
 * __SCHED_HINT_DISABLED  — registration failed; give up for this thread
 * otherwise              — this thread's live slot
 */
extern __thread struct sched_hint *__sched_hint_ptr
    __attribute__((tls_model("initial-exec")));

/* Cache value for "registration failed, stop trying". Real slots are
 * 64-byte aligned userspace addresses, so 1 can never be one. */
#define __SCHED_HINT_DISABLED ((struct sched_hint *)1)

/*
 * Register the calling thread and return its slot, or NULL on failure.
 * Defined once in the library (libsched_tag). Idempotent kernel-side,
 * but we call it at most once per thread: on failure the cache settles on the
 * disabled sentinel — permanent for this thread, since retrying would only win
 * back a rare ENOMEM and would cost a failed prctl per tag write on kernels
 * without support.
 *
 * The magic/version read doubles as the pre-touch: it faults the slot's page
 * in once, here in the cold path, instead of inside the first tagged region —
 * and it doubles as an ABI check: a mismatched header means the kernel's
 * struct layout may differ from this build's, so refuse to write.
 */
extern struct sched_hint *__sched_hint_register(void);

/*
 * Internal: this thread's slot, shared by the tag readers and writers
 * (sched_tag_set / sched_tag_get / sched_tag_test_magic). Deliberately
 * NOT exposed: handing out the raw slot would let callers touch the
 * kernel-stamped header or any field, so the only public surface is the
 * typed tag API below. Lazy-registers on first use. A cache value <= 1
 * is never a slot (slots are 64-byte aligned): NULL = first use,
 * __SCHED_HINT_DISABLED = gave up. Returns NULL when the mechanism is
 * unavailable (kernel without sched_hint support, or ABI mismatch).
 */
static inline struct sched_hint *__sched_hint_slot(void)
{
    struct sched_hint *h = __sched_hint_ptr;
    if ((uintptr_t)h <= 1)
        return h ? NULL : __sched_hint_register();
    return h;
}

/* Tag types corresponding to sched_hint fields */
typedef enum {
    FUNIT_DENSE,
    MEMORY_DENSE,
    ATOMIC_DENSE,
    UNSHARED,
    DEP_ROLE,
    LOAD_AHEAD
} sched_tag_type_t;

/* Bloom filter hash calculation (identical to the kernel's / LLVM pass logic).
 * One 64-byte-aligned pointer sets up to SCHED_HINT_BLOOM_K bit positions in a
 * 64-bit word. The K positions come from 16-bit-strided fields of the hash and
 * are combined with a straight-line OR (no branch/loop), so the scheduler can
 * issue them in parallel and out-of-order. Positions may collide, so fewer than
 * K bits can be set; that is fine because the scheduler keys on the whole magic
 * word (equal magic == same resource) rather than counting set bits. A null
 * pointer maps to 0, which is the intended "no address / no info" value.
 *
 * Returns __u64 to match the struct sched_hint magic fields exactly (the uapi
 * header uses __u64 == unsigned long long, a distinct pointer type from
 * uint64_t on LP64 even though the width matches). */
static inline __u64 __sched_tag_bloom_of(const void *ptr) {
    if (!ptr) return 0;
    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    addr &= ~63ULL; /* align to 64 bytes */
    uint64_t h = addr * SCHED_HINT_BLOOM_PRIME; /* single-source hash constant */
    return (1ULL << (h & 63))
         |  (1ULL << ((h >> 16) & 63))
         |  (1ULL << ((h >> 32) & 63))
         |  (1ULL << ((h >> 48) & 63));
}

/* OR `ptr`'s bloom bits into `*bloom` (idempotent). */
static inline void __sched_tag_bloom_add(__u64 *bloom, const void *ptr) {
    *bloom |= __sched_tag_bloom_of(ptr);
}

/* Base setter (no magic var). Returns the slot (NULL when unavailable)
 * so the layered setters can reuse it without a second TLS load. */
static inline struct sched_hint *__sched_tag_set_base(sched_tag_type_t type, uint8_t value) {
    struct sched_hint *hint = __sched_hint_slot();
    if (!hint)
        return NULL;
    switch (type) {
        case FUNIT_DENSE:   hint->funit_dense   = value; break;
        case MEMORY_DENSE:  hint->memory_dense  = value; break;
        case ATOMIC_DENSE:  hint->atomic_dense  = value; if (!value) hint->atomic_magic = 0; break;
        case UNSHARED:      hint->unshared      = value; if (!value) hint->unshared_magic = 0; break;
        case DEP_ROLE:      hint->dep_role      = value; if (!value) hint->dep_magic = 0; break;
        case LOAD_AHEAD:    hint->load_ahead    = value; break;
    }
    return hint;
}

/* Setter for pointer-based bloom filter calculation */
static inline void __sched_tag_set_ptr(sched_tag_type_t type, uint8_t value, const void *ptr) {
    struct sched_hint *hint = __sched_tag_set_base(type, value);
    if (hint && value && ptr) {
        if (type == ATOMIC_DENSE) __sched_tag_bloom_add(&hint->atomic_magic, ptr);
        else if (type == UNSHARED) __sched_tag_bloom_add(&hint->unshared_magic, ptr);
        else if (type == DEP_ROLE) __sched_tag_bloom_add(&hint->dep_magic, ptr);
    }
}

/* Setter for 64-bit static magic ID */
static inline void __sched_tag_set_id(sched_tag_type_t type, uint8_t value, uint64_t id) {
    struct sched_hint *hint = __sched_tag_set_base(type, value);
    if (hint && value && id) {
        if (type == ATOMIC_DENSE) hint->atomic_magic = id;
        else if (type == UNSHARED) hint->unshared_magic = id;
        else if (type == DEP_ROLE) hint->dep_magic = id;
    }
}

/*
 * Reader: the value currently stored in this thread's tag payload field, or
 * 0 when the mechanism is unavailable (kernel without sched_hint support, or
 * ABI mismatch). 0 means "no tag active", so an unsupported kernel reads as
 * "nothing set" rather than an error. This is the inverse of
 * sched_tag_set(type, value).
 *
 * Note it returns only the state byte, never the magic word: the raw magic is
 * an opaque address hash (or a static id) that callers should not reason
 * about. To probe it, use sched_tag_test_magic() below.
 */
static inline uint8_t sched_tag_get(sched_tag_type_t type) {
    struct sched_hint *hint = __sched_hint_slot();
    if (!hint)
        return 0;
    switch (type) {
        case FUNIT_DENSE:   return hint->funit_dense;
        case MEMORY_DENSE:  return hint->memory_dense;
        case ATOMIC_DENSE:  return hint->atomic_dense;
        case UNSHARED:      return hint->unshared;
        case DEP_ROLE:      return hint->dep_role;
        case LOAD_AHEAD:    return hint->load_ahead;
    }
    return 0;
}

/*
 * Predicate: does this thread's stored magic for `type` include the bloom bits
 * of `ptr`? This is how a caller probes co-scheduling without ever seeing the
 * opaque magic word.
 *
 * Only meaningful when the tag was set with a pointer magic (bloom mode):
 *     sched_tag_set(type, value, &lock);
 *     sched_tag_test_magic(type, &lock) -> true
 * A static-id set (sched_tag_set(type, value, 0xDEAD...)) stores the id
 * verbatim, not a bloom, so this test does not apply to it.
 *
 * Membership is "every bloom bit of ptr is present in the stored word"
 * (subset), i.e. the exact inverse of __sched_tag_bloom_add() for a single
 * pointer. Because the K raw positions can collide, a pointer may set fewer
 * than K bits, so the subset form is used rather than a popcount>=K test.
 */
static inline bool sched_tag_test_magic(sched_tag_type_t type, const void *ptr) {
    struct sched_hint *hint = __sched_hint_slot();
    __u64 stored, want;
    if (!hint || !ptr)
        return false;
    switch (type) {
        case ATOMIC_DENSE: stored = hint->atomic_magic; break;
        case UNSHARED:     stored = hint->unshared_magic; break;
        case DEP_ROLE:     stored = hint->dep_magic; break;
        default:           return false;
    }
    want = __sched_tag_bloom_of(ptr);
    return want != 0 && (stored & want) == want;
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
