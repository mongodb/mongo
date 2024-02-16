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

#include "tcmalloc/huge_page_aware_allocator.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/base/optimization.h"
#include "absl/container/flat_hash_map.h"
#include "absl/flags/flag.h"
#include "absl/meta/type_traits.h"
#include "absl/random/random.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/barrier.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/page_allocator_test_util.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/system-alloc.h"
#include "tcmalloc/testing/thread_manager.h"

ABSL_FLAG(std::string, tracefile, "", "file to pull trace from");
ABSL_FLAG(uint64_t, limit, 0, "");
ABSL_FLAG(bool, always_check_usage, false, "enable expensive memory checks");

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

using testing::HasSubstr;

class HugePageAwareAllocatorTest
    : public ::testing::TestWithParam<HugeRegionCountOption> {
 protected:
  HugePageAwareAllocatorTest() : rng_() {
    before_ = MallocExtension::GetRegionFactory();
    extra_ = new ExtraRegionFactory(before_);
    MallocExtension::SetRegionFactory(extra_);

    // HugePageAwareAllocator can't be destroyed cleanly, so we store a pointer
    // to one and construct in place.
    void* p = malloc(sizeof(HugePageAwareAllocator));
    allocator_ = new (p) HugePageAwareAllocator(MemoryTag::kNormal, GetParam());
  }

  ~HugePageAwareAllocatorTest() override {
    CHECK_CONDITION(ids_.empty());
    CHECK_CONDITION(total_ == Length(0));
    // We end up leaking both the backing allocations and the metadata.
    // The backing allocations are unmapped--it's silly, but not
    // costing us muchin a 64-bit address space.
    // The metadata is real memory, but there's barely any of it.
    // It'd be very complicated to rebuild the allocator to support
    // teardown, so we just put up with it.
    {
      absl::base_internal::SpinLockHolder h(&pageheap_lock);
      auto stats = allocator_->stats();
      if (stats.free_bytes + stats.unmapped_bytes != stats.system_bytes) {
        Crash(kCrash, __FILE__, __LINE__, stats.free_bytes,
              stats.unmapped_bytes, "!=", stats.system_bytes);
      }
    }

    free(allocator_);

    MallocExtension::SetRegionFactory(before_);
    delete extra_;
  }

  void CheckStats() {
    size_t actual_used_bytes = total_.in_bytes();
    BackingStats stats;
    {
      absl::base_internal::SpinLockHolder h2(&pageheap_lock);
      stats = allocator_->stats();
    }
    uint64_t used_bytes =
        stats.system_bytes - stats.free_bytes - stats.unmapped_bytes;
    ASSERT_EQ(used_bytes, actual_used_bytes);
  }

  uint64_t GetFreeBytes() {
    BackingStats stats;
    {
      absl::base_internal::SpinLockHolder h2(&pageheap_lock);
      stats = allocator_->stats();
    }
    return stats.free_bytes;
  }

  Span* AllocatorNew(Length n, size_t objects_per_span) {
    return allocator_->New(n, objects_per_span);
  }

  void AllocatorDelete(Span* s, size_t objects_per_span) {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    allocator_->Delete(s, objects_per_span);
  }

  Span* New(Length n, size_t objects_per_span) {
    absl::base_internal::SpinLockHolder h(&lock_);
    Span* span = AllocatorNew(n, objects_per_span);
    CHECK_CONDITION(span != nullptr);
    EXPECT_GE(span->num_pages(), n);
    const size_t id = next_id_++;
    total_ += n;
    CheckStats();
    // and distinct spans...
    CHECK_CONDITION(ids_.insert({span, id}).second);
    return span;
  }

  void Delete(Span* span, size_t objects_per_span) {
    Length n = span->num_pages();
    {
      absl::base_internal::SpinLockHolder h(&lock_);
      auto i = ids_.find(span);
      CHECK_CONDITION(i != ids_.end());
      const size_t id = i->second;
      ids_.erase(i);
      AllocatorDelete(span, objects_per_span);
      total_ -= n;
      CheckStats();
    }
  }

  // Mostly small things, some large ones.
  std::pair<Length, size_t> RandomAllocSize() {
    // TODO(b/128521238): scalable RNG
    absl::base_internal::SpinLockHolder h(&lock_);
    size_t objects = absl::Uniform<size_t>(rng_, 1, 256);
    if (absl::Bernoulli(rng_, 1.0 / 1000)) {
      Length n =
          Length(1024) * (1 + absl::LogUniform<int32_t>(rng_, 0, (1 << 8) - 1));
      n += Length(absl::Uniform<int32_t>(rng_, 0, 1024));
      return {n, objects};
    }
    return {Length(1 + absl::LogUniform<int32_t>(rng_, 0, (1 << 9) - 1)),
            objects};
  }

  Length ReleasePages(Length k) {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    return allocator_->ReleaseAtLeastNPages(k);
  }

  std::string Print() {
    std::string ret;
    const size_t kSize = 1 << 20;
    ret.resize(kSize);
    Printer p(&ret[0], kSize);
    allocator_->Print(&p);
    ret.erase(p.SpaceRequired());
    return ret;
  }

  std::string PrintInPbtxt() {
    std::string ret;
    const size_t kSize = 1 << 20;
    ret.resize(kSize);
    Printer p(&ret[0], kSize);
    {
      PbtxtRegion region(&p, kNested);
      allocator_->PrintInPbtxt(&region);
    }
    ret.erase(p.SpaceRequired());
    return ret;
  }

  HugePageAwareAllocator* allocator_;
  ExtraRegionFactory* extra_;
  AddressRegionFactory* before_;
  absl::BitGen rng_;
  absl::base_internal::SpinLock lock_;
  absl::flat_hash_map<Span*, size_t> ids_;
  size_t next_id_{0};
  Length total_;
  bool use_huge_regions_more_often_;
};

struct SpanInfo {
  Span* span;
  size_t objects_per_span;
};

TEST_P(HugePageAwareAllocatorTest, Fuzz) {
  std::vector<SpanInfo> allocs;
  for (int i = 0; i < 5000; ++i) {
    auto [n, objects] = RandomAllocSize();
    Span* s = New(n, objects);
    allocs.push_back(SpanInfo{s, objects});
  }
  static const size_t kReps = 50 * 1000;
  for (int i = 0; i < kReps; ++i) {
    SCOPED_TRACE(absl::StrFormat("%d reps, %d pages", i, total_.raw_num()));
    size_t index = absl::Uniform<int32_t>(rng_, 0, allocs.size());
    Span* old_span = allocs[index].span;
    const size_t old_objects = allocs[index].objects_per_span;
    Delete(old_span, old_objects);
    auto [n, objects] = RandomAllocSize();
    allocs[index] = SpanInfo{New(n, objects), objects};
  }

  for (auto s : allocs) {
    Delete(s.span, s.objects_per_span);
  }
}

// Prevent regression of the fragmentation problem that was reported in
// b/63301358, reproduced in CL/161345659 and (partially) fixed in CL/161305971.
TEST_P(HugePageAwareAllocatorTest, JustUnderMultipleOfHugepages) {
  std::vector<Span*> big_allocs, small_allocs;
  // Trigger creation of a hugepage with more than one allocation and plenty of
  // free space.
  small_allocs.push_back(New(Length(1), 1));
  small_allocs.push_back(New(Length(10), 1));
  // Limit iterations so that the huge page with the small allocs doesn't fill
  // up.
  size_t n_iter = (kPagesPerHugePage - Length(2)).raw_num();
  // Also limit memory usage to ~1 GB.
  n_iter = std::min((1 << 30) / (2 * kHugePageSize), n_iter);
  for (int i = 0; i < n_iter; ++i) {
    Length n = 2 * kPagesPerHugePage - Length(1);
    big_allocs.push_back(New(n, 1));
    small_allocs.push_back(New(Length(1), 1));
  }
  for (auto* span : big_allocs) {
    Delete(span, 1);
  }
  // We should have one hugepage that's full of small allocations and a bunch
  // of empty hugepages. The HugeCache will keep some of the empty hugepages
  // backed so free space should drop to a small multiple of the huge page size.
  EXPECT_LE(GetFreeBytes(), 20 * kHugePageSize);
  for (auto* span : small_allocs) {
    Delete(span, 1);
  }
}

TEST_P(HugePageAwareAllocatorTest, Multithreaded) {
  static const size_t kThreads = 16;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  absl::Barrier b1(kThreads);
  absl::Barrier b2(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.push_back(std::thread([this, &b1, &b2]() {
      absl::BitGen rng;
      std::vector<SpanInfo> allocs;
      for (int i = 0; i < 150; ++i) {
        auto [n, objects] = RandomAllocSize();
        allocs.push_back(SpanInfo{New(n, objects), objects});
      }
      b1.Block();
      static const size_t kReps = 4 * 1000;
      for (int i = 0; i < kReps; ++i) {
        size_t index = absl::Uniform<int32_t>(rng, 0, allocs.size());
        Delete(allocs[index].span, allocs[index].objects_per_span);
        auto [n, objects] = RandomAllocSize();
        allocs[index] = SpanInfo{New(n, objects), objects};
      }
      b2.Block();
      for (auto s : allocs) {
        Delete(s.span, s.objects_per_span);
      }
    }));
  }

  for (auto& t : threads) {
    t.join();
  }
}

TEST_P(HugePageAwareAllocatorTest, ReleasingLarge) {
  // Ensure the HugeCache has some free items:
  Delete(New(kPagesPerHugePage, 1), 1);
  ASSERT_LE(kPagesPerHugePage, ReleasePages(kPagesPerHugePage));
}

TEST_P(HugePageAwareAllocatorTest, ReleasingSmall) {
  const bool old_subrelease = Parameters::hpaa_subrelease();
  Parameters::set_hpaa_subrelease(true);

  const absl::Duration old_skip_subrelease_interval =
      Parameters::filler_skip_subrelease_interval();
  Parameters::set_filler_skip_subrelease_interval(absl::ZeroDuration());

  const absl::Duration old_skip_subrelease_short_interval =
      Parameters::filler_skip_subrelease_short_interval();
  Parameters::set_filler_skip_subrelease_short_interval(absl::ZeroDuration());

  const absl::Duration old_skip_subrelease_long_interval =
      Parameters::filler_skip_subrelease_long_interval();
  Parameters::set_filler_skip_subrelease_long_interval(absl::ZeroDuration());

  std::vector<Span*> live, dead;
  static const size_t N = kPagesPerHugePage.raw_num() * 128;
  for (int i = 0; i < N; ++i) {
    Span* span = New(Length(1), 1);
    ((i % 2 == 0) ? live : dead).push_back(span);
  }

  for (auto d : dead) {
    Delete(d, 1);
  }

  EXPECT_EQ(kPagesPerHugePage / 2, ReleasePages(Length(1)));

  for (auto l : live) {
    Delete(l, 1);
  }

  Parameters::set_hpaa_subrelease(old_subrelease);
  Parameters::set_filler_skip_subrelease_interval(old_skip_subrelease_interval);
  Parameters::set_filler_skip_subrelease_short_interval(
      old_skip_subrelease_short_interval);
  Parameters::set_filler_skip_subrelease_long_interval(
      old_skip_subrelease_long_interval);
}

TEST_P(HugePageAwareAllocatorTest, UseHugeRegion) {
  // This test verifies that we use HugeRegion for large allocations as soon as
  // the abandoned pages exceed 64MB, when we use abandoned count in addition to
  // slack for determining when to use region. If we use slack for computation,
  // this test should not trigger use of HugeRegion.
  static constexpr Length kSlack = kPagesPerHugePage / 2 - Length(2);
  static constexpr Length kSmallSize = kSlack;
  static constexpr Length kLargeSize = kPagesPerHugePage - kSlack;

  Length slack;
  Length small_pages;
  HugeLength donated_huge_pages;
  Length abandoned_pages;
  size_t active_regions;

  auto RefreshStats = [&]() {
    absl::base_internal::SpinLockHolder l(&pageheap_lock);
    slack = allocator_->info().slack();
    small_pages = allocator_->info().small();
    donated_huge_pages = allocator_->DonatedHugePages();
    abandoned_pages = allocator_->AbandonedPages();
    active_regions = allocator_->region().ActiveRegions();
  };

  std::vector<Span*> small_spans;
  std::vector<Span*> large_spans;
  const Length small_binary_size = HLFromBytes(64 * 1024 * 1024).in_pages();
  Length expected_abandoned;
  Length expected_slack;
  int huge_pages = 0;

  // We first allocate large objects such that expected abandoned pages (once we
  // deallocate those large objects) exceed the 64MB threshold. We place small
  // allocations on the donated pages so that the hugepages aren't released.
  while (true) {
    Span* large = New(kLargeSize, 1);
    Span* small = New(kSmallSize, 1);
    large_spans.emplace_back(large);
    small_spans.emplace_back(small);
    ++huge_pages;
    expected_abandoned += kLargeSize;
    expected_slack += kSlack;

    RefreshStats();
    EXPECT_EQ(abandoned_pages, Length(0));
    EXPECT_EQ(donated_huge_pages, NHugePages(huge_pages));
    EXPECT_EQ(slack, expected_slack);
    EXPECT_EQ(active_regions, 0);
    if (expected_abandoned >= small_binary_size) break;
  }

  // Reset the abandoned count and start releasing huge allocations. We should
  // start accumulating abandoned pages in filler. As we don't expect to trigger
  // HugeRegion yet, the number of active regions should be zero throughout.
  expected_abandoned = Length(0);
  for (auto l : large_spans) {
    Delete(l, 1);
    expected_abandoned += kLargeSize;
    expected_slack -= kSlack;
    RefreshStats();
    EXPECT_EQ(abandoned_pages, expected_abandoned);
    EXPECT_EQ(donated_huge_pages, NHugePages(huge_pages));
    EXPECT_EQ(slack, expected_slack);
    EXPECT_EQ(active_regions, 0);
  }
  large_spans.clear();

  RefreshStats();
  EXPECT_EQ(slack, Length(0));
  EXPECT_GE(abandoned_pages, small_binary_size);

  // At this point, we have exhausted the 64MB slack for the donated pages to
  // the filler. A large allocation should trigger allocation from a huge
  // region if we are in HugeRegionCountOption::kAbandonedCount mode. If we are
  // using slack for determining when to use region, we should allocate from
  // filler and number of donated pages should continue to grow.
  //
  // We allocate a slightly larger object than before (kLargeSize + Length(1))
  // to make sure that filler doesn't try to pack it on the pages we released
  // due to deallocations in the previous step.
  static constexpr Length kSmallSize2 = kSmallSize - Length(1);
  static constexpr Length kLargeSize2 = kLargeSize + Length(1);
  for (int i = 0; i < 100; ++i) {
    Span* large = New(kLargeSize2, 1);
    Span* small = New(kSmallSize2, 1);
    large_spans.emplace_back(large);
    small_spans.emplace_back(small);
    RefreshStats();
    if (GetParam() == HugeRegionCountOption::kSlack) {
      ASSERT_LT(slack, small_pages);
      ++huge_pages;
      EXPECT_EQ(abandoned_pages, expected_abandoned);
      EXPECT_EQ(donated_huge_pages, NHugePages(huge_pages));
      EXPECT_EQ(active_regions, 0);
    } else {
      RefreshStats();
      EXPECT_EQ(abandoned_pages, expected_abandoned);
      EXPECT_EQ(donated_huge_pages, NHugePages(huge_pages));
      EXPECT_EQ(active_regions, 1);
    }
  }

  // Clean up.
  for (auto l : large_spans) {
    Delete(l, 1);
  }
  for (auto s : small_spans) {
    Delete(s, 1);
  }
}

TEST_P(HugePageAwareAllocatorTest, DonatedHugePages) {
  // This test verifies that we accurately measure the amount of RAM that we
  // donate to the huge page filler when making large allocations, including
  // those kept alive after we deallocate.
  static constexpr Length kSlack = Length(2);
  static constexpr Length kLargeSize = 2 * kPagesPerHugePage - kSlack;
  static constexpr Length kSmallSize = Length(1);

  Span* large1 = New(kLargeSize, 1);
  Length slack;
  HugeLength donated_huge_pages;
  Length abandoned_pages;

  auto RefreshStats = [&]() {
    absl::base_internal::SpinLockHolder l(&pageheap_lock);
    slack = allocator_->info().slack();
    donated_huge_pages = allocator_->DonatedHugePages();
    abandoned_pages = allocator_->AbandonedPages();
  };
  RefreshStats();

  EXPECT_EQ(slack, kSlack);
  EXPECT_EQ(donated_huge_pages, NHugePages(1));
  EXPECT_EQ(abandoned_pages, Length(0));
  EXPECT_THAT(Print(), HasSubstr("filler donations 1"));
  EXPECT_THAT(PrintInPbtxt(), HasSubstr("filler_donated_huge_pages: 1"));
  EXPECT_THAT(PrintInPbtxt(), HasSubstr("filler_abandoned_pages: 0"));

  // Make a small allocation and then free the large allocation.  Slack should
  // fall, but we've kept alive our donation to the filler.
  Span* small = New(kSmallSize, 1);
  Delete(large1, 1);

  RefreshStats();

  EXPECT_EQ(slack, Length(0));
  EXPECT_EQ(donated_huge_pages, NHugePages(1));
  EXPECT_EQ(abandoned_pages, kPagesPerHugePage - kSlack);

  EXPECT_THAT(Print(), HasSubstr(absl::StrCat("filler donations 1")));
  EXPECT_THAT(PrintInPbtxt(), HasSubstr("filler_donated_huge_pages: 1"));
  EXPECT_THAT(PrintInPbtxt(),
              HasSubstr(absl::StrCat("filler_abandoned_pages: ",
                                     (kPagesPerHugePage - kSlack).raw_num())));

  // Make another large allocation.  The number of donated huge pages should
  // continue to increase.
  Span* large2 = New(kLargeSize, 1);

  RefreshStats();

  EXPECT_EQ(slack, kSlack);
  EXPECT_EQ(donated_huge_pages, NHugePages(2));
  EXPECT_EQ(abandoned_pages, kPagesPerHugePage - kSlack);

  EXPECT_THAT(Print(), HasSubstr(absl::StrCat("filler donations 2")));
  EXPECT_THAT(PrintInPbtxt(), HasSubstr("filler_donated_huge_pages: 2"));
  EXPECT_THAT(PrintInPbtxt(),
              HasSubstr(absl::StrCat("filler_abandoned_pages: ",
                                     (kPagesPerHugePage - kSlack).raw_num())));

  // Deallocating the small allocation finally reduces the reduce the number of
  // donations, as we were able reassemble the huge page for large1.
  Delete(small, 1);

  RefreshStats();

  EXPECT_EQ(slack, kSlack);
  EXPECT_EQ(donated_huge_pages, NHugePages(1));
  EXPECT_EQ(abandoned_pages, Length(0));

  EXPECT_THAT(Print(), HasSubstr(absl::StrCat("filler donations 1")));
  EXPECT_THAT(PrintInPbtxt(), HasSubstr("filler_donated_huge_pages: 1"));
  EXPECT_THAT(PrintInPbtxt(), HasSubstr("filler_abandoned_pages: 0"));

  // Deallocating everything should return slack to 0 and allow large2's
  // contiguous VSS to be reassembled.
  Delete(large2, 1);

  RefreshStats();

  EXPECT_EQ(slack, Length(0));
  EXPECT_EQ(donated_huge_pages, NHugePages(0));
  EXPECT_EQ(abandoned_pages, Length(0));

  EXPECT_THAT(Print(), HasSubstr(absl::StrCat("filler donations 0")));
  EXPECT_THAT(PrintInPbtxt(), HasSubstr("filler_donated_huge_pages: 0"));
  EXPECT_THAT(PrintInPbtxt(), HasSubstr("filler_abandoned_pages: 0"));
}

TEST_P(HugePageAwareAllocatorTest, SmallDonations) {
  // This test works with small donations (kHugePageSize/2,kHugePageSize]-bytes
  // in size to check statistics.
  static constexpr Length kSlack = Length(2);
  static constexpr Length kLargeSize = kPagesPerHugePage - kSlack;
  static constexpr Length kSmallSize = Length(1);
  static constexpr Length kSmallSize2 = kSlack;

  Span* large1 = New(kLargeSize, 1);
  Span* large2 = New(kLargeSize, 1);

  Length slack;
  HugeLength donated_huge_pages;
  Length abandoned_pages;

  auto RefreshStats = [&]() {
    absl::base_internal::SpinLockHolder l(&pageheap_lock);
    slack = allocator_->info().slack();
    donated_huge_pages = allocator_->DonatedHugePages();
    abandoned_pages = allocator_->AbandonedPages();
  };
  RefreshStats();

  EXPECT_EQ(slack, 2 * kSlack);
  EXPECT_EQ(donated_huge_pages, NHugePages(2));
  EXPECT_EQ(abandoned_pages, Length(0));
  EXPECT_TRUE(large1->donated());
  EXPECT_TRUE(large2->donated());
  // HugePageAwareAllocatorTest.DonatedHugePages verifies Print works correctly
  // for these stats.

  // Create two small allocations.  They will be placed on different huge pages
  // since kSmallSize+kSmallSize2 > kSlack for any single huge page.
  Span* small1 = New(kSmallSize, 1);
  Span* small2 = New(kSmallSize2, 1);

  RefreshStats();
  EXPECT_EQ(slack, 2 * kSlack);
  EXPECT_EQ(donated_huge_pages, NHugePages(2));
  EXPECT_EQ(abandoned_pages, Length(0));
  EXPECT_FALSE(small1->donated());
  EXPECT_FALSE(small2->donated());

  // To simplify the rest of the test, swap small1/small2 as required such that
  // small1 is on the same huge page as large1, etc.  This allows us to release
  // 2 allocations from the same huge page.
  if (HugePageContaining(large1->first_page()) !=
      HugePageContaining(small1->first_page())) {
    std::swap(small1, small2);
  }
  EXPECT_EQ(HugePageContaining(large1->first_page()),
            HugePageContaining(small1->first_page()));
  EXPECT_EQ(HugePageContaining(large2->first_page()),
            HugePageContaining(small2->first_page()));

  // Release both allocations from one huge page.  Donations should tick down
  // and no pages should be considered abandoned.
  Delete(large1, 1);
  Delete(small1, 1);

  RefreshStats();
  EXPECT_EQ(slack, kSlack);
  EXPECT_EQ(donated_huge_pages, NHugePages(1));
  EXPECT_EQ(abandoned_pages, Length(0));

  // Delete the large allocation on the second huge page.  Abandoned should tick
  // up.
  Delete(large2, 1);

  RefreshStats();
  EXPECT_EQ(slack, Length(0));
  EXPECT_EQ(donated_huge_pages, NHugePages(1));
  EXPECT_EQ(abandoned_pages, kLargeSize);

  // Reuse large2 and then deallocate it.  Our abandoned count stats should not
  // be double counted.
  large2 = New(kLargeSize, 1);

  RefreshStats();
  EXPECT_EQ(slack, kSlack);
  EXPECT_EQ(donated_huge_pages, NHugePages(1));
  EXPECT_EQ(abandoned_pages, kLargeSize);

  Delete(large2, 1);
  RefreshStats();
  EXPECT_EQ(slack, Length(0));
  EXPECT_EQ(donated_huge_pages, NHugePages(1));
  EXPECT_EQ(abandoned_pages, kLargeSize);

  // Cleanup
  Delete(small2, 1);

  RefreshStats();
  EXPECT_EQ(slack, Length(0));
  EXPECT_EQ(donated_huge_pages, NHugePages(0));
  EXPECT_EQ(abandoned_pages, Length(0));
}

TEST_P(HugePageAwareAllocatorTest, LargeDonations) {
  // A small allocation of size (kHugePageSize/2,kHugePageSize]-bytes can be
  // considered not donated if it filled in a gap on an otherwise mostly free
  // huge page that came from a donation.
  static constexpr Length kSmallSize = kPagesPerHugePage - Length(1);
  static constexpr Length kLargeSize = kPagesPerHugePage + Length(1);

  // large1 donates kSmallSize bytes to the filler.
  Span* large = New(kLargeSize, 1);
  Length slack;
  HugeLength donated_huge_pages;
  Length abandoned_pages;

  auto RefreshStats = [&]() {
    absl::base_internal::SpinLockHolder l(&pageheap_lock);
    slack = allocator_->info().slack();
    donated_huge_pages = allocator_->DonatedHugePages();
    abandoned_pages = allocator_->AbandonedPages();
  };
  RefreshStats();

  EXPECT_EQ(slack, kSmallSize);
  EXPECT_EQ(donated_huge_pages, NHugePages(1));
  EXPECT_EQ(abandoned_pages, Length(0));
  EXPECT_TRUE(large->donated());
  // HugePageAwareAllocatorTest.DonatedHugePages verifies Print works correctly
  // for these stats.

  Span* small = New(kSmallSize, 1);
  RefreshStats();

  // TODO(b/199203282): Current slack computation is unaware that this
  // allocation is on a donated page. It assumes that kSmallSize allocation
  // would also result in a slack. We would eliminate this once abandoned count
  // subsumes slack computation.
  EXPECT_EQ(slack, kSmallSize + Length(1));
  EXPECT_EQ(donated_huge_pages, NHugePages(1));
  EXPECT_EQ(abandoned_pages, Length(0));
  EXPECT_FALSE(small->donated());

  // small is on a donated hugepage.  None of the stats should change when it is
  // deallocated.
  Delete(small, 1);
  RefreshStats();
  EXPECT_EQ(slack, kSmallSize);
  EXPECT_EQ(donated_huge_pages, NHugePages(1));
  EXPECT_EQ(abandoned_pages, Length(0));

  // Cleanup.  Deallocate large.
  Delete(large, 1);
  RefreshStats();
  EXPECT_EQ(slack, Length(0));
  EXPECT_EQ(donated_huge_pages, NHugePages(0));
  EXPECT_EQ(abandoned_pages, Length(0));
}

TEST_P(HugePageAwareAllocatorTest, TailDonation) {
  // This test makes sure that we account for tail donations alone in the
  // abandoned pages.
  static constexpr Length kSmallSize = Length(1);
  static constexpr Length kSlack = kPagesPerHugePage - Length(1);
  static constexpr Length kLargeSize = 2 * kPagesPerHugePage - kSlack;

  // large donates kSlack to the filler.
  Span* large = New(kLargeSize, 1);
  Length slack;
  HugeLength donated_huge_pages;
  Length abandoned_pages;

  auto RefreshStats = [&]() {
    absl::base_internal::SpinLockHolder l(&pageheap_lock);
    slack = allocator_->info().slack();
    donated_huge_pages = allocator_->DonatedHugePages();
    abandoned_pages = allocator_->AbandonedPages();
  };
  RefreshStats();

  EXPECT_EQ(slack, kSlack);
  EXPECT_EQ(donated_huge_pages, NHugePages(1));
  EXPECT_EQ(abandoned_pages, Length(0));
  EXPECT_TRUE(large->donated());

  // We should allocate small on the donated page.
  Span* small = New(kSmallSize, 1);
  RefreshStats();
  EXPECT_EQ(slack, kSlack);
  EXPECT_EQ(donated_huge_pages, NHugePages(1));
  EXPECT_EQ(abandoned_pages, Length(0));
  EXPECT_FALSE(small->donated());

  // When we deallocate large, abandoned count should only account for the
  // abandoned pages from the tail huge page.
  Delete(large, 1);
  RefreshStats();
  EXPECT_EQ(slack, Length(0));
  EXPECT_EQ(donated_huge_pages, NHugePages(1));
  EXPECT_EQ(abandoned_pages, Length(1));

  // small is on a donated hugepage. Cleanup.
  Delete(small, 1);
  RefreshStats();
  EXPECT_EQ(slack, Length(0));
  EXPECT_EQ(donated_huge_pages, NHugePages(0));
  EXPECT_EQ(abandoned_pages, Length(0));

  // large donates kSlack to the filler.
  large = New(kLargeSize, 1);
  RefreshStats();
  EXPECT_EQ(slack, kSlack);
  EXPECT_EQ(donated_huge_pages, NHugePages(1));
  EXPECT_EQ(abandoned_pages, Length(0));
  EXPECT_TRUE(large->donated());

  // We should allocate small on the donated page.
  small = New(kSmallSize, 1);
  RefreshStats();
  EXPECT_EQ(slack, kSlack);
  EXPECT_EQ(donated_huge_pages, NHugePages(1));
  EXPECT_EQ(abandoned_pages, Length(0));

  // If we delete small first, abandoned_pages should not tick up.
  Delete(small, 1);
  RefreshStats();
  EXPECT_EQ(slack, kSlack);
  EXPECT_EQ(donated_huge_pages, NHugePages(1));
  EXPECT_EQ(abandoned_pages, Length(0));

  // Deallocating large. Cleanup. All stats should reset to zero.
  Delete(large, 1);
  RefreshStats();
  EXPECT_EQ(slack, Length(0));
  EXPECT_EQ(donated_huge_pages, NHugePages(0));
  EXPECT_EQ(abandoned_pages, Length(0));
}

TEST_P(HugePageAwareAllocatorTest, NotDonated) {
  // A small allocation of size (kHugePageSize/2,kHugePageSize]-bytes can be
  // considered not donated if it filled in a gap on an otherwise mostly free
  // huge page.
  static constexpr Length kSmallSize = Length(1);
  static constexpr Length kLargeSize = kPagesPerHugePage - kSmallSize;

  Span* small = New(kSmallSize, 1);

  Length slack;
  HugeLength donated_huge_pages;
  Length abandoned_pages;

  auto RefreshStats = [&]() {
    absl::base_internal::SpinLockHolder l(&pageheap_lock);
    slack = allocator_->info().slack();
    donated_huge_pages = allocator_->DonatedHugePages();
    abandoned_pages = allocator_->AbandonedPages();
  };
  RefreshStats();

  EXPECT_EQ(slack, Length(0));
  EXPECT_EQ(donated_huge_pages, NHugePages(0));
  EXPECT_EQ(abandoned_pages, Length(0));
  EXPECT_FALSE(small->donated());

  // We should allocate large on the free huge page. That is, this allocation
  // should not cause any donations to filler.
  Span* large = New(kLargeSize, 1);

  RefreshStats();
  // large contributes slack, but isn't donated.
  EXPECT_EQ(slack, kSmallSize);
  EXPECT_EQ(donated_huge_pages, NHugePages(0));
  EXPECT_EQ(abandoned_pages, Length(0));
  EXPECT_FALSE(large->donated());

  Delete(large, 1);
  RefreshStats();
  // large contributes slack, but isn't donated.
  EXPECT_EQ(slack, Length(0));
  EXPECT_EQ(donated_huge_pages, NHugePages(0));
  EXPECT_EQ(abandoned_pages, Length(0));

  // Cleanup.
  Delete(small, 1);
  RefreshStats();
  EXPECT_EQ(slack, Length(0));
  EXPECT_EQ(donated_huge_pages, NHugePages(0));
  EXPECT_EQ(abandoned_pages, Length(0));
}

TEST_P(HugePageAwareAllocatorTest, PageMapInterference) {
  // This test manipulates the test HugePageAwareAllocator while making
  // allocations/deallocations that interact with the real PageAllocator. The
  // two share a global PageMap.
  //
  // If this test begins failing, the two are likely conflicting by violating
  // invariants in the PageMap.
  std::vector<Span*> allocs;

  for (int i : {10, 20, 30}) {
    auto n = Length(i << 7);
    allocs.push_back(New(n, 1));
  }

  for (auto* a : allocs) {
    Delete(a, 1);
  }

  allocs.clear();

  // Do the same, but allocate something on the real page heap.
  for (int i : {10, 20, 30}) {
    auto n = Length(i << 7);
    allocs.push_back(New(n, 1));

    ::operator delete(::operator new(1 << 20));
  }

  for (auto* a : allocs) {
    Delete(a, 1);
  }
}

TEST_P(HugePageAwareAllocatorTest, LargeSmall) {
  const int kIters = 2000;
  const Length kSmallPages = Length(1);
  // Large block must be larger than 1 huge page.
  const Length kLargePages = 2 * kPagesPerHugePage - kSmallPages;
  std::vector<Span*> small_allocs;

  // Repeatedly allocate large and small allocations that fit into a multiple of
  // huge pages.  The large allocations are short lived and the small
  // allocations are long-lived.  We want to refrain from growing the heap size
  // without bound, keeping many huge pages alive because of the small
  // allocations.
  for (int i = 0; i < kIters; i++) {
    Span* large = New(kLargePages, 1);
    ASSERT_NE(large, nullptr);
    Span* small = New(kSmallPages, 1);
    ASSERT_NE(small, nullptr);

    small_allocs.push_back(small);
    Delete(large, 1);
  }

  BackingStats stats;
  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    stats = allocator_->stats();
  }

  constexpr size_t kBufferSize = 1024 * 1024;
  char buffer[kBufferSize];
  Printer printer(buffer, kBufferSize);
  allocator_->Print(&printer);
  // Verify that we have less free memory than we allocated in total. We have
  // to account for bytes tied up in the cache.
  EXPECT_LE(stats.free_bytes - allocator_->cache()->size().in_bytes(),
            kSmallPages.in_bytes() * kIters)
      << buffer;

  for (Span* small : small_allocs) {
    Delete(small, 1);
  }
}

// Tests an edge case in hugepage donation behavior.
TEST_P(HugePageAwareAllocatorTest, DonatedPageLists) {
  const Length kSmallPages = Length(1);
  // Large block must be larger than 1 huge page.
  const Length kLargePages = 2 * kPagesPerHugePage - 2 * kSmallPages;

  Span* large = New(kLargePages, 1);
  ASSERT_NE(large, nullptr);

  // Allocating small1 moves the backing huge page off of the donated pages
  // list.
  Span* small1 = New(kSmallPages, 1);
  ASSERT_NE(small1, nullptr);
  // This delete needs to have put the origin PageTracker back onto the right
  // free list.
  Delete(small1, 1);

  // This otherwise fails.
  Span* small2 = New(kSmallPages, 1);
  ASSERT_NE(small2, nullptr);
  Delete(small2, 1);

  // Clean up.
  Delete(large, 1);
}

TEST_P(HugePageAwareAllocatorTest, DonationAccounting) {
  const Length kSmallPages = Length(2);
  const Length kOneHugePageDonation = kPagesPerHugePage - kSmallPages;
  const Length kMultipleHugePagesDonation = 3 * kPagesPerHugePage - kSmallPages;

  // Each of these allocations should count as one donation, but only if they
  // are actually being reused.
  Span* large = New(kOneHugePageDonation, 1);
  ASSERT_NE(large, nullptr);

  // This allocation ensures that the donation is not counted.
  Span* small = New(kSmallPages, 1);
  ASSERT_NE(small, nullptr);

  Span* large2 = New(kMultipleHugePagesDonation, 1);
  ASSERT_NE(large2, nullptr);

  // This allocation ensures that the donation is not counted.
  Span* small2 = New(kSmallPages, 1);
  ASSERT_NE(small2, nullptr);

  Span* large3 = New(kOneHugePageDonation, 1);
  ASSERT_NE(large3, nullptr);

  Span* large4 = New(kMultipleHugePagesDonation, 1);
  ASSERT_NE(large4, nullptr);

  HugeLength donated;
  // Check donation count.
  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    donated = allocator_->DonatedHugePages();
  }
  EXPECT_EQ(donated, NHugePages(4));

  // Clean up.
  Delete(large, 1);
  Delete(large2, 1);
  Delete(large3, 1);
  Delete(large4, 1);
  Delete(small, 1);
  Delete(small2, 1);

  // Check donation count.
  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    donated = allocator_->DonatedHugePages();
  }
  EXPECT_EQ(donated, NHugePages(0));
}

// We'd like to test OOM behavior but this, err, OOMs. :)
// (Usable manually in controlled environments.
TEST_P(HugePageAwareAllocatorTest, DISABLED_OOM) {
  std::vector<Span*> objs;
  auto n = Length(1);
  while (true) {
    Span* s = New(n, 1);
    if (!s) break;
    objs.push_back(s);
    n *= 2;
  }
  for (auto s : objs) {
    Delete(s, 1);
  }
}

struct MemoryBytes {
  uint64_t virt;
  uint64_t phys;
};

int64_t pagesize = GetPageSize();

static size_t BytesInCore(void* p, size_t len) {
  static const size_t kBufSize = 1024;
  unsigned char buf[kBufSize];
  const size_t kChunk = pagesize * kBufSize;
  size_t resident = 0;
  while (len > 0) {
    // We call mincore in bounded size chunks (though typically one
    // chunk will cover an entire request.)
    const size_t chunk_len = std::min(kChunk, len);
    if (mincore(p, chunk_len, buf) != 0) {
      Crash(kCrash, __FILE__, __LINE__, "mincore failed, errno", errno);
    }
    const size_t lim = chunk_len / pagesize;
    for (size_t i = 0; i < lim; ++i) {
      if (buf[i] & 1) resident += pagesize;
    }
    len -= chunk_len;
    p = static_cast<char*>(p) + chunk_len;
  }

  return resident;
}

// Is any page of this hugepage resident?
bool HugePageResident(HugePage p) {
  return BytesInCore(p.start_addr(), kHugePageSize) > 0;
}

void Touch(PageId p) {
  // a tcmalloc-page may contain more than an actual kernel page
  volatile char* base = reinterpret_cast<char*>(p.start_addr());
  static size_t kActualPages = std::max<size_t>(kPageSize / pagesize, 1);
  for (int i = 0; i < kActualPages; ++i) {
    base[i * pagesize] = 1;
  }
}

// Fault an entire hugepage, as if THP chose to do so on an entirely
// empty hugepage. (In real life, this will usually, but not always,
// happen: we make sure it does so our accounting is accurate.)
void Touch(HugePage hp) {
  PageId p = hp.first_page();
  const PageId lim = p + kPagesPerHugePage;
  while (p < lim) {
    Touch(p);
    ++p;
  }
}

// Fault in memory across a span (SystemBack doesn't always do this.)
void TouchTHP(Span* s) {
  PageId p = s->first_page();
  PageId lim = s->last_page();
  HugePage last = HugePageContaining(nullptr);
  while (p <= lim) {
    HugePage hp = HugePageContaining(p);
    // Suppose that we are touching a hugepage for the first time (it
    // is entirely non-resident.) The page fault we take will usually
    // be promoted to a full transparent hugepage, and our accounting
    // assumes this is true.  But we can't actually guarantee that
    // (the kernel won't wait if memory is too fragmented.)  Do it ourselves
    // by hand, to ensure our mincore() calculations return the right answers.
    if (hp != last && !HugePageResident(hp)) {
      last = hp;
      Touch(hp);
    }

    // Regardless of whether we've optimistically faulted in a
    // hugepage, we also touch each page in the span.
    Touch(p);
    ++p;
  }
}

// Similar to above but much more careful about touching memory / mallocing
// and without the validation
class StatTest : public testing::Test {
 protected:
  StatTest() : rng_() {}

  class RegionFactory;

  class Region : public AddressRegion {
   public:
    Region(AddressRegion* underlying, RegionFactory* factory)
        : underlying_(underlying), factory_(factory) {}

    std::pair<void*, size_t> Alloc(size_t size, size_t alignment) override {
      std::pair<void*, size_t> ret = underlying_->Alloc(size, alignment);
      if (!ret.first) return {nullptr, 0};

      // we only support so many allocations here for simplicity
      CHECK_CONDITION(factory_->n_ < factory_->kNumAllocs);
      // Anything coming from the test allocator will request full
      // alignment.  Metadata allocations will not.  Since we can't
      // control the backing of metadata allocations, elide them.
      // TODO(b/128521238): this is not a good way to do this.
      if (alignment >= kHugePageSize) {
        factory_->allocs_[factory_->n_] = ret;
        factory_->n_++;
      }
      return ret;
    }

   private:
    AddressRegion* underlying_;
    RegionFactory* factory_;
  };

  class RegionFactory : public AddressRegionFactory {
   public:
    explicit RegionFactory(AddressRegionFactory* underlying)
        : underlying_(underlying), n_(0) {}

    AddressRegion* Create(void* start, size_t size, UsageHint hint) override {
      AddressRegion* underlying_region = underlying_->Create(start, size, hint);
      CHECK_CONDITION(underlying_region);
      void* region_space = MallocInternal(sizeof(Region));
      CHECK_CONDITION(region_space);
      return new (region_space) Region(underlying_region, this);
    }

    size_t GetStats(absl::Span<char> buffer) override {
      return underlying_->GetStats(buffer);
    }

    MemoryBytes Memory() {
      MemoryBytes b = {0, 0};
      for (int i = 0; i < n_; ++i) {
        void* p = allocs_[i].first;
        size_t len = allocs_[i].second;
        b.virt += len;
        b.phys += BytesInCore(p, len);
      }

      return b;
    }

    AddressRegionFactory* underlying() const { return underlying_; }

   private:
    friend class Region;
    AddressRegionFactory* underlying_;

    static constexpr size_t kNumAllocs = 1000;
    size_t n_;
    std::pair<void*, size_t> allocs_[kNumAllocs];
  };

  // Carefully get memory usage without touching anything.
  MemoryBytes GetSystemBytes() { return replacement_region_factory_.Memory(); }

  // This is essentially a test case set up, but run manually -
  // we can't guarantee gunit won't malloc between.
  void PrepTest() {
    memset(buf, 0, sizeof(buf));
    MallocExtension::ReleaseMemoryToSystem(std::numeric_limits<size_t>::max());
    SetRegionFactory(&replacement_region_factory_);
    alloc = new (buf) HugePageAwareAllocator(MemoryTag::kNormal);
  }

  ~StatTest() override {
    SetRegionFactory(replacement_region_factory_.underlying());
  }

  BackingStats Stats() {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    BackingStats stats = alloc->stats();
    return stats;
  }

  // Use bigger allocs here to ensure growth:
  Length RandomAllocSize() {
    // Since we touch all of the pages, try to avoid OOM'ing by limiting the
    // number of big allocations.
    const Length kMaxBigAllocs = Length(4096);

    if (big_allocs_ < kMaxBigAllocs && absl::Bernoulli(rng_, 1.0 / 50)) {
      auto n =
          Length(1024 * (1 + absl::LogUniform<int32_t>(rng_, 0, (1 << 9) - 1)));
      n += Length(absl::Uniform<int32_t>(rng_, 0, 1024));
      big_allocs_ += n;
      return n;
    }
    return Length(1 + absl::LogUniform<int32_t>(rng_, 0, (1 << 10) - 1));
  }

  Span* Alloc(Length n) {
    Span* span = alloc->New(n, 1);
    TouchTHP(span);
    if (n > span->num_pages()) {
      Crash(kCrash, __FILE__, __LINE__, n.raw_num(),
            "not <=", span->num_pages().raw_num());
    }
    n = span->num_pages();
    if (n > longest_) longest_ = n;
    total_ += n;
    if (total_ > peak_) peak_ = total_;
    return span;
  }

  void Free(Span* s) {
    Length n = s->num_pages();
    total_ -= n;
    {
      absl::base_internal::SpinLockHolder h(&pageheap_lock);
      alloc->Delete(s, 1);
    }
  }

  void CheckStats() {
    MemoryBytes here = GetSystemBytes();
    BackingStats stats = Stats();
    SmallSpanStats small;
    LargeSpanStats large;
    {
      absl::base_internal::SpinLockHolder h(&pageheap_lock);
      alloc->GetSmallSpanStats(&small);
      alloc->GetLargeSpanStats(&large);
    }

    size_t span_stats_free_bytes = 0, span_stats_released_bytes = 0;
    for (auto i = Length(0); i < kMaxPages; ++i) {
      span_stats_free_bytes += i.in_bytes() * small.normal_length[i.raw_num()];
      span_stats_released_bytes +=
          i.in_bytes() * small.returned_length[i.raw_num()];
    }
    span_stats_free_bytes += large.normal_pages.in_bytes();
    span_stats_released_bytes += large.returned_pages.in_bytes();

    const size_t alloced_bytes = total_.in_bytes();
    ASSERT_EQ(here.virt, stats.system_bytes);
    const size_t actual_unmapped = here.virt - here.phys;
    ASSERT_EQ(actual_unmapped, stats.unmapped_bytes);
    ASSERT_EQ(here.phys, stats.free_bytes + alloced_bytes);
    ASSERT_EQ(alloced_bytes,
              stats.system_bytes - stats.free_bytes - stats.unmapped_bytes);
    ASSERT_EQ(stats.free_bytes, span_stats_free_bytes);
    ASSERT_EQ(stats.unmapped_bytes, span_stats_released_bytes);
  }

  char buf[sizeof(HugePageAwareAllocator)];
  HugePageAwareAllocator* alloc;
  RegionFactory replacement_region_factory_{GetRegionFactory()};
  absl::BitGen rng_;

  Length total_;
  Length longest_;
  Length peak_;
  Length big_allocs_;
};

TEST_F(StatTest, Basic) {
  static const size_t kNumAllocs = 500;
  Span* allocs[kNumAllocs];

  const bool always_check_usage = absl::GetFlag(FLAGS_always_check_usage);

  PrepTest();
  // DO NOT MALLOC ANYTHING BELOW THIS LINE!  WE'RE TRYING TO CAREFULLY COUNT
  // ALLOCATIONS.
  // (note we can't stop background threads, but hopefully they're idle enough.)

  for (int i = 0; i < kNumAllocs; ++i) {
    Length k = RandomAllocSize();
    allocs[i] = Alloc(k);
    // stats are expensive, don't always check
    if (i % 10 != 0 && !always_check_usage) continue;
    CheckStats();
  }

  static const size_t kReps = 1000;
  for (int i = 0; i < kReps; ++i) {
    size_t index = absl::Uniform<int32_t>(rng_, 0, kNumAllocs);

    Free(allocs[index]);
    Length k = RandomAllocSize();
    allocs[index] = Alloc(k);

    if (absl::Bernoulli(rng_, 1.0 / 3)) {
      Length pages(absl::LogUniform<int32_t>(rng_, 0, (1 << 10) - 1) + 1);
      absl::base_internal::SpinLockHolder h(&pageheap_lock);
      alloc->ReleaseAtLeastNPages(pages);
    }

    // stats are expensive, don't always check
    if (i % 10 != 0 && !always_check_usage) continue;
    CheckStats();
  }

  for (int i = 0; i < kNumAllocs; ++i) {
    Free(allocs[i]);
    if (i % 10 != 0 && !always_check_usage) continue;
    CheckStats();
  }

  {
    CheckStats();
    pageheap_lock.Lock();
    auto final_stats = alloc->stats();
    pageheap_lock.Unlock();
    ASSERT_EQ(final_stats.free_bytes + final_stats.unmapped_bytes,
              final_stats.system_bytes);
  }

  // test over, malloc all you like
}

TEST_P(HugePageAwareAllocatorTest, ParallelRelease) {
  ThreadManager threads;
  constexpr int kThreads = 10;

  struct ABSL_CACHELINE_ALIGNED Metadata {
    absl::BitGen rng;
    std::vector<Span*> spans;
  };

  std::vector<Metadata> metadata;
  metadata.resize(kThreads);

  threads.Start(kThreads, [&](int thread_id) {
    Metadata& m = metadata[thread_id];

    if (thread_id == 0) {
      ReleasePages(Length(absl::Uniform(m.rng, 1, 1 << 10)));
      return;
    } else if (thread_id == 1) {
      benchmark::DoNotOptimize(Print());
      return;
    }

    if (absl::Bernoulli(m.rng, 0.6) || m.spans.empty()) {
      Span* s = AllocatorNew(Length(absl::LogUniform(m.rng, 1, 1 << 10)), 1);
      CHECK_CONDITION(s != nullptr);

      // Touch the contents of the buffer.  We later use it to verify we are the
      // only thread manipulating the Span, for example, if another thread
      // madvise DONTNEED'd the contents and zero'd them.
      const uintptr_t key = reinterpret_cast<uintptr_t>(s) ^ thread_id;
      *reinterpret_cast<uintptr_t*>(s->start_address()) = key;

      m.spans.push_back(s);
    } else {
      size_t index = absl::Uniform<size_t>(m.rng, 0, m.spans.size());

      Span* back = m.spans.back();
      Span* s = m.spans[index];
      m.spans[index] = back;
      m.spans.pop_back();

      const uintptr_t key = reinterpret_cast<uintptr_t>(s) ^ thread_id;
      EXPECT_EQ(*reinterpret_cast<uintptr_t*>(s->start_address()), key);

      AllocatorDelete(s, 1);
    }
  });

  absl::SleepFor(absl::Seconds(1));

  threads.Stop();

  for (auto& m : metadata) {
    for (Span* s : m.spans) {
      AllocatorDelete(s, 1);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    All, HugePageAwareAllocatorTest,
    testing::Values(HugeRegionCountOption::kSlack,
                    HugeRegionCountOption::kAbandonedCount));

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
