/*
 * Copyright 2026 Spare Cores
 * Licensed under Mozilla Public License 2.0
 */

#ifndef MEMBENCH_LATENCY_H
#define MEMBENCH_LATENCY_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "config.h"
#include "membench.h"
#include "platform.h"
#include "utils.h"

/* Optional library: NUMA support (Linux only) */
#ifdef USE_NUMA
#include <numa.h>
#include <numaif.h>
#endif

/* Prevent compiler from optimizing away operations */
static volatile uint64_t g_latency_sink = 0;

/* Maximum buffer size for latency test.
 * Must exceed largest L3 caches to measure true DRAM latency.
 * AMD EPYC 9754 (Genoa-X) has 1.1GB L3 cache, so we need > 1.1GB.
 * 2GB should cover any current processor. */
#define MAX_LATENCY_SIZE (2UL * 1024 * 1024 * 1024)  /* 2 GB */

/*
 * Memory latency test using pointer chasing
 *
 * This implementation is based on ram_bench by Emil Ernerfeldt:
 *   https://github.com/emilk/ram_bench
 *
 * Recommended by Alex Miller.
 *
 * Uses a linked list traversal approach where each node contains a payload
 * and a pointer to the next node. Nodes are allocated contiguously but
 * linked in random order to defeat hardware prefetchers.
 *
 * Key insight from ram_bench: random memory access cost is O(√N) due to
 * cache hierarchy (L1, L2, L3, RAM) and the fundamental limit that memory
 * within distance r from CPU is bounded by r² (Bekenstein bound).
 */

/* Node structure for linked list traversal (16 bytes like ram_bench)
 * The payload prevents compiler from optimizing away the traversal
 * and makes the structure cache-line realistic */
typedef struct LatencyNode LatencyNode;
struct LatencyNode {
    uint64_t payload;      /* Dummy data for realistic cache behavior */
    LatencyNode *next;     /* Pointer to next node in chain */
};

/* Statistical parameters for latency measurement */
#define LATENCY_MIN_SAMPLES 7        /* Minimum samples for statistical validity */
#define LATENCY_MAX_SAMPLES 21       /* Maximum samples (enough for robust statistics) */
#define LATENCY_TARGET_CV 0.05       /* Target coefficient of variation (5%) */

/* Allocate memory for latency chain with NUMA awareness and huge page support
 * Uses mmap with optional huge pages to reduce TLB overhead for large buffers */
static inline LatencyNode* alloc_latency_memory(const platform_info_t *pi, const bench_config_t *cfg, size_t num_nodes, size_t *alloc_size) {
    (void)pi;  /* only used with USE_NUMA */
    size_t size = num_nodes * sizeof(LatencyNode);
    *alloc_size = size;

    LatencyNode *memory = MAP_FAILED;
    int try_hugepages = cfg->use_hugepages && (size >= get_huge_page_threshold());

    if (try_hugepages) {
        /* Round up size to huge page boundary */
        size_t hp_size = get_huge_page_size();
        size_t aligned_size = (size + hp_size - 1) & ~(hp_size - 1);
        *alloc_size = aligned_size;

#ifdef MAP_HUGETLB
        /* Try explicit huge pages first */
        memory = (LatencyNode *)mmap(NULL, aligned_size, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (memory != MAP_FAILED) {
            if (cfg->verbose >= 2) {
                fprintf(stderr, "  Latency: allocated %zu bytes using explicit 2MB huge pages\n", aligned_size);
            }
        }
#endif

        /* Fall back to THP (Transparent Huge Pages) */
        if (memory == MAP_FAILED) {
            memory = (LatencyNode *)mmap(NULL, size, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (memory != MAP_FAILED) {
#ifdef MADV_HUGEPAGE
                if (madvise(memory, size, MADV_HUGEPAGE) == 0) {
                    if (cfg->verbose >= 2) {
                        fprintf(stderr, "  Latency: allocated %zu bytes with THP (transparent huge pages)\n", size);
                    }
                } else if (cfg->verbose >= 2) {
                    fprintf(stderr, "  Latency: allocated %zu bytes (THP hint failed)\n", size);
                }
#endif
                *alloc_size = size;  /* Reset to actual size for THP */
            }
        }
    }

    /* Regular allocation if huge pages disabled or failed */
    if (memory == MAP_FAILED) {
        memory = (LatencyNode *)mmap(NULL, size, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        *alloc_size = size;
    }

    if (memory == MAP_FAILED) return NULL;

#ifdef USE_NUMA
    /* Bind memory to NUMA node 0 (where CPU 0 is) for consistent latency measurement */
    if (numa_available() >= 0 && pi->numa_nodes > 1) {
        int node = numa_node_of_cpu(0);
        if (node >= 0) {
            unsigned long nodemask = 1UL << node;
            mbind(memory, *alloc_size, MPOL_BIND, &nodemask, pi->numa_nodes + 1, MPOL_MF_MOVE);
            if (cfg->verbose >= 2) {
                fprintf(stderr, "  Latency memory bound to NUMA node %d\n", node);
            }
        }
    }
#endif

    return memory;
}

/* Free latency chain memory allocated via mmap */
static inline void free_latency_memory(LatencyNode *memory, size_t alloc_size) {
    if (memory && alloc_size > 0) {
        munmap(memory, alloc_size);
    }
}

/* Initialize linked list with random traversal order
 * Memory is contiguous (good for allocation) but traversal is random
 * (defeats prefetcher, measures true memory latency)
 * Returns: start node pointer; caller must track alloc_size for freeing */
static inline LatencyNode* init_latency_chain(const platform_info_t *pi, const bench_config_t *cfg, size_t num_nodes, size_t *alloc_size) {
    if (num_nodes < 2) return NULL;

    /* Allocate contiguous memory for all nodes using NUMA-aware allocation */
    LatencyNode *memory = alloc_latency_memory(pi, cfg, num_nodes, alloc_size);
    if (!memory) return NULL;

    for (size_t i = 0; i < num_nodes; i++) {
        memory[i].payload = i;
        memory[i].next = &memory[i];
    }
    for (size_t i = num_nodes - 1; i > 0; i--) {
        size_t j = (size_t)rand() % i;
        LatencyNode *tmp = memory[i].next;
        memory[i].next = memory[j].next;
        memory[j].next = tmp;
    }
    LatencyNode *start = memory;

    return start;
}

/* Free latency chain - need base address and size */
static inline void free_latency_chain(LatencyNode *start, size_t num_nodes, size_t alloc_size) {
    if (!start || num_nodes == 0) return;

    /* Find the lowest address in the chain (that's where mmap'd block starts) */
    LatencyNode *min_addr = start;
    LatencyNode *node = start->next;
    size_t visited = 1;
    while (node != start && visited < num_nodes) {
        if (node < min_addr) min_addr = node;
        node = node->next;
        visited++;
    }

    free_latency_memory(min_addr, alloc_size);
}

/* Chase through linked list - each load depends on previous
 * Returns final node pointer to prevent optimization */
static inline __attribute__((always_inline))
LatencyNode* chase_latency_chain(LatencyNode *start, size_t count) {
    LatencyNode *node = start;
    volatile uint64_t sink = 0;  /* Prevent optimization */

    /* Unroll 8x to reduce loop overhead while maintaining dependency chain */
    size_t i = count;
    while (i >= 8) {
        sink += node->payload; node = node->next;
        sink += node->payload; node = node->next;
        sink += node->payload; node = node->next;
        sink += node->payload; node = node->next;
        sink += node->payload; node = node->next;
        sink += node->payload; node = node->next;
        sink += node->payload; node = node->next;
        sink += node->payload; node = node->next;
        i -= 8;
    }
    while (i > 0) {
        sink += node->payload;
        node = node->next;
        i--;
    }

    g_latency_sink += sink;  /* Store to global to prevent optimization */
    return node;
}

/* Result structure for latency measurement with statistics */
typedef struct {
    double median_ns;      /* Median latency (robust to outliers) */
    double mean_ns;        /* Mean latency */
    double stddev_ns;      /* Standard deviation */
    double cv;             /* Coefficient of variation (stddev/mean) */
    int num_samples;       /* Number of samples collected */
    size_t total_accesses; /* Total node accesses performed */
} latency_stats_t;

/* Target time per sample in seconds - long enough for timer precision,
 * short enough for reasonable total measurement time */
#define LATENCY_TARGET_SAMPLE_TIME 0.1  /* 100ms per sample */
#define LATENCY_MIN_SAMPLE_TIME 0.01    /* 10ms minimum for timer precision */
#define LATENCY_CALIBRATION_ACCESSES (1UL << 20)

/* Measure latency with statistical validity
 *
 * Strategy:
 * 1. Create random linked list covering the buffer size
 * 2. Warmup pass over the list
 * 3. Calibration run to estimate latency and size the samples
 * 4. Collect multiple independent time samples
 * 5. Continue until CV < target or max samples reached
 * 6. Report median (robust to outliers) and statistics
 *
 * Returns statistically valid latency measurement
 */
static inline latency_stats_t measure_latency_stats(const platform_info_t *pi, const bench_config_t *cfg, size_t buffer_size) {
    latency_stats_t stats = {0};

    /* Pin thread to CPU 0 for consistent latency measurement.
     * This prevents OS scheduler from migrating the thread during measurement,
     * which would cause inconsistent results due to cache effects and NUMA. */
    thread_affinity_t affinity;
    pin_thread_to_cpu0(&affinity, cfg->verbose);

    /* Calculate number of nodes that fit in buffer */
    size_t num_nodes = buffer_size / sizeof(LatencyNode);
    if (num_nodes < 64) num_nodes = 64;  /* Minimum for meaningful measurement */

    /* Initialize chain with NUMA-aware allocation */
    size_t alloc_size = 0;
    LatencyNode *start = init_latency_chain(pi, cfg, num_nodes, &alloc_size);
    if (!start) {
        fprintf(stderr, "Failed to allocate %zu bytes for latency test\n",
                num_nodes * sizeof(LatencyNode));
        restore_thread_affinity(&affinity);
        return stats;
    }

    size_t calibration_accesses = num_nodes < LATENCY_CALIBRATION_ACCESSES ? num_nodes : LATENCY_CALIBRATION_ACCESSES;

    /* Warmup: prime caches and stabilize CPU */
    LatencyNode *node = chase_latency_chain(start, calibration_accesses);

    /* Calibration: time a short run to estimate latency */
    double cal_start = get_time();
    node = chase_latency_chain(node, calibration_accesses);
    double cal_elapsed = get_time() - cal_start;

    double estimated_latency_s = cal_elapsed / calibration_accesses;
    size_t accesses_per_sample = calibration_accesses;
    if (estimated_latency_s > 0) {
        double target_accesses = LATENCY_TARGET_SAMPLE_TIME / estimated_latency_s;
        if (target_accesses > accesses_per_sample) accesses_per_sample = (size_t)target_accesses;
    }

    /* Sample collection */
    double samples[LATENCY_MAX_SAMPLES];
    int num_samples = 0;
    size_t total_accesses = 0;

    /* Collect samples until statistically valid or max reached */
    while (num_samples < LATENCY_MAX_SAMPLES) {
        size_t accesses_this_sample = accesses_per_sample;

        /* Time this sample */
        double start_time = get_time();
        node = chase_latency_chain(node, accesses_this_sample);
        double end_time = get_time();

        double elapsed = end_time - start_time;
        double latency_ns = (elapsed * 1e9) / accesses_this_sample;

        samples[num_samples++] = latency_ns;
        total_accesses += accesses_this_sample;

        /* Check if we have enough samples and they're stable */
        if (num_samples >= LATENCY_MIN_SAMPLES) {
            double mean = calculate_mean(samples, num_samples);
            double stddev = calculate_stddev(samples, num_samples, mean);
            double cv = (mean > 0) ? (stddev / mean) : 1.0;

            /* Stop if coefficient of variation is acceptable */
            if (cv < LATENCY_TARGET_CV) {
                break;
            }
        }
    }

    /* Calculate final statistics */
    double mean = calculate_mean(samples, num_samples);
    double stddev = calculate_stddev(samples, num_samples, mean);

    /* Sort for median calculation */
    qsort(samples, num_samples, sizeof(double), compare_double);
    double median = calculate_median(samples, num_samples);

    /* Populate result */
    stats.median_ns = median;
    stats.mean_ns = mean;
    stats.stddev_ns = stddev;
    stats.cv = (mean > 0) ? (stddev / mean) : 0;
    stats.num_samples = num_samples;
    stats.total_accesses = total_accesses;

    /* Cleanup */
    free_latency_chain(start, num_nodes, alloc_size);
    restore_thread_affinity(&affinity);

    return stats;
}

#endif /* MEMBENCH_LATENCY_H */
