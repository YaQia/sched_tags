/*===-- sched_tag.h - Source-level Scheduling Instrumentation --------------===*\
|*                                                                            *|
|* This header provides the `sched_tag` function/macro for directly           *|
|* instrumenting source code with scheduling tags. It is a lightweight        *|
|* alternative or complement to the LLVM-based `sched_tags.json` config.      *|
|*                                                                            *|
|* Usage Examples:                                                            *|
|*   1. Basic tag assignment:                                                 *|
|*      sched_tag(UNSHARED, 1);                                               *|
|*      critical_section();                                                   *|
|*      sched_tag(UNSHARED, 0);                                               *|
|*                                                                            *|
|*   2. Co-scheduling with a lock variable (Bloom Filter magic):              *|
|*      sched_tag(UNSHARED, 1, &my_mutex);                                    *|
|*      ...                                                                   *|
|*      sched_tag(UNSHARED, 0);                                               *|
|*                                                                            *|
|*   3. Cross-process co-scheduling with a Static Magic Number:               *|
|*      sched_tag(UNSHARED, 1, 0xDEADBEEF12345678ULL);                        *|
|*      ...                                                                   *|
|*      sched_tag(UNSHARED, 0);                                               *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

#ifndef SCHED_TAG_H
#define SCHED_TAG_H

#include "sched_hint.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The scheduling hint data resides in Thread-Local Storage (TLS).
 * It is typically allocated in the special "__sched_hint" section.
 * The period in "__sched_hint.data" requires inline assembly mapping.
 * We define it as a weak symbol so that GCC/Clang can compile this header
 * without needing the LLVM Pass. The linker will merge multiple weak
 * definitions into a single TLS instance.
 */
__attribute__((section("__sched_hint"), weak, tls_model("initial-exec")))
__thread struct sched_hint __sched_hint_data __asm__("__sched_hint.data") = {
    SCHED_HINT_MAGIC,
    SCHED_HINT_VERSION,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, {0}
};

#ifndef PR_SET_SCHED_HINT_OFFSET
#define PR_SET_SCHED_HINT_OFFSET 83
#endif

/*
 * When compiled directly with GCC/Clang (bypassing the LLVM Pass), we must
 * manually inform the kernel of the TLS offset. We use a weak constructor 
 * to ensure it runs at process startup. The linker merges multiple weak
 * definitions from different .o files, and the global weak guard ensures the 
 * body only executes exactly once across the entire process, even when 
 * mixing C and C++ translation units.
 */
__attribute__((weak)) int __sched_tag_prctl_initialized = 0;

__attribute__((constructor, weak))
void __sched_tag_init_prctl(void) {
    if (__sched_tag_prctl_initialized++) return;

    void *tp = __builtin_thread_pointer();
    long offset = (long)&__sched_hint_data - (long)tp;
    
    extern int prctl(int, unsigned long, unsigned long, unsigned long, unsigned long);
    prctl(PR_SET_SCHED_HINT_OFFSET, offset, (unsigned long)&__sched_hint_data, 0, 0);
}

/* Tag types corresponding to sched_hint fields */
typedef enum {
    COMPUTE_DENSE,
    BRANCH_DENSE,
    MEMORY_DENSE,
    ATOMIC_DENSE,
    IO_DENSE,
    UNSHARED,
    COMPUTE_PREP
} sched_tag_type_t;

/* Bloom filter hash calculation (identical to LLVM pass logic) */
static inline void __sched_tag_bloom_add(uint64_t *bloom, const void *ptr) {
    if (!ptr) return;
    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    addr &= ~63ULL; /* align to 64 bytes */
    uint64_t h = addr * 0x9E3779B97F4A7C15ULL; /* fibonacci hashing */
    *bloom |= (1ULL << (h & 63))
           |  (1ULL << ((h >> 16) & 63))
           |  (1ULL << ((h >> 32) & 63))
           |  (1ULL << ((h >> 48) & 63));
}

/* Base setter (no magic var) */
static inline void __sched_tag_set_base(sched_tag_type_t type, uint8_t value) {
    struct sched_hint *hint = &__sched_hint_data;
    switch (type) {
        case COMPUTE_DENSE: hint->compute_dense = value; break;
        case BRANCH_DENSE:  hint->branch_dense  = value; break;
        case MEMORY_DENSE:  hint->memory_dense  = value; break;
        case ATOMIC_DENSE:  hint->atomic_dense  = value; if (!value) hint->atomic_magic = 0; break;
        case IO_DENSE:      hint->io_dense      = value; break;
        case UNSHARED:      hint->unshared      = value; if (!value) hint->unshared_magic = 0; break;
        case COMPUTE_PREP:  hint->compute_prep  = value; break;
    }
}

/* Setter for pointer-based bloom filter calculation */
static inline void __sched_tag_set_ptr(sched_tag_type_t type, uint8_t value, const void *ptr) {
    __sched_tag_set_base(type, value);
    if (value && ptr) {
        struct sched_hint *hint = &__sched_hint_data;
        if (type == ATOMIC_DENSE) __sched_tag_bloom_add(&hint->atomic_magic, ptr);
        else if (type == UNSHARED) __sched_tag_bloom_add(&hint->unshared_magic, ptr);
    }
}

/* Setter for 64-bit static magic ID */
static inline void __sched_tag_set_id(sched_tag_type_t type, uint8_t value, uint64_t id) {
    __sched_tag_set_base(type, value);
    if (value && id) {
        struct sched_hint *hint = &__sched_hint_data;
        if (type == ATOMIC_DENSE) hint->atomic_magic = id;
        else if (type == UNSHARED) hint->unshared_magic = id;
    }
}

#ifdef __cplusplus
} /* extern "C" */

/* C++ Overloads */
static inline void sched_tag(sched_tag_type_t type, uint8_t value) {
    __sched_tag_set_base(type, value);
}

static inline void sched_tag(sched_tag_type_t type, uint8_t value, const void *ptr) {
    __sched_tag_set_ptr(type, value, ptr);
}

static inline void sched_tag(sched_tag_type_t type, uint8_t value, uint64_t id) {
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
#define sched_tag(...) __SCHED_TAG_MACRO(__VA_ARGS__, __sched_tag_3, __sched_tag_set_base)(__VA_ARGS__)

#endif /* __cplusplus */

#endif /* SCHED_TAG_H */
