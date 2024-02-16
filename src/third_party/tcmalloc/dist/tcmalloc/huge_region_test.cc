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
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/memory/memory.h"
#include "absl/random/random.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/stats.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

using testing::NiceMock;
using testing::Return;
using testing::StrictMock;

class HugeRegionTest : public ::testing::Test {
 protected:
  HugeRegionTest()
      :  // an unlikely magic page
        p_(HugePageContaining(reinterpret_cast<void*>(0x1faced200000))),
        region_({p_, region_.size()}, MemoryModifyFunction(MockUnback)) {
    // we usually don't care about backing calls, unless testing that
    // specifically.
    mock_ = absl::make_unique<NiceMock<MockBackingInterface>>();
  }

  ~HugeRegionTest() override { mock_.reset(nullptr); }

  // This is wordy, but necessary for mocking:
  class BackingInterface {
   public:
    virtual bool Unback(void* p, size_t len) = 0;
    virtual ~BackingInterface() {}
  };

  class MockBackingInterface : public BackingInterface {
   public:
    MOCK_METHOD(bool, Unback, (void* p, size_t len), (override));
  };

  static std::unique_ptr<MockBackingInterface> mock_;

  static bool MockUnback(void* p, size_t len) { return mock_->Unback(p, len); }

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
    CHECK_CONDITION(region_.MaybeGet(n, &ret.p, from_released));
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

std::unique_ptr<HugeRegionTest::MockBackingInterface> HugeRegionTest::mock_;

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

TEST_F(HugeRegionTest, Release) {
  mock_ = absl::make_unique<StrictMock<MockBackingInterface>>();
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
  EXPECT_EQ(NHugePages(2), region_.Release());
  CheckMock();

  // Now we're on exact boundaries so we should unback the whole range.
  Delete(d);
  ExpectUnback({p_ + NHugePages(12), NHugePages(2)});
  EXPECT_EQ(NHugePages(2), region_.Release());
  CheckMock();

  Delete(a);
  ExpectUnback({p_ + NHugePages(0), NHugePages(4)});
  EXPECT_EQ(NHugePages(4), region_.Release());
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

TEST_F(HugeRegionTest, Reback) {
  mock_ = absl::make_unique<StrictMock<MockBackingInterface>>();
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
                     BackingStats* stats, double* avg_age_backed,
                     double* avg_age_unbacked) {
      SmallSpanStats small;
      *large = LargeSpanStats();
      PageAgeHistograms ages(absl::base_internal::CycleClock::Now());
      region.AddSpanStats(&small, large, &ages);
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

      *avg_age_backed = ages.GetTotalHistogram(false)->avg_age();
      *avg_age_unbacked = ages.GetTotalHistogram(true)->avg_age();
    }
  };

  LargeSpanStats large;
  std::vector<Length> small_backed, small_unbacked;
  BackingStats stats;
  double avg_age_backed, avg_age_unbacked;

  absl::SleepFor(absl::Milliseconds(10));
  Helper::Stat(region_, &small_backed, &small_unbacked, &large, &stats,
               &avg_age_backed, &avg_age_unbacked);
  EXPECT_THAT(small_backed, testing::ElementsAre());
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(1, large.spans);
  EXPECT_EQ(Length(0), large.normal_pages);
  EXPECT_EQ(kLen, large.returned_pages);
  EXPECT_EQ(kBytes, stats.system_bytes);
  EXPECT_EQ(0, stats.free_bytes);
  EXPECT_EQ(kBytes, stats.unmapped_bytes);
  EXPECT_LE(0.01, avg_age_unbacked);
  EXPECT_EQ(0, avg_age_backed);

  // We don't, in production, use small allocations from the region, but
  // the API supports it, so test it here.
  Alloc a = Allocate(Length(1));
  Allocate(Length(1));
  Alloc b = Allocate(Length(2));
  Alloc barrier = Allocate(Length(1));
  Alloc c = Allocate(Length(3));
  Allocate(Length(1));
  const Length slack = kPagesPerHugePage - Length(9);

  absl::SleepFor(absl::Milliseconds(20));
  Helper::Stat(region_, &small_backed, &small_unbacked, &large, &stats,
               &avg_age_backed, &avg_age_unbacked);
  EXPECT_THAT(small_backed, testing::ElementsAre());
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(2, large.spans);
  EXPECT_EQ(slack, large.normal_pages);
  EXPECT_EQ(kLen - kPagesPerHugePage, large.returned_pages);
  EXPECT_EQ(kBytes, stats.system_bytes);
  EXPECT_EQ(slack.in_bytes(), stats.free_bytes);
  EXPECT_EQ((region_.size() - NHugePages(1)).in_bytes(), stats.unmapped_bytes);
  EXPECT_LE(0.02, avg_age_backed);
  EXPECT_LE(0.03, avg_age_unbacked);

  Delete(a);
  absl::SleepFor(absl::Milliseconds(30));
  Helper::Stat(region_, &small_backed, &small_unbacked, &large, &stats,
               &avg_age_backed, &avg_age_unbacked);
  EXPECT_THAT(small_backed, testing::ElementsAre(Length(1)));
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(2, large.spans);
  EXPECT_EQ(slack, large.normal_pages);
  EXPECT_EQ(kLen - kPagesPerHugePage, large.returned_pages);
  EXPECT_EQ(kBytes, stats.system_bytes);
  EXPECT_EQ((slack + Length(1)).in_bytes(), stats.free_bytes);
  EXPECT_EQ((region_.size() - NHugePages(1)).in_bytes(), stats.unmapped_bytes);
  EXPECT_LE((slack.raw_num() * 0.05 + 1 * 0.03) / (slack.raw_num() + 1),
            avg_age_backed);
  EXPECT_LE(0.06, avg_age_unbacked);

  Delete(b);
  absl::SleepFor(absl::Milliseconds(40));
  Helper::Stat(region_, &small_backed, &small_unbacked, &large, &stats,
               &avg_age_backed, &avg_age_unbacked);
  EXPECT_THAT(small_backed, testing::ElementsAre(Length(1), Length(2)));
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(2, large.spans);
  EXPECT_EQ(slack, large.normal_pages);
  EXPECT_EQ(kLen - kPagesPerHugePage, large.returned_pages);
  EXPECT_EQ(kBytes, stats.system_bytes);
  EXPECT_EQ((slack + Length(3)).in_bytes(), stats.free_bytes);
  EXPECT_EQ((region_.size() - NHugePages(1)).in_bytes(), stats.unmapped_bytes);
  EXPECT_LE(
      (slack.raw_num() * 0.09 + 1 * 0.07 + 2 * 0.04) / (slack.raw_num() + 3),
      avg_age_backed);
  EXPECT_LE(0.10, avg_age_unbacked);

  Delete(c);
  absl::SleepFor(absl::Milliseconds(50));
  Helper::Stat(region_, &small_backed, &small_unbacked, &large, &stats,
               &avg_age_backed, &avg_age_unbacked);
  EXPECT_THAT(small_backed,
              testing::ElementsAre(Length(1), Length(2), Length(3)));
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(2, large.spans);
  EXPECT_EQ(slack, large.normal_pages);
  EXPECT_EQ(kLen - kPagesPerHugePage, large.returned_pages);
  EXPECT_EQ(kBytes, stats.system_bytes);
  EXPECT_EQ((slack + Length(6)).in_bytes(), stats.free_bytes);
  EXPECT_EQ((region_.size() - NHugePages(1)).in_bytes(), stats.unmapped_bytes);
  EXPECT_LE((slack.raw_num() * 0.14 + 1 * 0.12 + 2 * 0.09 + 3 * 0.05) /
                (slack.raw_num() + 6),
            avg_age_backed);
  EXPECT_LE(0.15, avg_age_unbacked);

  Delete(barrier);
  absl::SleepFor(absl::Milliseconds(60));
  Helper::Stat(region_, &small_backed, &small_unbacked, &large, &stats,
               &avg_age_backed, &avg_age_unbacked);
  EXPECT_THAT(small_backed, testing::ElementsAre(Length(1), Length(6)));
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(2, large.spans);
  EXPECT_EQ(slack, large.normal_pages);
  EXPECT_EQ(kLen - kPagesPerHugePage, large.returned_pages);
  EXPECT_EQ(kBytes, stats.system_bytes);
  EXPECT_EQ((slack + Length(7)).in_bytes(), stats.free_bytes);
  EXPECT_EQ((region_.size() - NHugePages(1)).in_bytes(), stats.unmapped_bytes);
  EXPECT_LE(
      (slack.raw_num() * 0.20 + 1 * 0.18 + 2 * 0.15 + 3 * 0.11 + 1 * 0.06) /
          (slack.raw_num() + 7),
      avg_age_backed);
  EXPECT_LE(0.21, avg_age_unbacked);
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
  region_.AddSpanStats(&small, &large, nullptr);
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
  region_.AddSpanStats(&small, &large, nullptr);
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

static bool NilUnback(void* p, size_t bytes) {
  return true;
}

class HugeRegionSetTest : public testing::Test {
 protected:
  typedef HugeRegion Region;

  HugeRegionSetTest() { next_ = HugePageContaining(nullptr); }

  std::unique_ptr<Region> GetRegion() {
    // These regions are backed by "real" memory, but we don't touch it.
    std::unique_ptr<Region> r(
        new Region({next_, Region::size()}, MemoryModifyFunction(NilUnback)));
    next_ += Region::size();
    return r;
  }

  HugeRegionSet<Region> set_;
  HugePage next_;

  struct Alloc {
    PageId p;
    Length n;
  };
};

TEST_F(HugeRegionSetTest, Set) {
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
    Log(kLog, __FILE__, __LINE__, i, regions[i]->used_pages().raw_num(),
        regions[i]->free_pages().raw_num(),
        regions[i]->unmapped_pages().raw_num());
  }
  // Now first two should be "full" (ish)
  EXPECT_LE(Region::size().in_pages().raw_num() * 0.9,
            regions[0]->used_pages().raw_num());
  EXPECT_LE(Region::size().in_pages().raw_num() * 0.9,
            regions[1]->used_pages().raw_num());
  // and last two "empty" (ish.)
  EXPECT_LE(Region::size().in_pages().raw_num() * 0.9,
            regions[2]->unmapped_pages().raw_num());
  EXPECT_LE(Region::size().in_pages().raw_num() * 0.9,
            regions[3]->unmapped_pages().raw_num());

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

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
