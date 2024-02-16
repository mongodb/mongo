// Copyright 2021 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_INTERNAL_NUMA_H_
#define TCMALLOC_INTERNAL_NUMA_H_

#include <sched.h>
#include <stddef.h>
#include <sys/types.h>

#include <optional>

#include "absl/functional/function_ref.h"
#include "absl/types/optional.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/percpu.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Indicates how TCMalloc should handle binding memory regions to nodes within
// particular NUMA partitions.
enum class NumaBindMode {
  // Don't bind memory at all. Note that this does not make NUMA awareness
  // pointless so long as the NUMA memory policy of threads performing
  // allocations favors the local node. It does mean that we won't be certain
  // that memory is local to any particular partition, it will just be likely.
  kNone,
  // Attempt to bind memory but don't treat failure as fatal. If binding fails
  // then a warning will be logged & we'll then be in much the same state as
  // kNone.
  kAdvisory,
  // Strictly bind memory to nodes within the partition we expect - any error
  // in doing so is fatal & the program will crash. This allows a program to
  // ensure that memory is definitely bound to the set of nodes we expect.
  kStrict,
};

// We use the result of RseqCpuId() in GetCurrentPartition() to avoid branching
// in the fast path, but this means that the CPU number we look up in
// cpu_to_scaled_partition_ might equal kCpuIdUninitialized or
// kCpuIdUnsupported. We add this fudge factor to the value to compensate,
// ensuring that our accesses to the cpu_to_scaled_partition_ array are always
// in bounds.
static constexpr size_t kNumaCpuFudge = -subtle::percpu::kCpuIdUnsupported;

// Provides information about the topology of a NUMA system.
//
// In general we cannot know at compile time how many NUMA nodes the system
// that we run upon will include, but we also cannot size our data structures
// arbitrarily at runtime in the name of efficiency. In order to resolve the
// conflict between these two constraints we define the concept of a NUMA
// 'partition' as being an arbitrary set of NUMA nodes, disjoint from all other
// partitions. At compile time we select a fixed number of partitions to
// support, and at runtime we map each NUMA node in the system to a partition.
// If the number of supported partitions is greater than or equal to the number
// of NUMA nodes in the system then partition & node are effectively identical.
// If however the system has more nodes than we do partitions then nodes
// assigned to the same partition will share size classes & thus memory. This
// may incur a performance hit, but allows us to at least run on any system.
template <size_t NumPartitions, size_t ScaleBy = 1>
class NumaTopology {
 public:
  // Trivially zero initialize data members.
  constexpr NumaTopology() = default;

  // Initialize topology information. This must be called only once, before any
  // of the functions below.
  void Init();

  // Like Init(), but allows a test to specify a different `open_node_cpulist`
  // function in order to provide NUMA topology information that doesn't
  // reflect the system we're running upon.
  void InitForTest(absl::FunctionRef<int(size_t)> open_node_cpulist);

  // Returns true if NUMA awareness is available & enabled, otherwise false.
  bool numa_aware() const {
    // Explicitly checking NumPartitions here provides a compile time constant
    // false in cases where NumPartitions==1, allowing NUMA awareness to be
    // optimized away.
    return (NumPartitions > 1) && numa_aware_;
  }

  // Returns the number of NUMA partitions deemed 'active' - i.e. the number of
  // partitions that other parts of TCMalloc need to concern themselves with.
  // Checking this rather than using kNumaPartitions allows users to avoid work
  // on non-zero partitions when NUMA awareness is disabled.
  size_t active_partitions() const { return numa_aware() ? NumPartitions : 1; }

  // Return a value indicating how we should behave with regards to binding
  // memory regions to NUMA nodes.
  NumaBindMode bind_mode() const { return bind_mode_; }

  // Return the NUMA partition number to which the CPU we're currently
  // executing upon belongs. Note that whilst the CPU->partition mapping is
  // fixed, the return value of this function may change at arbitrary times as
  // this thread migrates between CPUs.
  size_t GetCurrentPartition() const;

  // Like GetCurrentPartition(), but returns a partition number multiplied by
  // ScaleBy.
  size_t GetCurrentScaledPartition() const;

  // Return the NUMA partition number to which `cpu` belongs.
  //
  // It is valid for cpu to equal subtle::percpu::kCpuIdUninitialized or
  // subtle::percpu::kCpuIdUnsupported. In either case partition 0 will be
  // returned.
  size_t GetCpuPartition(int cpu) const;

  // Like GetCpuPartition(), but returns a partition number multiplied by
  // ScaleBy.
  size_t GetCpuScaledPartition(int cpu) const;

  // Return a bitmap in which set bits identify the nodes that belong to the
  // specified NUMA `partition`.
  uint64_t GetPartitionNodes(int partition) const;

 private:
  // Maps from CPU number (plus kNumaCpuFudge) to NUMA partition.
  size_t cpu_to_scaled_partition_[CPU_SETSIZE + kNumaCpuFudge] = {0};
  // Maps from NUMA partition to a bitmap of NUMA nodes within the partition.
  uint64_t partition_to_nodes_[NumPartitions] = {0};
  // Indicates whether NUMA awareness is available & enabled.
  bool numa_aware_ = false;
  // Desired memory binding behavior.
  NumaBindMode bind_mode_ = NumaBindMode::kAdvisory;
};

// Opens a /sys/devices/system/node/nodeX/cpulist file for read only access &
// returns the file descriptor.
int OpenSysfsCpulist(size_t node);

// Parse a CPU list in the format used by
// /sys/devices/system/node/nodeX/cpulist files - that is, individual CPU
// numbers or ranges in the format <start>-<end> inclusive all joined by comma
// characters.
//
// Returns absl::nullopt on error.
//
// The read function is expected to operate much like the read syscall. It
// should read up to `count` bytes into `buf` and return the number of bytes
// actually read. If an error occurs during reading it should return -1 with
// errno set to an appropriate error code.
std::optional<cpu_set_t> ParseCpulist(
    absl::FunctionRef<ssize_t(char* buf, size_t count)> read);

// Initialize the data members of a NumaTopology<> instance.
//
// This function must only be called once per NumaTopology<> instance, and
// relies upon the data members of that instance being default initialized.
//
// The `open_node_cpulist` function is typically OpenSysfsCpulist but tests may
// use a different implementation.
//
// Returns true if we're actually NUMA aware; i.e. if we have CPUs mapped to
// multiple partitions.
bool InitNumaTopology(size_t cpu_to_scaled_partition[CPU_SETSIZE],
                      uint64_t* partition_to_nodes, NumaBindMode* bind_mode,
                      size_t num_partitions, size_t scale_by,
                      absl::FunctionRef<int(size_t)> open_node_cpulist);

// Returns the NUMA partition to which `node` belongs.
inline size_t NodeToPartition(const size_t node, const size_t num_partitions) {
  return node % num_partitions;
}

template <size_t NumPartitions, size_t ScaleBy>
inline void NumaTopology<NumPartitions, ScaleBy>::Init() {
  numa_aware_ =
      InitNumaTopology(cpu_to_scaled_partition_, partition_to_nodes_,
                       &bind_mode_, NumPartitions, ScaleBy, OpenSysfsCpulist);
}

template <size_t NumPartitions, size_t ScaleBy>
inline void NumaTopology<NumPartitions, ScaleBy>::InitForTest(
    absl::FunctionRef<int(size_t)> open_node_cpulist) {
  numa_aware_ =
      InitNumaTopology(cpu_to_scaled_partition_, partition_to_nodes_,
                       &bind_mode_, NumPartitions, ScaleBy, open_node_cpulist);
}

template <size_t NumPartitions, size_t ScaleBy>
inline size_t NumaTopology<NumPartitions, ScaleBy>::GetCurrentPartition()
    const {
  if constexpr (NumPartitions == 1) return 0;
  return GetCpuPartition(subtle::percpu::RseqCpuId());
}

template <size_t NumPartitions, size_t ScaleBy>
inline size_t NumaTopology<NumPartitions, ScaleBy>::GetCurrentScaledPartition()
    const {
  if constexpr (NumPartitions == 1) return 0;
  return GetCpuScaledPartition(subtle::percpu::RseqCpuId());
}

template <size_t NumPartitions, size_t ScaleBy>
inline size_t NumaTopology<NumPartitions, ScaleBy>::GetCpuPartition(
    const int cpu) const {
  return GetCpuScaledPartition(cpu) / ScaleBy;
}

template <size_t NumPartitions, size_t ScaleBy>
inline size_t NumaTopology<NumPartitions, ScaleBy>::GetCpuScaledPartition(
    const int cpu) const {
  if constexpr (NumPartitions == 1) return 0;
  return cpu_to_scaled_partition_[cpu + kNumaCpuFudge];
}

template <size_t NumPartitions, size_t ScaleBy>
inline uint64_t NumaTopology<NumPartitions, ScaleBy>::GetPartitionNodes(
    const int partition) const {
  return partition_to_nodes_[partition];
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_NUMA_H_
