/*===-- sched_hint.h - Scheduling hint section layout ---------------------===*\
|*                                                                            *|
|* This header defines the binary layout of the "__sched_hint" ELF section.   *|
|* It is shared between:                                                      *|
|*   1. The LLVM compiler pass  (produces the section at compile time)        *|
|*   2. The kernel ELF loader   (maps the section; pointer in task_struct)    *|
|*   3. Userspace runtime       (reads the hints via mapped pointer)          *|
|*                                                                            *|
|* There is one instance of struct sched_hint per THREAD (TLS). Each thread   *|
|* carries its own scheduling hint so the kernel scheduler can make           *|
|* per-thread decisions.                                                      *|
|*                                                                            *|
|* The section is named "__sched_hint" (no leading dot) so that the GNU       *|
|* linker automatically generates the symbol __start___sched_hint for         *|
|* convenient access.                                                         *|
|*                                                                            *|
|* NOTE: ABI is NOT yet stable (prototype/validation phase). The layout may   *|
|* change between iterations. Once stabilised, only append new fields at the  *|
|* end. Bump SCHED_HINT_VERSION when layout changes.                          *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

#ifndef SCHED_HINT_H
#define SCHED_HINT_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/*===----------------------------------------------------------------------===*\
|* Constants                                                                  *|
\*===----------------------------------------------------------------------===*/

#define SCHED_HINT_MAGIC   0x5348494EU /* "SHIN" in ASCII                     */
#define SCHED_HINT_VERSION 1
#ifndef PR_SET_SCHED_HINT_OFFSET
#define PR_SET_SCHED_HINT_OFFSET        83
#endif

/* Section name — valid C identifier so linker generates __start_ / __stop_   */
#define SCHED_HINT_SECTION "__sched_hint"

/* TLS storage-class specifier — portable across C11/C++11/GCC/Clang/MSVC    */
#if defined(__cplusplus) && __cplusplus >= 201103L
#define SCHED_HINT_TLS thread_local
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define SCHED_HINT_TLS _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#define SCHED_HINT_TLS __thread
#elif defined(_MSC_VER)
#define SCHED_HINT_TLS __declspec(thread)
#else
#define SCHED_HINT_TLS /* unsupported — fallback to plain global */
#endif

/*===----------------------------------------------------------------------===*\
|* compute-dense sub-type (bitmask, 3 bits)                                   *|
|* Bitmask of compute types present when SCHED_TAG_COMPUTE_DENSE is set.      *|
|* Multiple bits may be set simultaneously (e.g. INT | FLOAT).                *|
\*===----------------------------------------------------------------------===*/

#define SCHED_COMPUTE_NONE   0          /* no compute type present            */
#define SCHED_COMPUTE_INT    (1U << 0)  /* integer ALU                        */
#define SCHED_COMPUTE_FLOAT  (1U << 1)  /* floating-point                     */
#define SCHED_COMPUTE_SIMD   (1U << 2)  /* vector / SIMD                      */

/*===----------------------------------------------------------------------===*\
|* memory-dense sub-type (2 bits)                                             *|
|* Encodes the memory access pattern when SCHED_TAG_MEMORY_DENSE is set.      *|
\*===----------------------------------------------------------------------===*/

#define SCHED_MEMORY_NONE    0  /* no dominant memory pattern                 */
#define SCHED_MEMORY_STREAM  1  /* sequential / streaming access              */
#define SCHED_MEMORY_RANDOM  2  /* random access                              */

/*===----------------------------------------------------------------------===*\
|* struct sched_hint — per-thread scheduling hint (TLS instance)              *|
|*                                                                            *|
|* Layout (64 bytes total, naturally aligned):                                *|
|*                                                                            *|
|*   [0..3]    magic          — SCHED_HINT_MAGIC for validation               *|
|*   [4..7]    version        — struct layout version                         *|
|*                                                                            *|
|*   --- Tag payloads (fixed offsets for kernel fast-path access) ---         *|
|*   The scheduler checks each payload directly: non-zero = active.           *|
|*                                                                            *|
|*   [8]       compute_dense  — SCHED_COMPUTE_xxx sub-type bitmask            *|
|*   [9]       branch_dense   — 0/1                                           *|
|*   [10]      memory_dense   — SCHED_MEMORY_xxx sub-type                     *|
|*   [11]      atomic_dense   — 0/1                                           *|
|*   [12]      io_dense       — 0/1                                           *|
|*   [13]      unshared       — 0/1                                           *|
|*   [14]      compute_prep   — 0/1                                           *|
|*   [15]      reserved_pad   — alignment padding                             *|
|*                                                                            *|
|*   --- Extended payloads ---                                                *|
|*                                                                            *|
|*   [16..23]  atomic_magic   — 64-bit bloom filter for atomic co-scheduling  *|
|*             Encodes which base pointer addresses this thread's atomic-dense *|
|*             region operates on (AtomicRMW / CAS only).                     *|
|*                                                                            *|
|*             The address is aligned to a 64-byte cache line (masked & ~63)  *|
|*             before hashing to detect False Sharing/True Sharing contention.*|
|*                                                                            *|
|*             Hash:  h = (aligned_ptr) * 0x9E3779B97F4A7C15  (fibonacci)     *|
|*             Bits:  bloom |= (1 << (h & 63))                                *|
|*                         | (1 << ((h>>16) & 63))                            *|
|*                         | (1 << ((h>>32) & 63))                            *|
|*                         | (1 << ((h>>48) & 63))    (k=4 bits per ptr)      *|
|*             Multiple pointers are OR'd together (idempotent).              *|
|*                                                                            *|
|*             Scheduler comparison:                                          *|
|*               overlap = popcount(magic_a & magic_b) >= 4                   *|
|*               → true:  threads contend on same data → co-locate            *|
|*               → false: threads use different data   → don't interfere      *|
|*             magic == 0 means "unknown/no info" — do NOT co-locate.         *|
|*                                                                            *|
|*   [24..31]  dep_magic      — dependency magic number for IPC grouping      *|
|*   [32]      dep_role       — 0 = producer, 1 = consumer                    *|
|*   [33..39]  reserved1[7]   — future use                                    *|
|*                                                                            *|
|*   [40..63]  reserved2[24]  — generous padding for future tag payloads      *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

struct __attribute__((aligned(64))) sched_hint {
    /* ---- header (8 bytes) ---- */
    uint32_t magic;             /* SCHED_HINT_MAGIC                           */
    uint32_t version;           /* SCHED_HINT_VERSION                         */

    /* ---- tag payloads: byte-sized for simplicity (8 bytes) ---- */
    /* Non-zero = tag is active; the value itself encodes the sub-type.       */
    uint8_t  compute_dense;     /* SCHED_COMPUTE_xxx                          */
    uint8_t  branch_dense;      /* 0 or 1                                     */
    uint8_t  memory_dense;      /* SCHED_MEMORY_xxx                           */
    uint8_t  atomic_dense;      /* 0 or 1                                     */
    uint8_t  io_dense;          /* 0 or 1                                     */
    uint8_t  unshared;          /* 0 or 1                                     */
    uint8_t  compute_prep;      /* 0 or 1                                     */
    uint8_t  reserved_pad;      /* padding to 8-byte boundary                 */

    /* ---- extended payloads (24 bytes) ---- */
    uint64_t atomic_magic;      /* bloom filter for atomic co-scheduling       */
    uint64_t dep_magic;         /* dependency magic for IPC grouping          */
    uint8_t  dep_role;          /* 0 = producer, 1 = consumer                 */
    uint8_t  reserved1[7];      /* future use                                 */

    /* ---- reserved for future tag types (24 bytes) ---- */
    uint8_t  reserved2[24];
};

/* Compile-time size assertion: struct must be exactly 64 bytes              */
_Static_assert(sizeof(struct sched_hint) == 64,
               "sched_hint must be exactly 64 bytes");

/*===----------------------------------------------------------------------===*\
|* Access helpers                                                             *|
|*                                                                            *|
|* The hint variable is Thread-Local Storage (TLS). Every extern declaration  *|
|* must include the TLS specifier so the linker resolves it correctly         *|
|* (ELF symbol type STT_TLS).                                                 *|
|*                                                                            *|
|* IMPORTANT: The actual symbol name is "__sched_hint.data" (with a period), *|
|* which cannot be directly written in C/C++ source code. Use inline         *|
|* assembly or the helper in test/reader_dynamic.h to access it:             *|
|*                                                                            *|
|* In userspace (see test/reader_dynamic.h):                                  *|
|*   #include "reader_dynamic.h"                                              *|
|*   struct sched_hint *hint = get_sched_hint_data();                         *|
|*                                                                            *|
|* In kernel space (after your ELF loader modification):                      *|
|*   struct sched_hint *hint = current->sched_hint;                           *|
|*   if (hint && hint->magic == SCHED_HINT_MAGIC) { ... }                     *|
|*                                                                            *|
|* Fast-path check example (each payload is its own activity flag):           *|
|*   if (hint->compute_dense) {                                               *|
|*       uint8_t m = hint->compute_dense;                                     *|
|*       if (m & SCHED_COMPUTE_INT)   { ... }                                 *|
|*       if (m & SCHED_COMPUTE_FLOAT) { ... }                                 *|
|*       if (m & SCHED_COMPUTE_SIMD)  { ... }                                 *|
|*   }                                                                        *|
|*   if (hint->atomic_dense) { ... }                                          *|
\*===----------------------------------------------------------------------===*/

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SCHED_HINT_H */
