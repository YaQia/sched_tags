/*
 * test/critical_section.c — Critical section and CPU work functions
 *
 * These functions are designed to be instrumented by SchedTagPass with
 * the `unshared` tag to help the scheduler prevent priority inversion.
 *
 * When compiled with instrumentation:
 *   - critical_work() will have unshared=1 set between lock and unlock
 *   - unshared_magic will contain bloom filter hash of the mutex address
 *
 * This allows the scheduler to:
 *   1. Detect when a thread holds an exclusive resource
 *   2. Identify which resource is being held (via bloom filter)
 *   3. Prevent preemption by medium-priority threads when a high-priority
 *      thread is waiting for the same resource
 */

#include <pthread.h>
#include <stdint.h>

/*
 * critical_work - Performs work while holding a mutex
 *
 * @mutex: Pointer to the mutex to acquire
 * @iterations: Number of work iterations to perform
 *
 * This function:
 * 1. Acquires the mutex
 * 2. Performs CPU-intensive work (simulating critical section operations)
 * 3. Releases the mutex
 *
 * When instrumented with unshared tag:
 * - unshared=1 is SET before pthread_mutex_lock call
 * - unshared=0 is SET after pthread_mutex_unlock call
 * - unshared_magic contains bloom filter of &mutex
 */
void critical_work(pthread_mutex_t *mutex, int iterations) {
    /* 
     * IMPORTANT: The unshared tag will be inserted around these calls.
     * 
     * According to AGENTS.md:
     * - start query matches: @critical_work/call[func=pthread_mutex_lock]
     * - end query matches: @critical_work/call[func=pthread_mutex_unlock]
     * 
     * The instrumentation will insert:
     *   store i8 1, ptr @__sched_hint.data.unshared  ; SET before lock
     *   call pthread_mutex_lock(...)
     *   ... critical section ...
     *   call pthread_mutex_unlock(...)
     *   store i8 0, ptr @__sched_hint.data.unshared  ; CLR after unlock
     */
    
    pthread_mutex_lock(mutex);
    
    /* Simulate work inside critical section */
    volatile uint64_t sum = 0;
    for (int i = 0; i < iterations; i++) {
        /* Mix of operations to prevent over-optimization */
        sum += (uint64_t)i * 31;
        sum ^= (sum >> 17);
        sum += (sum << 5);
        
        /* Occasional memory barrier to simulate real work */
        if ((i & 0xFFFF) == 0) {
            __asm__ volatile("" ::: "memory");
        }
    }
    
    /* Use sum to prevent dead code elimination */
    *(volatile uint64_t *)&sum;
    
    pthread_mutex_unlock(mutex);
}

/*
 * cpu_intensive_work - Pure CPU work without any locking
 *
 * @iterations: Number of work iterations to perform
 *
 * This function simulates a medium-priority thread doing CPU-intensive
 * work. Without unshared tag protection, this would preempt a low-priority
 * thread that holds a mutex needed by a high-priority thread.
 */
void cpu_intensive_work(int iterations) {
    volatile uint64_t sum = 0;
    
    for (int i = 0; i < iterations; i++) {
        /* Compute-intensive operations */
        sum += (uint64_t)i * 17;
        sum ^= (sum >> 13);
        sum *= 0x5851F42D4C957F2DULL;  /* Some multiplication for variety */
        sum ^= (sum >> 29);
        
        /* Memory barrier every so often */
        if ((i & 0x3FFFF) == 0) {
            __asm__ volatile("" ::: "memory");
        }
    }
    
    /* Use sum to prevent dead code elimination */
    *(volatile uint64_t *)&sum;
}
