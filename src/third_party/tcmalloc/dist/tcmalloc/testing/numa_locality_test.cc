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

#include <array>
#include <atomic>
#include <new>
#include <string>
#include <tuple>
#include <vector>

#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/base/no_destructor.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/random/random.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/affinity.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

#if defined(ABSL_HAVE_ADDRESS_SANITIZER) ||   \
    defined(ABSL_HAVE_MEMORY_SANITIZER) ||    \
    defined(ABSL_HAVE_THREAD_SANITIZER) ||    \
    defined(ABSL_HAVE_HWADDRESS_SANITIZER) || \
    defined(ABSL_HAVE_DATAFLOW_SANITIZER) ||  \
    defined(ABSL_HAVE_LEAK_SANITIZER) || defined(__aarch64__)
#define TCMALLOC_TEST_DISABLE_FAKE_NUMA_FACTORY 1
#endif

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
  TC_CHECK_EQ(0, syscall(__NR_move_pages, /*pid=*/0, /*count=*/1, &page_addr,
                         /*nodes=*/nullptr, &status, /*flags=*/0));
  TC_CHECK_GE(status, 0);
  return status;
}

class FakeNumaAwareRegionFactory final : public tcmalloc::AddressRegionFactory {
 public:
  static constexpr size_t kAddrsAndHintsSize = 8;

  explicit FakeNumaAwareRegionFactory(AddressRegionFactory* under)
      : under_(under) {}

  AddressRegion* Create(void* start_addr, size_t size,
                        UsageHint hint) override {
    // We should never see kNormal under numa_aware.
    TC_CHECK(hint != UsageHint::kNormal ||
             !tc_globals.numa_topology().numa_aware());

    static std::atomic<uint32_t> log_idx = 0;
    addrs_and_hints_[log_idx % kAddrsAndHintsSize] =
        std::make_tuple(start_addr, size, hint);
    CHECK_LE(log_idx++, kAddrsAndHintsSize)
        << "Created more than kAddrsAndHintSize regions. If this is expected, "
           "please increase kAddrsAndHintSize.";

    return under_->Create(start_addr, size, hint);
  }

  size_t GetStats(absl::Span<char> buffer) override {
    CHECK(false) << "Not implemented.";
  }

  size_t GetStatsInPbtxt(absl::Span<char> buffer) override {
    CHECK(false) << "Not implemented.";
  }

  std::string AddrsAndHintsDebug() {
    std::string tracked_regions;
    bool first = true;
    for (const auto& [start, size, hint] : addrs_and_hints_) {
      if (!first) absl::StrAppend(&tracked_regions, "\n");
      if (start == nullptr) break;
      absl::StrAppend(&tracked_regions, "{start=", (uintptr_t)start,
                      ", end=", (uintptr_t)start + size, ", hint=", (int)hint,
                      "}");
      first = false;
    }
    return tracked_regions;
  }

  int NotFoundCount() { return not_found_.load(std::memory_order_relaxed); }

  void VerifyHint(void* const ptr, int expected_partition) {
    for (const auto& [start, size, hint] : addrs_and_hints_) {
      if (start == nullptr) break;
      if (ptr >= start && ptr < static_cast<const char*>(start) + size) {
        ++found_;
        // Ignore "special" hints, e.x. kInfrequentAllocation and
        // kInfrequentAccess.
        if (hint != UsageHint::kNormalNumaAwareS0 &&
            hint != UsageHint::kNormalNumaAwareS1) {
          return;
        }
        const int hinted_partition =
            hint == UsageHint::kNormalNumaAwareS1 ? 1 : 0;
        EXPECT_EQ(expected_partition, hinted_partition);
        return;
      }
    }

    ++not_found_;
    LOG(WARNING) << "Region not found for {ptr=" << (uintptr_t)ptr
                 << ", expected_partition=" << expected_partition << "} ("
                 << not_found_ << " not found so far, of "
                 << not_found_ + found_ << ").\n=== TRACKED REGIONS ===\n"
                 << AddrsAndHintsDebug();
  }

 private:
  std::array<std::tuple<void*, size_t, UsageHint>, kAddrsAndHintsSize>
      addrs_and_hints_ = {};
  AddressRegionFactory* under_ = nullptr;
  std::atomic<int> found_ = 0;
  std::atomic<int> not_found_ = 0;
};

FakeNumaAwareRegionFactory* GetFakeNumaAwareRegionFactory() {
  AddressRegionFactory* existing_factory = MallocExtension::GetRegionFactory();
  TC_CHECK_NE(existing_factory, nullptr);
  static absl::NoDestructor<FakeNumaAwareRegionFactory> f(existing_factory);
  return f.get();
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

#ifndef TCMALLOC_TEST_DISABLE_FAKE_NUMA_FACTORY
  FakeNumaAwareRegionFactory* logging_factory = GetFakeNumaAwareRegionFactory();
#else
  FakeNumaAwareRegionFactory* logging_factory = nullptr;
#endif  // TCMALLOC_TEST_DISABLE_FAKE_NUMA_FACTORY

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
      if (logging_factory) {
        logging_factory->VerifyHint(
            ptr, NodeToPartition(backing_node, kNumaPartitions));
      }
    }

    ::operator delete(ptr);
  }

#ifndef TCMALLOC_TEST_DISABLE_FAKE_NUMA_FACTORY
  // All of our allocations should match known regions.
  EXPECT_EQ(logging_factory->NotFoundCount(), 0);
#endif  // TCMALLOC_TEST_DISABLE_FAKE_NUMA_FACTORY
}

#ifndef TCMALLOC_TEST_DISABLE_FAKE_NUMA_FACTORY
static void install_factory() {
  // Install fake region factory to log hints for verification.
  MallocExtension::SetRegionFactory(GetFakeNumaAwareRegionFactory());
}

__attribute__((section(".preinit_array"), used)) void (
    *__local_install_factory_preinit)() = install_factory;
#endif  // TCMALLOC_TEST_DISABLE_FAKE_NUMA_FACTORY

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
