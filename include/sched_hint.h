/*===-- sched_hint.h - userspace glue over the shared sched_hint ABI ------===*\
|*                                                                            *|
|* The sched_hint ABI (struct sched_hint, all SCHED_* enums, magic/version,   *|
|* and bloom parameters) is defined ONCE in the kernel uapi header            *|
|* <linux/sched_hint.h>, installed via `make headers_install`. This file is   *|
|* a thin userspace wrapper that pulls in that single source of truth and     *|
|* adds the userspace-only glue (TLS specifier, ELF section name, and a       *|
|* prctl-number fallback) that has no place in the kernel ABI header.         *|
|*                                                                            *|
|* Build note: point the compiler at the installed headers, e.g.             *|
|*   -I <kernel>/usr/include   (after `make headers` / `make headers_install`)*|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

#ifndef SCHED_HINT_WRAPPER_H
#define SCHED_HINT_WRAPPER_H

/* Single source of truth: struct + enums + magic/version + bloom params. */
#include <linux/sched_hint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PR_SET_SCHED_HINT_OFFSET
#define PR_SET_SCHED_HINT_OFFSET        83
#endif

/* ELF section that carries the TLS instance (linker/pass convention).       */
#define SCHED_HINT_SECTION "__sched_hint"

/* TLS storage-class specifier — portable across C11/C++11/GCC/Clang/MSVC.   */
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

/*
 * Access note: the symbol the LLVM pass emits is "__sched_hint.data" (with a
 * period), which is not a legal C identifier. To read it directly, alias it
 * with inline asm, e.g.:
 *   extern SCHED_HINT_TLS struct sched_hint __sched_hint_data
 *       __asm__("__sched_hint.data");
 * (see test/reader_dynamic.h). In kernel space use current->sched_hint_kaddr.
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SCHED_HINT_WRAPPER_H */
