# sc-membench - Memory Bandwidth Benchmark

A portable, multi-platform memory bandwidth benchmark designed for comprehensive system analysis.

## Features

- **Multi-platform**: Works on x86, arm64, and other architectures
- **Multiple operations**: Measures read, write, copy bandwidth + memory latency
- **OpenMP parallelization**: Uses OpenMP for efficient multi-threaded bandwidth measurement
- **NUMA-aware**: Automatically handles NUMA systems with `proc_bind(spread)` thread placement
- **Cache-aware sizing**: Adaptive test sizes based on detected L1, L2, L3 cache hierarchy
- **Per-thread buffer model**: Like bw_mem, each thread gets its own buffer
- **Thread control**: Default uses all CPUs; optional auto-scaling to find optimal thread count
- **Latency measurement**: True memory latency using pointer chasing with statistical sampling
- **Statistically valid**: Latency reports median, stddev, and sample count (CV < 5%)
- **Best-of-N runs**: Bandwidth tests run multiple times, reports best result (like lmbench)
- **CSV output**: Machine-readable output for analysis

## Quick Start

```bash
# Compile
make

# Run with default settings (uses all CPUs, cache-aware sizes)
./membench

# Run with verbose output and 5 minute time limit
./membench -v -t 300

# Test specific buffer size (1MB per thread)
./membench -s 1024

# Compile with NUMA support (requires libnuma-dev)
make numa
./membench-numa -v
```

## Docker Usage

The easiest way to run sc-membench without building is using the pre-built Docker image:

```bash
# Run with default settings
docker run --rm ghcr.io/sparecores/membench:main

# Run with verbose output and time limit
docker run --rm ghcr.io/sparecores/membench:main -v -t 300

# Test specific buffer size
docker run --rm ghcr.io/sparecores/membench:main -s 1024

# Recommended: use --privileged and huge pages for best accuracy
docker run --rm --privileged ghcr.io/sparecores/membench:main -H -v

# Save output to file
docker run --rm --privileged ghcr.io/sparecores/membench:main -H > results.csv
```

**Notes:**
- The `--privileged` flag is recommended for optimal CPU pinning and NUMA support
- The `-H` flag enables huge pages automatically for large buffers (≥ 2× huge page size), no setup required

## Packaging

sc-membench is packaged in some distributions. For current packaging status and version information across repositories, see [repology.org/project/sc-membench/versions](https://repology.org/project/sc-membench/versions).

## Build Options

```bash
make              # Basic version (sysfs cache detection, Linux only)
make hwloc        # With hwloc 2 (recommended - portable cache detection)
make numa         # With NUMA support
make full         # With hwloc + NUMA (recommended for servers)
make all          # Build all versions
make clean        # Remove built files
make test         # Quick 30-second test run
```

### Recommended Build

For production use on servers, build with all features:

```bash
# Install dependencies first
sudo apt-get install libhugetlbfs-dev libhwloc-dev libnuma-dev  # Debian/Ubuntu
# or: sudo yum install libhugetlbfs-devel hwloc-devel numactl-devel  # RHEL/CentOS

# Build with full features
make full
./membench-full -v
```

## Usage

```
sc-membench - Memory Bandwidth Benchmark

Usage: ./membench [options]

Options:
  -h          Show help
  -v          Verbose output (use -vv for more detail)
  -s SIZE_KB  Test only this buffer size (in KB), e.g. -s 1024 for 1MB
  -r TRIES    Repeat each test N times, report best (default: 3)
  -f          Full sweep (test larger sizes up to memory limit)
  -p THREADS  Use exactly this many threads (default: num_cpus)
  -a          Auto-scaling: try different thread counts to find best
              (slower but finds optimal thread count per buffer size)
  -t SECONDS  Maximum runtime, 0 = unlimited (default: unlimited)
  -o OP       Run only this operation: read, write, copy, or latency
              Can be specified multiple times (default: all)
  -H          Enable huge pages for large buffers (>= 2x huge page size)
              Uses THP automatically, no setup required
  -R          Human-readable output with summary and benchmark scores
              (default: CSV output)
```

## Output Format

CSV output to stdout with columns:

| Column | Description |
|--------|-------------|
| `size_kb` | **Per-thread** buffer size (KB) |
| `operation` | Operation type: `read`, `write`, `copy`, or `latency` |
| `bandwidth_mb_s` | Aggregate bandwidth across all threads (MB/s), 0 for latency |
| `latency_ns` | Median memory latency (nanoseconds), 0 for bandwidth tests |
| `latency_stddev_ns` | Standard deviation of latency samples (nanoseconds), 0 for bandwidth |
| `latency_samples` | Number of samples collected for latency measurement, 0 for bandwidth |
| `threads` | Thread count used |
| `iterations` | Number of iterations performed |
| `elapsed_s` | Elapsed time for the test (seconds) |

**Total memory used** = `size_kb × threads` (or `× 2` for copy which needs src + dst).

### Example Output

```csv
size_kb,operation,bandwidth_mb_s,latency_ns,latency_stddev_ns,latency_samples,threads,iterations,elapsed_s
32,read,9309701.64,0,0,0,96,292056,0.094113
32,write,9868845.93,0,0,0,96,578703,0.175918
32,latency,0,1.77,0.00,7,1,7,0.254053
128,read,6410473.70,0,0,0,96,83556,0.156412
128,write,9883443.78,0,0,0,96,177556,0.215580
128,latency,0,3.93,0.00,7,1,7,0.689736
512,latency,0,5.66,0.01,7,1,7,0.654846
1024,latency,0,7.38,0.04,7,1,7,0.671615
32768,latency,0,44.90,0.03,7,1,7,1.050579
131072,latency,0,96.78,3.00,7,1,7,8.520152
262144,latency,0,122.22,0.90,7,1,7,21.756578
```

In this example ([Azure D96pls_v6](https://sparecores.com/server/azure/Standard_D96pls_v6) with 96 ARM cores, 64KB L1, 1MB L2, 128MB L3):
- **32KB**: Fits in L1 → very high bandwidth (~9.3 TB/s read), low latency (~1.8ns, stddev 0.00)
- **512KB**: Fits in L2 → good latency (~5.7ns, stddev 0.01)
- **32MB**: In L3 → moderate latency (~45ns, stddev 0.03)
- **128MB**: At L3 boundary → RAM latency visible (~97ns, stddev 3.0)
- **256MB**: Past L3 → pure RAM latency (~122ns, stddev 0.9)

## Human-Readable Output (`-R`)

Use `-R` for a formatted table with summary statistics and benchmark scores instead of CSV:

```bash
./membench -R
```

### Example Output

```
Size       Op          Bandwidth      Latency  Threads
----       --          ---------      -------  -------
32 KB      read         2.6 TB/s            -       32
32 KB      write        1.6 TB/s            -       32
32 KB      copy       464.4 GB/s            -       32
32 KB      latency             -       0.9 ns        1
128 KB     read         1.7 TB/s            -       32
128 KB     write      691.7 GB/s            -       32
128 KB     copy       495.5 GB/s            -       32
128 KB     latency             -       2.4 ns        1
...

================================================================================
                           BENCHMARK SUMMARY
================================================================================

BANDWIDTH (MB/s):
  Operation          Peak Weighted Avg
  ---------          ---- ------------
  Read            2612561      1680432
  Write           1605601       850445
  Copy             495476       372027

LATENCY:
  Best latency: 97.2 ns (RAM) at 131072 KB buffer

--------------------------------------------------------------------------------
BENCHMARK SCORE (higher is better):

  Bandwidth Score:      1571.2  (avg peak bandwidth in GB/s)
  Latency Score:          10.3  (1000 / latency_ns)

  >> COMBINED SCORE:      4024  (sqrt(bw_score × latency_score) × 100)
--------------------------------------------------------------------------------
```

### Summary Statistics

| Metric | Description |
|--------|-------------|
| **Peak** | Highest bandwidth achieved across all buffer sizes |
| **Weighted Avg** | Average weighted by log₂(size) — larger buffers count more |
| **Best latency** | Latency at the largest buffer size tested (closest to true RAM latency) |

### Benchmark Scores

The summary includes scores for easy comparison between systems:

| Score | Formula | Description |
|-------|---------|-------------|
| **Bandwidth Score** | `avg(peak_read, peak_write, peak_copy) / 1000` | Average peak bandwidth in GB/s |
| **Latency Score** | `1000 / latency_ns` | Inverse of RAM latency (higher = faster) |
| **Combined Score** | `sqrt(bw_score × latency_score) × 100` | Geometric mean of both (balanced) |

The **Combined Score** uses a geometric mean so that neither bandwidth nor latency dominates — both contribute equally to the final score.

### Score Comparability Warning

When using options that affect test coverage, a warning is displayed:

```
WARNING: Scores may not be comparable due to non-default options:
  - Time limit (-t 60) may have prevented testing larger buffer sizes
  - Fixed thread count (-p 4) instead of using all CPUs (32)
For comparable scores, run without -t, -p, or -s options.
```

**For comparable benchmark scores**, run without `-t`, `-p`, or `-s` options to ensure:
- All buffer sizes are tested (including large RAM-sized buffers)
- All CPUs are utilized (maximum bandwidth)
- Full cache hierarchy is exercised

## Operations Explained

### Read (`read`)
Reads all 64-bit words from the buffer using XOR (faster than addition, no carry chains). This measures pure read bandwidth.

```c
checksum ^= buffer[i];  // For all elements, using 8 independent accumulators
```

### Write (`write`)
Writes a pattern to all 64-bit words in the buffer. This measures pure write bandwidth.

```c
buffer[i] = pattern;  // For all elements
```

### Copy (`copy`)
Copies data from source to destination buffer. Reports bandwidth as `buffer_size / time` (matching lmbench's approach), not `2 × buffer_size / time`.

```c
dst[i] = src[i];  // For all elements
```

**Note:** Copy bandwidth is typically lower than read or write alone because it performs both operations. The reported bandwidth represents the buffer size traversed, not total bytes moved (read + write).

### Latency (`latency`)
Measures true memory access latency using **pointer chasing** with a linked list traversal approach inspired by [ram_bench](https://github.com/emilk/ram_bench) by Emil Ernerfeldt. Each memory access depends on the previous one, preventing CPU pipelining and prefetching.

```c
// Node structure (16 bytes) - realistic for linked list traversal
struct Node {
    uint64_t payload;  // Dummy data for realistic cache behavior
    Node *next;        // Pointer to next node
};

// Each load depends on previous (can't be optimized away)
node = node->next;  // Address comes from previous load
```

The buffer is initialized as a contiguous array of nodes linked in **randomized order** to defeat hardware prefetchers. This measures:
- L1/L2/L3 cache hit latency at small sizes
- DRAM access latency at large sizes
- True memory latency without pipelining effects

**Statistical validity**: The latency measurement collects **multiple independent samples** (7-21) and reports the **median** (robust to outliers) along with standard deviation. Sampling continues until coefficient of variation < 5% or maximum samples reached.

**CPU and NUMA pinning**: The latency test pins to CPU 0 and allocates memory on the local NUMA node (when compiled with NUMA support) for consistent, reproducible results.

Results are reported in **nanoseconds per access** with statistical measures:
- `latency_ns`: Median latency (robust central tendency)
- `latency_stddev_ns`: Standard deviation (measurement precision indicator)
- `latency_samples`: Number of samples collected (statistical effort)

**Large L3 cache support**: The latency test uses buffers up to 2GB (or 25% of RAM) to correctly measure DRAM latency even on processors with huge L3 caches like AMD EPYC 9754 (1.1GB L3 with 3D V-Cache).

## Memory Sizes Tested

The benchmark tests **per-thread buffer sizes** at cache transition points, automatically adapting to the detected cache hierarchy:

### Adaptive Cache-Aware Sizes

Based on detected L1, L2, L3 cache sizes (typically 10 sizes):

| Size | Purpose |
|------|---------|
| L1/2 | Pure L1 cache performance (e.g., 32KB for 64KB L1) |
| 2×L1 | L1→L2 transition |
| L2/2 | Mid L2 cache performance |
| L2 | L2 cache boundary |
| 2×L2 | L2→L3 transition |
| L3/4 | Mid L3 cache (for large L3 caches) |
| L3/2 | Late L3 cache |
| L3 | L3→RAM boundary |
| 2×L3 | Past L3, hitting RAM |
| 4×L3 | Deep into RAM |

With `-f` (full sweep), additional larger sizes are tested up to the memory limit.

### Cache Detection

With hwloc 2 (recommended), cache sizes are detected automatically on any platform.
Without hwloc, the benchmark uses sysctl (macOS/BSD) or parses `/sys/devices/system/cpu/*/cache/` (Linux).

If cache detection fails, sensible defaults are used (32KB L1, 256KB L2, 8MB L3).

## Thread Model (Per-Thread Buffers)

Like bw_mem, each thread gets its **own private buffer**:

```
Example for 1MB buffer size with 4 threads (read/write):
  Thread 0: 1MB buffer
  Thread 1: 1MB buffer
  Thread 2: 1MB buffer
  Thread 3: 1MB buffer
  Total memory: 4MB

Example for 1MB buffer size with 4 threads (copy):
  Thread 0: 1MB src + 1MB dst = 2MB
  Thread 1: 1MB src + 1MB dst = 2MB
  ...
  Total memory: 8MB
```

### Thread Modes

| Mode | Flag | Behavior |
|------|------|----------|
| **Default** | (none) | Use `num_cpus` threads |
| **Explicit** | `-p N` | Use exactly N threads |
| **Auto-scaling** | `-a` | Try 1, 2, 4, ..., num_cpus threads, report best |

### OpenMP Thread Affinity

You can fine-tune thread placement using OpenMP environment variables:

```bash
# Spread threads across NUMA nodes (default behavior)
OMP_PROC_BIND=spread OMP_PLACES=cores ./membench

# Bind threads close together (may reduce bandwidth on multi-socket)
OMP_PROC_BIND=close OMP_PLACES=cores ./membench

# Override thread count via environment
OMP_NUM_THREADS=8 ./membench
```

| Variable | Values | Effect |
|----------|--------|--------|
| `OMP_PROC_BIND` | `spread`, `close`, `master` | Thread distribution strategy |
| `OMP_PLACES` | `cores`, `threads`, `sockets` | Placement units |
| `OMP_NUM_THREADS` | Integer | Override thread count |

The default `proc_bind(spread)` in the code distributes threads evenly across NUMA nodes for maximum memory bandwidth.

### What the Benchmark Measures

- **Aggregate bandwidth**: Sum of all threads' bandwidth
- **Per-thread buffer**: Each thread works on its own memory region
- **No sharing**: Threads don't contend for the same cache lines

### Interpreting Results

- `size_kb` = buffer size per thread
- `threads` = number of threads used
- `bandwidth_mb_s` = total system bandwidth (all threads combined)
- Total memory = `size_kb × threads` (×2 for copy)

## NUMA Support

When compiled with `-DUSE_NUMA` and linked with `-lnuma`:

- Detects NUMA topology automatically
- Maps CPUs to their NUMA nodes
- Load-balances threads across NUMA nodes
- Binds each thread's memory to its local node
- Works transparently on UMA (single-node) systems

### NUMA Load Balancing

On multi-socket systems, OpenMP's `proc_bind(spread)` distributes threads **evenly across NUMA nodes** to ensure balanced utilization of all memory controllers.

**Example: 128 threads on a 2-node system (96 CPUs per node):**

```
Without spread (may cluster):           With proc_bind(spread):
  Thread 0-95  → Node 0 (96 threads)      Threads spread evenly across nodes
  Thread 96-127 → Node 1 (32 threads)     ~64 threads per node
  Result: Node 0 overloaded!              Result: Balanced utilization!
```

**Impact:**
- Higher bandwidth with balanced distribution
- More accurate measurement of total system memory bandwidth
- Exercises all memory controllers evenly

### NUMA-Local Memory

Each thread allocates its buffer directly on its local NUMA node using `numa_alloc_onnode()`:

```c
// Inside OpenMP parallel region with proc_bind(spread)
int cpu = sched_getcpu();
int node = numa_node_of_cpu(cpu);
buffer = numa_alloc_onnode(size, node);
```

This ensures:
- Memory is allocated on the same node as the accessing CPU
- No cross-node memory access penalties
- No memory migrations during the benchmark

### Verbose Output

Use `-v` to see the detected NUMA topology:

```
NUMA: 2 nodes detected (libnuma enabled)
NUMA topology:
  Node 0: 96 CPUs (first: 0, last: 95)
  Node 1: 96 CPUs (first: 96, last: 191)
```

## Huge Pages Support

Use `-H` to enable huge pages (2MB instead of 4KB). This reduces TLB (Translation Lookaside Buffer) pressure, which is especially beneficial for:

- **Large buffer tests**: A 2GB buffer needs 512K page table entries with 4KB pages, but only 1024 with 2MB huge pages
- **Latency tests**: Random pointer-chasing access patterns cause many TLB misses with small pages
- **Accurate measurements**: TLB overhead can distort results, making memory appear slower than it is

### Automatic and smart

The `-H` option is designed to "just work":

1. **Automatic threshold**: Huge pages are only used for buffers ≥ 2× huge page size (typically 4MB on systems with 2MB huge pages). The huge page size is detected dynamically via `libhugetlbfs`. Smaller buffers use regular pages automatically (no wasted memory, no user intervention needed).

2. **No setup required**: The benchmark uses **Transparent Huge Pages (THP)** via `madvise(MADV_HUGEPAGE)`, which is handled automatically by the Linux kernel. No root access or pre-allocation needed.

3. **Graceful fallback**: If THP isn't available, the benchmark falls back to regular pages transparently.

### How it works

When `-H` is enabled and buffer size ≥ threshold (2× huge page size):

1. **First tries explicit huge pages** (`MAP_HUGETLB`) for deterministic huge pages
2. **Falls back to THP** (`madvise(MADV_HUGEPAGE)`) which works without pre-configuration
3. **Falls back to regular pages** if neither is available

### Optional: Pre-allocating explicit huge pages

For the most deterministic results, you can pre-allocate explicit huge pages:

```bash
# Check current huge page status
grep Huge /proc/meminfo

# Calculate huge pages needed for BANDWIDTH tests (read/write/copy):
#   threads × buffer_size × 2 (for copy: src+dst) / 2MB
#
# Examples:
#   8 CPUs,  256 MiB buffer:   8 × 256 × 2 / 2 =  2,048 pages (4 GB)
#   64 CPUs, 256 MiB buffer:  64 × 256 × 2 / 2 = 16,384 pages (32 GB)
#  192 CPUs, 256 MiB buffer: 192 × 256 × 2 / 2 = 49,152 pages (96 GB)
#
# LATENCY tests run single-threaded, so need much less:
#   256 MiB buffer: 256 / 2 = 128 pages (256 MB)

# Allocate huge pages (requires root) - adjust for your system
echo 49152 | sudo tee /proc/sys/vm/nr_hugepages

# Run with huge pages (will use explicit huge pages if available)
./membench -H -v
```

However, this is **optional** - THP works well for most use cases without any setup, and doesn't require pre-allocation. If explicit huge pages run out, the benchmark automatically falls back to THP.

### Usage recommendation

Just add `-H` to your command line - the benchmark handles everything automatically:

```bash
# Recommended for production benchmarking
./membench -H

# With verbose output to see what's happening
./membench -H -v
```

The benchmark will use huge pages only where they help (large buffers) and regular pages where they don't (small buffers).

### Why latency improves more than bandwidth

You may notice that `-H` dramatically improves latency measurements (often 20-40% lower) while bandwidth stays roughly the same. This is expected:

**Latency tests** use pointer chasing - random jumps through memory. Each access requires address translation via the TLB (Translation Lookaside Buffer):

| Buffer Size | 4KB pages | 2MB huge pages |
|-------------|-----------|----------------|
| 128 MB | 32,768 pages | 64 pages |
| TLB fit? | No (TLB ~1000-2000 entries) | Yes |
| TLB misses | Frequent | Rare |

With 4KB pages on a 128MB buffer:
- 32,768 pages can't fit in the TLB
- Random pointer chasing causes frequent TLB misses
- Each TLB miss adds **10-20+ CPU cycles** (page table walk)
- Measured latency = true memory latency + TLB overhead

With 2MB huge pages:
- Only 64 pages easily fit in the TLB
- Almost no TLB misses
- Measured latency ≈ **true memory latency**

### Real-world benchmark results

#### Azure D96pls_v6 (ARM)

Measured on [**Azure D96pls_v6**](https://sparecores.com/server/azure/Standard_D96pls_v6) (96 ARM Neoverse-N2 cores, 2 NUMA nodes, L1d=64KB/core, L2=1MB/core, L3=128MB shared):

| Buffer | No Huge Pages | With THP (-H) | Improvement |
|--------|---------------|---------------|-------------|
| 32 KB  | 1.77 ns | 1.77 ns | HP not used (< 4MB) |
| 128 KB | 3.95 ns | 3.95 ns | HP not used (< 4MB) |
| 512 KB | 5.99 ns | 5.98 ns | HP not used (< 4MB) |
| 1 MB   | 11.52 ns | 10.92 ns | HP not used (< 4MB) |
| 2 MB   | 24.27 ns | 24.65 ns | HP not used (< 4MB) |
| **32 MB** | 44.90 ns | **36.23 ns** | **-19%** |
| **64 MB** | 49.40 ns | **40.77 ns** | **-17%** |
| **128 MB** | 92.50 ns | **78.32 ns** | **-15%** |
| **256 MB** | 121.92 ns | **107.65 ns** | **-12%** |
| **512 MB** | 140.97 ns | **118.74 ns** | **-16%** |

#### AWS c8a.metal-48xl (AMD)

Measured on [**AWS c8a.metal-48xl**](https://sparecores.com/server/aws/c8a.metal-48xl) (192 AMD EPYC 9R45 cores, 2 NUMA nodes, L1d=48KB/core, L2=1MB/core, L3=32MB/die):

| Buffer | No Huge Pages | With THP (-H) | Improvement |
|--------|---------------|---------------|-------------|
| 32 KB  | 0.89 ns | 0.89 ns | HP not used (< 4MB) |
| 128 KB | 2.43 ns | 2.45 ns | HP not used (< 4MB) |
| 512 KB | 3.32 ns | 3.35 ns | HP not used (< 4MB) |
| 1 MB   | 5.47 ns | 4.09 ns | HP not used (< 4MB) |
| 2 MB   | 8.85 ns | 8.85 ns | HP not used (< 4MB) |
| **8 MB** | 11.72 ns | **10.32 ns** | **-12%** |
| **16 MB** | 12.58 ns | **10.74 ns** | **-15%** |
| **32 MB** | **30.83 ns** | **11.29 ns** | **-63%** |
| **64 MB** | 84.81 ns | **75.25 ns** | **-11%** |
| **128 MB** | 117.75 ns | **105.45 ns** | **-10%** |

**Key observations:**
- **Small buffers (≤ 2MB)**: No significant difference — TLB can handle the page count
- **L3 boundary effect**: AMD shows **63% improvement at 32MB** (exactly at L3 size) — without huge pages, TLB misses make L3 appear like RAM!
- **L3 region**: 12-19% improvement with huge pages
- **RAM region**: 10-16% lower latency with huge pages
- **THP works automatically**: No pre-allocation needed, just use `-H`

**Bottom line**: Use `-H` for accurate latency measurements on large buffers. Without huge pages, TLB overhead can severely distort results, especially at cache boundaries.

**Bandwidth tests** don't improve as much because:
- Sequential access has better TLB locality (same pages accessed repeatedly)
- Hardware prefetchers hide TLB miss latency
- The memory bus is already saturated

## Consistent Results

Achieving consistent benchmark results on modern multi-core systems requires careful handling of:

### Thread Pinning

Threads are distributed across CPUs using OpenMP's `proc_bind(spread)` clause, which spreads threads evenly across NUMA nodes and physical cores. This prevents the OS scheduler from migrating threads between cores, which causes huge variability.

### NUMA-Aware Memory

On NUMA systems, each thread allocates memory directly on its local NUMA node using `numa_alloc_onnode()`. OpenMP's `proc_bind(spread)` ensures threads are distributed across NUMA nodes, then each thread allocates locally. This ensures:
- Memory is close to where it will be accessed
- No cross-node memory access penalties
- No memory migrations during the benchmark

### Bandwidth: Best-of-N Runs

Like lmbench (TRIES=11), each bandwidth test configuration runs multiple times and reports the best result:

1. First run is a warmup (discarded) to stabilize CPU frequency
2. Each configuration is then tested 3 times (configurable with `-r`)
3. Highest bandwidth is reported (best shows true hardware capability)

### Latency: Statistical Sampling

Latency measurements use a different approach optimized for statistical validity:

1. Thread is pinned to CPU 0 with NUMA-local memory
2. Multiple independent samples (7-21) are collected per measurement
3. Sampling continues until coefficient of variation < 5% or max samples reached
4. **Median** latency is reported (robust to outliers)
5. Standard deviation and sample count are included for validation

### Result

With these optimizations, benchmark variability is typically **<1%** (compared to 30-60% without them).

### Configuration

```bash
./membench -r 5    # Run each test 5 times instead of 3
./membench -r 1    # Single run (fastest, still consistent due to pinning)
./membench -p 16   # Use exactly 16 threads
./membench -a      # Auto-scale to find optimal thread count
```

## Comparison with lmbench

### Bandwidth (bw_mem)

| Aspect | sc-membench | lmbench bw_mem |
|--------|-------------|----------------|
| **Parallelism model** | OpenMP threads | Processes (fork) |
| **Buffer allocation** | Each thread has own buffer | Each process has own buffer |
| **Size reporting** | Per-thread buffer size | Per-process buffer size |
| **Read operation** | Reads 100% of data | `rd` reads 25% (strided) |
| **Copy reporting** | Buffer size / time | Buffer size / time |
| **Huge pages** | Built-in (`-H` flag) | Not supported (uses `valloc`) |
| **Operation selection** | `-o read/write/copy/latency` | Separate invocations per operation |
| **Output format** | CSV (stdout) | Text to stderr |
| **Full vs strided read** | Always 100% (`read`) | `rd` (25% strided) or `frd` (100%) |

**Key differences:**

1. **Size meaning**: Both report per-worker buffer size (comparable)
2. **Read operation**: bw_mem `rd` uses 32-byte stride (reads 25% of data at indices 0,4,8...124 per 512-byte chunk), reporting ~4x higher apparent bandwidth. Use `frd` for full read. sc-membench always reads 100%.
3. **Thread control**: sc-membench defaults to num_cpus threads; use `-a` for auto-scaling or `-p N` for explicit count
4. **Huge pages**: sc-membench has built-in support (`-H`) with automatic THP fallback; lmbench has no huge page support
5. **Workflow**: sc-membench runs all tests in one invocation; bw_mem requires separate runs per operation (`bw_mem 64m rd`, `bw_mem 64m wr`, etc.)

### Latency (lat_mem_rd)

sc-membench's `latency` operation is comparable to lmbench's `lat_mem_rd`:

| Aspect | sc-membench latency | lmbench lat_mem_rd |
|--------|---------------------|-------------------|
| **Method** | Pointer chasing (linked list) | Pointer chasing (array) |
| **Node structure** | 16 bytes (payload + pointer) | 8 bytes (pointer only) |
| **Pointer order** | Randomized (defeats prefetching) | Fixed backward stride (may be prefetched) |
| **Stride** | Random (visits all elements) | Configurable (default 64 bytes on 64-bit) |
| **Statistical validity** | Multiple samples, reports median + stddev | Single measurement |
| **CPU/NUMA pinning** | Pins to CPU 0, NUMA-local memory | No pinning |
| **Output** | Median nanoseconds + stddev + sample count | Nanoseconds |
| **Huge pages** | Built-in (`-H` flag) | Not supported |

Both measure memory latency using dependent loads that prevent pipelining.

**Key differences**:

1. **Prefetching vulnerability**: lat_mem_rd uses fixed backward stride, which modern CPUs may prefetch (the man page acknowledges: "vulnerable to smart, stride-sensitive cache prefetching policies"). sc-membench's randomized pointer chain defeats all prefetching, measuring true random-access latency.

2. **Statistical validity**: sc-membench collects 7-21 samples per measurement, reports median (robust to outliers) and standard deviation, and continues until coefficient of variation < 5%. This provides confidence in the results.

3. **Reproducibility**: CPU pinning and NUMA-local memory allocation eliminate variability from thread migration and remote memory access.

**Huge pages advantage**: With `-H`, sc-membench automatically uses huge pages for large buffers, eliminating TLB overhead that can inflate latency by 20-40% (see [benchmark results](#real-world-benchmark-results)).

## Interpreting Results

### Cache Effects
Look for bandwidth drops and latency increases as buffer sizes exceed cache levels:
- Dramatic change at L1 boundary (32-64KB per thread typically)
- Another change at L2 boundary (256KB-1MB per thread typically)
- Final change when total memory exceeds L3 (depends on thread count)

### Thread Configuration
- By default, all CPUs are used for maximum aggregate bandwidth
- Use `-p N` to test with a specific thread count
- Use `-a` to find optimal thread count (slower but thorough)
- Latency test: Always uses 1 thread (measures true access latency)

### Bandwidth Values
Typical modern systems:
- L1 cache: 200-500 GB/s (varies with frequency)
- L2 cache: 100-200 GB/s
- L3 cache: 50-100 GB/s
- Main memory: 20-100 GB/s (DDR4/DDR5, depends on channels)

### Latency Values
Typical modern systems:
- L1 cache: 1-2 ns
- L2 cache: 3-10 ns
- L3 cache: 10-40 ns (larger/3D V-Cache may be higher)
- Main memory: 25-50 ns (fast DDR5) to 60-120 ns (DDR4)

## Dependencies

### Build Requirements

- **Required**: C11 compiler with OpenMP support (gcc or clang)
- **Recommended**: hwloc 2.x for portable cache topology detection
- **Optional**: libnuma for NUMA support (Linux only)
- **Optional**: libhugetlbfs for huge page size detection (Linux only)

### Runtime Requirements

- **Required**: OpenMP runtime library (`libgomp1` on Debian/Ubuntu, `libgomp` on RHEL)
- **Optional**: libhwloc, libnuma, libhugetlbfs (same as build dependencies)

### Installing Dependencies

```bash
# Debian/Ubuntu - Build
apt-get install build-essential libhwloc-dev libnuma-dev libhugetlbfs-dev

# Debian/Ubuntu - Runtime only (e.g., Docker images)
apt-get install libgomp1 libhwloc15 libnuma1 libhugetlbfs-dev

# RHEL/CentOS/Fedora - Build
yum install gcc make hwloc-devel numactl-devel libhugetlbfs-devel

# RHEL/CentOS/Fedora - Runtime only
yum install libgomp hwloc-libs numactl-libs libhugetlbfs

# macOS (hwloc only, no NUMA)
brew install hwloc libomp
xcode-select --install

# FreeBSD (hwloc 2 required, not hwloc 1)
pkg install gmake hwloc2
```

### What Each Dependency Provides

| Library | Purpose | Platforms | Build/Runtime |
|---------|---------|-----------|---------------|
| **libgomp** | OpenMP runtime (parallel execution) | All | Both |
| **hwloc 2** | Cache topology detection (L1/L2/L3 sizes) | Linux, macOS, BSD | Both |
| **libnuma** | NUMA-aware memory allocation | Linux only | Both |
| **libhugetlbfs** | Huge page size detection | Linux only | Both |

**Note**: hwloc 2.x is required. hwloc 1.x uses a different API and is not supported.

Without hwloc, the benchmark falls back to sysctl (macOS/BSD) or `/sys/devices/system/cpu/*/cache/` (Linux).
Without libnuma, memory is allocated without NUMA awareness (may underperform on multi-socket systems).

## License

Mozilla Public License 2.0

## See Also

- [STREAM benchmark](https://www.cs.virginia.edu/stream/)
- [lmbench](https://sourceforge.net/projects/lmbench/)
- [ram_bench](https://github.com/emilk/ram_bench)

