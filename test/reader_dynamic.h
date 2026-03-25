/*
 * test/reader_dynamic.h — Access TLS variable with illegal C identifier name.
 *
 * The sched_hint TLS variable has symbol name "__sched_hint.data", which contains
 * a period and cannot be directly referenced in normal C/C++ source code.
 *
 * We use inline assembly to access it directly.
 */

#ifndef READER_DYNAMIC_H
#define READER_DYNAMIC_H

#include <stdio.h>
#include "../include/sched_hint.h"

/*
 * Access the __sched_hint.data TLS variable using inline assembly.
 * 
 * We can't write: extern __thread struct sched_hint __sched_hint.data;
 * because the period makes it an invalid C identifier.
 * 
 * Instead, we use asm to get its address.
 */
static inline struct sched_hint *get_sched_hint_data(void) {
    struct sched_hint *ptr;
    
#if defined(__x86_64__)
    /* Use mov from %fs:offset to access Initial-Exec TLS model */
    __asm__ (
        "mov __sched_hint.data@GOTTPOFF(%%rip), %0\n\t"
        "add %%fs:0, %0"
        : "=r" (ptr)
    );
#elif defined(__aarch64__)  
    extern __thread struct sched_hint __sched_hint_data __asm__("__sched_hint.data");
    ptr = &__sched_hint_data;
#else
    #error "Unsupported architecture - add TLS access code"
#endif
    
    return ptr;
}

/* Backward compatibility: findfunction with debug output */
static inline struct sched_hint *find_sched_hint_var(void) {
    struct sched_hint *hint = get_sched_hint_data();
    
    if (hint && hint->magic == SCHED_HINT_MAGIC) {
        fprintf(stderr, "[reader_dynamic] Accessed __sched_hint.data at %p\n", (void*)hint);
        fprintf(stderr, "[reader_dynamic] Magic: 0x%08x ✓\n", hint->magic);
    } else {
        fprintf(stderr, "[reader_dynamic] ERROR: Invalid access or bad magic\n");
    }
    
    return hint;
}

#endif /* READER_DYNAMIC_H */
