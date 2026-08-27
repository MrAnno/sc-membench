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
#include "platform.h"
#include "runner.h"

/* ============================================================================
 * Types
 * ============================================================================ */

const char *const OP_NAMES[] = {"read", "write", "copy", "latency"};

/* ============================================================================
 * Global state
 * ============================================================================ */

int g_verbose = 0;  /* 0=quiet, 1=summary, 2=detailed */
int g_full_sweep = 0;      /* If 1, test all sizes up to max; if 0, stop early when converged */
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
int g_auto_scaling = 0;

double g_max_runtime = DEFAULT_MAX_RUNTIME;

/* Huge pages support */
int g_use_hugepages = 0;

/* Operation selection bitmask (bit 0=read, 1=write, 2=copy, 3=latency) */
#define OP_MASK_ALL 0x0F  /* All operations enabled */
int g_ops_mask = OP_MASK_ALL;

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
