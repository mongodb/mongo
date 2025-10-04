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
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <thread>  // NOLINT(build/c++11)
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

struct AllocatorArgs {
  std::vector<void *> *arr;
  size_t req_size;
  size_t req_alignment;
  size_t block_objects;
  size_t m_dealloc_funcs;
  size_t n_alloc_funcs;
};

ABSL_ATTRIBUTE_NOINLINE ABSL_ATTRIBUTE_NO_TAIL_CALL void AllocateBaseCase(
    size_t offset, const AllocatorArgs &args) {
  size_t alloc_objects = args.m_dealloc_funcs * args.block_objects;
  for (size_t i = offset; i < offset + alloc_objects; ++i) {
    (*args.arr)[i] = operator new(
        args.req_size, static_cast<std::align_val_t>(args.req_alignment));
  }
}

ABSL_ATTRIBUTE_NOINLINE ABSL_ATTRIBUTE_NO_TAIL_CALL void AllocateRecursive(
    size_t offset, size_t recursion_depth, const AllocatorArgs &args) {
  if (recursion_depth == 0) {
    AllocateBaseCase(offset, args);
  } else {
    AllocateRecursive(offset, recursion_depth - 1, args);
  }
}
ABSL_ATTRIBUTE_NOINLINE ABSL_ATTRIBUTE_NO_TAIL_CALL void DeallocBaseCase(
    size_t offset, const AllocatorArgs &args) {
  for (size_t i = offset; i < offset + args.block_objects; ++i) {
    operator delete((*args.arr)[i],
                    static_cast<std::align_val_t>(args.req_alignment));
  }
}

ABSL_ATTRIBUTE_NOINLINE ABSL_ATTRIBUTE_NO_TAIL_CALL void DeallocRecursive(
    size_t offset, size_t recursion_depth, const AllocatorArgs &args) {
  if (recursion_depth == 0) {
    DeallocBaseCase(offset, args);
  } else {
    DeallocRecursive(offset, recursion_depth - 1, args);
  }
}

struct Params {
  int dealloc_bucket_counter;
  absl::Duration duration;
  absl::Duration sleep_time;
  size_t size;
  size_t alignment;
};

void testEntry(Params &p, const tcmalloc::Profile::Sample &e) {
  if (e.requested_size == p.size && e.requested_alignment == p.alignment) {
    EXPECT_LE(e.avg_lifetime, p.duration);
    EXPECT_GE(e.avg_lifetime, p.sleep_time / 1000);

    EXPECT_GE(e.stddev_lifetime, absl::ZeroDuration());

    // Comparing the standard deviation only makes sense if minimum and maximum
    // lifetime are not in the same bucket.
    absl::Duration diff = e.max_lifetime - e.min_lifetime;
    if (diff > absl::ZeroDuration()) {
      EXPECT_LE(e.stddev_lifetime, diff / 2);
    }

    EXPECT_LE(e.min_lifetime, p.duration);
    EXPECT_GE(e.min_lifetime, p.sleep_time);
    EXPECT_LE(e.max_lifetime, p.duration);
    EXPECT_GE(e.max_lifetime, p.sleep_time);
    EXPECT_LE(e.min_lifetime, e.avg_lifetime);
    EXPECT_LE(e.avg_lifetime, e.max_lifetime);

    auto log_optional_bool = [](std::optional<bool> item) {
      if (!item.has_value()) {
        return "none";
      }
      return item.value() ? "true" : "false";
    };

    p.dealloc_bucket_counter++;
    LOG(INFO) << "-------------------------------------------";
    LOG(INFO) << "e.count:" << e.count;
    LOG(INFO) << "e.allocated_size:" << e.allocated_size;
    LOG(INFO) << "e.requested_size:" << e.requested_size;
    LOG(INFO) << "e.requested_alignment" << e.requested_alignment;
    LOG(INFO) << "e.avg_lifetime:" << e.avg_lifetime;
    LOG(INFO) << "e.min_lifetime:" << e.min_lifetime;
    LOG(INFO) << "e.max_lifetime:" << e.max_lifetime;
    LOG(INFO) << "e.stddev_lifetime:" << e.stddev_lifetime;
    LOG(INFO) << "e.allocator_deallocator_physical_cpu_matched:"
              << log_optional_bool(
                     e.allocator_deallocator_physical_cpu_matched);
    LOG(INFO) << "e.allocator_deallocator_virtual_cpu_matched:"
              << log_optional_bool(e.allocator_deallocator_virtual_cpu_matched);
    LOG(INFO) << "e.allocator_deallocator_l3_matched:"
              << log_optional_bool(e.allocator_deallocator_l3_matched);
    LOG(INFO) << "e.allocator_deallocator_thread_matched:"
              << log_optional_bool(e.allocator_deallocator_thread_matched);
    LOG(INFO) << "e.profile_id:" << e.profile_id;
  }
}

class DeallocationzTest : public ::testing::Test {
 public:

  void Run() {
    if (CheckerIsActive()) {
      LOG(INFO) << "Skipping checks due to enabled checkers";
      return;
    }

    size_t objects =
        t_threads_ * n_alloc_funcs_ * m_dealloc_funcs_ * block_objects_;
    arr_.resize(objects);
    auto token = tcmalloc::MallocExtension::StartLifetimeProfiling();
    int64_t start_time = absl::GetCurrentTimeNanos();
    AllocDealloc();
    int64_t end_time = absl::GetCurrentTimeNanos();
    const tcmalloc::Profile profile = std::move(token).Stop();
    ASSERT_EQ(profile.Type(), tcmalloc::ProfileType::kLifetimes);

    Params p;
    p.dealloc_bucket_counter = 0;
    p.duration = absl::Nanoseconds(end_time - start_time);
    p.sleep_time = sleep_time_;
    p.size = req_size_;
    p.alignment = req_alignment_;

    // NOTE that function testEntry(...) gets called twice in Iterate() method
    // for deallocationz (one for allocation stack trace and one for
    // deallocation stack trace). This is important in the following
    // calculations.
    profile.Iterate(
        [&p](const tcmalloc::Profile::Sample &entry) { testEntry(p, entry); });

    // for multi-threaded, t_threads must be > 2 (due to design of the test)
    if (t_threads_ > 2) {
      // With t_threads_ > 2, we have no context, rpc_id 0, rpc_id 1, so we'll
      // see the full spectrum of possible context states.
      constexpr int kContexts = 3;

      // In each bucket, at least two counts are bigger than zero:
      //    - counts[cpu:similar, thread:similar] >= 1 -> 2 testEntry() calls
      //    - counts[cpu:similar, thread:different] >= 1 -> 2 testEntry() calls
      // So, in total, testEntry() function gets called 4 times for each
      // deallocationz bucket, which leads to at least 4 times increment of
      // p.dealloc_bucket_counter. So, having n allocator functions and m
      // deallocator functions, assuming 100% coverage, (nxm) deallocationz
      // buckets are expected and as a result, in total, we expect the value of
      // p.dealloc_bucket_counter to be at least 4x(nxm).
      EXPECT_GE(p.dealloc_bucket_counter,
                4 * n_alloc_funcs_ * m_dealloc_funcs_);
      // In the worst case, for each deallocationz bucket, all 4 possible cases
      // can happen (cpu:similar/different && thread:similar/different). So,
      // expecting 8 times calling function testEntry() and as a result, 8 times
      // of increment of the counter p.dealloc_bucket_counter for each bucket.
      // Having n allocator and m deallocator functions, in total 8x(nxm)
      // increment of the counter p.dealloc_bucket_counter
      EXPECT_LE(p.dealloc_bucket_counter,
                8 * kContexts * n_alloc_funcs_ * m_dealloc_funcs_);
    } else {  // if (t_threads <= 2)
      // In single-threaded case, each Bucket, at least
      // counts[thread:similar, cpu:similar] is positive: 2 testEntry() calls.
      // As a result, we have at least 2 times increment of
      // p.dealloc_bucket_counter. So, having n allocator functions and m
      // deallocator functions, assuming 100% coverage, (nxm) deallocationz
      // buckets are expect{ed} and as a result, in total, we expect the value
      // of p.dealloc_bucket_counter to be at least 2x(nxm).
      EXPECT_GE(p.dealloc_bucket_counter,
                2 * n_alloc_funcs_ * m_dealloc_funcs_);
      // In the worst case, for each deallocationz bucket, all the following
      // cases can happen:
      //  - (cpu:simliar, thread: similar) --> two testEntry() function calls.
      //  - (cpu:different, thread: similar) --> two testEntry() function calls.
      // So, in total, in the worst case, it is possible that the function
      // testEntry() gets called 4 times (which leads to 4 times of increment of
      // the counter p.dealloc_bucket_counter)
      EXPECT_LE(p.dealloc_bucket_counter,
                16 * n_alloc_funcs_ * m_dealloc_funcs_);
    }
  }

  void RunAlloc() {
    size_t objects =
        t_threads_ * n_alloc_funcs_ * m_dealloc_funcs_ * block_objects_;
    arr_.resize(objects);
    Allocate();
  }

  void RunDelloc() {
    size_t objects =
        t_threads_ * n_alloc_funcs_ * m_dealloc_funcs_ * block_objects_;
    arr_.resize(objects);
    Deallocate();
  }

 private:
  void AllocDealloc() {
    Allocate();
    // For testing allocation lifetime
    absl::SleepFor(sleep_time_);
    Deallocate();
  }

  void Allocate() {
    const AllocatorArgs args{&arr_,          req_size_,        req_alignment_,
                             block_objects_, m_dealloc_funcs_, n_alloc_funcs_};
    auto Run = [&args](const size_t offset, std::optional<uint64_t> rpc_id) {
      size_t alloc_objects = args.m_dealloc_funcs * args.block_objects;
      size_t new_offset = offset;
      for (size_t i = 0; i < args.n_alloc_funcs; ++i) {
        AllocateRecursive(new_offset, i, args);
        new_offset += alloc_objects;
      }
    };

    size_t offset = 0;
    size_t thread_objects = n_alloc_funcs_ * m_dealloc_funcs_ * block_objects_;
    std::vector<std::thread> threads;
    for (size_t i = 0; i < t_threads_; ++i) {
      std::optional<uint64_t> rpc_id;
      if (i > 0) {
        rpc_id = i % 2;
      }

      // Keep first thread constant between allocation/deallocation
      if (i == 0) {
        Run(offset, rpc_id);
      } else {
        // Capture by value so that we don't race on params.
        threads.emplace_back([=]() { Run(offset, rpc_id); });
      }
      offset += thread_objects;
    }
    for (auto &thread : threads) thread.join();
  }

  void Deallocate() {
    const AllocatorArgs args{&arr_,          req_size_,        req_alignment_,
                             block_objects_, m_dealloc_funcs_, n_alloc_funcs_};
    auto Run = [&args](const size_t offset, std::optional<uint64_t> rpc_id) {
      size_t new_offset = offset;
      for (size_t i = 0; i < args.n_alloc_funcs; ++i) {
        for (size_t j = 0; j < args.m_dealloc_funcs; ++j) {
          DeallocRecursive(new_offset, j, args);
          new_offset += args.block_objects;
        }
      }
    };

    std::vector<std::thread> threads;
    size_t thread_objects = n_alloc_funcs_ * m_dealloc_funcs_ * block_objects_;
    for (size_t i = 0; i < t_threads_; ++i) {
      std::optional<uint64_t> rpc_id;
      if (i > 0) {
        rpc_id = i % 2;
      }

      size_t offset = 0;
      if (i > 0) {
        // calculating the rotated offset
        size_t i_rotated = (i + 1) % t_threads_;
        if (i_rotated == 0) {
          i_rotated++;
        }
        offset = i_rotated * thread_objects;
      }

      // Keep first thread constant between allocation/deallocation
      if (i == 0) {
        Run(offset, rpc_id);
      } else {
        threads.emplace_back(
            // Capture by value so that we don't race on params.
            [=]() { Run(offset, rpc_id); });
      }
    }
    for (auto &thread : threads) thread.join();
  }

  std::vector<void *> arr_;

 protected:
  size_t req_size_;
  size_t req_alignment_;
  size_t block_objects_;
  size_t m_dealloc_funcs_;
  size_t n_alloc_funcs_;
  size_t t_threads_;
  absl::Duration sleep_time_;
};

// Single-threaded TEST
TEST_F(DeallocationzTest, SingleThreaded) {
  n_alloc_funcs_ = 3;
  m_dealloc_funcs_ = 4;
  block_objects_ = 10000;
  t_threads_ = 1;
  sleep_time_ = absl::Seconds(5);
  req_size_ = 1024 * 1024;
  req_alignment_ = 64;
  Run();
}

// Multi-threaded TEST
TEST_F(DeallocationzTest, MultiThreaded) {
  n_alloc_funcs_ = 2;
  m_dealloc_funcs_ = 2;
  block_objects_ = 10000;
  // for multi-threaded, t_threads must be > 2 (due to design of the test).
  t_threads_ = 4;
  sleep_time_ = absl::Seconds(9);
  req_size_ = 1024 * 1024;
  req_alignment_ = 64;
  Run();
}

// Test that having multiple concurrent deallocationz samples works
TEST_F(DeallocationzTest, ConcurrentDeallocationSamples) {
  if (CheckerIsActive()) {
    LOG(INFO) << "Skipping checks due to enabled checkers";
    return;
  }
  auto token = tcmalloc::MallocExtension::StartLifetimeProfiling();
  auto token2 = tcmalloc::MallocExtension::StartLifetimeProfiling();
  absl::SleepFor(absl::Milliseconds(100));
  auto token3 = tcmalloc::MallocExtension::StartLifetimeProfiling();

  EXPECT_GT(std::move(token).Stop().Duration(), absl::ZeroDuration());
  absl::SleepFor(absl::Milliseconds(100));
  EXPECT_GT(std::move(token2).Stop().Duration(), absl::ZeroDuration());
  EXPECT_GT(std::move(token3).Stop().Duration(), absl::ZeroDuration());
}

// Test LowLevelAlloc::Arena allocation/deallocation by having multiple
// iterations of the same tests, exercising the arena logic.
TEST_F(DeallocationzTest, MultipleAllocationPeriods) {
  if (CheckerIsActive()) {
    LOG(INFO) << "Skipping checks due to enabled checkers";
    return;
  }
  for (int i = 0; i < 100; i++) {
    auto token = tcmalloc::MallocExtension::StartLifetimeProfiling();
    absl::SleepFor(absl::Milliseconds(5));
    auto token2 = tcmalloc::MallocExtension::StartLifetimeProfiling();

    const tcmalloc::Profile unused = std::move(token).Stop();
    absl::SleepFor(absl::Milliseconds(1));
    EXPECT_GT(std::move(token2).Stop().Duration(), absl::ZeroDuration());
    absl::SleepFor(absl::Milliseconds(1));
  }
}

// Test for concurrency issues with multiple concurrent calls into the profiler
TEST_F(DeallocationzTest, ConcurrentProfilerRequests) {
  const int kThreadCount = 100;
  const int kIterations = 5;

  std::vector<std::thread> threads;
  for (int i = 0; i < kIterations; ++i) {
    for (int j = 0; j < kThreadCount; ++j) {
      threads.emplace_back([]() {
        auto token = tcmalloc::MallocExtension::StartLifetimeProfiling();
        absl::SleepFor(absl::Milliseconds(2));
        const tcmalloc::Profile unused = std::move(token).Stop();
        absl::SleepFor(absl::Milliseconds(1));
      });
    }
  }
  for (auto &thread : threads) thread.join();
}

TEST_F(DeallocationzTest, ConcurrentProfilerEnableDisable) {
  const absl::Duration kDuration = absl::Milliseconds(100);
  const int kIterations = 20;

  n_alloc_funcs_ = 3;
  m_dealloc_funcs_ = 4;
  block_objects_ = 64;
  t_threads_ = 1;
  sleep_time_ = absl::ZeroDuration();
  req_size_ = 1024 * 1024;
  req_alignment_ = 64;

  std::atomic<bool> completed = false;
  std::thread profiler_thread([&]() {
    for (int i = 0; i < kIterations; i++) {
      auto token = tcmalloc::MallocExtension::StartLifetimeProfiling();
      absl::SleepFor(kDuration);
      const tcmalloc::Profile unused = std::move(token).Stop();
      absl::SleepFor(absl::Milliseconds(1));
    }
    completed.store(true, std::memory_order_release);
  });

  while (!completed.load(std::memory_order_acquire)) {
    RunAlloc();
    RunDelloc();
    absl::SleepFor(absl::Milliseconds(1));
  }

  profiler_thread.join();
}

// Ensure we are not leaking memory for allocations where we did not observe
// the deallocation event
TEST_F(DeallocationzTest, ObserveAllocationButNotDeallocation) {
  if (CheckerIsActive()) {
    LOG(INFO) << "Skipping checks due to enabled checkers";
    return;
  }
  n_alloc_funcs_ = 3;
  m_dealloc_funcs_ = 4;
  block_objects_ = 64;
  t_threads_ = 1;
  sleep_time_ = absl::ZeroDuration();
  req_size_ = 1024 * 1024;
  req_alignment_ = 64;

  auto token = tcmalloc::MallocExtension::StartLifetimeProfiling();
  RunAlloc();
  EXPECT_GT(std::move(token).Stop().Duration(), absl::ZeroDuration());
  RunDelloc();
}

// Test that we can handle observing deallocations where we did not observe
// the allocation event
TEST_F(DeallocationzTest, ObserveDeallocationButNotAllocation) {
  if (CheckerIsActive()) {
    LOG(INFO) << "Skipping checks due to enabled checkers";
    return;
  }
  n_alloc_funcs_ = 3;
  m_dealloc_funcs_ = 4;
  block_objects_ = 64;
  t_threads_ = 1;
  sleep_time_ = absl::ZeroDuration();
  req_size_ = 1024 * 1024;
  req_alignment_ = 64;

  RunAlloc();
  auto token = tcmalloc::MallocExtension::StartLifetimeProfiling();
  RunDelloc();
  EXPECT_GT(std::move(token).Stop().Duration(), absl::ZeroDuration());
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

    EXPECT_TRUE(e.allocator_deallocator_physical_cpu_matched.has_value());
    EXPECT_TRUE(e.allocator_deallocator_virtual_cpu_matched.has_value());
    EXPECT_TRUE(e.allocator_deallocator_l3_matched.has_value());
    EXPECT_TRUE(e.allocator_deallocator_numa_matched.has_value());
    ASSERT_TRUE(e.allocator_deallocator_thread_matched.has_value());
    EXPECT_TRUE(e.allocator_deallocator_thread_matched.value());

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
  EXPECT_LE(counters.samples_count, 48);

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
