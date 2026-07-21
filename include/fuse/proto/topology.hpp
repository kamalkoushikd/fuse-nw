#ifndef FUSE_PROTO_TOPOLOGY_HPP
#define FUSE_PROTO_TOPOLOGY_HPP

// Stage 6 topology-aware placement (optional, deployment-specific).
//
// The protocol itself has no concept of GPUs, NUMA nodes, or any hardware
// topology. It only exposes a per-worker hook: an application that knows
// its own hardware layout can ask that a worker's thread be pinned to a
// specific CPU core and its memory preferentially allocated on a specific
// NUMA node. Discovering what those values should be is the application's
// job, not the protocol's. With no hint supplied, everything here is a
// no-op and workers behave exactly as in Stage 3.
//
// CPU pinning uses sched_setaffinity and is always available on Linux.
// NUMA binding uses libnuma and is compiled in only when libnuma's headers
// are present (FUSE_HAVE_NUMA); otherwise the NUMA hook degrades to a
// documented no-op.

#include <vector>

namespace fuse::proto {

struct TopologyHint {
    int cpu_core = -1;   // >= 0 pins the worker thread to this core
    int numa_node = -1;  // >= 0 prefers this node for the worker's memory
};

// Pins the calling thread to a single CPU core. Returns true on success,
// false if the core is invalid or the affinity call fails.
bool pin_current_thread_to_core(int core);

// Returns the CPU cores the calling thread is currently allowed to run on
// (the programmatic equivalent of /proc/<pid>/task/<tid>/status Cpus_allowed).
std::vector<int> current_thread_affinity();

// True if this build was compiled with libnuma support.
bool numa_supported();

// Best-effort: bind the calling thread's memory allocations to prefer the
// given NUMA node. A no-op (returns false) when NUMA support is not
// compiled in.
bool prefer_current_thread_numa_node(int node);

} // namespace fuse::proto

#endif // FUSE_PROTO_TOPOLOGY_HPP
