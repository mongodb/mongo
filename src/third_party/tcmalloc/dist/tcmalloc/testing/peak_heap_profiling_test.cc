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
#include <stdint.h>

#include <optional>

#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "absl/log/check.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace {

int64_t ProfileSize(ProfileType type) {
  int64_t total = 0;

  MallocExtension::SnapshotCurrent(type).Iterate(
      [&](const Profile::Sample& e) { total += e.sum; });
  return total;
}

size_t PeakMemoryUsage() {
  const auto usage = tcmalloc::MallocExtension::GetNumericProperty(
      "generic.peak_memory_usage");
  CHECK(usage.has_value());
  return usage.value();
}

class ScopedPeakGrowthFraction {
 public:
  explicit ScopedPeakGrowthFraction(double temporary_value)
      : previous_(TCMalloc_Internal_GetPeakSamplingHeapGrowthFraction()) {
    TCMalloc_Internal_SetPeakSamplingHeapGrowthFraction(temporary_value);
  }

  ~ScopedPeakGrowthFraction() {
    TCMalloc_Internal_SetPeakSamplingHeapGrowthFraction(previous_);
  }

 private:
  double previous_;
};

// NOTE: This test depends on being able to change the peak heap allocation.
// If you allocate a lot of memory before running it, it won't work! Thus it is
// in its own file.
//
// We could modify it to allocate up until we change the peak, but that will
// cause test failures to show up as OOMs instead, which is undesirable.
TEST(PeakHeapProfilingTest, PeakHeapTracking) {
  // Adjust high watermark threshold for our scenario, to be independent of
  // changes to the default.  As we use a random value for choosing our next
  // sampling point, we may overweight some allocations above their true size.
  ScopedPeakGrowthFraction s(1.25);

  int64_t start_peak_sz = ProfileSize(ProfileType::kPeakHeap);

  // make a large allocation to force a new peak heap sample
  // (total live: 50MiB)
  void* first = ::operator new(50 << 20);
  // TODO(b/183453911): Remove workaround for GCC 10.x deleting operator new,
  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=94295.
  benchmark::DoNotOptimize(first);
  int64_t expected_first = start_peak_sz + (50 << 20);
  int64_t peak_after_first = ProfileSize(ProfileType::kPeakHeap);
  int64_t numeric_property_peak_after_first = PeakMemoryUsage();
  EXPECT_NEAR(peak_after_first, expected_first, 10 << 20);
  EXPECT_NEAR(numeric_property_peak_after_first, expected_first, 10 << 20);

  // a small allocation shouldn't increase the peak
  // (total live: 54MiB)
  void* second = ::operator new(4 << 20);
  benchmark::DoNotOptimize(second);
  int64_t peak_after_second = ProfileSize(ProfileType::kPeakHeap);
  EXPECT_EQ(peak_after_second, peak_after_first);
  // For GetNumericProperty, it does increase.
  EXPECT_NEAR(PeakMemoryUsage(), numeric_property_peak_after_first + (4 << 20),
              10 << 20);

  // but a large one should
  // (total live: 254MiB)
  void* third = ::operator new(200 << 20);
  benchmark::DoNotOptimize(third);
  int64_t expected_third = peak_after_second + (200 << 20);
  int64_t peak_after_third = ProfileSize(ProfileType::kPeakHeap);
  int64_t numeric_property_peak_after_third = PeakMemoryUsage();
  EXPECT_NEAR(peak_after_third, expected_third, 10 << 20);
  EXPECT_NEAR(numeric_property_peak_after_third, expected_third, 10 << 20);

  // freeing everything shouldn't affect the peak
  // (total live: 0MiB)
  ::operator delete(first);
  EXPECT_EQ(ProfileSize(ProfileType::kPeakHeap), peak_after_third);
  EXPECT_EQ(PeakMemoryUsage(), numeric_property_peak_after_third);

  ::operator delete(second);
  EXPECT_EQ(ProfileSize(ProfileType::kPeakHeap), peak_after_third);
  EXPECT_EQ(PeakMemoryUsage(), numeric_property_peak_after_third);

  ::operator delete(third);
  EXPECT_EQ(ProfileSize(ProfileType::kPeakHeap), peak_after_third);
  EXPECT_EQ(PeakMemoryUsage(), numeric_property_peak_after_third);

  // going back up less than previous peak shouldn't affect the peak
  // (total live: 200MiB)
  void* fourth = ::operator new(100 << 20);
  benchmark::DoNotOptimize(fourth);
  void* fifth = ::operator new(100 << 20);
  benchmark::DoNotOptimize(fifth);
  EXPECT_EQ(ProfileSize(ProfileType::kPeakHeap), peak_after_third);
  EXPECT_EQ(PeakMemoryUsage(), numeric_property_peak_after_third);

  // passing the old peak significantly, even with many small allocations,
  // should generate a new one
  // (total live: 200MiB + 256MiB = 456MiB, 80% over the 254MiB peak)
  void* bitsy[1 << 10];
  for (int i = 0; i < 1 << 10; i++) {
    bitsy[i] = ::operator new(1 << 18);
    benchmark::DoNotOptimize(bitsy[i]);
  }
  EXPECT_GT(ProfileSize(ProfileType::kPeakHeap), peak_after_third);
  EXPECT_GT(PeakMemoryUsage(), peak_after_third);

  ::operator delete(fourth);
  ::operator delete(fifth);
  for (int i = 0; i < 1 << 10; i++) {
    ::operator delete(bitsy[i]);
  }
}

}  // namespace
}  // namespace tcmalloc
