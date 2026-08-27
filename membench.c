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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>

#include "membench.h"
#include "bandwidth.h"
#include "report.h"
#include "latency.h"
#include "platform.h"
#include "utils.h"

/* ============================================================================
 * Types
 * ============================================================================ */

const char *const OP_NAMES[] = {"read", "write", "copy", "latency"};

/* ============================================================================
 * Global state
 * ============================================================================ */

static volatile int g_running = 1;
int g_verbose = 0;  /* 0=quiet, 1=summary, 2=detailed */
static int g_full_sweep = 0;      /* If 1, test all sizes up to max; if 0, stop early when converged */
size_t g_single_size = 0;  /* If > 0, test only this size (in bytes) */
int g_human_readable = 0;  /* If 1, output human-readable format instead of CSV */
/* Number of times to run each benchmark, taking best result (like lmbench TRIES=11) */
#define DEFAULT_BENCHMARK_TRIES 3
int g_benchmark_tries = DEFAULT_BENCHMARK_TRIES;

/* Thread count options:
 * g_explicit_threads > 0: use exactly that many threads
 * g_explicit_threads == 0: use num_cpus (default)
 * g_auto_scaling: try multiple thread counts to find best */
int g_explicit_threads = 0;
static int g_auto_scaling = 0;

double g_max_runtime = DEFAULT_MAX_RUNTIME;

/* Huge pages support */
int g_use_hugepages = 0;

/* Operation selection bitmask (bit 0=read, 1=write, 2=copy, 3=latency) */
#define OP_MASK_ALL 0x0F  /* All operations enabled */
static int g_ops_mask = OP_MASK_ALL;

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
static int* get_thread_counts(const platform_info_t *pi, int *count) {
    int nproc = pi->num_cpus;
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
static size_t* get_sizes(const platform_info_t *pi, int *count) {
    int nthreads = g_explicit_threads > 0 ? g_explicit_threads : pi->num_cpus;
    if (nthreads < 1) nthreads = 1;

    /* Use detected cache sizes, with sensible defaults */
    size_t l1 = pi->l1_cache_size > 0 ? pi->l1_cache_size : 32768;      /* 32 KB */
    size_t l2 = pi->l2_cache_size > 0 ? pi->l2_cache_size : 262144;     /* 256 KB */
    size_t l3 = pi->l3_cache_size > 0 ? pi->l3_cache_size : 8388608;    /* 8 MB */

    /* Memory limit per thread */
    size_t max_size = pi->total_memory / 2 / nthreads;

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
static result_t find_best_config(const platform_info_t *pi, size_t buffer_size, operation_t op,
                                 int *thread_counts, int tc_count) {
    result_t best = {0};
    best.size = buffer_size;
    best.op = op;

    /* For latency test: single-thread, statistically valid measurement */
    if (op == OP_LATENCY) {
        size_t max_latency = MAX_LATENCY_SIZE;
        if (pi->total_memory / 4 < max_latency) {
            max_latency = pi->total_memory / 4;
        }
        size_t latency_size = (buffer_size > max_latency) ? max_latency : buffer_size;

        double start = get_time();
        latency_stats_t stats = measure_latency_stats(pi, latency_size);
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
            if (memory_needed > pi->total_memory / 4) {
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
        nthreads = pi->num_cpus;
    }

    /* Check memory limit and reduce threads if needed */
    int bufs_per_op = (op == OP_COPY) ? 2 : 1;
    size_t memory_needed = buffer_size * nthreads * bufs_per_op;
    while (nthreads > 1 && memory_needed > pi->total_memory / 4) {
        nthreads /= 2;
        memory_needed = buffer_size * nthreads * bufs_per_op;
    }

    best = run_benchmark_best(buffer_size, op, nthreads);
    best.size = buffer_size;

    return best;
}

static void run_all_benchmarks(const platform_info_t *pi) {
    double start_time = get_time();

    int tc_count;
    int *thread_counts = get_thread_counts(pi, &tc_count);

    /* Single size mode */
    if (g_single_size > 0) {
        if (g_verbose) {
            fprintf(stderr, "Testing buffer size: %zu KB per thread\n",
                    g_single_size / 1024);
        }

        print_csv_header();

        for (int op = 0; op < 4 && g_running; op++) {
            if (!(g_ops_mask & (1 << op))) continue;

            result_t best = find_best_config(pi, g_single_size, (operation_t)op,
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
        if (g_human_readable) print_summary(pi);
        return;
    }

    /* Normal mode: test all sizes */
    int size_count;
    size_t *sizes = get_sizes(pi, &size_count);

    if (g_verbose) {
        fprintf(stderr, "Testing %d buffer sizes (per thread, adaptive to cache hierarchy)\n", size_count);
        if (g_auto_scaling) {
            fprintf(stderr, "Thread mode: auto-scaling (trying 1-%d threads)\n", pi->num_cpus);
        } else if (g_explicit_threads > 0) {
            fprintf(stderr, "Thread mode: fixed %d threads\n", g_explicit_threads);
        } else {
            fprintf(stderr, "Thread mode: num_cpus (%d threads)\n", pi->num_cpus);
        }
        fprintf(stderr, "OpenMP: proc_bind(spread) for NUMA-aware thread placement\n");
    }

    print_csv_header();

    for (int s = 0; s < size_count && g_running; s++) {
        size_t size = sizes[s];

        for (int op = 0; op < 4 && g_running; op++) {
            if (!(g_ops_mask & (1 << op))) continue;

            result_t best = find_best_config(pi, size, (operation_t)op,
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
    if (g_human_readable) print_summary(pi);
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

    srand((unsigned int)time(NULL));  /* Seed RNG for pointer chain randomization */

    platform_info_t platform_info;
    platform_init(&platform_info);

    run_all_benchmarks(&platform_info);

    platform_deinit();

    return 0;
}
