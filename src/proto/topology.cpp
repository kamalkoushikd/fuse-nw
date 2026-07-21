// Enable the GNU CPU affinity macros (CPU_SET etc.) and pthread affinity.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "fuse/proto/topology.hpp"

#include <pthread.h>
#include <sched.h>

#if FUSE_HAVE_NUMA
#include <numa.h>
#endif

namespace fuse::proto {

bool pin_current_thread_to_core(int core) {
    if (core < 0 || core >= CPU_SETSIZE) {
        return false;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

std::vector<int> current_thread_affinity() {
    std::vector<int> cpus;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (pthread_getaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        return cpus;
    }
    for (int i = 0; i < CPU_SETSIZE; ++i) {
        if (CPU_ISSET(i, &set)) {
            cpus.push_back(i);
        }
    }
    return cpus;
}

bool numa_supported() {
#if FUSE_HAVE_NUMA
    return numa_available() != -1;
#else
    return false;
#endif
}

bool prefer_current_thread_numa_node(int node) {
#if FUSE_HAVE_NUMA
    if (node < 0 || numa_available() == -1) {
        return false;
    }
    // Prefer the node for subsequent allocations by this thread; a soft
    // policy so allocation still succeeds if the node is under pressure.
    numa_set_preferred(node);
    return true;
#else
    (void)node;
    return false;
#endif
}

} // namespace fuse::proto
