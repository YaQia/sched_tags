/*
 * test/priority_inversion_test.c — Priority inversion prevention test (CFS/FAIR)
 *
 * This test demonstrates how the `unshared` scheduling tag helps the CFS
 * scheduler prevent priority inversion WITHOUT modifying source code.
 *
 * In CFS, "priority" is controlled by nice values:
 *   - nice -20: highest priority (more CPU time)
 *   - nice +19: lowest priority (less CPU time)
 *
 * Classic Priority Inversion Scenario in CFS:
 * ===========================================
 *
 *   Time →
 *   
 *   High (nice 0):   blocked on mutex─────────────────┬─runs──┐
 *                                                     │       │
 *   Medium (nice 10):        ┌─gets CPU time──────────┴───────┤
 *                            │ (higher weight than Low)       │
 *   Low (nice 19):   ┌─lock──┼─starved by Medium──────────────┴─unlock─┐
 *                    │       │                                         │
 *                    └───────┴─────────────────────────────────────────┘
 *
 * The problem: CFS gives Medium more CPU time than Low (due to nice values),
 * so Low takes longer to complete its critical section, and High waits longer.
 *
 * Test Design:
 * ============
 * We measure LOW's critical section time in two scenarios:
 * 1. Without interference (baseline)
 * 2. With Medium threads competing for CPU
 *
 * If priority inversion occurs, scenario 2 takes much longer than scenario 1.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#include "../include/sched_hint.h"

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

/* Nice values for CFS priority (lower = higher priority) 
 * Without root, can only increase nice (0-19). 
 */
#define NICE_LOW      19      /* Lowest priority (holds the lock) */
#define NICE_MEDIUM   5       /* Medium priority (CPU hogs) - lower nice = more aggressive */
#define NICE_HIGH     0       /* Highest priority */

/* Number of medium priority "CPU hog" threads - more threads = more pressure */
#define NUM_MEDIUM_THREADS  8

/* Work iterations for LOW's critical section */
#define LOW_WORK_ITERATIONS     50000000
#define HIGH_WORK_ITERATIONS    64

/* CPU to pin all threads to */
#define PIN_CPU  0

/*===========================================================================*/
/* Shared state                                                              */
/*===========================================================================*/

static pthread_mutex_t shared_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Timing measurements */
static struct timespec low_lock_acquired;
static struct timespec low_lock_released;
static struct timespec high_start_wait;
static struct timespec high_got_lock;

/* Synchronization */
static pthread_barrier_t start_barrier;
static volatile bool low_in_cs = false;
static volatile bool medium_should_stop = false;
static volatile int medium_running_count = 0;

/*===========================================================================*/
/* External functions (from critical_section.c, may be instrumented)         */
/*===========================================================================*/

extern void critical_work(pthread_mutex_t *mutex, int iterations);

/*===========================================================================*/
/* Utility functions                                                         */
/*===========================================================================*/

static inline int64_t timespec_diff_us(struct timespec *start, struct timespec *end) {
    return ((int64_t)(end->tv_sec - start->tv_sec) * 1000000LL) +
           ((int64_t)(end->tv_nsec - start->tv_nsec) / 1000LL);
}

static void set_nice(int nice_val) {
    pid_t tid = syscall(SYS_gettid);
    int current = getpriority(PRIO_PROCESS, tid);
    
    /* Can only increase nice without root */
    if (nice_val < current && geteuid() != 0) {
        /* Silently use current value - we'll adjust expectations */
        return;
    }
    
    if (setpriority(PRIO_PROCESS, tid, nice_val) != 0) {
        if (errno != EACCES && errno != EPERM) {
            perror("setpriority");
        }
    }
}

static void pin_to_cpu(int cpu) {
    if (cpu < 0) return;
    
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
        perror("pthread_setaffinity_np");
    }
}

static void print_thread_info(const char *name, int nice_val) {
    pid_t tid = syscall(SYS_gettid);
    int actual_nice = getpriority(PRIO_PROCESS, tid);
    printf("[%-4s] tid=%d, nice=%d (requested %d)\n", name, tid, actual_nice, nice_val);
}

/*===========================================================================*/
/* Thread functions                                                          */
/*===========================================================================*/

/*
 * Low priority thread:
 * - Has the lowest priority (highest nice value)
 * - Acquires the mutex and does work in critical section
 */
static void *low_priority_thread(void *arg) {
    (void)arg;
    
    pin_to_cpu(PIN_CPU);
    set_nice(NICE_LOW);
    print_thread_info("LOW", NICE_LOW);
    
    pthread_barrier_wait(&start_barrier);
    
    printf("[LOW ] Waiting for medium threads to start...\n");
    
    /* Wait until all medium threads are running to ensure maximum interference */
    while (medium_running_count < NUM_MEDIUM_THREADS) {
        usleep(100);
    }
    printf("[LOW ] All %d medium threads running, entering critical section\n", 
           medium_running_count);
    
    clock_gettime(CLOCK_MONOTONIC, &low_lock_acquired);
    low_in_cs = true;
    
    /* 
     * Call critical_work which:
     * 1. Acquires the mutex
     * 2. Does CPU-intensive work (this is where unshared tag applies)
     * 3. Releases the mutex
     * 
     * When instrumented, unshared=1 will be set before lock and cleared after unlock.
     */
    critical_work(&shared_mutex, LOW_WORK_ITERATIONS);
    
    low_in_cs = false;
    clock_gettime(CLOCK_MONOTONIC, &low_lock_released);
    
    int64_t cs_time = timespec_diff_us(&low_lock_acquired, &low_lock_released);
    printf("[LOW ] Exited critical section (took %ld us)\n", cs_time);
    
    return NULL;
}

/*
 * Medium priority thread (CPU hog):
 * - Has medium priority
 * - Continuously does CPU work to compete with Low for CPU time
 */
static void *medium_priority_thread(void *arg) {
    int id = (int)(intptr_t)arg;
    char name[8];
    snprintf(name, sizeof(name), "MED%d", id);
    
    pin_to_cpu(PIN_CPU);
    set_nice(NICE_MEDIUM);
    print_thread_info(name, NICE_MEDIUM);
    
    pthread_barrier_wait(&start_barrier);
    
    /* Small delay to let Low acquire the lock first */
    usleep(1000);
    
    __atomic_fetch_add(&medium_running_count, 1, __ATOMIC_SEQ_CST);
    
    /* Tight CPU loop until told to stop */
    volatile uint64_t sum = 0;
    uint64_t iterations = 0;
    
    while (!medium_should_stop) {
        /* Compute-intensive work */
        for (int i = 0; i < 10000; i++) {
            sum += i * 31;
            sum ^= (sum >> 17);
            sum += (sum << 5);
        }
        iterations++;
    }
    
    printf("[%-4s] Stopped (iterations: %lu)\n", name, iterations);
    return NULL;
}

/*
 * High priority thread:
 * - Has the highest priority
 * - Waits for Low to enter critical section, then tries to acquire the lock
 */
static void *high_priority_thread(void *arg) {
    (void)arg;
    
    pin_to_cpu(PIN_CPU);
    set_nice(NICE_HIGH);
    print_thread_info("HIGH", NICE_HIGH);
    
    pthread_barrier_wait(&start_barrier);
    
    /* Wait for Low to be in critical section */
    while (!low_in_cs) {
        usleep(100);
    }
    usleep(5000);  /* Extra 5ms to ensure Low is deep into the critical section */
    
    printf("[HIGH] Attempting to acquire mutex (Low IN CS, %d medium threads)\n",
           medium_running_count);
    
    clock_gettime(CLOCK_MONOTONIC, &high_start_wait);
    
    /*
     * Use the same instrumented path as Low thread so unshared is set before
     * pthread_mutex_lock in this thread as well.
     *
     * This is critical for validating scheduler behavior at the blocking
     * (quiescent) point: High must carry unshared/unshared_magic when it sleeps
     * on the mutex.
     */
    critical_work(&shared_mutex, HIGH_WORK_ITERATIONS);

    clock_gettime(CLOCK_MONOTONIC, &high_got_lock);
    
    /* Signal medium threads to stop */
    medium_should_stop = true;
    
    int64_t wait_time = timespec_diff_us(&high_start_wait, &high_got_lock);
    printf("[HIGH] Acquired mutex! (waited %ld us)\n", wait_time);
    
    return NULL;
}

/*===========================================================================*/
/* Baseline measurement (no interference)                                    */
/*===========================================================================*/

static int64_t measure_baseline(void) {
    printf("=== Measuring baseline (no interference) ===\n");
    
    struct timespec start, end;
    pthread_t low_thread, high_thread;
    
    pin_to_cpu(PIN_CPU);
    set_nice(NICE_LOW);  /* Same nice as Low thread */
    
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* Create threads */
    pthread_create(&low_thread, NULL, low_priority_thread, NULL);
    pthread_create(&high_thread, NULL, high_priority_thread, NULL);
    
    /* Wait for completion */
    pthread_join(high_thread, NULL);
    pthread_join(low_thread, NULL);
    // pthread_mutex_lock(&shared_mutex);
    // 
    // /* Same work as Low thread does in critical section */
    // volatile uint64_t sum = 0;
    // for (int i = 0; i < LOW_WORK_ITERATIONS + HIGH_WORK_ITERATIONS; i++) {
    //     sum += (uint64_t)i * 31;
    //     sum ^= (sum >> 17);
    //     sum += (sum << 5);
    //     if ((i & 0xFFFF) == 0) {
    //         __asm__ volatile("" ::: "memory");
    //     }
    // }
    // (void)sum;
    // 
    // pthread_mutex_unlock(&shared_mutex);
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    int64_t baseline = timespec_diff_us(&start, &end);
    printf("Baseline critical section time: %ld us\n\n", baseline);
    
    return baseline;
}

/*===========================================================================*/
/* Sched hint verification                                                   */
/*===========================================================================*/

static void check_sched_hint(void) {
    extern __thread struct sched_hint __sched_hint_data_weak 
        __asm__("__sched_hint.data") __attribute__((weak));
    
    struct sched_hint *hint = &__sched_hint_data_weak;
    
    printf("\n=== Sched Hint Status ===\n");
    if (hint && hint->magic == SCHED_HINT_MAGIC) {
        printf("TLS variable:     FOUND (instrumented build)\n");
        printf("  magic:          0x%08x\n", hint->magic);
        printf("  unshared:       %u\n", hint->unshared);
        printf("  unshared_magic: 0x%016lx\n", (unsigned long)hint->unshared_magic);
    } else {
        printf("TLS variable:     NOT FOUND (plain build)\n");
    }
}

/*===========================================================================*/
/* Main                                                                      */
/*===========================================================================*/

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    
    pthread_t low_thread, high_thread;
    pthread_t medium_threads[NUM_MEDIUM_THREADS];
    
    printf("================================================================\n");
    printf("Priority Inversion Test (CFS/FAIR scheduler)\n");
    printf("================================================================\n\n");
    
    printf("Configuration:\n");
    printf("  Nice values:      Low=+%d, Medium=+%d, High=+%d\n",
           NICE_LOW, NICE_MEDIUM, NICE_HIGH);
    printf("  Medium threads:   %d (CPU hogs)\n", NUM_MEDIUM_THREADS);
    printf("  Low work:         %d iterations\n", LOW_WORK_ITERATIONS);
    printf("  Pinned to CPU:    %d\n", PIN_CPU);
    printf("\n");
    
    /* Measure baseline first */
    int64_t baseline = measure_baseline();
    
    /* Reset state */
    low_in_cs = false;
    medium_should_stop = false;
    medium_running_count = 0;
    
    /* Initialize barrier for all threads */
    int total_threads = 1 + NUM_MEDIUM_THREADS + 1;
    pthread_barrier_init(&start_barrier, NULL, total_threads);
    
    printf("=== Starting contention test with %d threads ===\n\n", total_threads);
    
    /* Create threads */
    pthread_create(&low_thread, NULL, low_priority_thread, NULL);
    
    for (int i = 0; i < NUM_MEDIUM_THREADS; i++) {
        pthread_create(&medium_threads[i], NULL, medium_priority_thread, 
                       (void *)(intptr_t)i);
    }
    
    pthread_create(&high_thread, NULL, high_priority_thread, NULL);
    
    /* Wait for completion */
    pthread_join(high_thread, NULL);
    pthread_join(low_thread, NULL);
    
    for (int i = 0; i < NUM_MEDIUM_THREADS; i++) {
        pthread_join(medium_threads[i], NULL);
    }
    
    pthread_barrier_destroy(&start_barrier);
    
    /* Results */
    printf("\n");
    printf("================================================================\n");
    printf("Results\n");
    printf("================================================================\n\n");
    
    int64_t cs_time = timespec_diff_us(&low_lock_acquired, &low_lock_released);
    int64_t wait_time = timespec_diff_us(&high_start_wait, &high_got_lock);
    
    printf("Baseline (no interference):     %ld us\n", baseline);
    printf("With interference:              %ld us\n", cs_time);
    printf("High's wait time:               %ld us\n", wait_time);
    printf("\n");
    
    double slowdown = (double)cs_time / (double)baseline;
    
    printf("Slowdown factor:                %.2fx\n", slowdown);
    printf("\n");
    
    if (slowdown > 3.0) {
        printf(">>> SEVERE PRIORITY INVERSION <<<\n");
        printf("    Low's critical section took %.1fx longer due to Medium threads.\n", slowdown);
        printf("    High had to wait much longer than necessary.\n");
    } else if (slowdown > 1.5) {
        printf(">>> PRIORITY INVERSION DETECTED <<<\n");
        printf("    Low's critical section took %.1fx longer due to interference.\n", slowdown);
    } else {
        printf(">>> NO SIGNIFICANT INVERSION <<<\n");
        printf("    Critical section time is close to baseline.\n");
        printf("    Scheduler appears to be protecting Low's execution.\n");
    }
    
    check_sched_hint();
    
    printf("\n================================================================\n");
    
    return 0;
}
