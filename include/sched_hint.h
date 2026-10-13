/*===-- sched_hint.h - userspace glue over the shared sched_hint ABI ------===*\
|*                                                                            *|
|* The sched_hint ABI (struct sched_hint, all SCHED_* enums, magic/version,   *|
|* and bloom parameters) is defined ONCE in the kernel uapi header            *|
|* <linux/sched_hint.h>, installed via `make headers_install`. This file is   *|
|* a thin userspace wrapper that pulls in that single source of truth and     *|
|* adds the userspace-only glue (TLS specifier and a prctl-number fallback)   *|
|* that has no place in the kernel ABI header.                                *|
|*                                                                            *|
|* Build note: point the compiler at the installed headers, e.g.             *|
|*   -I <kernel>/usr/include   (after `make headers` / `make headers_install`)*|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

#ifndef SCHED_HINT_WRAPPER_H
#define SCHED_HINT_WRAPPER_H

/* Single source of truth: struct + enums + magic/version + bloom params. */
#include <linux/sched_hint.h>
#include <linux/prctl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Registration prctl. Defined by the kernel uapi <linux/prctl.h> when the
 * headers come from a sched_hint kernel; the fallback covers stock headers. */
#ifndef PR_SET_SCHED_HINT
#define PR_SET_SCHED_HINT              83
#endif

/*
 * Slot access: prctl(PR_SET_SCHED_HINT, &word) backfills this thread's slot
 * address (a 64-byte block of kernel-owned pages mapped into the process).
 * There is no userspace sched_hint object to reach for — readers call
 * sched_hint_current() (sched_tag.h), writers go through sched_tag_set().
 */

#ifdef __cplusplus
}
#endif

#endif /* SCHED_HINT_WRAPPER_H */
