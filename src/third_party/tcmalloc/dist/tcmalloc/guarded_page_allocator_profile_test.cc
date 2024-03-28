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

#include <memory>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "absl/functional/function_ref.h"
#include "absl/log/check.h"
#include "tcmalloc/common.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {

// By placing this class in the tcmalloc_internal namespace, it may call the
// private method StackTraceFilter::Reset as a friend.
class GuardedPageAllocatorProfileTest : public testing::Test {
 public:
  struct NextSteps {
    bool stop = true;  // stop allocating
    bool free = true;  // free allocation
  };

  void SetUp() override { MallocExtension::ActivateGuardedSampling(); }

  // Return the number of allocations
  int AllocateUntil(size_t size,
                    absl::FunctionRef<NextSteps(void*)> evaluate_alloc) {
    int alloc_count = 0;
    while (true) {
      void* alloc = ::operator new(size);
      ++alloc_count;
      benchmark::DoNotOptimize(alloc);
      auto result = evaluate_alloc(alloc);
      // evaluate_alloc takes responsibility for delete/free if result.free is
      // set to false.
      if (result.free) {
        ::operator delete(alloc);
      }
      if (result.stop) {
        break;
      }
    }
    return alloc_count;
  }

  int AllocateGuardableUntil(
      size_t size, absl::FunctionRef<NextSteps(void*)> evaluate_alloc) {
    CHECK_LE(size, Static::guardedpage_allocator().page_size());
    return AllocateUntil(size, evaluate_alloc);
  }

  // Allocate until sample is guarded
  // Called to reduce the internal counter to -1, which will trigger resetting
  // the counter to the configured rate.
  void AllocateUntilGuarded() {
    AllocateGuardableUntil(968, [&](void* alloc) -> NextSteps {
      return {!IsNormalMemory(alloc) &&
                  Static::guardedpage_allocator().PointerIsMine(alloc),
              true};
    });
  }

  void ExamineSamples(
      Profile& profile, Profile::Sample::GuardedStatus sought_status,
      absl::FunctionRef<void(const Profile::Sample& s)> verify =
          [](const Profile::Sample& s) { /* do nothing */ }) {
    absl::flat_hash_set<Profile::Sample::GuardedStatus> found_statuses;
    int samples = 0;
    profile.Iterate([&](const Profile::Sample& s) {
      ++samples;
      found_statuses.insert(s.guarded_status);
      verify(s);
    });
    EXPECT_THAT(found_statuses, ::testing::Contains(sought_status));
  }

  void ResetStackTraceFilter() { tc_globals.guardedpage_allocator().Reset(); }
};

namespace {

TEST_F(GuardedPageAllocatorProfileTest, Guarded) {
  ScopedAlwaysSample always_sample;
  AllocateUntilGuarded();
  auto token = MallocExtension::StartAllocationProfiling();

  ResetStackTraceFilter();
  AllocateGuardableUntil(1051, [&](void* alloc) -> NextSteps {
    return {true, true};
  });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::Guarded);
}

TEST_F(GuardedPageAllocatorProfileTest, NotAttempted) {
  ScopedProfileSamplingRate profile_sampling_rate(4096);
  auto token = MallocExtension::StartAllocationProfiling();

  constexpr size_t alloc_size = 2 * 1024 * 1024;
  AllocateUntil(alloc_size, [&](void* alloc) -> NextSteps {
    return {true, true};
  });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::NotAttempted,
                 [&](const Profile::Sample& s) {
                   switch (s.guarded_status) {
                     case Profile::Sample::GuardedStatus::Guarded:
                       EXPECT_NE(alloc_size, s.requested_size);
                       break;
                     default:
                       break;
                   }
                 });
}

TEST_F(GuardedPageAllocatorProfileTest, LargerThanOnePage) {
  ScopedAlwaysSample always_sample;
  AllocateUntilGuarded();
  auto token = MallocExtension::StartAllocationProfiling();

  constexpr size_t alloc_size = kPageSize + 1;
  AllocateUntil(alloc_size, [&](void* alloc) -> NextSteps {
    return {true, true};
  });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::LargerThanOnePage,
                 [&](const Profile::Sample& s) {
                   switch (s.guarded_status) {
                     case Profile::Sample::GuardedStatus::Guarded:
                       EXPECT_NE(alloc_size, s.requested_size);
                       break;
                     default:
                       break;
                   }
                 });
}

TEST_F(GuardedPageAllocatorProfileTest, Disabled) {
  ScopedGuardedSamplingRate guarded_sampling_rate(-1);
  ScopedProfileSamplingRate profile_sampling_rate(1);
  auto token = MallocExtension::StartAllocationProfiling();

  AllocateGuardableUntil(1024, [&](void* alloc) -> NextSteps {
    return {true, true};
  });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::Disabled);
}

TEST_F(GuardedPageAllocatorProfileTest, RateLimited) {
  ScopedGuardedSamplingRate guarded_sampling_rate(1);
  ScopedProfileSamplingRate profile_sampling_rate(1);
  auto token = MallocExtension::StartAllocationProfiling();

  // Keep allocating until something is sampled
  constexpr size_t alloc_size = 1033;
  bool guarded_found = false;
  bool unguarded_found = false;
  AllocateGuardableUntil(alloc_size, [&](void* alloc) -> NextSteps {
    if (!IsNormalMemory(alloc)) {
      if (Static::guardedpage_allocator().PointerIsMine(alloc)) {
        guarded_found = true;
        ResetStackTraceFilter();
      } else {
        unguarded_found = true;
      }
      return {guarded_found && unguarded_found, true};
    }
    return {false, true};
  });

  // Ensure Guarded and RateLimited both occur for the alloc_size
  bool success_found = false;
  bool ratelimited_found = false;
  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::RateLimited,
                 [&](const Profile::Sample& s) {
                   if (s.requested_size != alloc_size) {
                     return;
                   }
                   switch (s.guarded_status) {
                     case Profile::Sample::GuardedStatus::Guarded:
                       success_found = true;
                       break;
                     case Profile::Sample::GuardedStatus::RateLimited:
                       ratelimited_found = true;
                       break;
                     default:
                       break;
                   }
                 });
  EXPECT_TRUE(success_found);
  EXPECT_TRUE(ratelimited_found);
}

TEST_F(GuardedPageAllocatorProfileTest, TooSmall) {
  ScopedAlwaysSample always_sample;
  AllocateUntilGuarded();
  auto token = MallocExtension::StartAllocationProfiling();

  // Next sampled allocation should be too small
  constexpr size_t alloc_size = 0;
  AllocateGuardableUntil(alloc_size, [&](void* alloc) -> NextSteps {
    return {true, true};
  });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::TooSmall,
                 [&](const Profile::Sample& s) {
                   switch (s.guarded_status) {
                     case Profile::Sample::GuardedStatus::Guarded:
                       EXPECT_NE(alloc_size, s.requested_size);
                       break;
                     case Profile::Sample::GuardedStatus::TooSmall:
                       EXPECT_EQ(alloc_size, s.requested_size);
                       break;
                     default:
                       break;
                   }
                 });
}

TEST_F(GuardedPageAllocatorProfileTest, NoAvailableSlots) {
  ScopedAlwaysSample always_sample;
  AllocateUntilGuarded();

  std::vector<std::unique_ptr<void, void (*)(void*)>> allocs;
  // Guard until there are no slots available.
  AllocateGuardableUntil(1039, [&](void* alloc) -> NextSteps {
    if (Static::guardedpage_allocator().PointerIsMine(alloc)) {
      allocs.emplace_back(alloc,
                          static_cast<void (*)(void*)>(::operator delete));
      ResetStackTraceFilter();
      return {Static::guardedpage_allocator().GetNumAvailablePages() == 0,
              false};
    }
    return {false, true};
  });

  auto token = MallocExtension::StartAllocationProfiling();
  // This should  fail for lack of slots
  AllocateGuardableUntil(1055, [&](void* alloc) -> NextSteps {
    return {!Static::guardedpage_allocator().PointerIsMine(alloc), true};
  });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::NoAvailableSlots);
}

TEST_F(GuardedPageAllocatorProfileTest, NeverSample) {
  ScopedProfileSamplingRate profile_sampling_rate(0);
  auto token = MallocExtension::StartAllocationProfiling();

  // This will not succeed in guarding anything.
  int alloc_count = AllocateGuardableUntil(1025, [&](void* alloc) -> NextSteps {
    return {true, true};
  });
  ASSERT_EQ(alloc_count, 1);

  auto profile = std::move(token).Stop();
  int samples = 0;
  profile.Iterate([&](const Profile::Sample& s) { ++samples; });
  EXPECT_EQ(samples, 0);
}

TEST_F(GuardedPageAllocatorProfileTest, Filtered) {
  auto token = MallocExtension::StartAllocationProfiling();
  int guarded_count = 0;
  AllocateGuardableUntil(1058, [&](void* alloc) -> NextSteps {
    guarded_count += Static::guardedpage_allocator().PointerIsMine(alloc);
    return {guarded_count == 4, true};
  });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::Filtered);
}

TEST_F(GuardedPageAllocatorProfileTest, FilteredWithRateLimiting) {
  // Have to have a rate that is less than every single one.
  ScopedGuardedSamplingRate scoped_guarded_sampling_rate(
      2 * tcmalloc::tcmalloc_internal::Parameters::profile_sampling_rate());
  AllocateUntilGuarded();

  auto token = MallocExtension::StartAllocationProfiling();
  // Obtain a few sample guarding canidates, which will eventualy yield at least
  // one that is filtered.
  int guarded_count = 0;
  int sampled_count = 0;
  AllocateGuardableUntil(1062, [&](void* alloc) -> NextSteps {
    if (!IsNormalMemory(alloc)) {
      if (Static::guardedpage_allocator().PointerIsMine(alloc)) {
        ++guarded_count;
      }
      ++sampled_count;
    }
    return {sampled_count > 1000, true};
  });

  EXPECT_GT(guarded_count, 0);

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::Filtered);
}

TEST_F(GuardedPageAllocatorProfileTest, DynamicParamChange) {
  ScopedGuardedSamplingRate scoped_guarded_sampling_rate(
      2 * tcmalloc::tcmalloc_internal::Parameters::profile_sampling_rate());
  for (int loop_count = 0; loop_count < 10; ++loop_count) {
    AllocateUntilGuarded();

    // Accumulate at least 2 guarded allocations.
    auto token = MallocExtension::StartAllocationProfiling();
    int guarded_count = 0;
    AllocateGuardableUntil(1063, [&](void* alloc) -> NextSteps {
      if (Static::guardedpage_allocator().PointerIsMine(alloc)) {
        ++guarded_count;
        ResetStackTraceFilter();
      }
      return {guarded_count > 1, true};
    });

    auto profile = std::move(token).Stop();
    ExamineSamples(profile, Profile::Sample::GuardedStatus::Guarded);
  }
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
