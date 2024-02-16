// Copyright 2021 The TCMalloc Authors
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

#include "tcmalloc/arena.h"

#include <stdint.h>

#include <new>

#include "gtest/gtest.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

std::align_val_t Align(int align) {
  return static_cast<std::align_val_t>(align);
}

TEST(Arena, AlignedAlloc) {
  Arena arena;
  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(arena.Alloc(64, Align(64))) % 64, 0);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(arena.Alloc(7)) % 8, 0);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(arena.Alloc(128, Align(64))) % 64, 0);
  for (int alignment = 1; alignment < 100; ++alignment) {
    EXPECT_EQ(reinterpret_cast<uintptr_t>(arena.Alloc(7, Align(alignment))) %
                  alignment,
              0);
  }
}

TEST(Arena, Stats) {
  Arena arena;

  ArenaStats stats;
  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    stats = arena.stats();
  }
  EXPECT_EQ(stats.bytes_allocated, 0);
  EXPECT_EQ(stats.bytes_unallocated, 0);
  EXPECT_EQ(stats.bytes_unavailable, 0);
  EXPECT_EQ(stats.bytes_nonresident, 0);
  EXPECT_EQ(stats.blocks, 0);

  // Trigger an allocation and grab new stats.
  ArenaStats stats_after_alloc;
  void* ptr;
  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    ptr = arena.Alloc(1, Align(1));
    stats_after_alloc = arena.stats();
  }
  EXPECT_NE(ptr, nullptr);

  EXPECT_EQ(stats_after_alloc.bytes_allocated, 1);
  EXPECT_GE(stats_after_alloc.bytes_unallocated, 0);
  EXPECT_EQ(stats_after_alloc.bytes_unavailable, 0);
  EXPECT_EQ(stats_after_alloc.bytes_nonresident, 0);
  EXPECT_EQ(stats_after_alloc.blocks, 1);

  // Trigger an allocation that is larger than the remaining free bytes.
  //
  // TODO(b/201694482): Optimize this.
  ArenaStats stats_after_alloc2;
  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    ptr = arena.Alloc(stats_after_alloc.bytes_unallocated + 1, Align(1));
    stats_after_alloc2 = arena.stats();
  }
  EXPECT_NE(ptr, nullptr);

  EXPECT_EQ(stats_after_alloc2.bytes_allocated,
            stats_after_alloc.bytes_unallocated + 2);
  EXPECT_GE(stats_after_alloc2.bytes_unallocated, 0);
  EXPECT_EQ(stats_after_alloc2.bytes_unavailable,
            stats_after_alloc.bytes_unallocated);
  EXPECT_EQ(stats_after_alloc.bytes_nonresident, 0);
  EXPECT_EQ(stats_after_alloc2.blocks, 2);
}

TEST(Arena, ReportUnmapped) {
  Arena arena;
  ArenaStats stats_after_alloc;
  void* ptr;
  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    ptr = arena.Alloc(10, Align(1));
    stats_after_alloc = arena.stats();
  }
  EXPECT_NE(ptr, nullptr);

  EXPECT_EQ(stats_after_alloc.bytes_allocated, 10);
  EXPECT_EQ(stats_after_alloc.bytes_nonresident, 0);

  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    arena.UpdateAllocatedAndNonresident(-5, 5);
    stats_after_alloc = arena.stats();
  }

  EXPECT_EQ(stats_after_alloc.bytes_allocated, 5);
  EXPECT_EQ(stats_after_alloc.bytes_nonresident, 5);

  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    arena.UpdateAllocatedAndNonresident(3, -3);
    stats_after_alloc = arena.stats();
  }

  EXPECT_EQ(stats_after_alloc.bytes_allocated, 8);
  EXPECT_EQ(stats_after_alloc.bytes_nonresident, 2);
}

TEST(Arena, BytesImpending) {
  Arena arena;

  ArenaStats stats;
  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    stats = arena.stats();
  }
  EXPECT_EQ(stats.bytes_allocated, 0);

  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    arena.UpdateAllocatedAndNonresident(100, 0);
    stats = arena.stats();
  }

  EXPECT_EQ(stats.bytes_allocated, 100);

  void* ptr;
  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    arena.UpdateAllocatedAndNonresident(-100, 0);
    ptr = arena.Alloc(100, Align(1));
    stats = arena.stats();
  }
  EXPECT_NE(ptr, nullptr);
  EXPECT_EQ(stats.bytes_allocated, 100);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
