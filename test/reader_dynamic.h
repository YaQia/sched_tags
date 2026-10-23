/*
 * test/reader_dynamic.h — Access the current thread's sched_hint slot.
 *
 * The kernel owns the slot and maps it VM_SHARED into the process; the library
 * caches the slot's user address in the shared TLS pointer @__sched_hint_ptr.
 * sched_tag.h's internal __sched_hint_slot() returns that pointer, registering
 * the thread on first use.
 */

#ifndef READER_DYNAMIC_H
#define READER_DYNAMIC_H

#include <stdio.h>
#include "../include/sched_tag.h"

/* The current thread's live slot, or NULL if the mechanism is unavailable. */
static inline struct sched_hint *get_sched_hint_data(void) {
    return __sched_hint_slot();
}

/* Keep the historical name used by the reader; just validate and report. */
static inline struct sched_hint *find_sched_hint_var(void) {
    struct sched_hint *hint = __sched_hint_slot();

    if (hint && hint->magic == SCHED_HINT_MAGIC) {
        fprintf(stderr, "[reader_dynamic] slot at %p, magic 0x%08x OK\n",
                (void *)hint, hint->magic);
    } else {
        fprintf(stderr, "[reader_dynamic] ERROR: no valid slot\n");
    }

    return hint;
}

#endif /* READER_DYNAMIC_H */
