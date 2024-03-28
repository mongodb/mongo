// Copyright 2019 The TCMalloc Authors
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
#include <sys/types.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/affinity.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc {
namespace {

using tcmalloc_internal::AllowedCpus;
using tcmalloc_internal::ScopedAffinityMask;

class SamplingMemoryTest : public ::testing::TestWithParam<size_t> {
 protected:
  SamplingMemoryTest() {
    MallocExtension::SetGuardedSamplingRate(-1);
  }

  size_t Property(absl::string_view name) {
    std::optional<size_t> result = MallocExtension::GetNumericProperty(name);
    TC_CHECK(result.has_value());
    return *result;
  }

  void SetSamplingInterval(int64_t val) {
    MallocExtension::SetProfileSamplingRate(val);
    // We do this to reset the per-thread sampler - it may have a
    // very large gap put in here if sampling had been disabled.
    void* ptr = ::operator new(1024 * 1024 * 1024);
    // TODO(b/183453911): Remove workaround for GCC 10.x deleting operator new,
    // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=94295.
    benchmark::DoNotOptimize(ptr);
    ::operator delete(ptr);
  }

  size_t CurrentHeapSize() {
    return Property("generic.current_allocated_bytes") +
           Property("tcmalloc.metadata_bytes");
  }

  // Return peak memory usage growth when allocating many "size" byte objects.
  ssize_t HeapGrowth(size_t size) {
    if (size < sizeof(void*)) {
      size = sizeof(void*);  // Must be able to fit a pointer in each object
    }

    // For speed, allocate smaller number of total bytes when size is small
    size_t total = 100 << 20;
    if (size <= 4096) {
      total = 30 << 20;
    }

    constexpr int kMaxTries = 10;

    for (int i = 0; i < kMaxTries; i++) {
      // We are trying to make precise measurements about the overhead of
      // allocations.  Keep harness-related allocations outside of our probe
      // points.
      //
      // We pin to a CPU and trigger an allocation of the target size to ensure
      // that the per-CPU slab has been initialized.
      std::vector<int> cpus = AllowedCpus();
      ScopedAffinityMask mask(cpus[0]);

      void* ptr = ::operator new(size);
      // TODO(b/183453911): Remove workaround for GCC 10.x deleting operator
      // new, https://gcc.gnu.org/bugzilla/show_bug.cgi?id=94295.
      benchmark::DoNotOptimize(ptr);
      ::operator delete(ptr);

      const size_t start_memory = CurrentHeapSize();

      void* list = nullptr;
      for (size_t alloc = 0; alloc < total; alloc += size) {
        void** object = reinterpret_cast<void**>(::operator new(size));
        benchmark::DoNotOptimize(object);

        *object = list;
        list = object;
      }

      const size_t peak_memory = CurrentHeapSize();

      while (list != nullptr) {
        void** object = reinterpret_cast<void**>(list);
        list = *object;
        ::operator delete(object);
      }

      if (mask.Tampered()) {
        continue;
      }

      return peak_memory - start_memory;
    }

    return 0;
  }
};

// Check that percent memory overhead created by sampling under the
// specified allocation pattern is not too large.
TEST_P(SamplingMemoryTest, Overhead) {
  const size_t size = GetParam();
  int64_t original = MallocExtension::GetProfileSamplingRate();
  SetSamplingInterval(0);
  const ssize_t baseline = HeapGrowth(size);

  SetSamplingInterval(original);

  const ssize_t with_sampling = HeapGrowth(size);

  // Allocating many MB's of memory should trigger some growth.
  EXPECT_NE(baseline, 0);
  EXPECT_NE(with_sampling, 0);

  const double percent =
      (static_cast<double>(with_sampling) - static_cast<double>(baseline)) *
      100.0 / static_cast<double>(baseline);

  double expectedOverhead = 10.2;
  // Larger page sizes have larger sampling overhead.
  if (tcmalloc_internal::kPageShift == 15) {
    expectedOverhead *= 2;
  } else if (tcmalloc_internal::kPageShift == 18) {
    expectedOverhead *= 3;
  }

  // some noise is unavoidable
  EXPECT_GE(percent, -expectedOverhead) << baseline << " " << with_sampling;
  EXPECT_LE(percent, expectedOverhead) << baseline << " " << with_sampling;
}

std::vector<size_t> InterestingSizes() {
  std::vector<size_t> ret;

  // Only use the first kNumBaseClasses size classes since classes after that
  // are intentionally duplicated.
  for (size_t size_class = 1; size_class < tcmalloc_internal::kNumBaseClasses;
       size_class++) {
    size_t size =
        tcmalloc::tcmalloc_internal::tc_globals.sizemap().class_to_size(
            size_class);
    if (size == 0) {
      continue;
    }
    ret.push_back(size);
  }
  // Add one size not covered by sizeclasses
  ret.push_back(ret.back() + 1);
  return ret;
}

INSTANTIATE_TEST_SUITE_P(AllSizeClasses, SamplingMemoryTest,
                         testing::ValuesIn(InterestingSizes()),
                         testing::PrintToStringParamName());

}  // namespace
}  // namespace tcmalloc

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
