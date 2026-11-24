/*===-- sched_tag.h - Source-level Scheduling Instrumentation -------------===*\
|*                                                                            *|
|* This header provides the token-based tag API (enter/leave), the atomic-set *|
|* helpers (add/clear), and the compatibility `sched_tag_set` writer plus the *|
|* `sched_tag_get` / `sched_tag_test_magic` readers.                          *|
|*                                                                            *|
|* Data model: each thread's tags live in a 64-byte slot inside kernel-owned  *|
|* pages. The first use on a thread registers it via                          *|
|* prctl(PR_SET_SCHED_HINT, &word) and caches the backfilled slot address in  *|
|* TLS. The kernel stamps the slot header; this side only writes payloads.    *|
|*                                                                            *|
|* Tag semantics:                                                            *|
|*   unshared/atomic/dep magic words are exact 64-bit identity keys          *|
|*   memory magic words are locality keys (local sketch + global identity)   *|
|*   unshared      current lock, overwrite + restore (nest by token)          *|
|*   atomic_dense exact atomic-variable set hash, accumulate + clear/restore  *|
|*   memory        hot-page locality + read/write mode, enter/leave           *|
|*   dep           current handoff object + role, overwrite + restore        *|
|*                                                                            *|
|* Usage Examples:                                                            *|
|*   1. unshared: current lock, nest-safe:                                    *|
|*      struct sched_tag_unshared t = sched_tag_enter_unshared(&my_mutex);     *|
|*      critical_section();                                                   *|
|*      sched_tag_leave_unshared(&t);                                         *|
|*                                                                            *|
|*   2. atomic set: add every variable in the region, then restore:           *|
|*      struct sched_tag_atomic t = sched_tag_enter_atomic();                 *|
|*      sched_tag_add_atomic(&counter_a);                                     *|
|*      sched_tag_add_atomic(&counter_b);                                     *|
|*      lock_free_region();                                                   *|
|*      sched_tag_leave_atomic(&t);                                           *|
|*                                                                            *|
|*   3. dep: current producer/consumer handoff object + role, nest-safe:      *|
|*      struct sched_tag_dep t =                                              *|
|*          sched_tag_enter_dep(&q, SCHED_DEP_CONSUMER);                      *|
|*      recv(q, ...);                                                         *|
|*      sched_tag_leave_dep(&t);                                              *|
|*                                                                            *|
|*   4. Cross-process / global identity (all magic tags have an _id variant):  *|
|*      struct sched_tag_unshared t = sched_tag_enter_unshared_id(0xDEADBEEF); *|
|*      ...                                                                   *|
|*      sched_tag_leave_unshared(&t);                                         *|
|*                                                                            *|
|*   5. Read back / probe (the raw struct is never exposed):                  *|
|*      uint8_t v      = sched_tag_get(UNSHARED);                             *|
|*      bool    same   = sched_tag_test_magic(UNSHARED, &my_mutex);           *|
|*                                                                            *|
|* Hints are sampled at scheduling hooks. A hint is only observed if it is    *|
|* active at a hook. Short userspace-only regions may never be observed.      *|
|* Hints are advisory and must not be used for correctness.                   *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

/*
 * Link users of this header with the library `libsched_tag.a`: it owns the
 * single per-thread registration, slot cache, process salt, and fork hook,
 * and this header is the inline API over it. A program that never uses tags
 * never references it, so the linker does not pull it in.
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

/* Tag types corresponding to sched_hint fields */
typedef enum {
    COMPUTE_DENSE,
    MEMORY_DENSE,
    ATOMIC_DENSE,
    UNSHARED,
    DEP_ROLE
} sched_tag_type_t;

struct sched_tag_compute {
    uint8_t state;
};

struct sched_tag_memory {
    uint64_t magic;
    uint8_t  state;
};

struct sched_tag_atomic {
    uint64_t magic;
    uint8_t  state;
};

struct sched_tag_unshared {
    uint64_t magic;
    uint8_t  state;
};

struct sched_tag_dep {
    uint64_t magic;
    uint8_t  role;
    uint8_t  state;
};

/* Scoped tag API */
static inline struct sched_tag_compute sched_tag_enter_compute(uint8_t mask);
static inline void sched_tag_leave_compute(struct sched_tag_compute *t);

static inline struct sched_tag_memory
sched_tag_enter_memory(uint8_t state);
static inline struct sched_tag_memory
sched_tag_enter_memory_id(uint8_t state, uint64_t id);
static inline void sched_tag_add_memory(const void *addr);
static inline void sched_tag_add_memory_id(uint64_t id);
static inline void sched_tag_leave_memory(struct sched_tag_memory *t);

static inline struct sched_tag_unshared
sched_tag_enter_unshared(const void *lock);
static inline struct sched_tag_unshared
sched_tag_enter_unshared_id(uint64_t id);
static inline void sched_tag_leave_unshared(struct sched_tag_unshared *t);

static inline struct sched_tag_atomic sched_tag_enter_atomic(void);
static inline struct sched_tag_atomic
sched_tag_enter_atomic_id(uint64_t id);
static inline void sched_tag_add_atomic(const void *var);
static inline void sched_tag_add_atomic_id(uint64_t id);
static inline void sched_tag_clear_atomic(void);
static inline void sched_tag_leave_atomic(struct sched_tag_atomic *t);

static inline struct sched_tag_dep
sched_tag_enter_dep(const void *handoff, uint8_t role);
static inline struct sched_tag_dep
sched_tag_enter_dep_id(uint64_t id, uint8_t role);
static inline void sched_tag_leave_dep(struct sched_tag_dep *t);

/* Readers / probes */
static inline uint8_t sched_tag_get(sched_tag_type_t type);
static inline bool sched_tag_test_key(sched_tag_type_t type, uint64_t key);
static inline bool sched_tag_test_magic(sched_tag_type_t type, const void *ptr);
static inline bool sched_tag_test_magic_id(sched_tag_type_t type, uint64_t id);

/* Key helpers */
static inline uint64_t sched_tag_hash64(uint64_t x);
static inline uint64_t sched_tag_key_ptr(const void *ptr);
static inline uint64_t sched_tag_key_id(uint64_t id);
static inline uint64_t sched_tag_global_id(uint64_t dev, uint64_t ino);

/* The compatibility sched_tag_set() is defined below. */

/* The compatibility sched_tag_set() is defined below. */

/*
 * Per-thread slot cache, defined once in the library (libsched_tag).
 * Shared by every translation unit, so all writers and readers hit the same
 * cell for the current thread.
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
 * Per-process hash salt, defined once in the library and initialized by its
 * constructor. All threads in the process share it. Pointer keys are only
 * meaningful inside this process; use the _id variants for cross-process
 * global identities.
 */
extern uint64_t __sched_tag_process_salt;

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
 * Internal: this thread's slot, shared by the tag readers and writers.
 * Deliberately NOT exposed: handing out the raw slot would let callers touch
 * the kernel-stamped header or any field, so the only public surface is the
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

/*
 * Exact identity keys.
 *
 *   pointer identity : hash64(ptr ^ per-process salt)
 *   static identity  : hash64(id)
 *
 * A null pointer / zero id maps to 0, which means "no key / no info".
 * hash64 is a stable non-cryptographic 64-bit hash. One odd-constant multiply
 * plus an xor-fold keeps it a bijection (different inputs cannot collide), while
 * the dependency chain stays short (~5 cycles) and independent pointers can be
 * hashed in parallel. Its arithmetic must match any cross-process producer of
 * the same static id.
 */
#define SCHED_TAG_HASH_MULT 0x9E3779B97F4A7C15ULL

static inline uint64_t sched_tag_hash64(uint64_t x) {
    x *= SCHED_TAG_HASH_MULT;
    x ^= x >> 32;
    return x;
}

static inline uint64_t sched_tag_key_ptr(const void *ptr) {
    if (!ptr)
        return 0;
    return sched_tag_hash64((uint64_t)(uintptr_t)ptr ^
                            __sched_tag_process_salt);
}

static inline uint64_t sched_tag_key_id(uint64_t id) {
    return id ? sched_tag_hash64(id) : 0;
}

static inline uint64_t sched_tag_global_id(uint64_t dev, uint64_t ino) {
    if (!ino)
        return 0;
    return sched_tag_hash64(dev ^ (ino * SCHED_TAG_HASH_MULT));
}

#define SCHED_TAG_PAGE_SHIFT 12

static inline uint64_t __sched_tag_memory_page_key(const void *addr)
{
    uint64_t page;

    if (!addr)
        return 0;
    page = (uint64_t)(uintptr_t)addr >> SCHED_TAG_PAGE_SHIFT;
    return sched_tag_hash64(page ^ __sched_tag_process_salt);
}

/* ------------------------------------------------------------------------- */
/* compute_dense: execution-resource mask, overwrite + restore                */
/* ------------------------------------------------------------------------- */

static inline struct sched_tag_compute
sched_tag_enter_compute(uint8_t mask)
{
    struct sched_tag_compute t = {0};
    struct sched_hint *h = __sched_hint_slot();

    if (!h)
        return t;
    t.state = h->compute_dense;
    h->compute_dense = mask;
    return t;
}

static inline void sched_tag_leave_compute(struct sched_tag_compute *t)
{
    struct sched_hint *h;

    if (!t)
        return;
    h = __sched_hint_slot();
    if (!h)
        return;
    h->compute_dense = t->state;
    t->state = 0;
}

/* ------------------------------------------------------------------------- */
/* unshared: current lock, overwrite + restore                                */
/* ------------------------------------------------------------------------- */

static inline struct sched_tag_unshared
__sched_tag_enter_unshared_key(uint64_t key, uint8_t state)
{
    struct sched_tag_unshared t = {0};
    struct sched_hint *h = __sched_hint_slot();

    if (!h)
        return t;
    t.magic = h->unshared_magic;
    t.state = h->unshared;
    h->unshared = state;
    h->unshared_magic = key;
    return t;
}

static inline struct sched_tag_unshared
sched_tag_enter_unshared(const void *lock)
{
    return __sched_tag_enter_unshared_key(sched_tag_key_ptr(lock),
                                          SCHED_UNSHARED_HELD);
}

static inline struct sched_tag_unshared
sched_tag_enter_unshared_id(uint64_t id)
{
    return __sched_tag_enter_unshared_key(id,
                                          SCHED_UNSHARED_HELD |
                                          SCHED_UNSHARED_GLOBAL);
}

static inline void sched_tag_leave_unshared(struct sched_tag_unshared *t)
{
    struct sched_hint *h;

    if (!t)
        return;
    h = __sched_hint_slot();
    if (!h)
        return;
    h->unshared = t->state;
    h->unshared_magic = t->magic;
    t->magic = 0;
    t->state = 0;
}

/* ------------------------------------------------------------------------- */
/* atomic_dense: exact set identity, accumulate + clear/restore              */
/* ------------------------------------------------------------------------- */

static inline struct sched_tag_atomic sched_tag_enter_atomic(void)
{
    struct sched_tag_atomic t = {0};
    struct sched_hint *h = __sched_hint_slot();

    if (!h)
        return t;
    t.magic = h->atomic_magic;
    t.state = h->atomic_dense;
    /* Start a fresh set; add_atomic() builds the identity. */
    h->atomic_dense = SCHED_ATOMIC_NONE;
    h->atomic_magic = 0;
    return t;
}

static inline struct sched_tag_atomic
sched_tag_enter_atomic_id(uint64_t id)
{
    struct sched_tag_atomic t = {0};
    struct sched_hint *h = __sched_hint_slot();

    if (!h)
        return t;
    t.magic = h->atomic_magic;
    t.state = h->atomic_dense;
    h->atomic_dense = SCHED_ATOMIC_DENSE | SCHED_ATOMIC_GLOBAL;
    h->atomic_magic = id;
    return t;
}

static inline void sched_tag_add_atomic_key(uint64_t key)
{
    struct sched_hint *h = __sched_hint_slot();

    if (!h)
        return;
    if (h->atomic_dense & SCHED_ATOMIC_GLOBAL)
        return;
    h->atomic_dense = SCHED_ATOMIC_DENSE;
    /* XOR is commutative, so set order does not matter. Each variable must
     * be added exactly once per region (callers should deduplicate). */
    h->atomic_magic ^= key;
}

static inline void sched_tag_add_atomic(const void *var)
{
    sched_tag_add_atomic_key(sched_tag_key_ptr(var));
}

static inline void sched_tag_add_atomic_id(uint64_t id)
{
    struct sched_hint *h = __sched_hint_slot();

    if (!h || !id)
        return;
    if (!(h->atomic_dense & SCHED_ATOMIC_GLOBAL))
        return;
    h->atomic_magic ^= id;
}

static inline void sched_tag_clear_atomic(void)
{
    struct sched_hint *h = __sched_hint_slot();

    if (!h)
        return;
    h->atomic_dense = SCHED_ATOMIC_NONE;
    h->atomic_magic = 0;
}

static inline void sched_tag_leave_atomic(struct sched_tag_atomic *t)
{
    struct sched_hint *h;

    if (!t)
        return;
    h = __sched_hint_slot();
    if (!h)
        return;
    h->atomic_dense = t->state;
    h->atomic_magic = t->magic;
    t->magic = 0;
    t->state = 0;
}

/* ------------------------------------------------------------------------- */
/* memory_dense: shared-memory locality + read/write mode                     */
/*                                                                           */
/* local  : memory_magic is a set hash over hot pages                         */
/* global : memory_magic is a cross-process object identity                   */
/* ------------------------------------------------------------------------- */

static inline struct sched_tag_memory
sched_tag_enter_memory(uint8_t state)
{
    struct sched_tag_memory t = {0};
    struct sched_hint *h = __sched_hint_slot();

    if (!h)
        return t;
    state &= SCHED_MEMORY_READ_MOSTLY | SCHED_MEMORY_WRITE_HEAVY;
    t.state = h->memory_dense;
    t.magic = h->memory_magic;
    h->memory_dense = state;
    h->memory_magic = 0;
    return t;
}

static inline struct sched_tag_memory
sched_tag_enter_memory_id(uint8_t state, uint64_t id)
{
    struct sched_tag_memory t = {0};
    struct sched_hint *h = __sched_hint_slot();

    if (!h)
        return t;
    state &= SCHED_MEMORY_READ_MOSTLY | SCHED_MEMORY_WRITE_HEAVY;
    t.state = h->memory_dense;
    t.magic = h->memory_magic;
    h->memory_dense = state ? (state | SCHED_MEMORY_GLOBAL) :
                              SCHED_MEMORY_NONE;
    h->memory_magic = state ? id : 0;
    return t;
}

static inline void sched_tag_add_memory(const void *addr)
{
    struct sched_hint *h = __sched_hint_slot();

    if (!h || !addr)
        return;
    if (!(h->memory_dense & (SCHED_MEMORY_READ_MOSTLY |
                             SCHED_MEMORY_WRITE_HEAVY)))
        return;
    if (h->memory_dense & SCHED_MEMORY_GLOBAL)
        return;
    h->memory_magic ^= __sched_tag_memory_page_key(addr);
}

static inline void sched_tag_add_memory_id(uint64_t id)
{
    struct sched_hint *h = __sched_hint_slot();

    if (!h || !id)
        return;
    if (!(h->memory_dense & (SCHED_MEMORY_READ_MOSTLY |
                             SCHED_MEMORY_WRITE_HEAVY)))
        return;
    if (!(h->memory_dense & SCHED_MEMORY_GLOBAL))
        return;
    h->memory_magic ^= id;
}

static inline void sched_tag_leave_memory(struct sched_tag_memory *t)
{
    struct sched_hint *h;

    if (!t)
        return;
    h = __sched_hint_slot();
    if (!h)
        return;
    h->memory_dense = t->state;
    h->memory_magic = t->magic;
    t->state = 0;
    t->magic = 0;
}

/* ------------------------------------------------------------------------- */
/* dep: current producer/consumer handoff object + role, overwrite + restore */
/*                                                                           */
/* The handoff object can be a pipe, queue, socket, channel, ring buffer, or  */
/* any other shared object that moves work/data from producers to consumers. */
/* The scheduler only sees (dep_magic, dep_role); the type is irrelevant.    */
/* ------------------------------------------------------------------------- */

static inline struct sched_tag_dep
__sched_tag_enter_dep_key(uint64_t key, uint8_t role, uint8_t flags)
{
    struct sched_tag_dep t = {0};
    struct sched_hint *h = __sched_hint_slot();

    if (!h)
        return t;
    t.magic = h->dep_magic;
    t.role = h->dep_role;

    role &= SCHED_DEP_BOTH;
    h->dep_role = role ? (role | flags) : SCHED_DEP_NONE;
    h->dep_magic = role ? key : 0;
    return t;
}

static inline struct sched_tag_dep
sched_tag_enter_dep(const void *handoff, uint8_t role)
{
    return __sched_tag_enter_dep_key(sched_tag_key_ptr(handoff), role, 0);
}

static inline struct sched_tag_dep
sched_tag_enter_dep_id(uint64_t id, uint8_t role)
{
    return __sched_tag_enter_dep_key(id, role, SCHED_DEP_GLOBAL);
}

static inline void sched_tag_leave_dep(struct sched_tag_dep *t)
{
    struct sched_hint *h;

    if (!t)
        return;
    h = __sched_hint_slot();
    if (!h)
        return;
    h->dep_role = t->role;
    h->dep_magic = t->magic;
    t->magic = 0;
    t->state = 0;
    t->role = 0;
}

/* ------------------------------------------------------------------------- */
/* Readers                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Reader: the value currently stored in this thread's tag payload field, or
 * 0 when the mechanism is unavailable (kernel without sched_hint support, or
 * ABI mismatch). 0 means "no tag active", so an unsupported kernel reads as
 * "nothing set" rather than an error. This is the inverse of
 * sched_tag_set(type, value).
 *
 * Note it returns only the state byte, never the magic word: the raw magic is
 * an opaque identity key that callers should not reason about. To probe it,
 * use sched_tag_test_magic() / sched_tag_test_key() below.
 */
static inline uint8_t sched_tag_get(sched_tag_type_t type) {
    struct sched_hint *hint = __sched_hint_slot();
    if (!hint)
        return 0;
    switch (type) {
        case COMPUTE_DENSE: return hint->compute_dense;
        case MEMORY_DENSE:  return hint->memory_dense;
        case ATOMIC_DENSE:  return hint->atomic_dense;
        case UNSHARED:      return hint->unshared;
        case DEP_ROLE:      return hint->dep_role;
    }
    return 0;
}

/*
 * Exact-key probe. Keys are compared for equality:
 *   unshared / dep : stored key equals the key of this object
 *   atomic         : stored set hash equals the hash of a one-element set
 *                    containing this object
 *
 * For a multi-variable atomic set, compare against the exact expected set
 * hash with sched_tag_test_key() rather than probing one member.
 */
static inline bool sched_tag_test_key(sched_tag_type_t type, uint64_t key) {
    struct sched_hint *hint = __sched_hint_slot();
    uint64_t stored;

    if (!hint || !key)
        return false;
    switch (type) {
        case ATOMIC_DENSE: stored = hint->atomic_magic; break;
        case UNSHARED:     stored = hint->unshared_magic; break;
        case DEP_ROLE:     stored = hint->dep_magic; break;
        default:           return false;
    }
    return stored == key;
}

static inline bool sched_tag_test_magic(sched_tag_type_t type, const void *ptr) {
    return sched_tag_test_key(type, sched_tag_key_ptr(ptr));
}

static inline bool sched_tag_test_magic_id(sched_tag_type_t type, uint64_t id) {
    return sched_tag_test_key(type, id);
}

/* ------------------------------------------------------------------------- */
/* Compatibility setter                                                       */
/*                                                                            */
/* sched_tag_set() is kept for source compatibility, but each tag keeps its    */
/* own exact-key semantics:                                                    */
/*   unshared / dep : overwrite the current exact key                         */
/*   atomic         : set the exact one-element set hash (use add_atomic()    */
/*                    for multi-variable sets, or enter_atomic()/leave)       */
/*   memory         : 3-arg pointer = local single page,                     */
/*                    3-arg id      = local static identity                  */
/*   global         : 3-arg integer id; enter_*_id() for scoped global     */
/* ------------------------------------------------------------------------- */

static inline struct sched_hint *__sched_tag_set_base(sched_tag_type_t type, uint8_t value) {
    struct sched_hint *hint = __sched_hint_slot();
    if (!hint)
        return NULL;
    switch (type) {
        case COMPUTE_DENSE: hint->compute_dense = value; break;
        case MEMORY_DENSE:  hint->memory_dense  = value & (SCHED_MEMORY_READ_MOSTLY |
                                                           SCHED_MEMORY_WRITE_HEAVY);
                            hint->memory_magic = 0; break;
        case ATOMIC_DENSE:  hint->atomic_dense  = value & SCHED_ATOMIC_DENSE;
                            hint->atomic_magic = 0; break;
        case UNSHARED:      hint->unshared      = value & SCHED_UNSHARED_HELD;
                            hint->unshared_magic = 0; break;
        case DEP_ROLE:      hint->dep_role      = value & SCHED_DEP_BOTH;
                            hint->dep_magic = 0; break;
    }
    return hint;
}

/* Setter for pointer-based local exact-key calculation */
static inline void __sched_tag_set_ptr(sched_tag_type_t type, uint8_t value, const void *ptr) {
    struct sched_hint *hint = __sched_tag_set_base(type, value);
    uint64_t key;
    if (!hint || !value)
        return;
    key = sched_tag_key_ptr(ptr);
    switch (type) {
        case ATOMIC_DENSE: hint->atomic_magic = key; break;
        case UNSHARED:     hint->unshared_magic = key; break;
        case DEP_ROLE:     hint->dep_magic = hint->dep_role ? key : 0; break;
        case MEMORY_DENSE: hint->memory_magic = __sched_tag_memory_page_key(ptr); break;
        default: break;
    }
}

/* Setter for 64-bit global identity */
static inline void __sched_tag_set_id(sched_tag_type_t type, uint8_t value, uint64_t id) {
    struct sched_hint *hint;

    if (type == COMPUTE_DENSE)
        return;
    hint = __sched_tag_set_base(type, value);
    if (!hint || !value)
        return;
    switch (type) {
        case MEMORY_DENSE:
            if (hint->memory_dense & (SCHED_MEMORY_READ_MOSTLY |
                                      SCHED_MEMORY_WRITE_HEAVY)) {
                hint->memory_dense |= SCHED_MEMORY_GLOBAL;
                hint->memory_magic = id;
            }
            break;
        case ATOMIC_DENSE:
            if (hint->atomic_dense & SCHED_ATOMIC_DENSE) {
                hint->atomic_dense |= SCHED_ATOMIC_GLOBAL;
                hint->atomic_magic = id;
            }
            break;
        case UNSHARED:
            if (hint->unshared & SCHED_UNSHARED_HELD) {
                hint->unshared |= SCHED_UNSHARED_GLOBAL;
                hint->unshared_magic = id;
            }
            break;
        case DEP_ROLE:
            if (hint->dep_role & SCHED_DEP_BOTH) {
                hint->dep_role |= SCHED_DEP_GLOBAL;
                hint->dep_magic = id;
            }
            break;
        default:
            break;
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

/* C macros with _Generic for C11 (allows static identity ids vs pointers) */
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
#define sched_tag_set(...) \
    __SCHED_TAG_MACRO(__VA_ARGS__, __sched_tag_3, __sched_tag_set_base)(__VA_ARGS__)

#endif /* __cplusplus */

#endif /* SCHED_TAG_H */
