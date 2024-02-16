// Copyright 2022 The TCMalloc Authors
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

#include "tcmalloc/deallocation_profiler.h"

#include <stddef.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/debugging/symbolize.h"
#include "absl/log/log.h"
#include "absl/strings/match.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"

namespace deallocationz = tcmalloc::deallocationz;

namespace {

// If one of the checkers is enabled, stubs are not linked in and the
// following tests will all fail.
static bool CheckerIsActive() {
#if defined(ADDRESS_SANITIZER) || defined(THREAD_SANITIZER) || \
    defined(MEMORY_SANITIZER)
  return true;
#else
  return false;
#endif
}

ABSL_ATTRIBUTE_NOINLINE ABSL_ATTRIBUTE_NO_TAIL_CALL void *SingleAlloc(
    int depth, uintptr_t size) {
  if (depth == 0) {
    return ::operator new(size);
  } else {
    return SingleAlloc(depth - 1, size);
  }
}

ABSL_ATTRIBUTE_NOINLINE ABSL_ATTRIBUTE_NO_TAIL_CALL void SingleDealloc(
    int depth, void *ptr) {
  if (depth == 0) {
    ::operator delete(ptr);
  } else {
    SingleDealloc(depth - 1, ptr);
  }
}

// Test to ensure counters have the correct values when running a predictable
// sequence of allocations/deallocations that are always sampled.
TEST(LifetimeProfiler, BasicCounterValues) {
  if (CheckerIsActive()) {
    return;
  }

  const int64_t kMallocSize = 4 * 1024 * 1024;
  const int kNumAllocations = 100;  // needs to be an even number
  const absl::Duration duration = absl::Microseconds(100);

  // Avoid unsample-related behavior
  tcmalloc::ScopedProfileSamplingRate test_sample_rate(1);

  void *ptr = nullptr;

  auto token = tcmalloc::MallocExtension::StartLifetimeProfiling();

  // Perform four allocation/deallocation pairs. The first batch should get
  // merged into one sample, the second batch into two different samples.
  for (int i = 0; i < kNumAllocations; i++) {
    ptr = SingleAlloc(2, kMallocSize);
    absl::SleepFor(duration);
    SingleDealloc(3, ptr);
  }

  for (int i = 0; i < kNumAllocations; i++) {
    ptr = SingleAlloc(2, ((i % 2) + 2) * kMallocSize);
    absl::SleepFor(duration);
    SingleDealloc(2, ptr);
  }

  absl::SleepFor(absl::Seconds(1));

  const tcmalloc::Profile profile = std::move(token).Stop();

  struct Counters {
    int samples_count = 0;
    std::vector<int> counts;
    int64_t sum = 0;
    int64_t total_count = 0;
    int64_t alloc_fn_count = 0;
    int64_t dealloc_fn_count = 0;
  };

  Counters counters;

  auto evaluate_profile = [&counters](const tcmalloc::Profile::Sample &e) {
    // Check how many times the stack contains SingleAlloc or SingleDealloc.
    int num_alloc = 0;
    int num_dealloc = 0;
    for (int i = 0; i < e.depth; i++) {
      const int kMaxFunctionNameLength = 1024;
      char str[kMaxFunctionNameLength];
      absl::Symbolize(e.stack[i], str, kMaxFunctionNameLength);
      if (absl::StrContains(str, "SingleAlloc")) {
        num_alloc++;
      }
      if (absl::StrContains(str, "SingleDealloc")) {
        num_dealloc++;
      }
    }

    // If it contains neither function, this might be another thread and
    // should be ignored (we only count calls from these two functions).
    if (num_alloc == 0 && num_dealloc == 0) {
      return;
    }

    counters.samples_count++;
    counters.counts.push_back(e.count);
    counters.sum += e.sum;

    // Positive counts are allocations, negative counts are deallocations
    if (e.count >= 0) {
      counters.total_count += e.count;
      EXPECT_GT(num_alloc, 0);
      counters.alloc_fn_count += e.count * num_alloc;
      EXPECT_EQ(num_dealloc, 0);
    } else {
      EXPECT_EQ(num_alloc, 0);
      EXPECT_GT(num_dealloc, 0);
      counters.dealloc_fn_count += -e.count * num_dealloc;
    }
  };

  profile.Iterate(evaluate_profile);

  // There should be three different allocation pairs. There are 2 samples for
  // each of them (alloc/dealloc) and depending on whether or not the thread
  // migrates CPU during the execution, there are 1 or 2 instances of each.
  EXPECT_GE(counters.samples_count, 6);
  EXPECT_LE(counters.samples_count, 12);

  // Every allocation gets counted twice
  EXPECT_EQ(counters.sum, 7 * kNumAllocations * kMallocSize);
  EXPECT_EQ(counters.total_count, 2 * kNumAllocations);
  // Expect that the SingleAlloc and SingleDealloc functions were recorded in
  // the stack trace.
  EXPECT_GT(counters.alloc_fn_count, 0);
  EXPECT_GT(counters.dealloc_fn_count, 0);
  // TODO(b/248332543): Investigate why the symbol count in the callstack is not
  // as expected for GCC opt builds.
#if defined(__clang__)
  EXPECT_EQ(counters.alloc_fn_count, 6 * kNumAllocations);
  EXPECT_EQ(counters.dealloc_fn_count, 7 * kNumAllocations);
#endif

  for (size_t i = 0; i < counters.counts.size(); i += 2) {
    EXPECT_EQ(counters.counts[i], -counters.counts[i + 1]);
  }
}

TEST(LifetimeProfiler, RecordLiveAllocations) {
  if (CheckerIsActive()) {
    return;
  }

  // Avoid unsample-related behavior
  tcmalloc::ScopedProfileSamplingRate test_sample_rate(1);
  constexpr int64_t kMallocSize = 4 * 1024 * 1024;
  constexpr int kAllocFrames = 2, kDeallocFrames = 3;
  constexpr absl::Duration kDuration = absl::Milliseconds(100);

  void *ptr = SingleAlloc(kAllocFrames, kMallocSize);
  absl::SleepFor(kDuration);

  auto token = tcmalloc::MallocExtension::StartLifetimeProfiling();
  SingleDealloc(kDeallocFrames, ptr);
  const tcmalloc::Profile profile = std::move(token).Stop();

  int sample_count = 0;
  int alloc_frames = 0;
  int dealloc_frames = 0;
  absl::Duration sample_lifetime;
  profile.Iterate([&](const tcmalloc::Profile::Sample &sample) {
    bool found_test_alloc = false;
    for (int i = 0; i < sample.depth; i++) {
      const int kMaxFunctionNameLength = 1024;
      char str[kMaxFunctionNameLength];
      absl::Symbolize(sample.stack[i], str, kMaxFunctionNameLength);
      if (absl::StrContains(str, "SingleAlloc")) {
        ++alloc_frames;
        found_test_alloc = true;
      }
      if (absl::StrContains(str, "SingleDealloc")) {
        ++dealloc_frames;
        found_test_alloc = true;
      }
    }

    // Skip samples which are unrelated to the test.
    if (!found_test_alloc) {
      return;
    }

    ++sample_count;
    // Both the alloc and dealloc sample have the same stats.
    sample_lifetime = sample.max_lifetime;
  });

  // Expect one sample each for allocation, deallocation.
  ASSERT_EQ(sample_count, 2);
  // Expect the same depth specified during alloc, dealloc. Add 1 to account for
  // the call from the test.
  EXPECT_EQ(alloc_frames, kAllocFrames + 1);
  EXPECT_EQ(dealloc_frames, kDeallocFrames + 1);
  EXPECT_GE(sample_lifetime, kDuration);
}

TEST(LifetimeProfiler, RecordCensoredAllocations) {
  if (CheckerIsActive()) {
    return;
  }

  // Avoid unsample-related behavior
  tcmalloc::ScopedProfileSamplingRate test_sample_rate(1);
  constexpr int64_t kMallocSize1 = 4 * 1024, kMallocSize2 = 2 * 1024;
  constexpr int kAllocFrames = 2;
  constexpr absl::Duration kDuration = absl::Milliseconds(100);

  // Allocated prior to profiling.
  void *ptr1 = SingleAlloc(kAllocFrames, kMallocSize1);

  auto token = tcmalloc::MallocExtension::StartLifetimeProfiling();

  // Change the requested size so that it always shows up as a different sample.
  void *ptr2 = SingleAlloc(kAllocFrames, kMallocSize2);

  const tcmalloc::Profile profile = std::move(token).Stop();
  absl::SleepFor(kDuration);

  // deallocations occur after samples are collected.
  SingleDealloc(1, ptr1);
  SingleDealloc(1, ptr2);

  int sample_count = 0;
  int alloc_frames = 0;
  ASSERT_NO_FATAL_FAILURE(
      profile.Iterate([&](const tcmalloc::Profile::Sample &sample) {
        bool found_test_alloc = false;
        for (int i = 0; i < sample.depth; i++) {
          const int kMaxFunctionNameLength = 1024;
          char str[kMaxFunctionNameLength];
          absl::Symbolize(sample.stack[i], str, kMaxFunctionNameLength);
          if (absl::StrContains(str, "SingleAlloc")) {
            ++alloc_frames;
            found_test_alloc = true;
          }
        }

        // Skip samples which are unrelated to the test.
        if (!found_test_alloc) {
          return;
        }

        // All the samples we collect in this test should be right-censored.
        ASSERT_TRUE(sample.is_censored);
        ++sample_count;
      }));

  // Expect 2 samples for ptrs and ptr2.
  ASSERT_EQ(sample_count, 2);
  // Expect the same depth specified during allocs for ptrs and ptr2.
  // Add 1 to account for the call from the test.
  EXPECT_EQ(alloc_frames, 2 * (kAllocFrames + 1));
}

TEST(LifetimeProfiler, LifetimeBucketing) {
  using deallocationz::internal::LifetimeNsToBucketedDuration;

  auto BucketizeDuration = [](uint64_t nanoseconds) {
    return LifetimeNsToBucketedDuration(nanoseconds);
  };

  EXPECT_EQ(absl::Nanoseconds(1), BucketizeDuration(0));
  EXPECT_EQ(absl::Nanoseconds(10), BucketizeDuration(31));
  EXPECT_EQ(absl::Nanoseconds(100), BucketizeDuration(104));
  EXPECT_EQ(absl::Nanoseconds(1000), BucketizeDuration(4245));
  EXPECT_EQ(absl::Nanoseconds(10000), BucketizeDuration(42435));
  EXPECT_EQ(absl::Nanoseconds(100000), BucketizeDuration(942435));
  EXPECT_EQ(absl::Nanoseconds(1000000), BucketizeDuration(1000000));
  EXPECT_EQ(absl::Nanoseconds(1000000), BucketizeDuration(1900000));
  EXPECT_EQ(absl::Nanoseconds(2000000), BucketizeDuration(2000000));
  EXPECT_EQ(absl::Nanoseconds(2000000), BucketizeDuration(2700000));
  EXPECT_EQ(absl::Nanoseconds(34000000), BucketizeDuration(34200040));
}

}  // namespace
