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

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <syscall.h>
#include <unistd.h>

#include <vector>

#include "gtest/gtest.h"
#include "absl/random/random.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/affinity.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

// Returns the NUMA node whose memory is used to back the allocation at ptr.
size_t BackingNode(void* const ptr) {
  // Ensure that at least the page containing the first byte of our
  // allocation is actually backed by physical memory by writing to it.
  memset(ptr, 42, 1);

  // The move_pages syscall expects page aligned addresses; we'll check the
  // page containing the first byte of the allocation at ptr.
  static const size_t page_size = GetPageSize();
  uintptr_t page_addr = reinterpret_cast<uintptr_t>(ptr) & ~(page_size - 1);

  // Retrieve the number of the NUMA node whose memory ptr is backed by. The
  // move_pages syscall will write this into status, which we initialize to an
  // invalid value to sanity check that we see it updated.
  int status = -1;
  CHECK_CONDITION(syscall(__NR_move_pages, /*pid=*/0, /*count=*/1, &page_addr,
                          /*nodes=*/nullptr, &status, /*flags=*/0) == 0);
  CHECK_CONDITION(status >= 0);
  return status;
}

// Test that allocations are performed using memory within the appropriate NUMA
// partition.
TEST(NumaLocalityTest, AllocationsAreLocal) {
  if (!tc_globals.numa_topology().numa_aware()) {
    GTEST_SKIP() << "NUMA awareness is disabled";
  }

  // We don't currently enforce NUMA locality for sampled allocations; disable
  // sampling for the test.
  ScopedNeverSample never_sample;

  absl::BitGen gen;
  constexpr size_t kIterations = 1000;
  for (size_t i = 0; i < kIterations; i++) {
    // We can only be sure that we obtain memory local to a particular node if
    // we constrain ourselves to run on CPUs within that node. We ensure that
    // here by constraining ourselves to a single CPU chosen randomly from the
    // set we're allowed to run upon.
    std::vector<int> allowed = AllowedCpus();
    const size_t target_cpu =
        allowed[absl::Uniform(gen, 0ul, allowed.size() - 1)];
    ScopedAffinityMask mask(target_cpu);

    // Discover which NUMA node we're currently local to.
    unsigned int local_node;
    ASSERT_EQ(syscall(__NR_getcpu, nullptr, &local_node, nullptr), 0);

    // Perform a randomly sized memory allocation.
    const size_t alloc_size = absl::Uniform(gen, 1ul, 5ul << 20);
    void* ptr = ::operator new(alloc_size);
    ASSERT_NE(ptr, nullptr);

    // Discover which NUMA node contains the memory backing that allocation.
    const size_t backing_node = BackingNode(ptr);

    if (mask.Tampered()) {
      // If we moved to another CPU then we may also have moved to a different
      // node at some arbitrary point in the midst of performing the
      // allocation, and all bets are off as to which node that allocation will
      // be local to. Simply try again.
      i--;
    } else {
      // We should observe that the allocation is backed by a node within the
      // same NUMA partition as the local node.
      EXPECT_EQ(NodeToPartition(backing_node, kNumaPartitions),
                NodeToPartition(local_node, kNumaPartitions));
    }

    ::operator delete(ptr);
  }
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
