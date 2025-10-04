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

#include "tcmalloc/huge_allocator.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/random/random.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/mock_metadata_allocator.h"
#include "tcmalloc/mock_virtual_allocator.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

class HugeAllocatorTest : public testing::TestWithParam<bool> {
 protected:
  HugeLength HugePagesRequested() {
    return vm_allocator_.huge_pages_requested_;
  }
  HugeLength HugePagesReceived() { return vm_allocator_.huge_pages_received_; }

  HugeAllocatorTest() {
    vm_allocator_.should_overallocate_ = GetParam();
    vm_allocator_.huge_pages_requested_ = NHugePages(0);
    vm_allocator_.huge_pages_received_ = NHugePages(0);
    // We don't use the first few bytes, because things might get weird
    // given zero pointers.
    vm_allocator_.backing_.resize(1024);
  }

  ~HugeAllocatorTest() override {
    vm_allocator_.backing_.clear();
  }

  size_t* GetActual(HugePage p) { return &vm_allocator_.backing_[p.index()]; }

  // We're dealing with a lot of memory, so we don't want to do full memset
  // and then check every byte for corruption.  So set the first and last
  // byte in each page...
  void CheckPages(HugeRange r, size_t c) {
    for (HugePage p = r.first; p < r.first + r.n; ++p) {
      EXPECT_EQ(c, *GetActual(p));
    }
  }

  void MarkPages(HugeRange r, size_t c) {
    for (HugePage p = r.first; p < r.first + r.n; ++p) {
      *GetActual(p) = c;
    }
  }

  void CheckStats(HugeLength expected_use) {
    const HugeLength received = HugePagesReceived();
    EXPECT_EQ(received, allocator_.system());
    HugeLength used = received - allocator_.size();
    EXPECT_EQ(used, expected_use);
  }

  FakeVirtualAllocator vm_allocator_;
  FakeMetadataAllocator metadata_allocator_;
  HugeAllocator allocator_{vm_allocator_, metadata_allocator_};
};

TEST_P(HugeAllocatorTest, Basic) {
  std::vector<std::pair<HugeRange, size_t>> allocs;
  absl::BitGen rng;
  size_t label = 0;
  HugeLength total = NHugePages(0);
  static const size_t kSize = 1000;
  HugeLength peak = total;
  for (int i = 0; i < kSize; ++i) {
    HugeLength len =
        NHugePages(absl::LogUniform<int32_t>(rng, 0, (1 << 12) - 1) + 1);
    auto r = allocator_.Get(len);
    ASSERT_TRUE(r.valid());
    total += len;
    peak = std::max(peak, total);
    CheckStats(total);
    MarkPages(r, label);
    allocs.push_back({r, label});
    label++;
  }

  for (int i = 0; i < 1000 * 25; ++i) {
    size_t index = absl::Uniform<int32_t>(rng, 0, kSize);
    std::swap(allocs[index], allocs[kSize - 1]);
    auto p = allocs[kSize - 1];
    CheckPages(p.first, p.second);
    total -= p.first.len();
    allocator_.Release(p.first);
    CheckStats(total);

    HugeLength len =
        NHugePages(absl::LogUniform<int32_t>(rng, 0, (1 << 12) - 1) + 1);
    auto r = allocator_.Get(len);
    ASSERT_TRUE(r.valid());
    ASSERT_EQ(r.len(), len);
    total += len;
    peak = std::max(peak, total);
    CheckStats(total);
    MarkPages(r, label);
    allocs[kSize - 1] = {r, label};
    label++;
  }
  for (auto p : allocs) {
    CheckPages(p.first, p.second);
    allocator_.Release(p.first);
  }
}

// Check that releasing small chunks of allocations works OK.
TEST_P(HugeAllocatorTest, Subrelease) {
  size_t label = 1;
  const HugeLength kLen = NHugePages(8);
  const HugeLength kTotal = kLen * (kLen / NHugePages(1) - 1);
  for (int i = 0; i < 100; ++i) {
    std::vector<std::pair<HugeRange, size_t>> allocs;
    // get allocs of kLen and release different sized sub-chunks of them -
    // make sure that doesn't break anything else.
    for (HugeLength j = NHugePages(1); j < kLen; ++j) {
      auto r = allocator_.Get(kLen);
      ASSERT_TRUE(r.valid());
      MarkPages(r, label);
      allocator_.Release({r.start(), j});
      allocs.push_back({{r.start() + j, kLen - j}, label});
      label++;
    }
    EXPECT_EQ(kTotal, HugePagesRequested());
    for (auto p : allocs) {
      CheckPages(p.first, p.second);
      allocator_.Release(p.first);
    }
  }
}

// Does subreleasing work OK for absurdly large allocations?
TEST_P(HugeAllocatorTest, SubreleaseLarge) {
  absl::BitGen rng;
  std::vector<std::pair<HugeRange, size_t>> allocs;
  size_t label = 1;
  const HugeLength kLimit = HLFromBytes(1024ul * 1024 * 1024 * 1024);
  for (HugeLength n = NHugePages(2); n < kLimit; n *= 2) {
    auto r = allocator_.Get(n);
    ASSERT_TRUE(r.valid());
    MarkPages(r, label);
    // chunk of less than half
    HugeLength chunk =
        NHugePages(absl::Uniform<int32_t>(rng, 0, n / NHugePages(2)) + 1);
    allocator_.Release({r.start(), chunk});
    allocs.push_back({{r.start() + chunk, n - chunk}, label});
    label++;
  }
  // reuse the released space
  const HugeLength total = HugePagesRequested();
  while (total == HugePagesRequested()) {
    HugeLength n =
        NHugePages(absl::LogUniform<int32_t>(rng, 0, (1 << 8) - 1) + 1);
    auto r = allocator_.Get(n);
    ASSERT_TRUE(r.valid());
    MarkPages(r, label);
    allocs.push_back({r, label});
    label++;
  }
  for (auto p : allocs) {
    CheckPages(p.first, p.second);
    allocator_.Release(p.first);
  }
}

// We don't care *that* much about vaddress space, but let's not be crazy.
// Don't fill tiny requests from big spaces.
TEST_P(HugeAllocatorTest, Fragmentation) {
  // Prime the pump with some random allocations.
  absl::BitGen rng;

  std::vector<HugeRange> free;
  constexpr int kSlots = 50;

  // Plan to insert a large allocation at the big_slot'th index, then free it
  // during the initial priming step (so we have at least a contiguous region of
  // at least big hugepages).
  HugeLength big = NHugePages(8);
  const int big_slot = absl::Uniform(rng, 0, kSlots);

  for (int i = 0; i < kSlots; ++i) {
    if (i == big_slot) {
      auto r = allocator_.Get(big);
      ASSERT_TRUE(r.valid());
      free.push_back(r);
    }

    auto r = allocator_.Get(NHugePages(1));
    ASSERT_TRUE(r.valid());
    if (absl::Bernoulli(rng, 1.0 / 2)) {
      free.push_back(r);
    }
  }
  size_t slots = free.size() - 1;
  for (auto r : free) {
    allocator_.Release(r);
  }
  free.clear();
  static const size_t kReps = 5;
  for (int i = 0; i < kReps; ++i) {
    SCOPED_TRACE(i);

    // Ensure we have a range of this size.
    HugeRange r = allocator_.Get(big);
    ASSERT_TRUE(r.valid());
    if (NHugePages(slots) > allocator_.size()) {
      // We should also have slots pages left over after allocating big
      for (int i = 0; i < slots; ++i) {
        HugeRange f = allocator_.Get(NHugePages(1));
        ASSERT_TRUE(f.valid());
        free.push_back(f);
      }
      for (auto f : free) {
        allocator_.Release(f);
      }
      free.clear();
    }
    allocator_.Release(r);
    // We should definitely have at least this many small spaces...
    for (int i = 0; i < slots; ++i) {
      r = allocator_.Get(NHugePages(1));
      ASSERT_TRUE(r.valid());
      free.push_back(r);
    }
    // that don't interfere with the available big space.
    auto before = allocator_.system();
    r = allocator_.Get(big);
    ASSERT_TRUE(r.valid());
    EXPECT_EQ(before, allocator_.system());
    allocator_.Release(r);
    for (auto r : free) {
      allocator_.Release(r);
    }
    free.clear();
    slots += big.raw_num();
    big += big;
  }
}

// Check that we only request as much as we actually need from the system.
TEST_P(HugeAllocatorTest, Frugal) {
  HugeLength total = NHugePages(0);
  static const size_t kSize = 1000;
  for (int i = 1; i < kSize; ++i) {
    HugeLength len = NHugePages(i);
    // toss the range, we ain't using it
    ASSERT_TRUE(allocator_.Get(len).valid());

    total += len;
    CheckStats(total);
    EXPECT_EQ(total, HugePagesRequested());
  }
}

TEST_P(HugeAllocatorTest, Stats) {
  struct Helper {
    static void Stats(const HugeAllocator* huge, size_t* num_spans,
                      Length* pages) {
      SmallSpanStats small;
      LargeSpanStats large;
      huge->AddSpanStats(&small, &large);
      for (auto i = Length(0); i < kMaxPages; ++i) {
        EXPECT_EQ(0, small.normal_length[i.raw_num()]);
        EXPECT_EQ(0, small.returned_length[i.raw_num()]);
      }
      *num_spans = large.spans;
      EXPECT_EQ(Length(0), large.normal_pages);
      *pages = large.returned_pages;
    }
  };

  if (GetParam()) {
    // Ensure overallocation doesn't skew our measurements below.
    allocator_.Release(allocator_.Get(NHugePages(7)));
  }
  const HugeRange r = allocator_.Get(NHugePages(8));
  ASSERT_TRUE(r.valid());
  const HugePage p = r.start();
  // Break it into 3 ranges, separated by one-page regions,
  // so we can easily track the internal state in stats.
  const HugeRange r1 = {p, NHugePages(1)};
  const HugeRange b1 = {p + NHugePages(1), NHugePages(1)};
  const HugeRange r2 = {p + NHugePages(2), NHugePages(2)};
  const HugeRange b2 = {p + NHugePages(4), NHugePages(1)};
  const HugeRange r3 = {p + NHugePages(5), NHugePages(3)};

  size_t num_spans;
  Length pages;

  Helper::Stats(&allocator_, &num_spans, &pages);
  EXPECT_EQ(0, num_spans);
  EXPECT_EQ(Length(0), pages);

  allocator_.Release(r1);
  Helper::Stats(&allocator_, &num_spans, &pages);
  EXPECT_EQ(1, num_spans);
  EXPECT_EQ(NHugePages(1).in_pages(), pages);

  allocator_.Release(r2);
  Helper::Stats(&allocator_, &num_spans, &pages);
  EXPECT_EQ(2, num_spans);
  EXPECT_EQ(NHugePages(3).in_pages(), pages);

  allocator_.Release(r3);
  Helper::Stats(&allocator_, &num_spans, &pages);
  EXPECT_EQ(3, num_spans);
  EXPECT_EQ(NHugePages(6).in_pages(), pages);

  allocator_.Release(b1);
  allocator_.Release(b2);
  Helper::Stats(&allocator_, &num_spans, &pages);
  EXPECT_EQ(1, num_spans);
  EXPECT_EQ(NHugePages(8).in_pages(), pages);
}

// Make sure we're well-behaved in the presence of OOM (and that we do
// OOM at some point...)
TEST_P(HugeAllocatorTest, OOM) {
  HugeLength n = NHugePages(1);
  while (allocator_.Get(n).valid()) {
    n *= 2;
  }
}

INSTANTIATE_TEST_SUITE_P(
    NormalOverAlloc, HugeAllocatorTest, testing::Values(false, true),
    +[](const testing::TestParamInfo<bool>& info) {
      return info.param ? "overallocates" : "normal";
    });

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
