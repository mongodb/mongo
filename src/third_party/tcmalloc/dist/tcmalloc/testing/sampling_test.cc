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
//
// This tests SnapshotCurrent.  It does this by doing a bunch of allocations and
// then evaluating the returned profile.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>
#include <optional>
#include <string>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/debugging/symbolize.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/random/random.h"
#include "absl/types/optional.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

constexpr int kMaxDemangledFunctionName = 256;

bool StackMatches(const char* target, const void* const* stack, size_t len) {
  char buf[kMaxDemangledFunctionName];

  for (size_t i = 0; i < len; ++i) {
    if (!absl::Symbolize(stack[i], buf, sizeof(buf))) continue;
    if (strstr(buf, target) != nullptr) return true;
  }

  return false;
}

template <bool CheckSize>
size_t CountMatchingBytes(const char* target, Profile profile) {
  size_t sum = 0;
  profile.Iterate([&](const Profile::Sample& e) {
    if (e.requested_size == 10000 || !CheckSize) {
      if (StackMatches(target, e.stack, e.depth)) {
        sum += static_cast<size_t>(e.sum);
      }
    }
  });

  return sum;
}

ABSL_ATTRIBUTE_NOINLINE static void* AllocateAllocate(bool align) {
  void* p;
  if (align) {
    // A 10000 byte allocation aligned to 2K will use a 10K size class
    // and get 'charged' identically to malloc(10000).
    TC_CHECK_EQ(0, posix_memalign(&p, 2048, 10000));
  } else {
    p = malloc(10000);
  }
  benchmark::DoNotOptimize(p);
  return p;
}

class SamplingTest : public testing::TestWithParam<int64_t> {};

TEST_P(SamplingTest, ParamChange) {
  static const size_t kIters = 80 * 1000;
  std::vector<void*> allocs;
  allocs.reserve(kIters * 2);

  ScopedGuardedSamplingRate gs(-1);
  size_t bytes;
  {
    ScopedProfileSamplingRate s(GetParam());
    for (int i = 0; i < kIters; ++i) {
      // Sample a mix of aligned and unaligned
      allocs.push_back(AllocateAllocate(i % 20 == 0));
    }

    bytes = CountMatchingBytes<true>(
        "AllocateAllocate",
        MallocExtension::SnapshotCurrent(ProfileType::kHeap));
    if (GetParam() > 0) {
      EXPECT_LE(500 * 1024 * 1024, bytes);
      EXPECT_GE(1000 * 1024 * 1024, bytes);
    } else {
      EXPECT_EQ(0, bytes);
    }
  }

  // We change back the samping parameter (~ScopedProfileSamplingRate above) and
  // allocate more, *without* deleting the old allocs--we should sample at the
  // new rate, and reweighting should correctly blend samples from before and
  // after the change.
  for (int i = 0; i < kIters; ++i) {
    allocs.push_back(AllocateAllocate(i % 20 == 0));
  }

  bytes = CountMatchingBytes<true>(
      "AllocateAllocate", MallocExtension::SnapshotCurrent(ProfileType::kHeap));
  if (GetParam() > 0) {
    EXPECT_LE(1000 * 1024 * 1024, bytes);
    EXPECT_GE(2000 * 1024 * 1024, bytes);
  } else {
    // samples that don't exist can't be reweighted properly
    EXPECT_LE(500 * 1024 * 1024, bytes);
    EXPECT_GE(1000 * 1024 * 1024, bytes);
  }

  for (auto p : allocs) {
    free(p);
  }
}

INSTANTIATE_TEST_SUITE_P(SampleParameters, SamplingTest,
                         testing::Values(0, 100000),
                         testing::PrintToStringParamName());

ABSL_ATTRIBUTE_NOINLINE static void* AllocateZeroByte() {
  void* p = ::operator new(0);
  ::benchmark::DoNotOptimize(p);
  return p;
}

TEST(Sampling, AlwaysSampling) {
  ScopedGuardedSamplingRate gs(-1);
  ScopedProfileSamplingRate s(1);

  static const size_t kIters = 80 * 1000;
  std::vector<void*> allocs;
  allocs.reserve(kIters);
  for (int i = 0; i < kIters; ++i) {
    allocs.push_back(AllocateZeroByte());
  }
  const absl::optional<size_t> alloc_size =
      MallocExtension::GetAllocatedSize(allocs[0]);
  ASSERT_THAT(alloc_size, testing::Ne(std::nullopt));
  EXPECT_GT(*alloc_size, 0);

  size_t bytes = CountMatchingBytes<false>(
      "AllocateZeroByte", MallocExtension::SnapshotCurrent(ProfileType::kHeap));
  EXPECT_EQ(*alloc_size * kIters, bytes);

  for (void* p : allocs) {
    ::operator delete(p);
  }
}

ABSL_ATTRIBUTE_NOINLINE static void* AllocateRandomBytes() {
  absl::BitGen rng;
  return ::operator new(absl::LogUniform<size_t>(rng, 1, 1 << 21));
}

// We disable inlining and tail calls to ensure that we have a stack entry for
// that function.
ABSL_ATTRIBUTE_NOINLINE ABSL_ATTRIBUTE_NO_TAIL_CALL Profile
GetHeapSnapshotNoinline() {
  return MallocExtension::SnapshotCurrent(ProfileType::kHeap);
}

TEST(Sampling, InternalFragmentation) {
  ScopedGuardedSamplingRate gs(-1);
  ScopedProfileSamplingRate s(1);

  static constexpr size_t kBytes = 1 << 30;
  // A power of 2 is very likely a size class, but we will check this.
  static constexpr size_t kLowFragmentationSize = 1 << 13;
  // A power of 2 + 1 is very unlikely to correspond exactly to a size class.
  static constexpr size_t kHighFragmentationSize = (1 << 13) + 1;

  static constexpr int kNumLow = kBytes / kLowFragmentationSize;
  static constexpr int kNumHigh = kBytes / kHighFragmentationSize;

  std::vector<void*> low, high;
  low.reserve(kNumLow);
  high.reserve(kNumHigh);

  const absl::optional<size_t> starting_fragmentation =
      MallocExtension::GetNumericProperty(
          "tcmalloc.sampled_internal_fragmentation");
  ASSERT_THAT(starting_fragmentation, testing::Ne(std::nullopt));

  auto ProfiledFragmentation = [&]() {
    size_t fragmentation = 0;
    auto profile = GetHeapSnapshotNoinline();
    profile.Iterate([&](const tcmalloc::Profile::Sample& e) {
      EXPECT_GE(e.allocated_size, e.requested_size);
      EXPECT_GT(e.allocated_size, 0);
      // Ignore temporary allocations from creating the snapshot. These
      // allocations are not accounted for in
      // "tcmalloc.sampled_internal_fragmentation".
      if (StackMatches("GetHeapSnapshotNoinline", e.stack, e.depth)) {
        LOG(INFO) << "ignoring allocation of size " << e.requested_size
                  << " within `MallocExtension::SnapshotCurrent`";
        return;
      }
      fragmentation +=
          (e.allocated_size - e.requested_size) * (e.sum / e.allocated_size);
    });
    return fragmentation;
  };

  const size_t starting_profiled_fragmentation = ProfiledFragmentation();

  for (int i = 0; i < kNumLow; i++) {
    low.push_back(::operator new(kLowFragmentationSize));
  }

  const absl::optional<size_t> actual_low =
      MallocExtension::GetAllocatedSize(low[0]);
  ASSERT_THAT(actual_low, testing::Ne(std::nullopt));

  for (int i = 0; i < kNumHigh; i++) {
    high.push_back(::operator new(kHighFragmentationSize));
  }

  const absl::optional<size_t> actual_high =
      MallocExtension::GetAllocatedSize(high[0]);
  ASSERT_THAT(actual_high, testing::Ne(std::nullopt));

  const size_t ending_profiled_fragmentation = ProfiledFragmentation();

  const absl::optional<size_t> ending_fragmentation =
      MallocExtension::GetNumericProperty(
          "tcmalloc.sampled_internal_fragmentation");
  ASSERT_THAT(ending_fragmentation, testing::Ne(std::nullopt));
  const std::string stats = MallocExtension::GetStats();

  for (void* p : low) {
    sized_delete(p, kLowFragmentationSize);
  }
  for (void* p : high) {
    sized_delete(p, kHighFragmentationSize);
  }

  EXPECT_EQ(*actual_low, kLowFragmentationSize);
  EXPECT_GT(*actual_high, kHighFragmentationSize);

  // Expect that the two means of computing fragmentation (reading the profile
  // directly and asking GetNumericProperty) are within a kTolerance fraction of
  // each other.
  static constexpr double kTolerance = 0.02;
  EXPECT_NEAR(*starting_fragmentation, starting_profiled_fragmentation,
              starting_profiled_fragmentation * kTolerance);
  EXPECT_NEAR(*ending_fragmentation, ending_profiled_fragmentation,
              ending_profiled_fragmentation * kTolerance);

  // Compare *ending_fragmentation with what we recorded in GetStats()
  auto pos = stats.find("MALLOC SAMPLED PROFILES");
  ASSERT_NE(pos, std::string::npos);

  size_t current, fragmentation, peak;
  int ret = sscanf(
      stats.c_str() + pos,
      "MALLOC SAMPLED PROFILES: %zu bytes (current), %zu bytes (internal "
      "fragmentation), %zu bytes (peak)\n",
      &current, &fragmentation, &peak);
  ASSERT_EQ(ret, 3);

  EXPECT_GT(current, 0);
  EXPECT_GT(peak, 0);
  EXPECT_NEAR(fragmentation, *ending_fragmentation,
              *ending_fragmentation * kTolerance);

  // ending - starting should be roughly due to allocating
  // kHighFragmentationSize objects kNumHigh times.
  EXPECT_NEAR(*starting_fragmentation +
                  (*actual_high - kHighFragmentationSize) * kNumHigh,
              *ending_fragmentation, *ending_fragmentation * kTolerance);
}

}  // namespace
}  // namespace tcmalloc
