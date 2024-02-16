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

// These tests assume TCMalloc is not linked in, and therefore the features
// exposed by MallocExtension should be no-ops, but otherwise safe to call.

#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "absl/random/random.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace {

TEST(MallocExtension, SnapshotCurrentIsEmpty) {
  // Allocate memory to use the allocator.
  absl::BitGen gen;
  int bytes_remaining = 1 << 24;
  std::vector<void*> ptrs;

  while (bytes_remaining > 0) {
    int size = absl::LogUniform<int>(gen, 0, 1 << 20);
    ptrs.push_back(::operator new(size));
    bytes_remaining -= size;
  }

  // All of the profiles should be empty.
  ProfileType types[] = {
      ProfileType::kHeap,
      ProfileType::kFragmentation, ProfileType::kPeakHeap,
      ProfileType::kAllocations,
  };

  for (auto t : types) {
    SCOPED_TRACE(static_cast<int>(t));

    Profile p = MallocExtension::SnapshotCurrent(t);
    int samples = 0;
    p.Iterate([&](const Profile::Sample&) { samples++; });

    EXPECT_EQ(samples, 0);
  }

  for (void* ptr : ptrs) {
    ::operator delete(ptr);
  }
}

TEST(MallocExtension, AllocationProfile) {
  auto token = MallocExtension::StartAllocationProfiling();

  // Allocate memory to use the allocator.
  absl::BitGen gen;
  int bytes_remaining = 1 << 24;
  std::vector<void*> ptrs;

  while (bytes_remaining > 0) {
    int size = absl::LogUniform<int>(gen, 0, 1 << 20);
    ptrs.push_back(::operator new(size));
    bytes_remaining -= size;
  }

  // Finish profiling and verify the profile is empty.
  Profile p = std::move(token).Stop();
  int samples = 0;
  p.Iterate([&](const Profile::Sample&) { samples++; });

  EXPECT_EQ(samples, 0);

  for (void* ptr : ptrs) {
    ::operator delete(ptr);
  }
}

}  // namespace
}  // namespace tcmalloc
