/*
 * Copyright 2026 Spare Cores
 * Licensed under Mozilla Public License 2.0
 */

#include "platform.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* ============================================================================
 * Detected machine
 * ============================================================================ */

platform_info_t g_platform;

/* NUMA topology - CPUs per node for balanced thread distribution */
#define MAX_NUMA_NODES 64
#define MAX_CPUS_PER_NODE 512
static int g_cpus_per_node[MAX_NUMA_NODES];           /* Count of CPUs on each node */
static int g_node_cpus[MAX_NUMA_NODES][MAX_CPUS_PER_NODE];  /* CPU IDs for each node */

/* Minimum total buffer size - adaptive based on cache topology */
static size_t g_min_total_size = 4096;  /* Default 4KB, updated after cache detection */

/* ============================================================================
 * Cache topology detection using hwloc (portable: x86, arm64, etc.)
 *
 * Install hwloc:
 *   Debian/Ubuntu: apt-get install libhwloc-dev
 *   RHEL/CentOS:   yum install hwloc-devel
 *   macOS:         brew install hwloc
 * ============================================================================ */

#ifdef USE_HWLOC
#include <hwloc.h>

static hwloc_topology_t g_topology = NULL;

/* Detect cache sizes using hwloc */
static void init_cache_info(void) {
    if (hwloc_topology_init(&g_topology) < 0) {
        goto use_defaults;
    }

    if (hwloc_topology_load(g_topology) < 0) {
        hwloc_topology_destroy(g_topology);
        g_topology = NULL;
        goto use_defaults;
    }

    /* Find cache sizes by iterating through cache objects */
    int depth;

    /* L1 Data Cache */
    depth = hwloc_get_type_depth(g_topology, HWLOC_OBJ_L1CACHE);
    if (depth != HWLOC_TYPE_DEPTH_UNKNOWN) {
        hwloc_obj_t obj = hwloc_get_obj_by_depth(g_topology, depth, 0);
        if (obj && obj->attr && obj->attr->cache.type != HWLOC_OBJ_CACHE_INSTRUCTION) {
            g_platform.l1_cache_size = obj->attr->cache.size;
        }
    }

    /* L2 Cache */
    depth = hwloc_get_type_depth(g_topology, HWLOC_OBJ_L2CACHE);
    if (depth != HWLOC_TYPE_DEPTH_UNKNOWN) {
        hwloc_obj_t obj = hwloc_get_obj_by_depth(g_topology, depth, 0);
        if (obj && obj->attr) {
            g_platform.l2_cache_size = obj->attr->cache.size;
        }
    }

    /* L3 Cache */
    depth = hwloc_get_type_depth(g_topology, HWLOC_OBJ_L3CACHE);
    if (depth != HWLOC_TYPE_DEPTH_UNKNOWN) {
        hwloc_obj_t obj = hwloc_get_obj_by_depth(g_topology, depth, 0);
        if (obj && obj->attr) {
            g_platform.l3_cache_size = obj->attr->cache.size;
        }
    }

    /* Count total L3 cache (sum across all L3 objects for distributed caches) */
    if (g_platform.l3_cache_size > 0) {
        depth = hwloc_get_type_depth(g_topology, HWLOC_OBJ_L3CACHE);
        int num_l3 = hwloc_get_nbobjs_by_depth(g_topology, depth);
        if (g_verbose && num_l3 > 1) {
            fprintf(stderr, "Note: %d L3 caches detected (distributed across dies)\n", num_l3);
        }
    }

use_defaults:
    /* Set defaults if detection failed */
    if (g_platform.l1_cache_size == 0) g_platform.l1_cache_size = 32 * 1024;      /* 32 KB */
    if (g_platform.l2_cache_size == 0) g_platform.l2_cache_size = 256 * 1024;     /* 256 KB */
    if (g_platform.l3_cache_size == 0) g_platform.l3_cache_size = 8 * 1024 * 1024; /* 8 MB */

    /* Calculate adaptive minimum total size:
     * Use 16KB per thread × num_cpus so each thread has a reliable buffer size.
     * This ensures all CPUs can participate with meaningful measurements. */
    g_min_total_size = 16384 * g_platform.num_cpus;  /* 16KB per thread minimum */

    if (g_verbose) {
        fprintf(stderr, "Cache (hwloc): L1d=%zuKB, L2=%zuKB, L3=%zuKB (per core)\n",
                g_platform.l1_cache_size / 1024, g_platform.l2_cache_size / 1024, g_platform.l3_cache_size / 1024);
        fprintf(stderr, "Minimum total test size: %zu KB (16KB × %d CPUs)\n",
                g_min_total_size / 1024, g_platform.num_cpus);
    }
}

static void cleanup_hwloc(void) {
    if (g_topology) {
        hwloc_topology_destroy(g_topology);
        g_topology = NULL;
    }
}

#else /* !USE_HWLOC - fallback to platform-specific methods */

#ifdef PLATFORM_LINUX
/* Parse cache size from sysfs (handles "48K", "1024K", "32768K" format) */
static size_t parse_cache_size_sysfs(const char *str) {
    size_t size = 0;
    char unit = 0;
    if (sscanf(str, "%zu%c", &size, &unit) >= 1) {
        if (unit == 'K' || unit == 'k') size *= 1024;
        else if (unit == 'M' || unit == 'm') size *= 1024 * 1024;
    }
    return size;
}

/* Read cache info from sysfs (Linux-specific) */
static void init_cache_info_linux(void) {
    char path[256];
    char buf[64];
    FILE *f;

    for (int index = 0; index < 10; index++) {
        /* Read level */
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu0/cache/index%d/level", index);
        f = fopen(path, "r");
        if (!f) continue;
        int level = -1;
        if (fgets(buf, sizeof(buf), f)) level = atoi(buf);
        fclose(f);
        if (level < 0) continue;

        /* Read type */
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu0/cache/index%d/type", index);
        f = fopen(path, "r");
        if (!f) continue;
        char type[32] = "";
        if (fgets(type, sizeof(type), f)) type[strcspn(type, "\n")] = 0;
        fclose(f);

        /* Skip instruction caches */
        if (strcmp(type, "Instruction") == 0) continue;

        /* Read size */
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu0/cache/index%d/size", index);
        f = fopen(path, "r");
        if (!f) continue;
        size_t size = 0;
        if (fgets(buf, sizeof(buf), f)) size = parse_cache_size_sysfs(buf);
        fclose(f);

        if (size == 0) continue;

        switch (level) {
            case 1: if (g_platform.l1_cache_size == 0) g_platform.l1_cache_size = size; break;
            case 2: if (g_platform.l2_cache_size == 0) g_platform.l2_cache_size = size; break;
            case 3: if (g_platform.l3_cache_size == 0) g_platform.l3_cache_size = size; break;
        }
    }
}
#endif /* PLATFORM_LINUX */

#ifdef PLATFORM_MACOS
/* Read cache info from sysctl (macOS-specific) */
static void init_cache_info_macos(void) {
    size_t size;
    size_t len = sizeof(size);

    /* L1 data cache */
    if (sysctlbyname("hw.l1dcachesize", &size, &len, NULL, 0) == 0 && size > 0) {
        g_platform.l1_cache_size = size;
    }

    /* L2 cache */
    len = sizeof(size);
    if (sysctlbyname("hw.l2cachesize", &size, &len, NULL, 0) == 0 && size > 0) {
        g_platform.l2_cache_size = size;
    }

    /* L3 cache (may not exist on all Macs) */
    len = sizeof(size);
    if (sysctlbyname("hw.l3cachesize", &size, &len, NULL, 0) == 0 && size > 0) {
        g_platform.l3_cache_size = size;
    }
}
#endif /* PLATFORM_MACOS */

#ifdef PLATFORM_BSD
/* Read cache info from sysctl (BSD-specific) */
static void init_cache_info_bsd(void) {
    /* FreeBSD and other BSDs have limited sysctl cache info.
     * Try standard hw.cacheXXX values, fall back to defaults. */
    size_t size;
    size_t len = sizeof(size);

    /* Try various BSD sysctl names */
    if (sysctlbyname("hw.l1dcachesize", &size, &len, NULL, 0) == 0 && size > 0) {
        g_platform.l1_cache_size = size;
    }
    len = sizeof(size);
    if (sysctlbyname("hw.l2cachesize", &size, &len, NULL, 0) == 0 && size > 0) {
        g_platform.l2_cache_size = size;
    }
    len = sizeof(size);
    if (sysctlbyname("hw.l3cachesize", &size, &len, NULL, 0) == 0 && size > 0) {
        g_platform.l3_cache_size = size;
    }
}
#endif /* PLATFORM_BSD */

/* Platform-agnostic cache info initialization */
static void init_cache_info(void) {
    const char *method = "defaults";

#ifdef PLATFORM_LINUX
    init_cache_info_linux();
    method = "sysfs";
#endif

#ifdef PLATFORM_MACOS
    init_cache_info_macos();
    method = "sysctl";
#endif

#ifdef PLATFORM_BSD
    init_cache_info_bsd();
    method = "sysctl";
#endif

    /* Set defaults if detection failed */
    if (g_platform.l1_cache_size == 0) g_platform.l1_cache_size = 32 * 1024;      /* 32 KB */
    if (g_platform.l2_cache_size == 0) g_platform.l2_cache_size = 256 * 1024;     /* 256 KB */
    if (g_platform.l3_cache_size == 0) g_platform.l3_cache_size = 8 * 1024 * 1024; /* 8 MB */

    /* Calculate adaptive minimum total size:
     * Use 16KB per thread × num_cpus so each thread has a reliable buffer size. */
    g_min_total_size = 16384 * g_platform.num_cpus;  /* 16KB per thread minimum */

    if (g_verbose) {
        fprintf(stderr, "Cache (%s): L1d=%zuKB, L2=%zuKB, L3=%zuKB (per core)\n",
                method, g_platform.l1_cache_size / 1024, g_platform.l2_cache_size / 1024,
                g_platform.l3_cache_size / 1024);
        fprintf(stderr, "Minimum total test size: %zu KB (16KB × %d CPUs)\n",
                g_min_total_size / 1024, g_platform.num_cpus);
    }
}

static void cleanup_hwloc(void) {
    /* No-op when hwloc is not used */
}

#endif /* USE_HWLOC */

/* ============================================================================
 * NUMA support
 * ============================================================================ */

static void init_numa_topology(void) {
    /* Initialize topology arrays */
    memset(g_cpus_per_node, 0, sizeof(g_cpus_per_node));
    memset(g_node_cpus, 0, sizeof(g_node_cpus));

#ifdef USE_NUMA
    if (numa_available() >= 0 && g_platform.numa_nodes > 1) {
        /* Build CPU-to-node mapping using libnuma */
        for (int cpu = 0; cpu < g_platform.num_cpus && cpu < MAX_NUMA_NODES * MAX_CPUS_PER_NODE; cpu++) {
            int node = numa_node_of_cpu(cpu);
            if (node >= 0 && node < MAX_NUMA_NODES) {
                int idx = g_cpus_per_node[node];
                if (idx < MAX_CPUS_PER_NODE) {
                    g_node_cpus[node][idx] = cpu;
                    g_cpus_per_node[node]++;
                }
            }
        }

        if (g_verbose) {
            fprintf(stderr, "NUMA topology:\n");
            for (int node = 0; node < g_platform.numa_nodes; node++) {
                fprintf(stderr, "  Node %d: %d CPUs (first: %d, last: %d)\n",
                        node, g_cpus_per_node[node],
                        g_cpus_per_node[node] > 0 ? g_node_cpus[node][0] : -1,
                        g_cpus_per_node[node] > 0 ? g_node_cpus[node][g_cpus_per_node[node]-1] : -1);
            }
        }
    } else
#endif
    {
        /* UMA or NUMA not enabled: all CPUs on "node 0" */
        for (int cpu = 0; cpu < g_platform.num_cpus && cpu < MAX_CPUS_PER_NODE; cpu++) {
            g_node_cpus[0][cpu] = cpu;
        }
        g_cpus_per_node[0] = g_platform.num_cpus < MAX_CPUS_PER_NODE ? g_platform.num_cpus : MAX_CPUS_PER_NODE;
    }
}

static void init_numa(void) {
#ifdef USE_NUMA
    if (numa_available() >= 0) {
        g_platform.numa_nodes = numa_max_node() + 1;
        if (g_verbose) {
            fprintf(stderr, "NUMA: %d nodes detected (libnuma enabled)\n", g_platform.numa_nodes);
        }
    } else {
        g_platform.numa_nodes = 1;
        if (g_verbose) {
            fprintf(stderr, "NUMA: not available (libnuma enabled but no NUMA support)\n");
        }
    }
#else
    g_platform.numa_nodes = 1;
    if (g_verbose) {
        fprintf(stderr, "NUMA: disabled (compile with -DUSE_NUMA -lnuma to enable)\n");
    }
#endif

    /* Build NUMA topology after detecting nodes */
    init_numa_topology();
}


/* ============================================================================
 * System info
 * ============================================================================ */

static void init_system_info(void) {
    /* Get number of CPUs (POSIX, works on all platforms) */
    g_platform.num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (g_platform.num_cpus < 1) g_platform.num_cpus = 1;

    /* Get total memory (platform-specific methods) */
    g_platform.total_memory = 0;

#ifdef PLATFORM_LINUX
    /* Linux: sysconf is reliable */
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0) {
        g_platform.total_memory = (size_t)pages * (size_t)page_size;
    }
#endif

#ifdef PLATFORM_MACOS
    /* macOS: use sysctl hw.memsize */
    int64_t memsize = 0;
    size_t len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) == 0 && memsize > 0) {
        g_platform.total_memory = (size_t)memsize;
    }
#endif

#ifdef PLATFORM_BSD
    /* BSD: try hw.physmem or hw.realmem */
    unsigned long physmem = 0;
    size_t len = sizeof(physmem);
    if (sysctlbyname("hw.physmem", &physmem, &len, NULL, 0) == 0 && physmem > 0) {
        g_platform.total_memory = (size_t)physmem;
    } else {
        /* Fallback to sysconf */
        long pages = sysconf(_SC_PHYS_PAGES);
        long page_size = sysconf(_SC_PAGESIZE);
        if (pages > 0 && page_size > 0) {
            g_platform.total_memory = (size_t)pages * (size_t)page_size;
        }
    }
#endif

    /* Fallback if detection failed */
    if (g_platform.total_memory == 0) {
        long pages = sysconf(_SC_PHYS_PAGES);
        long page_size = sysconf(_SC_PAGESIZE);
        if (pages > 0 && page_size > 0) {
            g_platform.total_memory = (size_t)pages * (size_t)page_size;
        } else {
            g_platform.total_memory = 1024UL * 1024 * 1024;  /* Default 1GB */
        }
    }

    if (g_verbose) {
        fprintf(stderr, "System: %d CPUs, %.2f GB memory\n",
                g_platform.num_cpus, g_platform.total_memory / (1024.0 * 1024 * 1024));
    }

    /* Detect cache topology (must be called after g_platform.num_cpus is set) */
    init_cache_info();
}

void platform_init(void) {
    init_system_info();
    init_numa();
}

void platform_deinit(void) {
    cleanup_hwloc();
}
