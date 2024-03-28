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

#include "tcmalloc/huge_region.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/memory/memory.h"
#include "absl/random/random.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_cache.h"
#include "tcmalloc/huge_page_subrelease.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/stats.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

using testing::NiceMock;
using testing::Return;

class HugeRegionTest : public ::testing::Test {
 protected:
  HugeRegionTest()
      : mock_(std::make_unique<NiceMock<MockBackingInterface>>()),
        // an unlikely magic page
        p_(HugePageContaining(reinterpret_cast<void*>(0x1faced200000))),
        region_({p_, region_.size()}, *mock_) {
    // we usually don't care about backing calls, unless testing that
    // specifically.
  }

  ~HugeRegionTest() override { mock_.reset(nullptr); }

  class MockBackingInterface : public MemoryModifyFunction {
   public:
    MOCK_METHOD(bool, Unback, (void* p, size_t len), ());

    bool operator()(void* p, size_t len) override { return Unback(p, len); }
  };

  std::unique_ptr<MockBackingInterface> mock_;

  void CheckMock() { testing::Mock::VerifyAndClearExpectations(mock_.get()); }

  void ExpectUnback(HugeRange r, bool success = true) {
    void* ptr = r.start_addr();
    size_t bytes = r.byte_len();
    EXPECT_CALL(*mock_, Unback(ptr, bytes)).WillOnce(Return(success));
  }

  struct Alloc {
    PageId p;
    Length n;
    size_t mark;
  };

  HugePage p_;
  typedef HugeRegion Region;
  Region region_;
  size_t next_mark_{0};
  size_t marks_[Region::size().in_pages().raw_num()];

  void Mark(Alloc a) {
    EXPECT_LE(p_.first_page(), a.p);
    size_t index = (a.p - p_.first_page()).raw_num();
    size_t end = index + a.n.raw_num();
    EXPECT_LE(end, region_.size().in_pages().raw_num());
    for (; index < end; ++index) {
      marks_[index] = a.mark;
    }
  }

  void Check(Alloc a) {
    EXPECT_LE(p_.first_page(), a.p);
    size_t index = (a.p - p_.first_page()).raw_num();
    size_t end = index + a.n.raw_num();
    EXPECT_LE(end, region_.size().in_pages().raw_num());
    for (; index < end; ++index) {
      EXPECT_EQ(a.mark, marks_[index]);
    }
  }

  Alloc Allocate(Length n) {
    bool from_released;
    return Allocate(n, &from_released);
  }

  Alloc Allocate(Length n, bool* from_released) {
    Alloc ret;
    TC_CHECK(region_.MaybeGet(n, &ret.p, from_released));
    ret.n = n;
    ret.mark = ++next_mark_;
    Mark(ret);
    return ret;
  }

  void Delete(Alloc a) {
    Check(a);
    region_.Put(a.p, a.n, false);
  }

  void DeleteUnback(Alloc a) {
    Check(a);
    region_.Put(a.p, a.n, true);
  }
};

TEST_F(HugeRegionTest, Basic) {
  Length total;
  std::vector<Alloc> allocs;
  for (Length n(1); total + n < region_.size().in_pages(); ++n) {
    allocs.push_back(Allocate(n));
    total += n;
    EXPECT_EQ(total, region_.used_pages());
  }

  // Free every other alloc
  std::vector<Length> lengths;
  std::vector<Alloc> new_allocs;
  for (int j = 0; j < allocs.size(); ++j) {
    if (j % 2 == 0) {
      new_allocs.push_back(allocs[j]);
      continue;
    }
    Length n = allocs[j].n;
    Delete(allocs[j]);
    total -= n;
    EXPECT_EQ(total, region_.used_pages());
    lengths.push_back(n);
  }
  allocs.swap(new_allocs);
  // and reallocate them in a random order:
  std::shuffle(lengths.begin(), lengths.end(), absl::BitGen());
  // This should fit, since thge allocator is best-fit
  // and we have unique gaps of each size.
  for (auto n : lengths) {
    allocs.push_back(Allocate(n));
    total += n;
    EXPECT_EQ(total, region_.used_pages());
  }

  for (auto a : allocs) {
    Delete(a);
  }
}

TEST_F(HugeRegionTest, ReqsBacking) {
  const Length n = kPagesPerHugePage;
  std::vector<Alloc> allocs;
  // should back the first page
  bool from_released;
  allocs.push_back(Allocate(n - Length(1), &from_released));
  EXPECT_TRUE(from_released);
  // nothing
  allocs.push_back(Allocate(Length(1), &from_released));
  EXPECT_FALSE(from_released);
  // second page
  allocs.push_back(Allocate(Length(1), &from_released));
  EXPECT_TRUE(from_released);
  // third, fourth, fifth
  allocs.push_back(Allocate(3 * n, &from_released));
  EXPECT_TRUE(from_released);

  for (auto a : allocs) {
    Delete(a);
  }
}

TEST_F(HugeRegionTest, ReleaseFrac) {
  const Length n = kPagesPerHugePage;
  bool from_released;
  auto a = Allocate(n * 20, &from_released);
  EXPECT_TRUE(from_released);

  Delete(a);
  ExpectUnback({p_ + NHugePages(0), NHugePages(2)});
  EXPECT_EQ(NHugePages(2), region_.Release(NHugePages(2).in_pages()));
  CheckMock();

  ExpectUnback({p_ + NHugePages(2), NHugePages(1)});
  EXPECT_EQ(NHugePages(1), region_.Release(NHugePages(1).in_pages()));
  CheckMock();

  ExpectUnback({p_ + NHugePages(3), NHugePages(8)});
  EXPECT_EQ(NHugePages(8), region_.Release(NHugePages(8).in_pages()));
  CheckMock();

  ExpectUnback({p_ + NHugePages(11), NHugePages(9)});
  EXPECT_EQ(NHugePages(9), region_.Release(NHugePages(9).in_pages()));
  CheckMock();
}

TEST_F(HugeRegionTest, Release) {
  const Length n = kPagesPerHugePage;
  bool from_released;
  auto a = Allocate(n * 4 - Length(1), &from_released);
  EXPECT_TRUE(from_released);

  auto b = Allocate(n * 3, &from_released);
  EXPECT_TRUE(from_released);

  auto c = Allocate(n * 5 + Length(1), &from_released);
  EXPECT_TRUE(from_released);

  auto d = Allocate(n * 2, &from_released);
  EXPECT_TRUE(from_released);

  auto e = Allocate(n / 2, &from_released);
  EXPECT_TRUE(from_released);
  auto f = Allocate(n / 2, &from_released);
  EXPECT_FALSE(from_released);

  // Don't unback the first or last hugepage this touches -- since they
  // overlap with others.
  Delete(b);
  ExpectUnback({p_ + NHugePages(4), NHugePages(2)});
  EXPECT_EQ(NHugePages(2), region_.Release(NHugePages(2).in_pages()));
  CheckMock();

  // Now we're on exact boundaries so we should unback the whole range.
  Delete(d);
  ExpectUnback({p_ + NHugePages(12), NHugePages(2)});
  EXPECT_EQ(NHugePages(2), region_.Release(NHugePages(2).in_pages()));
  CheckMock();

  Delete(a);
  ExpectUnback({p_ + NHugePages(0), NHugePages(4)});
  EXPECT_EQ(NHugePages(4), region_.Release(NHugePages(4).in_pages()));
  CheckMock();

  // Should work just as well with aggressive Put():
  ExpectUnback({p_ + NHugePages(6), NHugePages(6)});
  DeleteUnback(c);
  CheckMock();

  // And this _shouldn't_ do anything (page still in use)
  DeleteUnback(e);
  // But this should:
  ExpectUnback({p_ + NHugePages(14), NHugePages(1)});
  DeleteUnback(f);
  CheckMock();
}

TEST_F(HugeRegionTest, ReleaseFailure) {
  const Length n = kPagesPerHugePage;
  bool from_released;
  auto a = Allocate(n * 4 - Length(1), &from_released);
  EXPECT_TRUE(from_released);
  EXPECT_EQ(NHugePages(4), region_.backed());

  // Don't unback the first or last hugepage this touches -- since they
  // overlap with others.
  Delete(a);
  ExpectUnback({p_, NHugePages(4)}, false);
  EXPECT_EQ(NHugePages(0), region_.Release(NHugePages(4).in_pages()));
  EXPECT_EQ(NHugePages(4), region_.backed());
  CheckMock();

  // Reallocate.
  a = Allocate(n * 4 - Length(1), &from_released);
  EXPECT_FALSE(from_released);
  Delete(a);

  EXPECT_EQ(NHugePages(4), region_.backed());
}

TEST_F(HugeRegionTest, Reback) {
  const Length n = kPagesPerHugePage / 4;
  bool from_released;
  // Even in back/unback cycles we should still call the functions
  // on every transition.
  for (int i = 0; i < 20; ++i) {
    std::vector<Alloc> allocs;
    allocs.push_back(Allocate(n, &from_released));
    EXPECT_TRUE(from_released);
    allocs.push_back(Allocate(n, &from_released));
    EXPECT_FALSE(from_released);
    allocs.push_back(Allocate(n, &from_released));
    EXPECT_FALSE(from_released);
    allocs.push_back(Allocate(n, &from_released));
    EXPECT_FALSE(from_released);

    std::shuffle(allocs.begin(), allocs.end(), absl::BitGen());
    DeleteUnback(allocs[0]);
    DeleteUnback(allocs[1]);
    DeleteUnback(allocs[2]);

    ExpectUnback({p_, NHugePages(1)});
    DeleteUnback(allocs[3]);
    CheckMock();
  }
}

TEST_F(HugeRegionTest, Stats) {
  const Length kLen = region_.size().in_pages();
  const size_t kBytes = kLen.in_bytes();
  struct Helper {
    static void Stat(const Region& region, std::vector<Length>* small_backed,
                     std::vector<Length>* small_unbacked, LargeSpanStats* large,
                     BackingStats* stats) {
      SmallSpanStats small;
      *large = LargeSpanStats();
      region.AddSpanStats(&small, large);
      small_backed->clear();
      small_unbacked->clear();
      for (auto i = Length(0); i < kMaxPages; ++i) {
        for (int j = 0; j < small.normal_length[i.raw_num()]; ++j) {
          small_backed->push_back(i);
        }

        for (int j = 0; j < small.returned_length[i.raw_num()]; ++j) {
          small_unbacked->push_back(i);
        }
      }

      *stats = region.stats();
    }
  };

  LargeSpanStats large;
  std::vector<Length> small_backed, small_unbacked;
  BackingStats stats;

  Helper::Stat(region_, &small_backed, &small_unbacked, &large, &stats);
  EXPECT_THAT(small_backed, testing::ElementsAre());
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(1, large.spans);
  EXPECT_EQ(Length(0), large.normal_pages);
  EXPECT_EQ(kLen, large.returned_pages);
  EXPECT_EQ(kBytes, stats.system_bytes);
  EXPECT_EQ(0, stats.free_bytes);
  EXPECT_EQ(kBytes, stats.unmapped_bytes);

  // We don't, in production, use small allocations from the region, but
  // the API supports it, so test it here.
  Alloc a = Allocate(Length(1));
  Allocate(Length(1));
  Alloc b = Allocate(Length(2));
  Alloc barrier = Allocate(Length(1));
  Alloc c = Allocate(Length(3));
  Allocate(Length(1));
  const Length slack = kPagesPerHugePage - Length(9);

  Helper::Stat(region_, &small_backed, &small_unbacked, &large, &stats);
  EXPECT_THAT(small_backed, testing::ElementsAre());
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(2, large.spans);
  EXPECT_EQ(slack, large.normal_pages);
  EXPECT_EQ(kLen - kPagesPerHugePage, large.returned_pages);
  EXPECT_EQ(kBytes, stats.system_bytes);
  EXPECT_EQ(slack.in_bytes(), stats.free_bytes);
  EXPECT_EQ((region_.size() - NHugePages(1)).in_bytes(), stats.unmapped_bytes);

  Delete(a);
  Helper::Stat(region_, &small_backed, &small_unbacked, &large, &stats);
  EXPECT_THAT(small_backed, testing::ElementsAre(Length(1)));
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(2, large.spans);
  EXPECT_EQ(slack, large.normal_pages);
  EXPECT_EQ(kLen - kPagesPerHugePage, large.returned_pages);
  EXPECT_EQ(kBytes, stats.system_bytes);
  EXPECT_EQ((slack + Length(1)).in_bytes(), stats.free_bytes);
  EXPECT_EQ((region_.size() - NHugePages(1)).in_bytes(), stats.unmapped_bytes);

  Delete(b);
  Helper::Stat(region_, &small_backed, &small_unbacked, &large, &stats);
  EXPECT_THAT(small_backed, testing::ElementsAre(Length(1), Length(2)));
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(2, large.spans);
  EXPECT_EQ(slack, large.normal_pages);
  EXPECT_EQ(kLen - kPagesPerHugePage, large.returned_pages);
  EXPECT_EQ(kBytes, stats.system_bytes);
  EXPECT_EQ((slack + Length(3)).in_bytes(), stats.free_bytes);
  EXPECT_EQ((region_.size() - NHugePages(1)).in_bytes(), stats.unmapped_bytes);

  Delete(c);
  Helper::Stat(region_, &small_backed, &small_unbacked, &large, &stats);
  EXPECT_THAT(small_backed,
              testing::ElementsAre(Length(1), Length(2), Length(3)));
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(2, large.spans);
  EXPECT_EQ(slack, large.normal_pages);
  EXPECT_EQ(kLen - kPagesPerHugePage, large.returned_pages);
  EXPECT_EQ(kBytes, stats.system_bytes);
  EXPECT_EQ((slack + Length(6)).in_bytes(), stats.free_bytes);
  EXPECT_EQ((region_.size() - NHugePages(1)).in_bytes(), stats.unmapped_bytes);

  Delete(barrier);
  Helper::Stat(region_, &small_backed, &small_unbacked, &large, &stats);
  EXPECT_THAT(small_backed, testing::ElementsAre(Length(1), Length(6)));
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(2, large.spans);
  EXPECT_EQ(slack, large.normal_pages);
  EXPECT_EQ(kLen - kPagesPerHugePage, large.returned_pages);
  EXPECT_EQ(kBytes, stats.system_bytes);
  EXPECT_EQ((slack + Length(7)).in_bytes(), stats.free_bytes);
  EXPECT_EQ((region_.size() - NHugePages(1)).in_bytes(), stats.unmapped_bytes);
}

// Test that free regions are broken down properly when they cross
// page boundaries that change the backed/unbacked state.
TEST_F(HugeRegionTest, StatBreakdown) {
  const Length n = kPagesPerHugePage;
  Alloc a = Allocate(n / 4);
  Alloc b = Allocate(n * 3 + n / 3);
  Alloc c = Allocate((n - n / 3 - n / 4) + n * 5 + n / 5);
  Alloc d = Allocate(n - (n / 5) - Length(1));
  // This unbacks the middle 2 hugepages, but not the beginning or
  // trailing region
  ExpectUnback(
      HugeRange::Make(HugePageContaining(b.p) + NHugePages(1), NHugePages(2)));
  DeleteUnback(b);
  Delete(c);
  SmallSpanStats small;
  LargeSpanStats large;
  region_.AddSpanStats(&small, &large);
  // Backed beginning of hugepage 0, unbacked range in middle of b,
  // long backed range from c, unbacked tail of allocation.
  EXPECT_EQ(4, large.spans);
  // Tail end of A's page, B/C combined page + all of C.
  EXPECT_EQ((n - n / 4) + n * 6 + (n / 5), large.normal_pages);
  // The above fill up 10 total pages.
  EXPECT_EQ(2 * n + (Region::size().raw_num() - 10) * n, large.returned_pages);
  EXPECT_EQ(1, small.normal_length[1]);

  EXPECT_EQ(Length(1) + large.normal_pages + large.returned_pages +
                region_.used_pages(),
            Region::size().in_pages());
  Delete(a);
  Delete(d);
}

TEST_F(HugeRegionTest, StatBreakdownReleaseFailure) {
  const Length n = kPagesPerHugePage;
  Alloc a = Allocate(n / 4);
  Alloc b = Allocate(n * 3 + n / 3);
  Alloc c = Allocate((n - n / 3 - n / 4) + n * 5 + n / 5);
  Alloc d = Allocate(n - (n / 5) - Length(1));
  // This tries to unback the middle 2 hugepages, but not the beginning or
  // trailing region, but fails.
  ExpectUnback(
      HugeRange::Make(HugePageContaining(b.p) + NHugePages(1), NHugePages(2)),
      /*success=*/false);
  DeleteUnback(b);
  Delete(c);
  SmallSpanStats small;
  LargeSpanStats large;
  region_.AddSpanStats(&small, &large);
  // Backed beginning of hugepage A/B/C/D and the unbacked tail of allocation.
  EXPECT_EQ(2, large.spans);
  // Tail end of A's page, all of B, all of C.
  EXPECT_EQ((n - n / 4) + n * 8 + (n / 5), large.normal_pages);
  // The above fill up 10 total pages.
  EXPECT_EQ((Region::size().raw_num() - 10) * n, large.returned_pages);
  EXPECT_EQ(1, small.normal_length[1]);

  EXPECT_EQ(Length(1) + large.normal_pages + large.returned_pages +
                region_.used_pages(),
            Region::size().in_pages());
  Delete(a);
  Delete(d);
}

class NilUnback final : public MemoryModifyFunction {
 public:
  bool operator()(void* p, size_t bytes) override { return true; }
};

class HugeRegionSetTest
    : public ::testing::TestWithParam<HugeRegionUsageOption> {
 protected:
  typedef HugeRegion Region;

  static int64_t FakeClock() { return clock_; }
  static double GetFakeClockFrequency() {
    return absl::ToDoubleNanoseconds(absl::Seconds(2));
  }
  static void Advance(absl::Duration d) {
    clock_ += absl::ToDoubleSeconds(d) * GetFakeClockFrequency();
  }
  static void ResetClock() { clock_ = 1234; }

  HugeRegionSetTest()
      : set_(/*use_huge_region_more_often=*/GetParam(),
             Clock{.now = FakeClock, .freq = GetFakeClockFrequency}) {
    next_ = HugePageContaining(nullptr);
  }

  std::unique_ptr<Region> GetRegion() {
    // These regions are backed by "real" memory, but we don't touch it.
    std::unique_ptr<Region> r(new Region({next_, Region::size()}, nil_unback_));
    next_ += Region::size();
    return r;
  }

  struct Alloc {
    PageId p;
    Length n;
  };

  Alloc Allocate(Length n) {
    bool from_released;
    return Allocate(n, &from_released);
  }

  Alloc Allocate(Length n, bool* from_released) {
    Alloc ret;
    TC_CHECK(set_.MaybeGet(n, &ret.p, from_released));
    ret.n = n;
    return ret;
  }

  void Delete(Alloc a) { TC_CHECK(set_.MaybePut(a.p, a.n)); }

  Length ReleasePagesByPeakDemand(Length desired,
                                  SkipSubreleaseIntervals intervals = {},
                                  bool hit_limit = false) {
    return set_.ReleasePagesByPeakDemand(desired, intervals, hit_limit);
  }

  Length HardReleasePages(Length desired) {
    return set_.ReleasePagesByPeakDemand(desired, SkipSubreleaseIntervals{},
                                         /*hit_limit=*/true);
  }

  bool UseHugeRegionMoreOften() const { return set_.UseHugeRegionMoreOften(); }

  NilUnback nil_unback_;
  HugeRegionSet<Region> set_;
  HugePage next_;

  static int64_t clock_;
};

int64_t HugeRegionSetTest::clock_{1234};

TEST_P(HugeRegionSetTest, Release) {
  absl::BitGen rng;
  PageId p;
  constexpr Length kSize = kPagesPerHugePage + Length(1);
  bool from_released;
  ASSERT_FALSE(set_.MaybeGet(Length(1), &p, &from_released));
  auto r1 = GetRegion();
  set_.Contribute(r1.get());

  std::vector<Alloc> allocs;

  while (set_.MaybeGet(kSize, &p, &from_released)) {
    allocs.push_back({p, kSize});
  }
  BackingStats stats = set_.stats();
  EXPECT_EQ(stats.unmapped_bytes, 0);

  for (auto a : allocs) {
    ASSERT_TRUE(set_.MaybePut(a.p, a.n));
  }

  stats = set_.stats();
  EXPECT_EQ(stats.unmapped_bytes,
            UseHugeRegionMoreOften() ? 0 : stats.system_bytes);
  // All the huge pages in the region would be free, but backed, when
  // huge-region-more-often feature is enabled.
  EXPECT_EQ(r1->free_backed().raw_num(),
            UseHugeRegionMoreOften() ? Region::size().raw_num() : 0);
  Length released = set_.ReleasePages(/*release_fraction=*/1.0);
  stats = set_.stats();
  EXPECT_EQ(released.in_bytes(),
            UseHugeRegionMoreOften() ? stats.system_bytes : 0);
  EXPECT_EQ(r1->free_backed().in_bytes(), 0);
  EXPECT_EQ(stats.unmapped_bytes, stats.system_bytes);
}

// Tests that HugeRegions releases all pages in free hugepages when
// skip-subrelease intervals are not set.
TEST_P(HugeRegionSetTest, ReleasePagesWithoutIntervals) {
  absl::BitGen rng;
  PageId p;
  constexpr Length kSize = kPagesPerHugePage + Length(1);
  bool from_released;
  ASSERT_FALSE(set_.MaybeGet(Length(1), &p, &from_released));
  auto r1 = GetRegion();
  set_.Contribute(r1.get());

  std::vector<Alloc> allocs;

  while (set_.MaybeGet(kSize, &p, &from_released)) {
    allocs.push_back({p, kSize});
  }
  BackingStats stats = set_.stats();
  EXPECT_EQ(stats.unmapped_bytes, 0);

  for (auto a : allocs) {
    ASSERT_TRUE(set_.MaybePut(a.p, a.n));
  }

  stats = set_.stats();
  EXPECT_EQ(stats.unmapped_bytes,
            UseHugeRegionMoreOften() ? 0 : stats.system_bytes);
  // All the huge pages in the region would be free, but backed, when
  // huge-region-more-often feature is enabled.
  EXPECT_EQ(r1->free_backed().raw_num(),
            UseHugeRegionMoreOften() ? Region::size().raw_num() : 0);
  Length released = ReleasePagesByPeakDemand(r1->free_backed().in_pages());
  stats = set_.stats();
  EXPECT_EQ(released.in_bytes(),
            UseHugeRegionMoreOften() ? stats.system_bytes : 0);
  EXPECT_EQ(r1->free_backed().in_bytes(), 0);
  EXPECT_EQ(stats.unmapped_bytes, stats.system_bytes);
}

// Tests that releasing zero pages is allowed and does not crash.
TEST_P(HugeRegionSetTest, ReleaseZero) {
  // Trying to release no pages should not crash.
  EXPECT_EQ(ReleasePagesByPeakDemand(
                Length(0),
                SkipSubreleaseIntervals{.short_interval = absl::Seconds(10),
                                        .long_interval = absl::Seconds(600)}),
            Length(0));
}

// Tests the number of pages that are released for different skip subrelease
// intervals.
TEST_P(HugeRegionSetTest, SkipSubrelease) {
  // This test is sensitive to the number of pages per hugepage, as we are
  // printing raw stats.
  if (kPagesPerHugePage != Length(256)) {
    GTEST_SKIP();
  }

  if (!UseHugeRegionMoreOften()) {
    GTEST_SKIP() << "Skipping this test as background release is available "
                    "only when we use huge regions more often.";
  }

  auto r1 = GetRegion();
  set_.Contribute(r1.get());
  // Firstly, this test generates a peak (long-term demand peak) and waits for
  // time interval a. Then, it generates a higher peak plus a short-term
  // fluctuation peak, and waits for time interval b. It then generates a trough
  // in demand and tries to subrelease. Finally, it waits for time interval c to
  // generate the highest peak for evaluating subrelease correctness. Skip
  // subrelease selects those demand points using provided time intervals.
  const auto demand_pattern = [&](absl::Duration a, absl::Duration b,
                                  absl::Duration c,
                                  SkipSubreleaseIntervals intervals,
                                  bool release_phase_1, bool release_phase_2) {
    const Length N = kPagesPerHugePage;
    // First peak: min_demand 3/4N, max_demand 1N.
    Alloc peak1a = Allocate(3 * N / 4);
    Alloc peak1b = Allocate(N / 4);
    Advance(a);
    // Second peak: min_demand 0, max_demand 2N.
    Delete(peak1a);
    Delete(peak1b);

    Alloc half = Allocate(N / 2);
    Alloc tiny1 = Allocate(N / 4);
    Alloc tiny2 = Allocate(N / 4);

    // To force a peak, we allocate 3/4 and 1/4 of a huge page.  This is
    // necessary after we delete `half` below, as a half huge page for the
    // peak would fill into the gap previously occupied by it.
    Alloc peak2a = Allocate(3 * N / 4);
    Alloc peak2b = Allocate(N / 4);
    //    EXPECT_EQ(set_.used_pages(), 2 * N);
    Delete(peak2a);
    Delete(peak2b);
    Advance(b);
    Delete(half);
    EXPECT_EQ(set_.free_backed(), NHugePages(1));
    // The number of released pages is limited to the number of free pages.
    EXPECT_EQ(ReleasePagesByPeakDemand(10 * N, intervals),
              release_phase_1 ? kPagesPerHugePage : Length(0));
    //
    Advance(c);
    // Third peak: min_demand 1/2N, max_demand (2+1/2)N.
    Alloc peak3a = Allocate(3 * N / 4);
    Alloc peak3b = Allocate(N / 4);

    Alloc peak4a = Allocate(3 * N / 4);
    Alloc peak4b = Allocate(N / 4);

    Delete(tiny1);
    Delete(tiny2);
    Delete(peak3a);
    Delete(peak3b);
    Delete(peak4a);
    Delete(peak4b);

    EXPECT_EQ(set_.free_backed(), NHugePages(3));
    EXPECT_EQ(ReleasePagesByPeakDemand(10 * N, intervals),
              release_phase_2 ? NHugePages(3).in_pages() : Length(0));
    HardReleasePages(10 * N);
  };

  {
    // Uses peak interval for skipping subrelease. We should correctly skip
    // 128 pages.
    SCOPED_TRACE("demand_pattern 1");
    demand_pattern(absl::Minutes(2), absl::Minutes(1), absl::Minutes(3),
                   SkipSubreleaseIntervals{.peak_interval = absl::Minutes(3)},
                   /*release_phase_1=*/false,
                   /*release_phase_2=*/false);
  }

  Advance(absl::Minutes(30));

  {
    // Repeats the "demand_pattern 1" test with additional short-term and
    // long-term intervals, to show that skip-subrelease prioritizes using
    // peak_interval.
    SCOPED_TRACE("demand_pattern 2");
    demand_pattern(
        absl::Minutes(2), absl::Minutes(1), absl::Minutes(3),
        SkipSubreleaseIntervals{.peak_interval = absl::Minutes(3),
                                .short_interval = absl::Milliseconds(10),
                                .long_interval = absl::Milliseconds(20)},
        /*release_phase_1=*/false,
        /*release_phase_2=*/false);
  }

  Advance(absl::Minutes(30));

  {
    // Uses peak interval for skipping subrelease, subreleasing all free pages.
    // The short-term interval is not used, as we prioritize using demand peak.
    SCOPED_TRACE("demand_pattern 3");
    demand_pattern(absl::Minutes(6), absl::Minutes(3), absl::Minutes(3),
                   SkipSubreleaseIntervals{.peak_interval = absl::Minutes(2),
                                           .short_interval = absl::Minutes(5)},
                   /*release_phase_1=*/true,
                   /*release_phase_2=*/false);
  }

  Advance(absl::Minutes(30));

  {
    // Skip subrelease feature is disabled if all intervals are zero.
    SCOPED_TRACE("demand_pattern 4");
    demand_pattern(absl::Minutes(1), absl::Minutes(1), absl::Minutes(4),
                   SkipSubreleaseIntervals{},
                   /*release_phase_1=*/true,
                   /*release_phase_2=*/true);
  }

  Advance(absl::Minutes(30));

  {
    // Uses short-term and long-term intervals for skipping subrelease.
    SCOPED_TRACE("demand_pattern 5");
    demand_pattern(absl::Minutes(3), absl::Minutes(2), absl::Minutes(7),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(3),
                                           .long_interval = absl::Minutes(6)},
                   /*release_phase_1=*/false,
                   /*release_phase_2=*/false);
  }

  Advance(absl::Minutes(30));

  {
    // Uses short-term and long-term intervals for skipping subrelease,
    // subreleasing all free pages.
    SCOPED_TRACE("demand_pattern 6");
    demand_pattern(absl::Minutes(4), absl::Minutes(2), absl::Minutes(3),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(1),
                                           .long_interval = absl::Minutes(2)},
                   /*release_phase_1=*/true,
                   /*release_phase_2=*/false);
  }
  Advance(absl::Minutes(30));

  {
    // Uses only short-term interval for skipping subrelease.
    SCOPED_TRACE("demand_pattern 7");
    demand_pattern(absl::Minutes(4), absl::Minutes(2), absl::Minutes(3),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(3)},
                   /*release_phase_1=*/false,
                   /*release_phase_2=*/false);
  }

  Advance(absl::Minutes(30));

  {
    // Uses only long-term interval for skipping subrelease, subreleased all
    // free pages.
    SCOPED_TRACE("demand_pattern 8");
    demand_pattern(absl::Minutes(4), absl::Minutes(2), absl::Minutes(3),
                   SkipSubreleaseIntervals{.long_interval = absl::Minutes(2)},
                   /*release_phase_1=*/true,
                   /*release_phase_2=*/true);
  }

  Advance(absl::Minutes(30));

  // This captures a corner case: If we hit another peak immediately after a
  // subrelease decision (in the same time series epoch), do not count this as
  // a correct subrelease decision.
  {
    SCOPED_TRACE("demand_pattern 9");
    demand_pattern(absl::Milliseconds(10), absl::Milliseconds(10),
                   absl::Milliseconds(10),
                   SkipSubreleaseIntervals{.peak_interval = absl::Minutes(2)},
                   /*release_phase_1=*/false,
                   /*release_phase_2=*/false);
  }
  // Repeats the "demand_pattern 9" test using short-term and long-term
  // intervals, to show that subrelease decisions are evaluated independently.
  {
    SCOPED_TRACE("demand_pattern 10");
    demand_pattern(absl::Milliseconds(10), absl::Milliseconds(10),
                   absl::Milliseconds(10),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(1),
                                           .long_interval = absl::Minutes(2)},
                   /*release_phase_1=*/false,
                   /*release_phase_2=*/false);
  }

  Advance(absl::Minutes(30));

  // Ensure that the tracker is updated.
  auto tiny = Allocate(Length(1));
  Delete(tiny);

  std::string buffer(1024 * 1024, '\0');
  {
    Printer printer(&*buffer.begin(), buffer.size());
    set_.Print(&printer);
  }
  buffer.resize(strlen(buffer.c_str()));

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugeRegion: Since the start of the execution, 14 subreleases (7680 pages) were skipped due to either recent (120s) peaks, or the sum of short-term (60s) fluctuations and long-term (120s) trends.
HugeRegion: 0.0000% of decisions confirmed correct, 0 pending (0.0000% of pages, 0 pending), as per anticipated 300s realized fragmentation.
)"));
}

// Tests that HugeRegion releases all free hugepages when hit_limit is set to
// true.
TEST_P(HugeRegionSetTest, HardRelease) {
  absl::BitGen rng;
  PageId p;
  constexpr Length kSize = kPagesPerHugePage + Length(1);
  bool from_released;
  ASSERT_FALSE(set_.MaybeGet(Length(1), &p, &from_released));
  auto r1 = GetRegion();
  set_.Contribute(r1.get());

  std::vector<Alloc> allocs;

  while (set_.MaybeGet(kSize, &p, &from_released)) {
    allocs.push_back({p, kSize});
  }
  BackingStats stats = set_.stats();
  EXPECT_EQ(stats.unmapped_bytes, 0);

  for (auto a : allocs) {
    ASSERT_TRUE(set_.MaybePut(a.p, a.n));
  }

  stats = set_.stats();
  EXPECT_EQ(stats.unmapped_bytes,
            UseHugeRegionMoreOften() ? 0 : stats.system_bytes);
  // All the huge pages in the region would be free, but backed, when
  // huge-region-more-often feature is enabled.
  EXPECT_EQ(r1->free_backed().raw_num(),
            UseHugeRegionMoreOften() ? Region::size().raw_num() : 0);
  Length released = HardReleasePages(r1->free_backed().in_pages());
  stats = set_.stats();
  EXPECT_EQ(released.in_bytes(),
            UseHugeRegionMoreOften() ? stats.system_bytes : 0);
  EXPECT_EQ(r1->free_backed().in_bytes(), 0);
  EXPECT_EQ(stats.unmapped_bytes, stats.system_bytes);
}

TEST_P(HugeRegionSetTest, Set) {
  absl::BitGen rng;
  PageId p;
  constexpr Length kSize = kPagesPerHugePage + Length(1);
  bool from_released;
  ASSERT_FALSE(set_.MaybeGet(Length(1), &p, &from_released));
  auto r1 = GetRegion();
  auto r2 = GetRegion();
  auto r3 = GetRegion();
  auto r4 = GetRegion();
  set_.Contribute(r1.get());
  set_.Contribute(r2.get());
  set_.Contribute(r3.get());
  set_.Contribute(r4.get());

  std::vector<Alloc> allocs;
  std::vector<Alloc> doomed;

  while (set_.MaybeGet(kSize, &p, &from_released)) {
    allocs.push_back({p, kSize});
  }

  // Define a random set by shuffling, then move half of the allocations into
  // doomed.
  std::shuffle(allocs.begin(), allocs.end(), rng);
  doomed.insert(doomed.begin(), allocs.begin() + allocs.size() / 2,
                allocs.end());
  allocs.erase(allocs.begin() + allocs.size() / 2, allocs.end());

  for (auto d : doomed) {
    ASSERT_TRUE(set_.MaybePut(d.p, d.n));
  }

  for (size_t i = 0; i < 100 * 1000; ++i) {
    const size_t N = allocs.size();
    size_t index = absl::Uniform<int32_t>(rng, 0, N);
    std::swap(allocs[index], allocs[N - 1]);
    auto a = allocs.back();
    ASSERT_TRUE(set_.MaybePut(a.p, a.n));
    allocs.pop_back();
    ASSERT_TRUE(set_.MaybeGet(kSize, &p, &from_released));
    allocs.push_back({p, kSize});
  }

  // Random traffic should have defragmented our allocations into full
  // and empty regions, and released the empty ones.  Annoyingly, we don't
  // know which region is which, so we have to do a bit of silliness:
  std::vector<Region*> regions = {r1.get(), r2.get(), r3.get(), r4.get()};
  std::sort(regions.begin(), regions.end(),
            [](const Region* a, const Region* b) -> bool {
              return a->used_pages() > b->used_pages();
            });

  for (int i = 0; i < regions.size(); i++) {
    TC_LOG("i=%v used=%v free=%v unmapped=%v", i, regions[i]->used_pages(),
           regions[i]->free_pages(), regions[i]->unmapped_pages());
  }
  // Now first two should be "full" (ish)
  EXPECT_LE(Region::size().in_pages().raw_num() * 0.9,
            regions[0]->used_pages().raw_num());
  EXPECT_LE(Region::size().in_pages().raw_num() * 0.9,
            regions[1]->used_pages().raw_num());
  // and last two "empty" (ish.)
  if (UseHugeRegionMoreOften()) {
    EXPECT_EQ(regions[2]->unmapped_pages().raw_num(), 0);
    EXPECT_EQ(regions[3]->unmapped_pages().raw_num(), 0);
    EXPECT_GT(regions[2]->free_backed().raw_num(),
              Region::size().raw_num() * 0.9);
    EXPECT_GT(regions[3]->free_backed().raw_num(),
              Region::size().raw_num() * 0.9);
  } else {
    EXPECT_LE(Region::size().in_pages().raw_num() * 0.9,
              regions[2]->unmapped_pages().raw_num());
    EXPECT_LE(Region::size().in_pages().raw_num() * 0.9,
              regions[3]->unmapped_pages().raw_num());
    EXPECT_EQ(regions[2]->free_backed().raw_num(), 0);
    EXPECT_EQ(regions[3]->free_backed().raw_num(), 0);
  }

  // Check the stats line up.
  auto stats = set_.stats();
  auto raw = r1->stats();
  raw += r2->stats();
  raw += r3->stats();
  raw += r4->stats();
  EXPECT_EQ(raw.system_bytes, stats.system_bytes);
  EXPECT_EQ(raw.unmapped_bytes, stats.unmapped_bytes);
  EXPECT_EQ(raw.free_bytes, stats.free_bytes);

  // Print out the stats for inspection of formats.
  std::vector<char> buf(64 * 1024);
  Printer out(&buf[0], buf.size());
  set_.Print(&out);
  printf("%s\n", &buf[0]);
}

INSTANTIATE_TEST_SUITE_P(
    All, HugeRegionSetTest,
    testing::Values(HugeRegionUsageOption::kDefault,
                    HugeRegionUsageOption::kUseForAllLargeAllocs));

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
