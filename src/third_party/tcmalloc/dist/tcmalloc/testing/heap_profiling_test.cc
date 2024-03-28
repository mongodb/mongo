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
#include <stdio.h>
#include <sys/mman.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "tcmalloc/internal/profile.pb.h"
#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/low_level_alloc.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/profile_builder.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/test_allocator_harness.h"
#include "tcmalloc/testing/thread_manager.h"

namespace tcmalloc {
namespace {

auto process_start = absl::Now();

class HeapProfilingTest : public ::testing::TestWithParam<int64_t> {};

// Verify that heap profiling sessions concurrent with allocations/deallocations
// do not crash, as they all use `tc_globals.sampled_allocation_recorder_`. Also
// check that the data in the sample make sense. Here the
// allocations/deallocations can happen on the same thread or the object is
// allocated in one thread, transferred to another thread and deleted there.
TEST_P(HeapProfilingTest, GetHeapProfileWhileAllocAndDealloc) {
  ScopedProfileSamplingRate s(GetParam());
  const int kThreads = 10;
  ThreadManager manager;
  AllocatorHarness harness(kThreads);

  // Some threads are busy with allocating and deallocating.
  manager.Start(kThreads, [&](int thread_id) { harness.Run(thread_id); });

  absl::Time test_start = absl::Now();
  // Another few threads busy with iterating different kinds of heap profiles.
  for (auto t : {
           ProfileType::kHeap,
           ProfileType::kFragmentation,
           ProfileType::kPeakHeap,
       }) {
    manager.Start(2, [&, t](int) {
      MallocExtension::SnapshotCurrent(t).Iterate(
          [&](const Profile::Sample& s) {
            // Inspect a few fields in the sample.
            CHECK_GE(s.sum, 0);
            CHECK_GT(s.depth, 0);
            CHECK_GT(s.requested_size, 0);
            CHECK_GT(s.allocated_size, 0);
            // Note: there may be few allocations that happened before
            // process_start initialization.
            CHECK_GT(s.allocation_time, process_start - absl::Seconds(10));
            CHECK_LT(s.allocation_time, test_start + absl::Seconds(20));
          });
    });
  }

  absl::SleepFor(absl::Seconds(1));
  manager.Stop();
}

// Test at different sampling rates, from always sampling to lower sampling
// probabilities. This is stress testing and attempts to expose potential
// failure modes when we only have sampled allocations and when we have a mix of
// sampled/unsampled allocations.
INSTANTIATE_TEST_SUITE_P(SamplingRates, HeapProfilingTest,
                         testing::Values(1, 1 << 7, 1 << 14, 1 << 21),
                         testing::PrintToStringParamName());

TEST(HeapProfilingTest, AllocateDifferentSizes) {
  const int num_allocations = 1000;
  const size_t requested_size1 = (1 << 19) + 1;
  const size_t requested_size2 = (1 << 20) + 1;
  int requested_size1_count = 0;
  int requested_size2_count = 0;

  // First allocate some large objects at a specific size, verify through heap
  // profile, and deallocate them.
  void* allocations1[num_allocations];
  for (int i = 0; i < num_allocations; i++) {
    allocations1[i] = ::operator new(requested_size1);
  }

  MallocExtension::SnapshotCurrent(ProfileType::kHeap)
      .Iterate([&](const Profile::Sample& s) {
        if (s.requested_size == requested_size1) requested_size1_count++;
        if (s.requested_size == requested_size2) requested_size2_count++;
      });

  EXPECT_GT(requested_size1_count, 0);
  EXPECT_EQ(requested_size2_count, 0);
  requested_size1_count = 0;

  for (int i = 0; i < num_allocations; i++) {
    ::operator delete(allocations1[i]);
  }

  // Next allocate some large objects at a different size, verify through heap
  // profile, and deallocate them.
  void* allocations2[num_allocations];
  for (int i = 0; i < num_allocations; i++) {
    allocations2[i] = ::operator new(requested_size2);
  }

  MallocExtension::SnapshotCurrent(ProfileType::kHeap)
      .Iterate([&](const Profile::Sample& s) {
        if (s.requested_size == requested_size1) requested_size1_count++;
        if (s.requested_size == requested_size2) requested_size2_count++;
      });

  EXPECT_EQ(requested_size1_count, 0);
  EXPECT_GT(requested_size2_count, 0);

  for (int i = 0; i < num_allocations; i++) {
    ::operator delete(allocations2[i]);
  }
}

TEST(HeapProfilingTest, CheckResidency) {
  ScopedProfileSamplingRate s(1);
  const int num_allocations = 1000;
  const size_t requested_size = (1 << 19) + 1;

  void* allocations[num_allocations];
  for (int i = 0; i < num_allocations; i++) {
    allocations[i] = ::operator new(requested_size);
  }

  bool mlock_failure = false;
  for (int i = 0; i < num_allocations; i++) {
    if (::mlock(allocations[i], requested_size) != 0) {
      mlock_failure = true;
      for (int j = 0; j < requested_size; ++j) {
        static_cast<volatile char*>(allocations[i])[j] = 0x20;
      }
    }
  }
  if (mlock_failure) {
    absl::FPrintF(
        stderr,
        "one or more mlocks failed, which could cause test flakiness\n");
  }

  // Collect the heap profile and look for residency info.
  auto converted_or = tcmalloc_internal::MakeProfileProto(
      MallocExtension::SnapshotCurrent(ProfileType::kHeap));
  ASSERT_TRUE(converted_or.ok());
  const auto& converted = **converted_or;

  // Look for "sampled_resident_bytes" string in string table.
  std::optional<int> sampled_resident_bytes_id;
  for (int i = 0, n = converted.string_table().size(); i < n; ++i) {
    if (converted.string_table(i) == "sampled_resident_bytes") {
      sampled_resident_bytes_id = i;
    }
  }
  ASSERT_TRUE(sampled_resident_bytes_id.has_value());

  size_t resident_size = 0;
  for (const auto& sample : converted.sample()) {
    for (const auto& label : sample.label()) {
      if (label.key() == sampled_resident_bytes_id) {
        resident_size += label.num();
      }
    }
  }

  EXPECT_GE(resident_size, num_allocations * requested_size);
  EXPECT_LE(resident_size, num_allocations * requested_size * 2);

  for (int i = 0; i < num_allocations; i++) {
    // throw away the error
    ::munlock(allocations[i], requested_size);
  }
  for (int i = 0; i < num_allocations; i++) {
    ::operator delete(allocations[i]);
  }
}

// Make sure users can allocate when iterating over the heap samples. For now
// `MallocExtension::SnapshotCurrent()` uses `StackTraceTable` to make a copy of
// the sampled allocations from `tc_globals.sampled_allocation_recorder()` and
// then iterate from the `StackTraceTable`. Ideally, we would want to avoid the
// extra copy and iterate over sampled allocations directly. However, this would
// result in deadlocks for the test case below. If we `Iterate()` directly on
// `tc_globals.sampled_allocation_recorder()`, we hold the per-sample lock. As
// we add data to a hashtable that stores allocations (always sampled here), the
// hashtable can decide to `resize()`, deallocates the same sampled allocation
// it is iterating at, wants to get the per-sample lock and ends up with a
// deadlock. At the current state, making copies over sampled allocations and
// iterate over those copies would not deadlock and the test case below passes.
TEST(HeapProfilingTest, AllocateWhileIterating) {
  ScopedProfileSamplingRate s(1);
  absl::flat_hash_set<void*> set;
  // This fills up the slots in hashtable and so there is a good chance it would
  // call `resize()` when inserting new entries later. This makes it easier for
  // the deadlock to happen (>95% of the cases when directly iterating over
  // `tc_globals.sampled_allocation_recorder()`).
  set.reserve(1);
  set.insert(::operator new(100));
  for (int i = 0; i < 3; i++) {
    MallocExtension::SnapshotCurrent(ProfileType::kHeap)
        .Iterate(
            [&](const Profile::Sample& s) { set.insert(::operator new(100)); });
  }
  for (void* obj : set) {
    ::operator delete(obj);
  }
}

}  // namespace
}  // namespace tcmalloc
