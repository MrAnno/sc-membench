/*
 * sc-membench - Portable Memory Bandwidth and Latency Benchmark
 *
 * A multi-platform memory benchmark that:
 * - Works on Linux, macOS, FreeBSD, and other Unix-like systems
 * - Works on x86, arm64, and other architectures
 * - Measures read, write, and copy bandwidth using OpenMP
 * - Measures memory latency using pointer chasing
 * - Handles NUMA automatically (works on non-NUMA too)
 * - Sweeps through cache and memory sizes
 * - Finds optimal thread count for peak bandwidth
 * - Outputs CSV format for analysis
 *
 * Compile (recommended - use make for auto-detection):
 *   make              # Auto-detect available features
 *   make basic        # Minimal build, no optional dependencies
 *   make full         # All features (Linux: hwloc + numa + hugetlbfs)
 *
 * Usage:
 *   ./membench [options]
 *   ./membench -h   # Show help
 *
 * Copyright 2026 Spare Cores
 * Licensed under Mozilla Public License 2.0
 */

/* Enable GNU extensions on Linux for CPU affinity (must be before includes) */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <omp.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <math.h>

#include "platform.h"
#include "utils.h"

/* Optional library: NUMA support (Linux only) */
#ifdef USE_NUMA
#include <numa.h>
#include <numaif.h>
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define VERSION "1.2.2"

/* Target time per individual measurement (seconds) */
#define TARGET_TIME_PER_TEST 0.25

/* Minimum iterations per test (keep low for large buffers that take seconds per iteration) */
#define MIN_ITERATIONS 3

/* Maximum iterations per test */
#define MAX_ITERATIONS 10000000

/* Default total runtime target (seconds). 0 = unlimited */
#define DEFAULT_MAX_RUNTIME 0

/* Fixed RAM sizes for when we need to measure pure memory bandwidth */
#define RAM_SIZE_1 (64UL * 1024 * 1024)   /* 64 MB - definitely past any L3 */
#define RAM_SIZE_2 (256UL * 1024 * 1024)  /* 256 MB - more RAM data points */

/* ============================================================================
 * Types
 * ============================================================================ */

typedef enum {
    OP_READ,
    OP_WRITE,
    OP_COPY,
    OP_LATENCY   /* Memory latency test using pointer chasing */
} operation_t;

static const char* OP_NAMES[] = {"read", "write", "copy", "latency"};

typedef struct {
    size_t size;
    operation_t op;
    int threads;
    double bandwidth_mb_s;  /* For read/write/copy */
    double latency_ns;      /* For latency test (median) */
    double latency_mean_ns; /* For latency test (mean) */
    double latency_stddev_ns; /* For latency test (standard deviation) */
    double latency_cv;      /* Coefficient of variation (stddev/mean) */
    int latency_samples;    /* Number of samples for latency measurement */
    double elapsed_s;
    int iterations;
} result_t;

/* Summary statistics structure */
typedef struct {
    /* Peak bandwidth for large buffer sizes (RAM speed) */
    double peak_read_mb_s;
    double peak_write_mb_s;
    double peak_copy_mb_s;

    /* Best latency for large buffer sizes (RAM latency) */
    double best_latency_ns;

    /* Weighted average bandwidth (larger sizes weighted more) */
    double weighted_avg_read_mb_s;
    double weighted_avg_write_mb_s;
    double weighted_avg_copy_mb_s;

    /* Counts and weights for weighted average */
    double read_weight_sum;
    double write_weight_sum;
    double copy_weight_sum;
    double read_bw_weighted_sum;
    double write_bw_weighted_sum;
    double copy_bw_weighted_sum;

    /* Track the largest size tested for "RAM" results */
    size_t largest_size_tested;

    /* Count of measurements */
    int read_count;
    int write_count;
    int copy_count;
    int latency_count;
} summary_t;

static summary_t g_summary = {0};

/* ============================================================================
 * Global state
 * ============================================================================ */

static volatile int g_running = 1;
int g_verbose = 0;  /* 0=quiet, 1=summary, 2=detailed (shared with platform.c) */
static int g_full_sweep = 0;      /* If 1, test all sizes up to max; if 0, stop early when converged */
static size_t g_single_size = 0;  /* If > 0, test only this size (in bytes) */
static int g_human_readable = 0;  /* If 1, output human-readable format instead of CSV */
/* Number of times to run each benchmark, taking best result (like lmbench TRIES=11) */
#define DEFAULT_BENCHMARK_TRIES 3
static int g_benchmark_tries = DEFAULT_BENCHMARK_TRIES;

/* Thread count options:
 * g_explicit_threads > 0: use exactly that many threads
 * g_explicit_threads == 0: use num_cpus (default)
 * g_auto_scaling: try multiple thread counts to find best */
static int g_explicit_threads = 0;
static int g_auto_scaling = 0;

static double g_max_runtime = DEFAULT_MAX_RUNTIME;

/* Huge pages support */
static int g_use_hugepages = 0;

/* Operation selection bitmask (bit 0=read, 1=write, 2=copy, 3=latency) */
#define OP_MASK_ALL 0x0F  /* All operations enabled */
static int g_ops_mask = OP_MASK_ALL;


/* ============================================================================
 * Memory operations
 * ============================================================================ */

/* Prevent compiler from optimizing away operations */
static volatile uint64_t g_sink = 0;

/*
 * Memory operations - heavily optimized for bandwidth measurement
 * Key techniques:
 * 1. Multiple independent accumulators to break dependency chains
 * 2. Large unrolling (32 elements = 256 bytes per iteration)
 * 3. Force inlining to eliminate call overhead
 */

/* Read operation: XOR all 64-bit words with independent accumulators
 * XOR is faster than ADD and has no carry dependency chains */
static inline __attribute__((always_inline))
uint64_t mem_read(const void *buf, size_t size) {
    const uint64_t *p = (const uint64_t *)buf;
    const uint64_t *end = p + (size / sizeof(uint64_t));

    /* Use 8 independent accumulators - each one handles every 8th element */
    uint64_t x0 = 0, x1 = 0, x2 = 0, x3 = 0;
    uint64_t x4 = 0, x5 = 0, x6 = 0, x7 = 0;

    /* Process 32 elements (256 bytes) per iteration */
    while (p + 32 <= end) {
        x0 ^= p[0];  x1 ^= p[1];  x2 ^= p[2];  x3 ^= p[3];
        x4 ^= p[4];  x5 ^= p[5];  x6 ^= p[6];  x7 ^= p[7];
        x0 ^= p[8];  x1 ^= p[9];  x2 ^= p[10]; x3 ^= p[11];
        x4 ^= p[12]; x5 ^= p[13]; x6 ^= p[14]; x7 ^= p[15];
        x0 ^= p[16]; x1 ^= p[17]; x2 ^= p[18]; x3 ^= p[19];
        x4 ^= p[20]; x5 ^= p[21]; x6 ^= p[22]; x7 ^= p[23];
        x0 ^= p[24]; x1 ^= p[25]; x2 ^= p[26]; x3 ^= p[27];
        x4 ^= p[28]; x5 ^= p[29]; x6 ^= p[30]; x7 ^= p[31];
        p += 32;
    }

    /* Handle remaining elements */
    while (p + 8 <= end) {
        x0 ^= p[0]; x1 ^= p[1]; x2 ^= p[2]; x3 ^= p[3];
        x4 ^= p[4]; x5 ^= p[5]; x6 ^= p[6]; x7 ^= p[7];
        p += 8;
    }
    while (p < end) {
        x0 ^= *p++;
    }

    return x0 ^ x1 ^ x2 ^ x3 ^ x4 ^ x5 ^ x6 ^ x7;
}

/* Write operation: fill with pattern, heavily unrolled */
static inline __attribute__((always_inline))
void mem_write(void *buf, size_t size, uint64_t pattern) {
    uint64_t *p = (uint64_t *)buf;
    uint64_t *end = p + (size / sizeof(uint64_t));

    /* Process 32 elements (256 bytes) per iteration */
    while (p + 32 <= end) {
        p[0]  = pattern; p[1]  = pattern; p[2]  = pattern; p[3]  = pattern;
        p[4]  = pattern; p[5]  = pattern; p[6]  = pattern; p[7]  = pattern;
        p[8]  = pattern; p[9]  = pattern; p[10] = pattern; p[11] = pattern;
        p[12] = pattern; p[13] = pattern; p[14] = pattern; p[15] = pattern;
        p[16] = pattern; p[17] = pattern; p[18] = pattern; p[19] = pattern;
        p[20] = pattern; p[21] = pattern; p[22] = pattern; p[23] = pattern;
        p[24] = pattern; p[25] = pattern; p[26] = pattern; p[27] = pattern;
        p[28] = pattern; p[29] = pattern; p[30] = pattern; p[31] = pattern;
        p += 32;
    }

    /* Handle remaining */
    while (p < end) {
        *p++ = pattern;
    }
}

/* Copy operation: copy from src to dst, heavily unrolled */
static inline __attribute__((always_inline))
void mem_copy(void *dst, const void *src, size_t size) {
    const uint64_t *s = (const uint64_t *)src;
    uint64_t *d = (uint64_t *)dst;
    const uint64_t *end = s + (size / sizeof(uint64_t));

    /* Process 32 elements (256 bytes) per iteration */
    while (s + 32 <= end) {
        d[0]  = s[0];  d[1]  = s[1];  d[2]  = s[2];  d[3]  = s[3];
        d[4]  = s[4];  d[5]  = s[5];  d[6]  = s[6];  d[7]  = s[7];
        d[8]  = s[8];  d[9]  = s[9];  d[10] = s[10]; d[11] = s[11];
        d[12] = s[12]; d[13] = s[13]; d[14] = s[14]; d[15] = s[15];
        d[16] = s[16]; d[17] = s[17]; d[18] = s[18]; d[19] = s[19];
        d[20] = s[20]; d[21] = s[21]; d[22] = s[22]; d[23] = s[23];
        d[24] = s[24]; d[25] = s[25]; d[26] = s[26]; d[27] = s[27];
        d[28] = s[28]; d[29] = s[29]; d[30] = s[30]; d[31] = s[31];
        s += 32;
        d += 32;
    }

    /* Handle remaining */
    while (s < end) {
        *d++ = *s++;
    }
}

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

/* Fisher-Yates shuffle for node pointer array */
static void shuffle_nodes(LatencyNode **nodes, size_t n) {
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = (size_t)rand() % (i + 1);
        LatencyNode *tmp = nodes[i];
        nodes[i] = nodes[j];
        nodes[j] = tmp;
    }
}

/* Allocate memory for latency chain with NUMA awareness and huge page support
 * Uses mmap with optional huge pages to reduce TLB overhead for large buffers */
static LatencyNode* alloc_latency_memory(size_t num_nodes, size_t *alloc_size) {
    size_t size = num_nodes * sizeof(LatencyNode);
    *alloc_size = size;

    LatencyNode *memory = MAP_FAILED;
    int try_hugepages = g_use_hugepages && (size >= get_huge_page_threshold());

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
            if (g_verbose >= 2) {
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
                    if (g_verbose >= 2) {
                        fprintf(stderr, "  Latency: allocated %zu bytes with THP (transparent huge pages)\n", size);
                    }
                } else if (g_verbose >= 2) {
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
    if (numa_available() >= 0 && g_platform.numa_nodes > 1) {
        int node = numa_node_of_cpu(0);
        if (node >= 0) {
            unsigned long nodemask = 1UL << node;
            mbind(memory, *alloc_size, MPOL_BIND, &nodemask, g_platform.numa_nodes + 1, MPOL_MF_MOVE);
            if (g_verbose >= 2) {
                fprintf(stderr, "  Latency memory bound to NUMA node %d\n", node);
            }
        }
    }
#endif

    return memory;
}

/* Free latency chain memory allocated via mmap */
static void free_latency_memory(LatencyNode *memory, size_t alloc_size) {
    if (memory && alloc_size > 0) {
        munmap(memory, alloc_size);
    }
}

/* Initialize linked list with random traversal order
 * Memory is contiguous (good for allocation) but traversal is random
 * (defeats prefetcher, measures true memory latency)
 * Returns: start node pointer; caller must track alloc_size for freeing */
static LatencyNode* init_latency_chain(size_t num_nodes, size_t *alloc_size) {
    if (num_nodes < 2) return NULL;

    /* Allocate contiguous memory for all nodes using NUMA-aware allocation */
    LatencyNode *memory = alloc_latency_memory(num_nodes, alloc_size);
    if (!memory) return NULL;

    /* Initialize payloads (also touches pages for NUMA first-touch policy) */
    for (size_t i = 0; i < num_nodes; i++) {
        memory[i].payload = i;  /* Unique payload for each node */
    }

    /* Create array of pointers for shuffling */
    LatencyNode **nodes = (LatencyNode **)malloc(num_nodes * sizeof(LatencyNode *));
    if (!nodes) {
        free_latency_memory(memory, *alloc_size);
        return NULL;
    }

    for (size_t i = 0; i < num_nodes; i++) {
        nodes[i] = &memory[i];
    }

    /* Shuffle to create random traversal order */
    shuffle_nodes(nodes, num_nodes);

    /* Link nodes in shuffled order (circular) */
    for (size_t i = 0; i < num_nodes - 1; i++) {
        nodes[i]->next = nodes[i + 1];
    }
    nodes[num_nodes - 1]->next = nodes[0];  /* Close the loop */

    LatencyNode *start = nodes[0];
    free(nodes);

    return start;
}

/* Free latency chain - need base address and size */
static void free_latency_chain(LatencyNode *start, size_t num_nodes, size_t alloc_size) {
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

    g_sink += sink;  /* Store to global to prevent optimization */
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

/* Measure latency with statistical validity
 *
 * Strategy:
 * 1. Create random linked list covering the buffer size
 * 2. Warmup by traversing the list once
 * 3. Calibration run to estimate latency and calculate traversals needed
 * 4. Collect multiple independent time samples
 * 5. Continue until CV < target or max samples reached
 * 6. Report median (robust to outliers) and statistics
 *
 * Returns statistically valid latency measurement
 */
static latency_stats_t measure_latency_stats(size_t buffer_size) {
    latency_stats_t stats = {0};

    /* Pin thread to CPU 0 for consistent latency measurement.
     * This prevents OS scheduler from migrating the thread during measurement,
     * which would cause inconsistent results due to cache effects and NUMA. */
    pin_thread_to_cpu0();

    /* Calculate number of nodes that fit in buffer */
    size_t num_nodes = buffer_size / sizeof(LatencyNode);
    if (num_nodes < 64) num_nodes = 64;  /* Minimum for meaningful measurement */

    /* Initialize chain with NUMA-aware allocation */
    size_t alloc_size = 0;
    LatencyNode *start = init_latency_chain(num_nodes, &alloc_size);
    if (!start) {
        fprintf(stderr, "Failed to allocate %zu bytes for latency test\n",
                num_nodes * sizeof(LatencyNode));
        return stats;
    }

    /* Warmup: single traversal to prime caches and stabilize CPU */
    chase_latency_chain(start, num_nodes);

    /* Calibration: time a single traversal to estimate latency */
    double cal_start = get_time();
    chase_latency_chain(start, num_nodes);
    double cal_elapsed = get_time() - cal_start;

    /* Calculate traversals needed to achieve target sample time */
    double estimated_latency_s = cal_elapsed / num_nodes;
    size_t traversals_per_sample;

    if (estimated_latency_s > 0) {
        /* Calculate traversals to reach target sample time */
        double target_accesses = LATENCY_TARGET_SAMPLE_TIME / estimated_latency_s;
        traversals_per_sample = (size_t)(target_accesses / num_nodes);

        /* Ensure at least 1 full traversal per sample */
        if (traversals_per_sample < 1) traversals_per_sample = 1;

        /* Cap at reasonable maximum for very fast (L1) accesses */
        if (traversals_per_sample > 10000) traversals_per_sample = 10000;
    } else {
        /* Fallback: at least 1 traversal */
        traversals_per_sample = 1;
    }

    /* Sample collection */
    double samples[LATENCY_MAX_SAMPLES];
    int num_samples = 0;
    size_t total_accesses = 0;

    /* Collect samples until statistically valid or max reached */
    while (num_samples < LATENCY_MAX_SAMPLES) {
        size_t accesses_this_sample = num_nodes * traversals_per_sample;

        /* Time this sample */
        double start_time = get_time();
        chase_latency_chain(start, accesses_this_sample);
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

    return stats;
}

/* ============================================================================
 * Memory allocation
 * ============================================================================ */

static void* alloc_buffer(size_t size) {
    void *buf = MAP_FAILED;
    int try_hugepages = g_use_hugepages && (size >= get_huge_page_threshold());

    if (try_hugepages) {
        /*
         * Strategy: prefer THP over explicit huge pages because:
         * 1. THP doesn't require pre-allocation by root
         * 2. THP is managed automatically by the kernel
         * 3. Explicit huge pages may fail if pool isn't configured
         *
         * We try explicit huge pages first only because they're more
         * deterministic (guaranteed 2MB pages vs THP's best-effort).
         */

#ifdef MAP_HUGETLB
        /* Round up size to huge page boundary for explicit huge pages */
        size_t hp_size = get_huge_page_size();
        size_t aligned_size = (size + hp_size - 1) & ~(hp_size - 1);

        /* Try explicit huge pages (uses pre-allocated pool if available) */
        buf = mmap(NULL, aligned_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (buf != MAP_FAILED) {
            if (g_verbose >= 2) {
                fprintf(stderr, "  Allocated %zu bytes using explicit %zu KB huge pages\n",
                        aligned_size, hp_size / 1024);
            }
            /* Touch all pages to ensure they're allocated */
            memset(buf, 0, size);
            return buf;
        }
        /* Explicit huge pages failed - likely no pool configured, try THP */
#endif

        /* Use mmap + madvise for Transparent Huge Pages (no pre-allocation needed) */
        buf = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (buf != MAP_FAILED) {
#ifdef MADV_HUGEPAGE
            /* Hint to kernel: please use huge pages for this region.
             * The kernel will use THP if available and beneficial.
             * This doesn't require root or pre-allocation. */
            if (madvise(buf, size, MADV_HUGEPAGE) == 0) {
                if (g_verbose >= 2) {
                    fprintf(stderr, "  Allocated %zu bytes with THP (transparent huge pages)\n", size);
                }
            } else if (g_verbose >= 2) {
                fprintf(stderr, "  Allocated %zu bytes (THP hint failed, using regular pages)\n", size);
            }
#else
            if (g_verbose >= 2) {
                fprintf(stderr, "  Allocated %zu bytes (THP not available on this system)\n", size);
            }
#endif
            /* Touch all pages to ensure they're allocated */
            memset(buf, 0, size);
            return buf;
        }
    }

    /* Regular allocation: small buffers, huge pages disabled, or fallback */
    buf = mmap(NULL, size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (buf == MAP_FAILED) {
        return NULL;
    }

    /* Touch all pages to ensure they're allocated */
    memset(buf, 0, size);

    return buf;
}

static void free_buffer(void *buf, size_t size) {
    if (buf) {
        munmap(buf, size);
    }
}

/* ============================================================================
 * OpenMP Bandwidth Benchmark
 * ============================================================================ */

/*
 * Run bandwidth benchmark using OpenMP.
 *
 * Key features:
 * - proc_bind(spread) distributes threads across NUMA nodes
 * - Per-thread NUMA-local buffer allocation
 * - Implicit barrier synchronization (more efficient than pthread_barrier)
 * - 8-accumulator read for optimal bandwidth measurement
 */
static result_t run_benchmark_omp(size_t size, operation_t op, int nthreads) {
    result_t result = {0};
    result.size = size;
    result.op = op;
    result.threads = nthreads;

    /* Allocate arrays for per-thread buffers and results */
    void **src_bufs = calloc(nthreads, sizeof(void*));
    void **dst_bufs = calloc(nthreads, sizeof(void*));
    double *thread_elapsed = calloc(nthreads, sizeof(double));
    uint64_t *thread_checksums = calloc(nthreads, sizeof(uint64_t));
    int alloc_failed = 0;

    if (!src_bufs || !dst_bufs || !thread_elapsed || !thread_checksums) {
        free(src_bufs);
        free(dst_bufs);
        free(thread_elapsed);
        free(thread_checksums);
        return result;
    }

    /* Set OpenMP thread count */
    omp_set_num_threads(nthreads);

    /* Phase 1: Parallel allocation with NUMA awareness
     * proc_bind(spread) distributes threads across NUMA nodes,
     * then each thread allocates memory locally */
    #pragma omp parallel proc_bind(spread)
    {
        int tid = omp_get_thread_num();

#ifdef USE_NUMA
        /* Get current CPU and its NUMA node (OpenMP has placed us optimally) */
        if (numa_available() >= 0) {
            int cpu = sched_getcpu();
            int node = numa_node_of_cpu(cpu);
            if (node >= 0) {
                /* Allocate on local NUMA node */
                src_bufs[tid] = numa_alloc_onnode(size, node);
                if (op == OP_COPY) {
                    dst_bufs[tid] = numa_alloc_onnode(size, node);
                }
            }
        }
#endif

        /* Fallback: regular allocation if NUMA not available or failed */
        if (!src_bufs[tid]) {
            src_bufs[tid] = alloc_buffer(size);
        }
        if (op == OP_COPY && !dst_bufs[tid]) {
            dst_bufs[tid] = alloc_buffer(size);
        }

        /* Check allocation success */
        if (!src_bufs[tid] || (op == OP_COPY && !dst_bufs[tid])) {
            #pragma omp atomic write
            alloc_failed = 1;
        }

        /* Initialize buffer (first-touch for NUMA) */
        if (src_bufs[tid]) {
            memset(src_bufs[tid], 0xAA, size);
        }
        if (dst_bufs[tid]) {
            memset(dst_bufs[tid], 0, size);
        }
    }

    if (alloc_failed) {
        /* Cleanup on allocation failure */
        for (int i = 0; i < nthreads; i++) {
#ifdef USE_NUMA
            if (numa_available() >= 0) {
                if (src_bufs[i]) numa_free(src_bufs[i], size);
                if (dst_bufs[i]) numa_free(dst_bufs[i], size);
            } else
#endif
            {
                free_buffer(src_bufs[i], size);
                free_buffer(dst_bufs[i], size);
            }
        }
        free(src_bufs);
        free(dst_bufs);
        free(thread_elapsed);
        free(thread_checksums);
        if (g_verbose) {
            fprintf(stderr, "Failed to allocate %zu bytes × %d threads\n", size, nthreads);
        }
        return result;
    }

    /* Phase 2: Calibration - estimate iterations needed */
    int iterations = MIN_ITERATIONS;
    {
        /* Warmup */
        g_sink += mem_read(src_bufs[0], size);

        /* Time single iteration */
        double t_start = get_time();
        switch (op) {
            case OP_READ:
                g_sink += mem_read(src_bufs[0], size);
                break;
            case OP_WRITE:
                mem_write(src_bufs[0], size, 0x1234567890ABCDEFULL);
                break;
            case OP_COPY:
                mem_copy(dst_bufs[0], src_bufs[0], size);
                break;
            default:
                break;
        }
        double time_per_iter = get_time() - t_start;

        if (time_per_iter > 1e-9) {
            iterations = (int)(TARGET_TIME_PER_TEST / time_per_iter);
            if (iterations < MIN_ITERATIONS) iterations = MIN_ITERATIONS;
            if (iterations > MAX_ITERATIONS) iterations = MAX_ITERATIONS;
        }
    }
    result.iterations = iterations;

    /* Phase 3: Timed measurement with all threads
     * OpenMP implicit barrier ensures all threads start together */
    #pragma omp parallel proc_bind(spread)
    {
        int tid = omp_get_thread_num();
        void *src = src_bufs[tid];
        void *dst = dst_bufs[tid];
        uint64_t checksum = 0;

        /* Implicit barrier here - all threads synchronized */

        double t_start = get_time();

        switch (op) {
            case OP_READ:
                for (int i = 0; i < iterations; i++) {
                    checksum ^= mem_read(src, size);
                }
                break;
            case OP_WRITE:
                for (int i = 0; i < iterations; i++) {
                    mem_write(src, size, (uint64_t)i);
                }
                break;
            case OP_COPY:
                for (int i = 0; i < iterations; i++) {
                    mem_copy(dst, src, size);
                }
                break;
            default:
                break;
        }

        double t_end = get_time();

        thread_elapsed[tid] = t_end - t_start;
        thread_checksums[tid] = checksum;
    }

    /* Find max elapsed time (determines overall bandwidth) */
    double max_elapsed = 0;
    uint64_t total_checksum = 0;
    for (int i = 0; i < nthreads; i++) {
        if (thread_elapsed[i] > max_elapsed) {
            max_elapsed = thread_elapsed[i];
        }
        total_checksum ^= thread_checksums[i];
    }

    g_sink += total_checksum;
    result.elapsed_s = max_elapsed;

    /* Calculate bandwidth = (size per thread * threads * iterations) / time
     * This gives aggregate bandwidth across all threads.
     * Note: for copy, we report buffer size (not 2x) to match bw_mem convention */
    if (max_elapsed > 0) {
        size_t bytes_transferred = (size_t)size * nthreads * iterations;
        result.bandwidth_mb_s = (bytes_transferred / (1024.0 * 1024.0)) / max_elapsed;
    }

    /* Cleanup */
    for (int i = 0; i < nthreads; i++) {
#ifdef USE_NUMA
        if (numa_available() >= 0) {
            if (src_bufs[i]) numa_free(src_bufs[i], size);
            if (dst_bufs[i]) numa_free(dst_bufs[i], size);
        } else
#endif
        {
            free_buffer(src_bufs[i], size);
            free_buffer(dst_bufs[i], size);
        }
    }
    free(src_bufs);
    free(dst_bufs);
    free(thread_elapsed);
    free(thread_checksums);

    return result;
}

/* Run single-threaded benchmark (for small buffers or latency) */
static result_t run_benchmark_single(size_t size, operation_t op) {
    result_t result = {0};
    result.size = size;
    result.op = op;
    result.threads = 1;

    void *src = alloc_buffer(size);
    void *dst = (op == OP_COPY) ? alloc_buffer(size) : NULL;

    if (!src || (op == OP_COPY && !dst)) {
        free_buffer(src, size);
        free_buffer(dst, size);
        return result;
    }

    memset(src, 0xAA, size);
    if (dst) memset(dst, 0, size);

    /* Warmup */
    g_sink += mem_read(src, size);

    /* Calibrate */
    double t_start = get_time();
    switch (op) {
        case OP_READ: g_sink += mem_read(src, size); break;
        case OP_WRITE: mem_write(src, size, 0x1234567890ABCDEFULL); break;
        case OP_COPY: mem_copy(dst, src, size); break;
        default: break;
    }
    double time_per_iter = get_time() - t_start;

    int iterations = MIN_ITERATIONS;
    if (time_per_iter > 1e-9) {
        iterations = (int)(TARGET_TIME_PER_TEST / time_per_iter);
        if (iterations < MIN_ITERATIONS) iterations = MIN_ITERATIONS;
        if (iterations > MAX_ITERATIONS) iterations = MAX_ITERATIONS;
    }
    result.iterations = iterations;

    /* Timed run */
    uint64_t checksum = 0;
    t_start = get_time();

    switch (op) {
        case OP_READ:
            for (int i = 0; i < iterations; i++) {
                checksum ^= mem_read(src, size);
            }
            break;
        case OP_WRITE:
            for (int i = 0; i < iterations; i++) {
                mem_write(src, size, (uint64_t)i);
            }
            break;
        case OP_COPY:
            for (int i = 0; i < iterations; i++) {
                mem_copy(dst, src, size);
            }
            break;
        default:
            break;
    }

    double elapsed = get_time() - t_start;

    g_sink += checksum;
    result.elapsed_s = elapsed;

    if (elapsed > 0) {
        size_t bytes_transferred = size * iterations;
        result.bandwidth_mb_s = (bytes_transferred / (1024.0 * 1024.0)) / elapsed;
    }

    free_buffer(src, size);
    free_buffer(dst, size);

    return result;
}

/* Main benchmark runner - dispatches to OpenMP or single-threaded */
static result_t run_benchmark(size_t size, operation_t op, int nthreads) {
    if (nthreads == 1) {
        return run_benchmark_single(size, op);
    }
    return run_benchmark_omp(size, op, nthreads);
}

/* Run benchmark multiple times and return best result (like lmbench TRIES)
 * For bandwidth: best = highest bandwidth
 * For latency: best = lowest latency
 *
 * First run is a warmup (discarded) to allow CPU frequency to ramp up
 * and caches to warm. This dramatically reduces result variability.
 */
static result_t run_benchmark_best(size_t size, operation_t op, int nthreads) {
    result_t best = {0};

    /* Warmup run - discarded.
     * This allows: CPU to reach turbo frequency, caches to warm,
     * thread scheduling to stabilize. Critical for consistent results. */
    (void)run_benchmark(size, op, nthreads);

    for (int try = 0; try < g_benchmark_tries; try++) {
        result_t r = run_benchmark(size, op, nthreads);

        if (try == 0) {
            best = r;
        } else {
            if (op == OP_LATENCY) {
                /* For latency: lower is better */
                if (r.latency_ns > 0 && r.latency_ns < best.latency_ns) {
                    best = r;
                }
            } else {
                /* For bandwidth: higher is better */
                if (r.bandwidth_mb_s > best.bandwidth_mb_s) {
                    best = r;
                }
            }
        }
    }

    return best;
}

/* ============================================================================
 * Main benchmark loop
 * ============================================================================ */

/* Generate thread counts dynamically based on CPU count (for auto-scaling mode)
 *
 * Strategy:
 * - Powers of 2 from 1 up to nproc
 * - Always include nproc itself (if not already a power of 2)
 * - No oversubscription (causes unreliable results)
 *
 * Examples:
 *   4 cores:   1, 2, 4               (3 values)
 *   32 cores:  1, 2, 4, 8, 16, 32    (6 values)
 *   48 cores:  1, 2, 4, 8, 16, 32, 48 (7 values)
 */
static int* get_thread_counts(int *count) {
    int nproc = g_platform.num_cpus;
    if (nproc < 1) nproc = 1;

    /* Cap at nproc - oversubscription causes unreliable benchmark results
     * due to context switching, cache thrashing, and scheduler interference */
    int max_threads = nproc;

    /* Allocate more than enough space */
    int *tc = malloc(32 * sizeof(int));
    int n = 0;

    /* Add powers of 2 up to nproc */
    for (int t = 1; t <= max_threads; t *= 2) {
        tc[n++] = t;
    }

    /* Add nproc if not already in list (i.e., not a power of 2) */
    if (tc[n-1] != nproc) {
        tc[n++] = nproc;
    }

    tc[n] = 0;  /* Sentinel */
    *count = n;
    return tc;
}

/* Get sizes to test (per-thread buffer sizes) - adaptive based on cache hierarchy
 *
 * Generates sizes at critical cache transition points to show:
 * 1. Pure L1 performance
 * 2. L1→L2 transition
 * 3. Pure L2 performance
 * 4. L2→L3 transition
 * 5. L3 region
 * 6. Pure RAM bandwidth
 *
 * All sizes are strictly increasing with no overlaps.
 */
static size_t* get_sizes(int *count) {
    int nthreads = g_explicit_threads > 0 ? g_explicit_threads : g_platform.num_cpus;
    if (nthreads < 1) nthreads = 1;

    /* Use detected cache sizes, with sensible defaults */
    size_t l1 = g_platform.l1_cache_size > 0 ? g_platform.l1_cache_size : 32768;      /* 32 KB */
    size_t l2 = g_platform.l2_cache_size > 0 ? g_platform.l2_cache_size : 262144;     /* 256 KB */
    size_t l3 = g_platform.l3_cache_size > 0 ? g_platform.l3_cache_size : 8388608;    /* 8 MB */

    /* Memory limit per thread */
    size_t max_size = g_platform.total_memory / 2 / nthreads;

    /* Build strictly increasing size sequence */
    size_t sizes_list[20];
    int n = 0;
    size_t prev = 0;

    /* Helper macro to add size if > prev and <= max_size */
    #define ADD_SIZE(sz) do { \
        size_t _s = round_to_power_of_2(sz); \
        if (_s > prev && _s <= max_size) { sizes_list[n++] = _s; prev = _s; } \
    } while(0)

    /* L1 region */
    ADD_SIZE(l1 / 2);

    /* L1→L2 transition */
    ADD_SIZE(l1 * 2);

    /* L2 region */
    ADD_SIZE(l2 / 2);
    ADD_SIZE(l2);

    /* L2→L3 transition */
    ADD_SIZE(l2 * 2);

    /* L3 region */
    if (l3 > l2 * 4) {
        ADD_SIZE(l3 / 4);
    }
    ADD_SIZE(l3 / 2);

    /* L3→RAM transition */
    ADD_SIZE(l3);

    /* RAM region */
    ADD_SIZE(l3 * 2);
    ADD_SIZE(l3 * 4);

    /* Full sweep: add larger sizes up to memory limit */
    if (g_full_sweep) {
        size_t ram_size = RAM_SIZE_2 * 2;
        while (ram_size <= max_size && n < 18) {
            ADD_SIZE(ram_size);
            ram_size *= 2;
        }
    }

    #undef ADD_SIZE

    /* Ensure at least one size */
    if (n == 0) {
        sizes_list[n++] = 4096;
    }

    /* Copy to result array */
    size_t *sizes = malloc((n + 1) * sizeof(size_t));
    for (int i = 0; i < n; i++) {
        sizes[i] = sizes_list[i];
    }
    sizes[n] = 0;
    *count = n;
    return sizes;
}

static void print_csv_header(void) {
    if (g_human_readable) {
        printf("\n%-10s %-8s %12s %12s %8s\n",
               "Size", "Op", "Bandwidth", "Latency", "Threads");
        printf("%-10s %-8s %12s %12s %8s\n",
               "----", "--", "---------", "-------", "-------");
    } else {
        printf("size_kb,operation,bandwidth_mb_s,latency_ns,latency_stddev_ns,latency_samples,threads,iterations,elapsed_s\n");
    }
}

static void print_result(const result_t *r) {
    size_t size_kb = r->size / 1024;

    if (g_human_readable) {
        char size_buf[32], bw_buf[32];
        format_size(size_kb, size_buf, sizeof(size_buf));

        if (r->op == OP_LATENCY) {
            printf("%-10s %-8s %12s %9.1f ns %8d\n",
                   size_buf, OP_NAMES[r->op], "-", r->latency_ns, r->threads);
        } else {
            format_bandwidth(r->bandwidth_mb_s, bw_buf, sizeof(bw_buf));
            printf("%-10s %-8s %12s %12s %8d\n",
                   size_buf, OP_NAMES[r->op], bw_buf, "-", r->threads);
        }
    } else {
        if (r->op == OP_LATENCY) {
            /* For latency test: report median, stddev, and sample count for statistical validity
             * Median is robust to outliers and provides reliable central tendency
             * StdDev indicates measurement precision
             * Sample count shows measurement effort */
            printf("%zu,%s,0,%.2f,%.2f,%d,%d,%d,%.6f\n",
                   size_kb, OP_NAMES[r->op], r->latency_ns,
                   r->latency_stddev_ns, r->latency_samples,
                   r->threads, r->iterations, r->elapsed_s);
        } else {
            /* For bandwidth tests, latency fields are 0 */
            printf("%zu,%s,%.2f,0,0,0,%d,%d,%.6f\n",
                   size_kb, OP_NAMES[r->op], r->bandwidth_mb_s,
                   r->threads, r->iterations, r->elapsed_s);
        }
    }
}

/* Update summary statistics with a new result */
static void update_summary(const result_t *r) {
    /* Weight by log2 of size - larger sizes get more weight */
    double weight = log2((double)r->size / 1024.0 + 1.0);

    /* Track largest size tested */
    if (r->size > g_summary.largest_size_tested) {
        g_summary.largest_size_tested = r->size;
    }

    switch (r->op) {
        case OP_READ:
            g_summary.read_count++;
            if (r->bandwidth_mb_s > g_summary.peak_read_mb_s) {
                g_summary.peak_read_mb_s = r->bandwidth_mb_s;
            }
            g_summary.read_bw_weighted_sum += r->bandwidth_mb_s * weight;
            g_summary.read_weight_sum += weight;
            break;

        case OP_WRITE:
            g_summary.write_count++;
            if (r->bandwidth_mb_s > g_summary.peak_write_mb_s) {
                g_summary.peak_write_mb_s = r->bandwidth_mb_s;
            }
            g_summary.write_bw_weighted_sum += r->bandwidth_mb_s * weight;
            g_summary.write_weight_sum += weight;
            break;

        case OP_COPY:
            g_summary.copy_count++;
            if (r->bandwidth_mb_s > g_summary.peak_copy_mb_s) {
                g_summary.peak_copy_mb_s = r->bandwidth_mb_s;
            }
            g_summary.copy_bw_weighted_sum += r->bandwidth_mb_s * weight;
            g_summary.copy_weight_sum += weight;
            break;

        case OP_LATENCY:
            g_summary.latency_count++;
            /* For latency, track the largest buffer size tested for the most RAM-like result */
            if (r->latency_ns > 0 && r->size >= g_summary.largest_size_tested) {
                /* Always update with the largest size measurement */
                g_summary.best_latency_ns = r->latency_ns;
            }
            break;
    }
}

/* Print summary statistics */
static void print_summary(void) {
    fprintf(stderr, "\n");
    fprintf(stderr, "================================================================================\n");
    fprintf(stderr, "                           BENCHMARK SUMMARY\n");
    fprintf(stderr, "================================================================================\n\n");

    /* Calculate weighted averages */
    if (g_summary.read_weight_sum > 0) {
        g_summary.weighted_avg_read_mb_s = g_summary.read_bw_weighted_sum / g_summary.read_weight_sum;
    }
    if (g_summary.write_weight_sum > 0) {
        g_summary.weighted_avg_write_mb_s = g_summary.write_bw_weighted_sum / g_summary.write_weight_sum;
    }
    if (g_summary.copy_weight_sum > 0) {
        g_summary.weighted_avg_copy_mb_s = g_summary.copy_bw_weighted_sum / g_summary.copy_weight_sum;
    }

    /* Print bandwidth results */
    fprintf(stderr, "BANDWIDTH (MB/s):\n");
    fprintf(stderr, "  %-10s %12s %12s\n", "Operation", "Peak", "Weighted Avg");
    fprintf(stderr, "  %-10s %12s %12s\n", "---------", "----", "------------");

    if (g_summary.read_count > 0) {
        fprintf(stderr, "  %-10s %12.0f %12.0f\n", "Read",
                g_summary.peak_read_mb_s, g_summary.weighted_avg_read_mb_s);
    }
    if (g_summary.write_count > 0) {
        fprintf(stderr, "  %-10s %12.0f %12.0f\n", "Write",
                g_summary.peak_write_mb_s, g_summary.weighted_avg_write_mb_s);
    }
    if (g_summary.copy_count > 0) {
        fprintf(stderr, "  %-10s %12.0f %12.0f\n", "Copy",
                g_summary.peak_copy_mb_s, g_summary.weighted_avg_copy_mb_s);
    }

    /* Print latency results */
    if (g_summary.latency_count > 0 && g_summary.best_latency_ns > 0) {
        fprintf(stderr, "\nLATENCY:\n");
        const char *cache_note = "";
        if (g_summary.largest_size_tested < 1024 * 1024) {
            cache_note = " (L2/L3 cache)";
        } else if (g_summary.largest_size_tested < 64 * 1024 * 1024) {
            cache_note = " (L3 cache/RAM)";
        } else {
            cache_note = " (RAM)";
        }
        fprintf(stderr, "  Best latency: %.1f ns%s at %zu KB buffer\n",
                g_summary.best_latency_ns, cache_note, g_summary.largest_size_tested / 1024);
    }

    /* Calculate and print composite benchmark score
     * Score formula: geometric mean of bandwidth scores, divided by latency factor
     * Higher is better for all components */
    fprintf(stderr, "\n");
    fprintf(stderr, "--------------------------------------------------------------------------------\n");
    fprintf(stderr, "BENCHMARK SCORE (higher is better):\n\n");

    /* Individual scores */
    double bw_total = 0;
    int bw_count = 0;

    if (g_summary.peak_read_mb_s > 0) {
        bw_total += g_summary.peak_read_mb_s;
        bw_count++;
    }
    if (g_summary.peak_write_mb_s > 0) {
        bw_total += g_summary.peak_write_mb_s;
        bw_count++;
    }
    if (g_summary.peak_copy_mb_s > 0) {
        bw_total += g_summary.peak_copy_mb_s;
        bw_count++;
    }

    /* Bandwidth score: average of peak bandwidths (in GB/s for nicer numbers) */
    double bw_score = 0;
    if (bw_count > 0) {
        bw_score = (bw_total / bw_count) / 1000.0;  /* Convert MB/s to GB/s */
        fprintf(stderr, "  Bandwidth Score:    %8.1f  (avg peak bandwidth in GB/s)\n", bw_score);
    }

    /* Latency score: inverse of latency (higher = faster memory) */
    double latency_score = 0;
    if (g_summary.best_latency_ns > 0) {
        latency_score = 1000.0 / g_summary.best_latency_ns;  /* 1000/ns gives reasonable scale */
        fprintf(stderr, "  Latency Score:      %8.1f  (1000 / latency_ns)\n", latency_score);
    }

    /* Combined score: geometric mean if both available, otherwise just bandwidth */
    double combined_score = 0;
    if (bw_score > 0 && latency_score > 0) {
        combined_score = sqrt(bw_score * latency_score) * 100;  /* Scale for nice numbers */
        fprintf(stderr, "\n  >> COMBINED SCORE:  %8.0f  (sqrt(bw_score × latency_score) × 100)\n", combined_score);
    } else if (bw_score > 0) {
        combined_score = bw_score * 100;
        fprintf(stderr, "\n  >> COMBINED SCORE:  %8.0f  (bandwidth only, no latency data)\n", combined_score);
    }

    fprintf(stderr, "--------------------------------------------------------------------------------\n");

    /* Warn if options that affect score comparability were used */
    int has_warnings = 0;
    if (g_max_runtime > 0 || g_explicit_threads > 0 || g_single_size > 0) {
        fprintf(stderr, "\n");
        fprintf(stderr, "WARNING: Scores may not be comparable due to non-default options:\n");
        if (g_max_runtime > 0) {
            fprintf(stderr, "  - Time limit (-t %.0f) may have prevented testing larger buffer sizes\n", g_max_runtime);
            has_warnings = 1;
        }
        if (g_explicit_threads > 0) {
            fprintf(stderr, "  - Fixed thread count (-p %d) instead of using all CPUs (%d)\n",
                    g_explicit_threads, g_platform.num_cpus);
            has_warnings = 1;
        }
        if (g_single_size > 0) {
            fprintf(stderr, "  - Single buffer size (-s %zu KB) instead of full sweep\n",
                    g_single_size / 1024);
            has_warnings = 1;
        }
        if (has_warnings) {
            fprintf(stderr, "For comparable scores, run without -t, -p, or -s options.\n");
        }
    }
    fprintf(stderr, "\n");
}

/* Maximum buffer size for latency test.
 * Must exceed largest L3 caches to measure true DRAM latency.
 * AMD EPYC 9754 (Genoa-X) has 1.1GB L3 cache, so we need > 1.1GB.
 * 2GB should cover any current processor. */
#define MAX_LATENCY_SIZE (2UL * 1024 * 1024 * 1024)  /* 2 GB */

/* Find best configuration for a given buffer size and operation.
 *
 * This follows bw_mem's approach:
 * - buffer_size is the per-thread buffer size
 * - Total memory = buffer_size * threads (or buffer_size * threads * 2 for copy)
 *
 * Three modes:
 * 1. Auto-scaling (g_auto_scaling=1): Try multiple thread counts, find best
 * 2. Explicit threads (g_explicit_threads>0): Use exactly that many threads
 * 3. Default (neither): Use num_cpus threads
 */
static result_t find_best_config(size_t buffer_size, operation_t op,
                                 int *thread_counts, int tc_count) {
    result_t best = {0};
    best.size = buffer_size;
    best.op = op;

    /* For latency test: single-thread, statistically valid measurement */
    if (op == OP_LATENCY) {
        size_t max_latency = MAX_LATENCY_SIZE;
        if (g_platform.total_memory / 4 < max_latency) {
            max_latency = g_platform.total_memory / 4;
        }
        size_t latency_size = (buffer_size > max_latency) ? max_latency : buffer_size;

        double start = get_time();
        latency_stats_t stats = measure_latency_stats(latency_size);
        double elapsed = get_time() - start;

        best.size = buffer_size;
        best.op = op;
        best.threads = 1;
        best.latency_ns = stats.median_ns;
        best.latency_mean_ns = stats.mean_ns;
        best.latency_stddev_ns = stats.stddev_ns;
        best.latency_cv = stats.cv;
        best.latency_samples = stats.num_samples;
        best.elapsed_s = elapsed;
        best.iterations = stats.num_samples;

        return best;
    }

    /* Bandwidth tests */
    int nthreads;

    if (g_auto_scaling) {
        /* Auto-scaling mode: try all thread counts, find best */
        for (int i = 0; i < tc_count; i++) {
            nthreads = thread_counts[i];
            if (nthreads < 1) continue;

            int bufs_per_op = (op == OP_COPY) ? 2 : 1;
            size_t memory_needed = buffer_size * nthreads * bufs_per_op;
            if (memory_needed > g_platform.total_memory / 4) {
                continue;
            }

            result_t r = run_benchmark_best(buffer_size, op, nthreads);
            r.size = buffer_size;

            if (r.bandwidth_mb_s > best.bandwidth_mb_s) {
                best = r;
            }
        }

        if (best.bandwidth_mb_s == 0) {
            best = run_benchmark_best(buffer_size, op, 1);
            best.size = buffer_size;
        }

        return best;
    }

    /* Fixed thread count mode */
    if (g_explicit_threads > 0) {
        nthreads = g_explicit_threads;
    } else {
        nthreads = g_platform.num_cpus;
    }

    /* Check memory limit and reduce threads if needed */
    int bufs_per_op = (op == OP_COPY) ? 2 : 1;
    size_t memory_needed = buffer_size * nthreads * bufs_per_op;
    while (nthreads > 1 && memory_needed > g_platform.total_memory / 4) {
        nthreads /= 2;
        memory_needed = buffer_size * nthreads * bufs_per_op;
    }

    best = run_benchmark_best(buffer_size, op, nthreads);
    best.size = buffer_size;

    return best;
}

static void run_all_benchmarks(void) {
    double start_time = get_time();

    int tc_count;
    int *thread_counts = get_thread_counts(&tc_count);

    /* Single size mode */
    if (g_single_size > 0) {
        if (g_verbose) {
            fprintf(stderr, "Testing buffer size: %zu KB per thread\n",
                    g_single_size / 1024);
        }

        print_csv_header();

        for (int op = 0; op < 4 && g_running; op++) {
            if (!(g_ops_mask & (1 << op))) continue;

            result_t best = find_best_config(g_single_size, (operation_t)op,
                                            thread_counts, tc_count);

            if (best.bandwidth_mb_s > 0 || best.latency_ns > 0) {
                print_result(&best);
                if (g_human_readable) update_summary(&best);
                fflush(stdout);
            }
        }

        free(thread_counts);

        if (g_verbose) {
            double total = get_time() - start_time;
            fprintf(stderr, "Total runtime: %.1f seconds\n", total);
        }

        /* Print summary in human-readable mode */
        if (g_human_readable) print_summary();
        return;
    }

    /* Normal mode: test all sizes */
    int size_count;
    size_t *sizes = get_sizes(&size_count);

    if (g_verbose) {
        fprintf(stderr, "Testing %d buffer sizes (per thread, adaptive to cache hierarchy)\n", size_count);
        if (g_auto_scaling) {
            fprintf(stderr, "Thread mode: auto-scaling (trying 1-%d threads)\n", g_platform.num_cpus);
        } else if (g_explicit_threads > 0) {
            fprintf(stderr, "Thread mode: fixed %d threads\n", g_explicit_threads);
        } else {
            fprintf(stderr, "Thread mode: num_cpus (%d threads)\n", g_platform.num_cpus);
        }
        fprintf(stderr, "OpenMP: proc_bind(spread) for NUMA-aware thread placement\n");
    }

    print_csv_header();

    for (int s = 0; s < size_count && g_running; s++) {
        size_t size = sizes[s];

        for (int op = 0; op < 4 && g_running; op++) {
            if (!(g_ops_mask & (1 << op))) continue;

            result_t best = find_best_config(size, (operation_t)op,
                                             thread_counts, tc_count);

            if (best.bandwidth_mb_s > 0 || best.latency_ns > 0) {
                print_result(&best);
                if (g_human_readable) update_summary(&best);
                fflush(stdout);
            }

            if (g_max_runtime > 0) {
                double elapsed = get_time() - start_time;
                if (elapsed > g_max_runtime) {
                    if (g_verbose) {
                        fprintf(stderr, "Time limit reached (%.1f s)\n", elapsed);
                    }
                    g_running = 0;
                    break;
                }
            }
        }
    }

    free(sizes);
    free(thread_counts);

    if (g_verbose) {
        double total = get_time() - start_time;
        fprintf(stderr, "Total runtime: %.1f seconds\n", total);
    }

    /* Print summary in human-readable mode */
    if (g_human_readable) print_summary();
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void usage(const char *prog) {
    fprintf(stderr, "sc-membench %s - Memory Bandwidth Benchmark (OpenMP)\n\n", VERSION);
    fprintf(stderr, "Usage: %s [options]\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -h          Show this help\n");
    fprintf(stderr, "  -V          Print version and exit\n");
    fprintf(stderr, "  -v          Verbose output (use -vv for more detail)\n");
    fprintf(stderr, "  -s SIZE_KB  Test only this buffer size (in KB), e.g. -s 1024 for 1MB\n");
    fprintf(stderr, "  -f          Full sweep (test all sizes up to memory limit)\n");
    fprintf(stderr, "              Default: test up to 512 MB per thread\n");
    fprintf(stderr, "  -p THREADS  Use exactly this many threads (default: num_cpus)\n");
    fprintf(stderr, "  -a          Auto-scaling: try different thread counts to find best\n");
    fprintf(stderr, "              (slower but finds optimal thread count per buffer size)\n");
    fprintf(stderr, "  -t SECONDS  Maximum runtime, 0 = unlimited (default: unlimited)\n");
    fprintf(stderr, "  -r TRIES    Repeat each test N times, report best (default: %d)\n", DEFAULT_BENCHMARK_TRIES);
    fprintf(stderr, "  -o OP       Run only this operation: read, write, copy, or latency\n");
    fprintf(stderr, "              Can be specified multiple times (default: all)\n");
    fprintf(stderr, "  -H          Enable huge pages for large buffers (>= 4MB)\n");
    fprintf(stderr, "              Uses THP (no setup needed) or explicit 2MB pages\n");
    fprintf(stderr, "              Automatically skipped for small buffers\n");
    fprintf(stderr, "  -R          Human-readable output with summary (default: CSV)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "OpenMP Thread Affinity (environment variables):\n");
    fprintf(stderr, "  OMP_PROC_BIND=spread  Spread threads across NUMA nodes (default)\n");
    fprintf(stderr, "  OMP_PLACES=cores      One thread per physical core\n");
    fprintf(stderr, "  OMP_NUM_THREADS=N     Override thread count\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Output: CSV to stdout with columns:\n");
    fprintf(stderr, "  size_kb           - Per-thread buffer size (KB)\n");
    fprintf(stderr, "  operation         - read, write, copy, or latency\n");
    fprintf(stderr, "  bandwidth_mb_s    - Aggregate bandwidth in MB/s (0 for latency)\n");
    fprintf(stderr, "  latency_ns        - Median memory latency in ns (0 for bandwidth)\n");
    fprintf(stderr, "  latency_stddev_ns - Latency standard deviation in ns (0 for bandwidth)\n");
    fprintf(stderr, "  latency_samples   - Number of samples for latency measurement\n");
    fprintf(stderr, "  threads           - Thread count used\n");
    fprintf(stderr, "  iterations        - Iterations performed\n");
    fprintf(stderr, "  elapsed_s         - Elapsed time in seconds\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Latency measurement uses linked list traversal with random node order\n");
    fprintf(stderr, "to defeat prefetchers. Statistical validity ensured via multiple samples\n");
    fprintf(stderr, "until coefficient of variation < 5%% or max samples reached.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Memory model: each thread gets its own buffer.\n");
    fprintf(stderr, "Total memory = size_kb × threads (×2 for copy: src + dst).\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Compile with -DUSE_NUMA -lnuma for explicit NUMA allocation.\n");
}

int main(int argc, char *argv[]) {
    int opt;
    int ops_specified = 0;  /* Track if -o was used */

    while ((opt = getopt(argc, argv, "hvfas:t:r:p:o:VHR")) != -1) {
        switch (opt) {
            case 'h':
                usage(argv[0]);
                return 0;
            case 'V':
                printf("%s\n", VERSION);
                return 0;
            case 'v':
                g_verbose++;
                break;
            case 'f':
                g_full_sweep = 1;
                break;
            case 'a':
                g_auto_scaling = 1;
                break;
            case 'r':
                g_benchmark_tries = atoi(optarg);
                if (g_benchmark_tries < 1) g_benchmark_tries = 1;
                break;
            case 'p':
                g_explicit_threads = atoi(optarg);
                if (g_explicit_threads < 1) {
                    fprintf(stderr, "Invalid thread count: %s\n", optarg);
                    return 1;
                }
                break;
            case 's': {
                long size_kb = atol(optarg);
                if (size_kb <= 0) {
                    fprintf(stderr, "Invalid size: %s\n", optarg);
                    return 1;
                }
                g_single_size = (size_t)size_kb * 1024;  /* Convert KB to bytes */
                break;
            }
            case 't':
                g_max_runtime = atof(optarg);
                if (g_max_runtime < 0) {
                    fprintf(stderr, "Invalid runtime: %s (use 0 for unlimited)\n", optarg);
                    return 1;
                }
                break;
            case 'o': {
                /* First -o clears the default "all" mask */
                if (!ops_specified) {
                    g_ops_mask = 0;
                    ops_specified = 1;
                }
                /* Parse operation name */
                if (strcmp(optarg, "read") == 0) {
                    g_ops_mask |= (1 << OP_READ);
                } else if (strcmp(optarg, "write") == 0) {
                    g_ops_mask |= (1 << OP_WRITE);
                } else if (strcmp(optarg, "copy") == 0) {
                    g_ops_mask |= (1 << OP_COPY);
                } else if (strcmp(optarg, "latency") == 0) {
                    g_ops_mask |= (1 << OP_LATENCY);
                } else {
                    fprintf(stderr, "Invalid operation: %s (use: read, write, copy, latency)\n", optarg);
                    return 1;
                }
                break;
            }
            case 'H':
                g_use_hugepages = 1;
                break;
            case 'R':
                g_human_readable = 1;
                break;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    /* Initialize */
    srand((unsigned int)time(NULL));  /* Seed RNG for pointer chain randomization */
    init_system_info();
    init_numa();

    /* Run benchmarks */
    run_all_benchmarks();

    /* Cleanup */
    cleanup_hwloc();

    return 0;
}
