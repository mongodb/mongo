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

#include <atomic>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <thread>  // NOLINT(build/c++11)
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/blocking_counter.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "tcmalloc/internal/declarations.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/new_extension.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

TEST(AllocationSampleTest, TokenAbuse) {
  auto token = MallocExtension::StartAllocationProfiling();
  void* ptr = ::operator new(512 * 1024 * 1024);
  // TODO(b/183453911): Remove workaround for GCC 10.x deleting operator new,
  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=94295.
  benchmark::DoNotOptimize(ptr);
  ::operator delete(ptr);
  // Repeated Claims should happily return null.
  auto profile = std::move(token).Stop();
  int count = 0;
  profile.Iterate([&](const Profile::Sample&) { count++; });

#if !defined(UNDEFINED_BEHAVIOR_SANITIZER)
  // UBSan does not implement our profiling API, but running the test can
  // validate the correctness of the new/delete pairs.
  EXPECT_EQ(count, 1);
#endif

  auto profile2 = std::move(token).Stop();  // NOLINT: use-after-move intended
  int count2 = 0;
  profile2.Iterate([&](const Profile::Sample&) { count2++; });
  EXPECT_EQ(count2, 0);

  // Delete (on the scope ending) without Claim should also be OK.
  { MallocExtension::StartAllocationProfiling(); }
}

// Verify that profiling sessions concurrent with allocations do not crash due
// to mutating pointers accessed by the sampling code (b/143623146).
TEST(AllocationSampleTest, RaceToClaim) {
  MallocExtension::SetProfileSamplingRate(1 << 14);

  absl::BlockingCounter counter(2);
  std::atomic<bool> stop{false};

  std::thread t1([&]() {
    counter.DecrementCount();

    while (!stop) {
      auto token = MallocExtension::StartAllocationProfiling();
      absl::SleepFor(absl::Microseconds(1));
      auto profile = std::move(token).Stop();
    }
  });

  std::thread t2([&]() {
    counter.DecrementCount();

    const int kNum = 1000000;
    std::vector<void*> ptrs;
    while (!stop) {
      for (int i = 0; i < kNum; i++) {
        ptrs.push_back(::operator new(1));
      }
      for (void* p : ptrs) {
        sized_delete(p, 1);
      }
      ptrs.clear();
    }
  });

  // Verify the threads are up and running before we start the clock.
  counter.Wait();

  absl::SleepFor(absl::Seconds(1));

  stop.store(true);

  t1.join();
  t2.join();
}

// Similar to the AllocationSampleTest but for DeallocationSample which uses a
// similar API susceptible to the same race condition. It should be possible to
// combine these to into a single test if the deallocation profiler is
// refactored to use a template-d interface shared with the allocation profiler.
TEST(DeallocationSampleTest, RaceToClaim) {
  MallocExtension::SetProfileSamplingRate(1 << 14);

  absl::BlockingCounter counter(2);
  std::atomic<bool> stop{false};

  std::thread t1([&]() {
    counter.DecrementCount();

    while (!stop) {
      auto token = MallocExtension::StartLifetimeProfiling();
      absl::SleepFor(absl::Microseconds(1));
      auto profile = std::move(token).Stop();
    }
  });

  std::thread t2([&]() {
    counter.DecrementCount();

    const int kNum = 1000000;
    std::vector<void*> ptrs;
    while (!stop) {
      for (int i = 0; i < kNum; i++) {
        ptrs.push_back(::operator new(1));
      }
      for (void* p : ptrs) {
        sized_delete(p, 1);
      }
      ptrs.clear();
    }
  });

  // Verify the threads are up and running before we start the clock.
  counter.Wait();

  absl::SleepFor(absl::Seconds(1));

  stop.store(true);

  t1.join();
  t2.join();
}

TEST(AllocationSampleTest, SampleAccuracy) {
  // Disable GWP-ASan, since it allocates different sizes than normal samples.
  MallocExtension::SetGuardedSamplingRate(-1);

  // Allocate about 512 MiB each of various sizes. For _some_ but not all
  // sizes, delete it as we go--it shouldn't matter for the sample count.
  static const size_t kTotalPerSize = 512 * 1024 * 1024;

  // (object size, object alignment, keep objects)
  struct Requests {
    size_t size;
    size_t alignment;
    std::optional<tcmalloc::hot_cold_t> hot_cold;
    bool expected_hot;
    bool keep;
    // objects we don't delete as we go
    void* list = nullptr;
  };
  std::vector<Requests> sizes = {
      {8, 0, std::nullopt, true, false},
      {16, 16, std::nullopt, true, true},
      {1024, 0, std::nullopt, true, false},
      {64 * 1024, 64, std::nullopt, true, false},
      {512 * 1024, 0, std::nullopt, true, true},
      {1024 * 1024, 128, std::nullopt, true, true},
      // As an implementation detail, 32 is not allocated to a cold size class.
      {32, 0, tcmalloc::hot_cold_t{0}, true, true},
      {64, 0, tcmalloc::hot_cold_t{255}, true, true},
      {8192, 0, tcmalloc::hot_cold_t{0}, false, true},
  };
  absl::btree_set<size_t> sizes_expected;
  for (auto s : sizes) {
    sizes_expected.insert(s.size);
  }
  auto token = MallocExtension::StartAllocationProfiling();

  // We use new/delete to allocate memory, as malloc returns objects aligned to
  // std::max_align_t.
  for (auto& s : sizes) {
    for (size_t bytes = 0; bytes < kTotalPerSize; bytes += s.size) {
      void* obj;
      if (s.alignment > 0) {
        obj = operator new(s.size, static_cast<std::align_val_t>(s.alignment));
      } else if (s.hot_cold.has_value()) {
        obj = operator new(s.size, *s.hot_cold);
      } else {
        obj = operator new(s.size);
      }
      if (s.keep) {
        tcmalloc_internal::SLL_Push(&s.list, obj);
      } else if (s.alignment > 0) {
        operator delete(obj, static_cast<std::align_val_t>(s.alignment));
      } else {
        sized_delete(obj, s.size);
      }
    }
  }
  auto profile = std::move(token).Stop();

  // size -> bytes seen
  absl::flat_hash_map<size_t, size_t> m;

  // size -> alignment request
  absl::flat_hash_map<size_t, size_t> alignment;

  // size -> access_hint
  absl::flat_hash_map<size_t, hot_cold_t> access_hint;

  // size -> access_allocated
  absl::flat_hash_map<size_t, Profile::Sample::Access> access_allocated;

  for (auto s : sizes) {
    alignment[s.size] = s.alignment;
    access_hint[s.size] = s.hot_cold.value_or(hot_cold_t{255});
    access_allocated[s.size] = s.expected_hot ? Profile::Sample::Access::Hot
                                              : Profile::Sample::Access::Cold;
  }

  profile.Iterate([&](const tcmalloc::Profile::Sample& e) {
    // Skip unexpected sizes.  They may have been triggered by a background
    // thread.
    if (sizes_expected.find(e.allocated_size) == sizes_expected.end()) {
      return;
    }

    SCOPED_TRACE(e.requested_size);

    // Don't check stack traces until we have evidence that's broken, it's
    // tedious and done fairly well elsewhere.
    m[e.allocated_size] += e.sum;
    EXPECT_EQ(alignment[e.requested_size], e.requested_alignment);
    EXPECT_EQ(access_hint[e.requested_size], e.access_hint);
    EXPECT_EQ(access_allocated[e.requested_size], e.access_allocated);
  });

#if !defined(UNDEFINED_BEHAVIOR_SANITIZER)
  // UBSan does not implement our profiling API, but running the test can
  // validate the correctness of the new/delete pairs.
  size_t max_bytes = 0, min_bytes = std::numeric_limits<size_t>::max();
  EXPECT_EQ(m.size(), sizes_expected.size());
  for (auto seen : m) {
    size_t bytes = seen.second;
    min_bytes = std::min(min_bytes, bytes);
    max_bytes = std::max(max_bytes, bytes);
  }
  // Hopefully we're in a fairly small range, that contains our actual
  // allocation.
  EXPECT_GE((min_bytes * 3) / 2, max_bytes);
  EXPECT_LE((min_bytes * 3) / 4, kTotalPerSize);
  EXPECT_LE(kTotalPerSize, (max_bytes * 4) / 3);
#endif

  // Remove the objects we left alive
  for (auto& s : sizes) {
    while (s.list != nullptr) {
      void* obj = tcmalloc_internal::SLL_Pop(&s.list);
      if (s.alignment > 0) {
        operator delete(obj, static_cast<std::align_val_t>(s.alignment));
      } else {
        operator delete(obj);
      }
    }
  }
}

TEST(FragmentationzTest, Accuracy) {
  // Increase sampling rate to decrease flakiness.
  ScopedProfileSamplingRate ps(512 * 1024);
  // Disable GWP-ASan, since it allocates different sizes than normal samples.
  ScopedGuardedSamplingRate gs(-1);

  // a fairly odd allocation size - will be rounded to 128.  This lets
  // us find our record in the table.
  static const size_t kItemSize = 115;
  // allocate about 3.5 GiB:
  static const size_t kNumItems = 32 * 1024 * 1024;

  std::vector<std::unique_ptr<char[]>> keep;
  std::vector<std::unique_ptr<char[]>> drop;
  // hint expected sizes:
  drop.reserve(kNumItems * 8 / 10);
  keep.reserve(kNumItems * 2 / 10);

  // We allocate many items, then free 80% of them "randomly". (To
  // decrease noise and speed up, we just keep every 5th one exactly.)
  for (int i = 0; i < kNumItems; ++i) {
    // Ideally we should use a malloc() here, for consistency; but unique_ptr
    // doesn't come with a have a "free()" deleter; use ::operator new insted.
    (i % 5 == 0 ? keep : drop)
        .push_back(std::unique_ptr<char[]>(
            static_cast<char*>(::operator new[](kItemSize))));
  }
  drop.resize(0);

  // there are at least 64 items per span here. (8/10)^64 = 6.2e-7 ~= 0
  // probability we actually managed to free a page; every page is fragmented.
  // We still have 20% or so of it allocated, so we should see 80% of it
  // charged to these allocations as fragmentations.
  auto profile = MallocExtension::SnapshotCurrent(ProfileType::kFragmentation);

  // Pull out the fragmentationz entry corresponding to this
  size_t requested_size = 0;
  size_t allocated_size = 0;
  size_t sum = 0;
  size_t count = 0;
  profile.Iterate([&](const Profile::Sample& e) {
    if (e.requested_size != kItemSize) return;

    if (requested_size == 0) {
      allocated_size = e.allocated_size;
      requested_size = e.requested_size;
    } else {
      // we will usually have single entry in
      // profile, but in builds without optimization
      // our fast-path code causes same call-site to
      // have two different stack traces. Thus we
      // expect and deal with second entry for same
      // allocation.
      EXPECT_EQ(requested_size, e.requested_size);
      EXPECT_EQ(allocated_size, e.allocated_size);
    }
    sum += e.sum;
    count += e.count;
  });

  double frag_bytes = sum;
  double real_frag_bytes =
      static_cast<double>(allocated_size * kNumItems) * 0.8;
  // We should be pretty close with this much data.
  EXPECT_NEAR(real_frag_bytes, frag_bytes, real_frag_bytes * 0.15)
      << " sum = " << sum << " allocated = " << allocated_size
      << " requested = " << requested_size << " count = " << count;
}

}  // namespace
}  // namespace tcmalloc
