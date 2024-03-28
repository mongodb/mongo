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

#include "tcmalloc/stack_trace_table.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <initializer_list>
#include <ostream>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/macros.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// Rather than deal with heap allocating stack/tags, AllocationEntry contains
// them inline.
struct AllocationEntry {
  int64_t sum;
  int count;
  size_t requested_size;
  size_t requested_alignment;
  size_t allocated_size;
  uint8_t access_hint;
  bool cold_allocated;
  int depth;
  void* stack[64];

  friend bool operator==(const AllocationEntry& x, const AllocationEntry& y);
  friend bool operator!=(const AllocationEntry& x, const AllocationEntry& y) {
    return !(x == y);
  }

  friend std::ostream& operator<<(std::ostream& os, const AllocationEntry& e) {
    os << "sum = " << e.sum << "; ";
    os << "count = " << e.count << "; ";

    std::vector<std::string> ptrs;
    for (int i = 0; i < e.depth; i++) {
      ptrs.push_back(absl::StrFormat("%p", e.stack[i]));
    }
    os << "stack = [" << absl::StrJoin(ptrs, ", ") << "]; ";

    os << "requested_size = " << e.requested_size << "; ";
    os << "requested_alignment = " << e.requested_alignment << "; ";
    os << "allocated_size = " << e.allocated_size << "; ";
    os << "access_hint = " << e.access_hint << "; ";
    os << "cold_allocated = " << e.cold_allocated << "; ";

    return os;
  }
};

inline bool operator==(const AllocationEntry& x, const AllocationEntry& y) {
  if (x.sum != y.sum) {
    return false;
  }

  if (x.count != y.count) {
    return false;
  }

  if (x.depth != y.depth) {
    return false;
  }

  if (x.depth > 0 && !std::equal(x.stack, x.stack + x.depth, y.stack)) {
    return false;
  }

  if (x.requested_size != y.requested_size) {
    return false;
  }

  if (x.requested_alignment != y.requested_alignment) {
    return false;
  }

  if (x.allocated_size != y.allocated_size) {
    return false;
  }

  if (x.access_hint != y.access_hint) {
    return false;
  }

  if (x.cold_allocated != y.cold_allocated) {
    return false;
  }

  return true;
}

void CheckTraces(const StackTraceTable& table,
                 std::initializer_list<AllocationEntry> expected) {
  std::vector<AllocationEntry> actual;

  table.Iterate([&](const Profile::Sample& e) {
    AllocationEntry tmp;
    tmp.sum = e.sum;
    tmp.count = e.count;
    tmp.depth = e.depth;
    ASSERT_LE(tmp.depth, ABSL_ARRAYSIZE(tmp.stack));
    std::copy(e.stack, e.stack + e.depth, tmp.stack);

    tmp.requested_size = e.requested_size;
    tmp.requested_alignment = e.requested_alignment;
    tmp.allocated_size = e.allocated_size;
    tmp.access_hint = static_cast<uint8_t>(e.access_hint);
    tmp.cold_allocated = e.access_allocated == Profile::Sample::Access::Cold;

    actual.push_back(tmp);
  });

  EXPECT_THAT(actual, testing::UnorderedElementsAreArray(expected));
}

void AddTrace(StackTraceTable* table, double count, const StackTrace& t) {
  table->AddTrace(count, t);
}

TEST(StackTraceTableTest, StackTraceTable) {
  // If this test is not linked against TCMalloc, the global arena used for
  // StackTraceTable's profile samples will not be initialized.
  tc_globals.InitIfNecessary();

  // Empty table
  {
    SCOPED_TRACE("empty");

    StackTraceTable table(ProfileType::kHeap);
    EXPECT_EQ(0, table.depth_total());

    CheckTraces(table, {});
  }

  StackTrace t1 = {};
  t1.requested_size = static_cast<uintptr_t>(512);
  t1.requested_alignment = static_cast<uintptr_t>(16);
  t1.allocated_size = static_cast<uintptr_t>(1024);
  t1.access_hint = 3;
  t1.cold_allocated = true;
  t1.depth = static_cast<uintptr_t>(2);
  t1.stack[0] = reinterpret_cast<void*>(1);
  t1.stack[1] = reinterpret_cast<void*>(2);
  t1.weight = 513;

  const AllocationEntry k1 = {
      .sum = 1024,
      .count = 1,
      .requested_size = 512,
      .requested_alignment = 16,
      .allocated_size = 1024,
      .access_hint = 3,
      .cold_allocated = true,
      .depth = 2,
      .stack = {reinterpret_cast<void*>(1), reinterpret_cast<void*>(2)},
  };

  StackTrace t2 = {};
  t2.requested_size = static_cast<uintptr_t>(375);
  t2.requested_alignment = static_cast<uintptr_t>(0);
  t2.allocated_size = static_cast<uintptr_t>(512);
  t2.access_hint = 254;
  t2.cold_allocated = false;
  t2.depth = static_cast<uintptr_t>(2);
  t2.stack[0] = reinterpret_cast<void*>(2);
  t2.stack[1] = reinterpret_cast<void*>(1);
  t2.weight = 376;

  const AllocationEntry k2 = {
      .sum = 512,
      .count = 1,
      .requested_size = 375,
      .requested_alignment = 0,
      .allocated_size = 512,
      .access_hint = 254,
      .cold_allocated = false,
      .depth = 2,
      .stack = {reinterpret_cast<void*>(2), reinterpret_cast<void*>(1)},
  };

  // Table w/ just t1
  {
    SCOPED_TRACE("t1");

    StackTraceTable table(ProfileType::kHeap);
    AddTrace(&table, 1.0, t1);
    EXPECT_EQ(2, table.depth_total());

    CheckTraces(table, {k1});
  }

  // We made our last sample at t1.weight (2<<20 bytes).  We sample according to
  // t1.requested_size + 1 (513 bytes).  Therefore we overweight the sample to
  // construct the distribution.
  //
  // We rely on the profiling tests to verify that this correctly reconstructs
  // the distribution (+/- an error tolerance)
  auto t1_nontrivial_weight = t1;
  t1_nontrivial_weight.weight = 2 << 20;
  const int t1_sampled_weight =
      static_cast<double>(t1_nontrivial_weight.weight) /
      (t1_nontrivial_weight.requested_size + 1);
  ASSERT_EQ(t1_sampled_weight, 4088);
  const AllocationEntry k1_unsampled = {
      .sum = t1_sampled_weight * 1024,
      .count = t1_sampled_weight,
      .requested_size = 512,
      .requested_alignment = 16,
      .allocated_size = 1024,
      .access_hint = 3,
      .cold_allocated = true,
      .depth = 2,
      .stack = {reinterpret_cast<void*>(1), reinterpret_cast<void*>(2)},
  };

  // Table w/ just t1 (unsampled)
  {
    SCOPED_TRACE("t1 unsampled");

    StackTraceTable table(ProfileType::kHeap);
    AddTrace(&table, 1.0, t1_nontrivial_weight);
    EXPECT_EQ(2, table.depth_total());

    CheckTraces(table, {k1_unsampled});
  }

  // Table w/ 2x t1
  {
    SCOPED_TRACE("2x t1");

    StackTraceTable table(ProfileType::kHeap);
    AddTrace(&table, 1.0, t1);
    AddTrace(&table, 1.0, t1);
    EXPECT_EQ(4, table.depth_total());

    CheckTraces(table, {k1, k1});
  }

  // Table w/ t1, t2
  {
    SCOPED_TRACE("t1, t2");

    StackTraceTable table(ProfileType::kHeap);
    AddTrace(&table, 1.0, t1);
    AddTrace(&table, 1.0, t2);
    EXPECT_EQ(4, table.depth_total());
    CheckTraces(table, {k1, k2});
  }

  // Table w/ 1.2 x t1, 1 x t2.
  // Note that t1's 1.2 count will be rounded to 1.0.
  {
    SCOPED_TRACE("1.2 t1, t2");

    StackTraceTable table(ProfileType::kHeap);
    AddTrace(&table, 1.0, t2);
    AddTrace(&table, 1.2, t1);
    EXPECT_EQ(4, table.depth_total());

    CheckTraces(table, {k1, k2});
  }

  // Same stack as t1, but w/ different size
  StackTrace t3 = {};
  t3.requested_size = static_cast<uintptr_t>(13);
  t3.requested_alignment = static_cast<uintptr_t>(0);
  t3.allocated_size = static_cast<uintptr_t>(17);
  t3.access_hint = 3;
  t3.cold_allocated = false;
  t3.depth = static_cast<uintptr_t>(2);
  t3.stack[0] = reinterpret_cast<void*>(1);
  t3.stack[1] = reinterpret_cast<void*>(2);
  t3.weight = 14;

  const AllocationEntry k3 = {
      .sum = 17,
      .count = 1,
      .requested_size = 13,
      .requested_alignment = 0,
      .allocated_size = 17,
      .access_hint = 3,
      .cold_allocated = false,
      .depth = 2,
      .stack = {reinterpret_cast<void*>(1), reinterpret_cast<void*>(2)},
  };

  // Table w/ t1, t3
  {
    SCOPED_TRACE("t1, t3");

    StackTraceTable table(ProfileType::kHeap);
    AddTrace(&table, 1.0, t1);
    AddTrace(&table, 1.0, t3);
    EXPECT_EQ(4, table.depth_total());

    CheckTraces(table, {k1, k3});
  }

  // Same stack as t1, but w/ different alignment
  StackTrace t4 = {};
  t4.requested_size = static_cast<uintptr_t>(512);
  t4.requested_alignment = static_cast<uintptr_t>(32);
  t4.allocated_size = static_cast<uintptr_t>(1024);
  t4.access_hint = 3;
  t4.cold_allocated = false;
  t4.depth = static_cast<uintptr_t>(2);
  t4.stack[0] = reinterpret_cast<void*>(1);
  t4.stack[1] = reinterpret_cast<void*>(2);
  t4.weight = 513;

  const AllocationEntry k4 = {
      .sum = 1024,
      .count = 1,
      .requested_size = 512,
      .requested_alignment = 32,
      .allocated_size = 1024,
      .access_hint = 3,
      .cold_allocated = false,
      .depth = 2,
      .stack = {reinterpret_cast<void*>(1), reinterpret_cast<void*>(2)},
  };

  // Table w/ t1, t4
  {
    SCOPED_TRACE("t1, t4");

    StackTraceTable table(ProfileType::kHeap);
    AddTrace(&table, 1.0, t1);
    AddTrace(&table, 1.0, t4);
    EXPECT_EQ(4, table.depth_total());

    CheckTraces(table, {k1, k4});
  }

  // Same stack as t1, but w/ different hint
  StackTrace t5 = {};
  t5.requested_size = static_cast<uintptr_t>(512);
  t5.requested_alignment = static_cast<uintptr_t>(32);
  t5.allocated_size = static_cast<uintptr_t>(1024);
  t5.access_hint = 4;
  t5.cold_allocated = true;
  t5.depth = static_cast<uintptr_t>(2);
  t5.stack[0] = reinterpret_cast<void*>(1);
  t5.stack[1] = reinterpret_cast<void*>(2);
  t5.weight = 513;

  const AllocationEntry k5 = {
      .sum = 1024,
      .count = 1,
      .requested_size = 512,
      .requested_alignment = 32,
      .allocated_size = 1024,
      .access_hint = 4,
      .cold_allocated = true,
      .depth = 2,
      .stack = {reinterpret_cast<void*>(1), reinterpret_cast<void*>(2)},
  };

  // Table w/ t1, t5
  {
    SCOPED_TRACE("t1, t5");

    StackTraceTable table(ProfileType::kHeap);
    AddTrace(&table, 1.0, t1);
    AddTrace(&table, 1.0, t5);
    EXPECT_EQ(4, table.depth_total());

    CheckTraces(table, {k1, k5});
  }
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
