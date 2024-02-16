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
#include <string>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "absl/functional/function_ref.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

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

  // Allocate until sample is guarded
  // Called to reduce the internal counter to -1, which will trigger resetting
  // the counter to the configured rate.
  void AllocateUntilGuarded() {
    AllocateUntil(968, [&](void* alloc) -> NextSteps {
      return {IsSampledMemory(alloc) &&
                  Static::guardedpage_allocator().PointerIsMine(alloc),
              true};
    });
  }

  void ExamineSamples(
      Profile& profile, Profile::Sample::GuardedStatus sought_status,
      absl::flat_hash_set<Profile::Sample::GuardedStatus> allowable_statuses,
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
    found_statuses.erase(sought_status);
    EXPECT_THAT(found_statuses, ::testing::IsSubsetOf(allowable_statuses));
  }
};

TEST_F(GuardedPageAllocatorProfileTest, Guarded) {
  ScopedAlwaysSample sas;
  AllocateUntilGuarded();
  auto token = MallocExtension::StartAllocationProfiling();

  AllocateUntil(1051, [&](void* alloc) -> NextSteps { return {true, true}; });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::Guarded, {});
}

TEST_F(GuardedPageAllocatorProfileTest, NotAttempted) {
  ScopedProfileSamplingRate spsr(4096);
  auto token = MallocExtension::StartAllocationProfiling();

  constexpr size_t alloc_size = 2 * 1024 * 1024;
  AllocateUntil(alloc_size, [&](void* alloc) -> NextSteps {
    return {true, true};
  });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::NotAttempted,
                 {Profile::Sample::GuardedStatus::Guarded},
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
  ScopedAlwaysSample sas;
  AllocateUntilGuarded();
  auto token = MallocExtension::StartAllocationProfiling();

  constexpr size_t alloc_size = kPageSize + 1;
  AllocateUntil(alloc_size, [&](void* alloc) -> NextSteps {
    return {true, true};
  });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::LargerThanOnePage,
                 {Profile::Sample::GuardedStatus::Guarded},
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
  ScopedGuardedSamplingRate sgsr(-1);
  ScopedProfileSamplingRate spsr(1);
  auto token = MallocExtension::StartAllocationProfiling();

  AllocateUntil(1024, [&](void* alloc) -> NextSteps { return {true, true}; });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::Disabled, {});
}

TEST_F(GuardedPageAllocatorProfileTest, RateLimited) {
  ScopedGuardedSamplingRate sgsr(1);
  ScopedProfileSamplingRate spsr(1);
  auto token = MallocExtension::StartAllocationProfiling();

  // Keep allocating until something is sampled
  constexpr size_t alloc_size = 1033;
  bool guarded_found = false;
  bool unguarded_found = false;
  AllocateUntil(alloc_size, [&](void* alloc) -> NextSteps {
    if (IsSampledMemory(alloc)) {
      if (Static::guardedpage_allocator().PointerIsMine(alloc)) {
        guarded_found = true;
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
                 {Profile::Sample::GuardedStatus::Guarded},
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
  ScopedAlwaysSample sas;
  AllocateUntilGuarded();
  auto token = MallocExtension::StartAllocationProfiling();

  // Next sampled allocation should be too small
  constexpr size_t alloc_size = 0;
  AllocateUntil(alloc_size, [&](void* alloc) -> NextSteps {
    return {true, true};
  });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::TooSmall,
                 {Profile::Sample::GuardedStatus::RateLimited,
                  Profile::Sample::GuardedStatus::Guarded},
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
  ScopedAlwaysSample sas;
  AllocateUntilGuarded();

  std::vector<std::unique_ptr<char>> allocs;
  // Guard until there are no slots available.
  AllocateUntil(1039, [&](void* alloc) -> NextSteps {
    if (Static::guardedpage_allocator().PointerIsMine(alloc)) {
      allocs.emplace_back(static_cast<char*>(alloc));
      return {Static::guardedpage_allocator().GetNumAvailablePages() == 0,
              false};
    }
    return {false, true};
  });

  auto token = MallocExtension::StartAllocationProfiling();
  // This should  fail for lack of slots
  constexpr size_t alloc_size = 1055;
  AllocateUntil(alloc_size, [&](void* alloc) -> NextSteps {
    return {true, true};
  });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::NoAvailableSlots, {});
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
