/*
 * bench/atomic_contention.c — Benchmark to validate atomic_dense scheduling
 *
 * This benchmark creates multiple threads that contend on shared atomic
 * variables. It verifies whether the scheduler co-locates threads on the
 * same physical core (different SMT siblings) to maximize cache coherency.
 *
 * Build:
 *   clang -O2 bench/atomic_contention.c -pthread -o /tmp/atomic_contention
 *
 * Or with instrumentation (requires prior compilation of pass):
 *   clang -O2 -fpass-plugin=build/pass/libSchedTagPass.so \
 *         bench/atomic_contention.c -pthread -o /tmp/atomic_contention_tagged
 *
 * Run:
 *   /tmp/atomic_contention [num_threads] [duration_ms]
 *   # Example: 4 threads, 100ms duration
 *   /tmp/atomic_contention 4 100
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <string.h>
#include <sys/syscall.h>

/*==========================================================================*/
/* Configuration                                                             */
/*==========================================================================*/

#define MAX_THREADS 256
#define MAX_CPUS 256
#define SAMPLE_INTERVAL_US 100  /* Sample CPU affinity every 100us */

/*==========================================================================*/
/* Shared atomic counters — the contention point                            */
/*==========================================================================*/

static _Atomic uint64_t counter_a = 0;
static _Atomic uint64_t counter_b = 0;
static _Atomic uint64_t global_sum = 0;

/*==========================================================================*/
/* Per-thread state                                                          */
/*==========================================================================*/

typedef struct {
    int thread_id;
    uint64_t iterations;
    uint64_t local_sum;
    
    /* CPU affinity samples */
    int *cpu_samples;
    size_t num_samples;
    size_t samples_capacity;
} thread_state_t;

static thread_state_t thread_states[MAX_THREADS];
static int num_threads = 2;
static int duration_ms = 50;  /* Default: 50ms (> 4ms timeslice) */
static volatile int stop_flag = 0;

/*==========================================================================*/
/* CPU topology detection                                                    */
/*==========================================================================*/

/* Maps logical CPU ID to physical core ID */
static int cpu_to_core[MAX_CPUS];
static int num_cpus = 0;

/* Read physical core ID from sysfs */
static int get_physical_core_id(int cpu) {
    char path[256];
    snprintf(path, sizeof(path), 
             "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
    
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    
    int core_id = -1;
    fscanf(f, "%d", &core_id);
    fclose(f);
    return core_id;
}

static void detect_cpu_topology(void) {
    num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cpus > MAX_CPUS) num_cpus = MAX_CPUS;
    
    printf("Detecting CPU topology...\n");
    for (int cpu = 0; cpu < num_cpus; cpu++) {
        cpu_to_core[cpu] = get_physical_core_id(cpu);
        if (cpu_to_core[cpu] >= 0) {
            printf("  CPU %d → Physical Core %d\n", cpu, cpu_to_core[cpu]);
        }
    }
    printf("\n");
}

/* Check if two CPUs are SMT siblings (same physical core) */
static int are_smt_siblings(int cpu1, int cpu2) {
    if (cpu1 < 0 || cpu1 >= num_cpus || cpu2 < 0 || cpu2 >= num_cpus)
        return 0;
    return cpu_to_core[cpu1] == cpu_to_core[cpu2] && cpu_to_core[cpu1] >= 0;
}

/*==========================================================================*/
/* Worker thread                                                             */
/*==========================================================================*/

static void record_cpu_sample(thread_state_t *state, int cpu) {
    if (state->num_samples >= state->samples_capacity) {
        state->samples_capacity = state->samples_capacity ? 
                                  state->samples_capacity * 2 : 1024;
        int *new_samples = (int *)realloc(state->cpu_samples, 
                                          state->samples_capacity * sizeof(int));
        if (!new_samples) return;  /* Out of memory, skip this sample */
        state->cpu_samples = new_samples;
    }
    state->cpu_samples[state->num_samples++] = cpu;
}

static void* worker_thread(void *arg) {
    thread_state_t *state = (thread_state_t*)arg;
    
    /* Pin to all CPUs (let scheduler decide) */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int i = 0; i < num_cpus; i++) {
        CPU_SET(i, &cpuset);
    }
    sched_setaffinity(0, sizeof(cpuset), &cpuset);
    
    uint64_t iters = 0;
    uint64_t sum = 0;
    struct timespec last_sample = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &last_sample);
    
    /* Record initial CPU */
    int cpu = sched_getcpu();
    record_cpu_sample(state, cpu);
    
    while (!stop_flag) {
        /* Atomic-dense loop: repeatedly increment shared counters */
        for (int batch = 0; batch < 100; batch++) {
            /* Atomic operations on shared variables */
            uint64_t a = atomic_fetch_add_explicit(&counter_a, 1, 
                                                   memory_order_relaxed);
            uint64_t b = atomic_fetch_add_explicit(&counter_b, 1, 
                                                   memory_order_relaxed);
            sum += a + b;
            iters++;
        }
        
        /* Periodically sample CPU affinity */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t elapsed_us = (now.tv_sec - last_sample.tv_sec) * 1000000ULL +
                             (now.tv_nsec - last_sample.tv_nsec) / 1000;
        
        if (elapsed_us >= SAMPLE_INTERVAL_US) {
            cpu = sched_getcpu();
            record_cpu_sample(state, cpu);
            last_sample = now;
        }
    }
    
    state->iterations = iters;
    state->local_sum = sum;
    atomic_fetch_add(&global_sum, sum);
    
    return NULL;
}

/*==========================================================================*/
/* Analysis                                                                  */
/*==========================================================================*/

static void analyze_cpu_affinity(void) {
    printf("=== CPU Affinity Analysis ===\n\n");
    
    /* Count co-location events */
    uint64_t total_sample_pairs = 0;
    uint64_t smt_sibling_pairs = 0;
    uint64_t same_cpu_pairs = 0;
    
    /* For each pair of threads, check how often they run on SMT siblings */
    for (int i = 0; i < num_threads; i++) {
        for (int j = i + 1; j < num_threads; j++) {
            thread_state_t *t1 = &thread_states[i];
            thread_state_t *t2 = &thread_states[j];
            
            /* Use the minimum sample count */
            size_t min_samples = t1->num_samples < t2->num_samples ? 
                                 t1->num_samples : t2->num_samples;
            
            int same_cpu_count = 0;
            int smt_sibling_count = 0;
            
            for (size_t s = 0; s < min_samples; s++) {
                int cpu1 = t1->cpu_samples[s];
                int cpu2 = t2->cpu_samples[s];
                
                if (cpu1 == cpu2) {
                    same_cpu_count++;
                } else if (are_smt_siblings(cpu1, cpu2)) {
                    smt_sibling_count++;
                }
            }
            
            total_sample_pairs += min_samples;
            same_cpu_pairs += same_cpu_count;
            smt_sibling_pairs += smt_sibling_count;
            
            printf("Thread %d ↔ Thread %d:\n", i, j);
            printf("  Samples:           %zu\n", min_samples);
            printf("  Same CPU:          %d (%.2f%%)\n", 
                   same_cpu_count, 
                   100.0 * same_cpu_count / min_samples);
            printf("  SMT siblings:      %d (%.2f%%)\n", 
                   smt_sibling_count,
                   100.0 * smt_sibling_count / min_samples);
            printf("  Co-located total:  %d (%.2f%%)\n\n",
                   same_cpu_count + smt_sibling_count,
                   100.0 * (same_cpu_count + smt_sibling_count) / min_samples);
        }
    }
    
    printf("=== Overall Statistics ===\n");
    printf("Total sample pairs:    %lu\n", total_sample_pairs);
    printf("Same CPU pairs:        %lu (%.2f%%)\n", 
           same_cpu_pairs, 100.0 * same_cpu_pairs / total_sample_pairs);
    printf("SMT sibling pairs:     %lu (%.2f%%)\n", 
           smt_sibling_pairs, 100.0 * smt_sibling_pairs / total_sample_pairs);
    printf("Co-located pairs:      %lu (%.2f%%)\n\n",
           same_cpu_pairs + smt_sibling_pairs,
           100.0 * (same_cpu_pairs + smt_sibling_pairs) / total_sample_pairs);
    
    if (smt_sibling_pairs > total_sample_pairs / 20) {  /* > 5% */
        printf("✓ GOOD: High SMT co-location detected!\n");
        printf("  The scheduler is placing threads with atomic contention\n");
        printf("  on the same physical core (SMT siblings).\n");
    } else {
        printf("✗ POOR: Low SMT co-location detected.\n");
        printf("  Threads are scattered across different physical cores.\n");
        printf("  Expected behavior: scheduler should co-locate atomic-dense\n");
        printf("  threads on SMT siblings to improve cache coherency.\n");
    }
    printf("\n");
}

static void print_per_thread_stats(void) {
    printf("=== Per-Thread Statistics ===\n");
    for (int i = 0; i < num_threads; i++) {
        thread_state_t *t = &thread_states[i];
        printf("Thread %d:\n", i);
        printf("  Iterations:  %lu\n", t->iterations);
        printf("  Local sum:   %lu\n", t->local_sum);
        printf("  CPU samples: %zu\n", t->num_samples);
        
        /* Show CPU distribution */
        int cpu_counts[MAX_CPUS] = {0};
        for (size_t s = 0; s < t->num_samples; s++) {
            int cpu = t->cpu_samples[s];
            if (cpu >= 0 && cpu < MAX_CPUS) {
                cpu_counts[cpu]++;
            }
        }
        
        printf("  CPU distribution:\n");
        for (int cpu = 0; cpu < num_cpus; cpu++) {
            if (cpu_counts[cpu] > 0) {
                printf("    CPU %2d (Core %2d): %5d samples (%.1f%%)\n",
                       cpu, cpu_to_core[cpu], cpu_counts[cpu],
                       100.0 * cpu_counts[cpu] / t->num_samples);
            }
        }
        printf("\n");
    }
}

/*==========================================================================*/
/* Main                                                                      */
/*==========================================================================*/

int main(int argc, char *argv[]) {
    if (argc > 1) {
        num_threads = atoi(argv[1]);
        if (num_threads < 2 || num_threads > MAX_THREADS) {
            fprintf(stderr, "Error: num_threads must be in [2, %d]\n", 
                    MAX_THREADS);
            return 1;
        }
    }
    
    if (argc > 2) {
        duration_ms = atoi(argv[2]);
        if (duration_ms < 5 || duration_ms > 10000) {
            fprintf(stderr, "Error: duration_ms must be in [5, 10000]\n");
            return 1;
        }
    }
    
    printf("=== Atomic Contention Benchmark ===\n");
    printf("Threads:         %d\n", num_threads);
    printf("Duration:        %d ms\n", duration_ms);
    printf("Sample interval: %d us\n", SAMPLE_INTERVAL_US);
    printf("\n");
    
    detect_cpu_topology();
    
    /* Initialize thread states */
    memset(thread_states, 0, sizeof(thread_states));
    for (int i = 0; i < num_threads; i++) {
        thread_states[i].thread_id = i;
    }
    
    /* Create worker threads */
    pthread_t threads[MAX_THREADS];
    printf("Starting %d worker threads...\n", num_threads);
    
    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], NULL, worker_thread, &thread_states[i]);
    }
    
    /* Let threads run for specified duration */
    usleep(duration_ms * 1000);
    
    /* Signal stop */
    stop_flag = 1;
    
    /* Wait for threads to finish */
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    
    double elapsed_sec = (end_time.tv_sec - start_time.tv_sec) +
                         (end_time.tv_nsec - start_time.tv_nsec) / 1e9;
    
    printf("Completed in %.3f seconds\n\n", elapsed_sec);
    
    /* Compute total iterations */
    uint64_t total_iters = 0;
    for (int i = 0; i < num_threads; i++) {
        total_iters += thread_states[i].iterations;
    }
    
    printf("=== Performance Summary ===\n");
    printf("Total iterations:       %lu\n", total_iters);
    printf("Throughput:             %.2f M ops/sec\n", 
           total_iters / elapsed_sec / 1e6);
    printf("Counter A:              %lu\n", 
           atomic_load(&counter_a));
    printf("Counter B:              %lu\n", 
           atomic_load(&counter_b));
    printf("Global sum:             %lu\n", 
           atomic_load(&global_sum));
    printf("\n");
    
    print_per_thread_stats();
    analyze_cpu_affinity();
    
    /* Cleanup */
    for (int i = 0; i < num_threads; i++) {
        free(thread_states[i].cpu_samples);
    }
    
    return 0;
}
