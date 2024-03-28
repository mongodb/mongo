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

#ifndef TCMALLOC_HUGE_REGION_H_
#define TCMALLOC_HUGE_REGION_H_
#include <stddef.h>
#include <stdint.h>

#include <algorithm>

#include "absl/base/attributes.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/base/optimization.h"
#include "absl/time/time.h"
#include "tcmalloc/huge_cache.h"
#include "tcmalloc/huge_page_subrelease.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/range_tracker.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

enum class HugeRegionUsageOption : bool {
  // This is a default behavior. We use slack to determine when to use
  // HugeRegion. When slack is greater than 64MB (to ignore small binaries), and
  // greater than the number of small allocations, we allocate large allocations
  // from HugeRegion.
  kDefault,
  // When the experiment TEST_ONLY_TCMALLOC_USE_HUGE_REGIONS_MORE_OFTEN is
  // enabled, we use number of abandoned pages in addition to slack to make a
  // decision. If the size of abandoned pages plus slack exceeds 64MB (to ignore
  // small binaries), we use HugeRegion for large allocations.
  kUseForAllLargeAllocs
};

// Track allocations from a fixed-size multiple huge page region.
// Similar to PageTracker but a few important differences:
// - crosses multiple hugepages
// - backs region on demand
// - supports breaking up the partially-allocated region for use elsewhere
//
// This is intended to help with fast allocation of regions too large
// for HugePageFiller, but too small to round to a full hugepage; both
// lengths that do fit in a hugepage, but often wouldn't fit in
// available gaps (1.75 MiB), and lengths that don't fit, but would
// introduce unacceptable fragmentation (2.1 MiB).
//
class HugeRegion : public TList<HugeRegion>::Elem {
 public:
  // We could template this if there was any need.
  static constexpr HugeLength kRegionSize = HLFromBytes(1024 * 1024 * 1024);
  static constexpr size_t kNumHugePages = kRegionSize.raw_num();
  static constexpr HugeLength size() { return kRegionSize; }

  // REQUIRES: r.len() == size(); r unbacked.
  HugeRegion(HugeRange r,
             MemoryModifyFunction& unback ABSL_ATTRIBUTE_LIFETIME_BOUND);
  HugeRegion() = delete;

  // If available, return a range of n free pages, setting *from_released =
  // true iff the returned range is currently unbacked.
  // Returns false if no range available.
  bool MaybeGet(Length n, PageId* p, bool* from_released);

  // Return [p, p + n) for new allocations.
  // If release=true, release any hugepages made empty as a result.
  // REQUIRES: [p, p + n) was the result of a previous MaybeGet.
  void Put(PageId p, Length n, bool release);

  // Release <desired> numbae of pages from free-and-backed hugepages from
  // region.
  HugeLength Release(Length desired);

  // Is p located in this region?
  bool contains(PageId p) { return location_.contains(p); }

  // Stats
  Length used_pages() const { return Length(tracker_.used()); }
  Length free_pages() const {
    return size().in_pages() - unmapped_pages() - used_pages();
  }
  Length unmapped_pages() const { return (size() - nbacked_).in_pages(); }

  void AddSpanStats(SmallSpanStats* small, LargeSpanStats* large) const;

  HugeLength backed() const;

  // Returns the number of hugepages that have been fully free (i.e. no
  // allocated pages on them), but are backed. We release hugepages lazily when
  // huge-regions-more-often feature is enabled.
  HugeLength free_backed() const;

  void Print(Printer* out) const;
  void PrintInPbtxt(PbtxtRegion* detail) const;

  BackingStats stats() const;

  // We don't define this as operator< because it's a rather specialized order.
  bool BetterToAllocThan(const HugeRegion* rhs) const {
    return longest_free() < rhs->longest_free();
  }

  void prepend_it(HugeRegion* other) { this->prepend(other); }

  void append_it(HugeRegion* other) { this->append(other); }

 private:
  RangeTracker<kRegionSize.in_pages().raw_num()> tracker_;

  HugeRange location_;

  static int64_t AverageWhens(Length a, int64_t a_when, Length b,
                              int64_t b_when) {
    const double aw = static_cast<double>(a.raw_num()) * a_when;
    const double bw = static_cast<double>(b.raw_num()) * b_when;
    return static_cast<int64_t>((aw + bw) / (a.raw_num() + b.raw_num()));
  }

  Length longest_free() const { return Length(tracker_.longest_free()); }

  // Adjust counts of allocs-per-hugepage for [p, p + n) being added/removed.

  // *from_released is set to true iff [p, p + n) is currently unbacked
  void Inc(PageId p, Length n, bool* from_released);
  // If release is true, unback any hugepage that becomes empty.
  void Dec(PageId p, Length n, bool release);

  HugeLength UnbackHugepages(bool should_unback[kNumHugePages]);

  // How many pages are used in each hugepage?
  Length pages_used_[kNumHugePages];
  // Is this hugepage backed?
  bool backed_[kNumHugePages];
  HugeLength nbacked_;
  int64_t last_touched_[kNumHugePages];
  HugeLength total_unbacked_{NHugePages(0)};

  MemoryModifyFunction& unback_;
};

// Manage a set of regions from which we allocate.
// Strategy: Allocate from the most fragmented region that fits.
template <typename Region>
class HugeRegionSet {
 public:
  // For testing with mock clock.
  HugeRegionSet(HugeRegionUsageOption use_huge_region_more_often, Clock clock)
      : n_(0),
        use_huge_region_more_often_(use_huge_region_more_often),
        regionstats_tracker_(clock, absl::Minutes(10), absl::Minutes(5)) {}

  explicit HugeRegionSet(HugeRegionUsageOption use_huge_region_more_often)
      : HugeRegionSet(
            use_huge_region_more_often,
            Clock{.now = absl::base_internal::CycleClock::Now,
                  .freq = absl::base_internal::CycleClock::Frequency}) {}

  // If available, return a range of n free pages, setting *from_released =
  // true iff the returned range is currently unbacked.
  // Returns false if no range available.
  bool MaybeGet(Length n, PageId* page, bool* from_released);

  // Return an allocation to a region (if one matches!)
  bool MaybePut(PageId p, Length n);

  // Add region to the set.
  void Contribute(Region* region);

  // Tries to release up to <desired> number of pages from fully-free but backed
  // hugepages in HugeRegions. <intervals> defines the skip-subrelease
  // intervals, but unlike HugePageFiller skip-subrelease, it only releases free
  // hugepages.
  // Releases all free and backed hugepages to system when <hit_limit> is set to
  // true. Else, it uses intervals to determine recent demand as seen by
  // HugeRegions to compute realized fragmentation. It may only release as much
  // memory in free pages as determined by the realized fragmentation.
  // Returns the number of pages actually released.
  Length ReleasePagesByPeakDemand(Length desired,
                                  SkipSubreleaseIntervals intervals,
                                  bool hit_limit);

  // Release hugepages that are unused but backed.
  // Releases up to <release_fraction> times number of free-but-backed hugepages
  // from each huge region. Note that this clamps release_fraction between 0 and
  // 1 if a fraction outside those bounds is specified.
  Length ReleasePages(double release_fraction);

  void Print(Printer* out) const;
  void PrintInPbtxt(PbtxtRegion* hpaa) const;
  void AddSpanStats(SmallSpanStats* small, LargeSpanStats* large) const;
  BackingStats stats() const;
  HugeLength free_backed() const;
  size_t ActiveRegions() const;
  bool UseHugeRegionMoreOften() const {
    return use_huge_region_more_often_ ==
           HugeRegionUsageOption::kUseForAllLargeAllocs;
  }

 private:
  void Fix(Region* r) {
    // We've changed r's fragmentation--move it through the list to the
    // correct home (if needed).
    Rise(r);
    Fall(r);
  }

  // Check if r has to move forward in the list.
  void Rise(Region* r) {
    auto prev = list_.at(r);
    --prev;
    if (prev == list_.end()) return;           // we're at the front
    if (!r->BetterToAllocThan(*prev)) return;  // we're far enough forward
    list_.remove(r);
    for (auto iter = prev; iter != list_.end(); --iter) {
      if (!r->BetterToAllocThan(*iter)) {
        iter->append_it(r);
        return;
      }
    }
    list_.prepend(r);
  }

  // Check if r has to move backward in the list.
  void Fall(Region* r) {
    auto next = list_.at(r);
    ++next;
    if (next == list_.end()) return;          // we're at the back
    if (!next->BetterToAllocThan(r)) return;  // we're far enough back
    list_.remove(r);
    for (auto iter = next; iter != list_.end(); ++iter) {
      if (!iter->BetterToAllocThan(r)) {
        iter->prepend_it(r);
        return;
      }
    }
    list_.append(r);
  }

  // Add r in its sorted place.
  void AddToList(Region* r) {
    for (Region* curr : list_) {
      if (r->BetterToAllocThan(curr)) {
        curr->prepend_it(r);
        return;
      }
    }

    // Note this handles the empty-list case
    list_.append(r);
  }

  using StatsTrackerType = SubreleaseStatsTracker<600>;
  StatsTrackerType::SubreleaseStats GetSubreleaseStats() const {
    StatsTrackerType::SubreleaseStats stats;
    for (Region* region : list_) {
      stats.num_pages += region->used_pages();
      stats.free_pages += region->free_pages();
      stats.unmapped_pages += region->unmapped_pages();
      stats.huge_pages[StatsTrackerType::kRegular] += region->size();
    }
    stats.num_pages_subreleased = subrelease_stats_.num_pages_subreleased;
    return stats;
  }

  Length used_pages() const {
    Length used;
    for (Region* region : list_) {
      used += region->used_pages();
    }
    return used;
  }

  Length free_pages() const {
    Length free;
    for (Region* region : list_) {
      free += region->free_pages();
    }
    return free;
  }

  HugeLength size() const {
    HugeLength size;
    for (Region* region : list_) {
      size += region->size();
    }
    return size;
  }

  size_t n_;
  HugeRegionUsageOption use_huge_region_more_often_;
  // Sorted by longest_free increasing.
  TList<Region> list_;

  // Computes the recent demand to compute the number of pages that may be
  // released. <desired> determines an upper-bound on the number of pages to
  // release.
  // Returns number of pages that may be released based on recent demand.
  Length GetDesiredReleasablePages(Length desired,
                                   SkipSubreleaseIntervals intervals);

  // Functionality related to tracking demand.
  void UpdateStatsTracker();
  StatsTrackerType regionstats_tracker_;
  SubreleaseStats subrelease_stats_;
};

// REQUIRES: r.len() == size(); r unbacked.
inline HugeRegion::HugeRegion(HugeRange r, MemoryModifyFunction& unback)
    : tracker_{},
      location_(r),
      pages_used_{},
      backed_{},
      nbacked_(NHugePages(0)),
      unback_(unback) {
  int64_t now = absl::base_internal::CycleClock::Now();
  for (int i = 0; i < kNumHugePages; ++i) {
    last_touched_[i] = now;
    // These are already 0 but for clarity...
    pages_used_[i] = Length(0);
    backed_[i] = false;
  }
}

inline bool HugeRegion::MaybeGet(Length n, PageId* p, bool* from_released) {
  if (n > longest_free()) return false;
  auto index = Length(tracker_.FindAndMark(n.raw_num()));

  PageId page = location_.start().first_page() + index;
  *p = page;

  // the last hugepage we touch
  Inc(page, n, from_released);
  return true;
}

// If release=true, release any hugepages made empty as a result.
inline void HugeRegion::Put(PageId p, Length n, bool release) {
  Length index = p - location_.start().first_page();
  tracker_.Unmark(index.raw_num(), n.raw_num());

  Dec(p, n, release);
}

// Release hugepages that are unused but backed.
// TODO(b/199203282): We release up to <desired> pages from free but backed
// hugepages from the region. We can explore a more sophisticated mechanism
// similar to Filler/Cache, that accounts for a recent peak while releasing
// pages.
inline HugeLength HugeRegion::Release(Length desired) {
  const Length free_yet_backed = free_backed().in_pages();
  const Length to_release = std::min(desired, free_yet_backed);

  HugeLength release_target = NHugePages(0);
  bool should_unback[kNumHugePages] = {};
  for (size_t i = 0; i < kNumHugePages; ++i) {
    if (backed_[i] && pages_used_[i] == Length(0)) {
      should_unback[i] = true;
      ++release_target;
    }

    if (release_target.in_pages() >= to_release) break;
  }
  return UnbackHugepages(should_unback);
}

inline void HugeRegion::AddSpanStats(SmallSpanStats* small,
                                     LargeSpanStats* large) const {
  size_t index = 0, n;
  Length f, u;
  // This is complicated a bit by the backed/unbacked status of pages.
  while (tracker_.NextFreeRange(index, &index, &n)) {
    // [index, index + n) is an *unused* range.  As it may cross
    // hugepages, we may need to truncate it so it is either a
    // *free* or a *released* range, and compute a reasonable value
    // for its "when".
    PageId p = location_.start().first_page() + Length(index);
    const HugePage hp = HugePageContaining(p);
    size_t i = (hp - location_.start()) / NHugePages(1);
    const bool backed = backed_[i];
    Length truncated;
    int64_t when = 0;
    while (n > 0 && backed_[i] == backed) {
      const PageId lim = (location_.start() + NHugePages(i + 1)).first_page();
      Length here = std::min(Length(n), lim - p);
      when = AverageWhens(truncated, when, here, last_touched_[i]);
      truncated += here;
      n -= here.raw_num();
      p += here;
      i++;
      TC_ASSERT(i < kNumHugePages || n == 0);
    }
    n = truncated.raw_num();
    const bool released = !backed;
    if (released) {
      u += Length(n);
    } else {
      f += Length(n);
    }
    if (Length(n) < kMaxPages) {
      if (small != nullptr) {
        if (released) {
          small->returned_length[n]++;
        } else {
          small->normal_length[n]++;
        }
      }
    } else {
      if (large != nullptr) {
        large->spans++;
        if (released) {
          large->returned_pages += Length(n);
        } else {
          large->normal_pages += Length(n);
        }
      }
    }

    index += n;
  }
  TC_CHECK_EQ(f, free_pages());
  TC_CHECK_EQ(u, unmapped_pages());
}

inline HugeLength HugeRegion::free_backed() const {
  HugeLength r = NHugePages(0);
  for (size_t i = 0; i < kNumHugePages; ++i) {
    if (backed_[i] && pages_used_[i] == Length(0)) {
      ++r;
    }
  }
  return r;
}

inline HugeLength HugeRegion::backed() const {
  HugeLength b;
  for (int i = 0; i < kNumHugePages; ++i) {
    if (backed_[i]) {
      ++b;
    }
  }

  return b;
}

inline void HugeRegion::Print(Printer* out) const {
  const size_t kib_used = used_pages().in_bytes() / 1024;
  const size_t kib_free = free_pages().in_bytes() / 1024;
  const size_t kib_longest_free = longest_free().in_bytes() / 1024;
  const HugeLength unbacked = size() - backed();
  const size_t mib_unbacked = unbacked.in_mib();
  out->printf(
      "HugeRegion: %zu KiB used, %zu KiB free, "
      "%zu KiB contiguous space, %zu MiB unbacked, "
      "%zu MiB unbacked lifetime\n",
      kib_used, kib_free, kib_longest_free, mib_unbacked,
      total_unbacked_.in_bytes() / 1024 / 1024);
}

inline void HugeRegion::PrintInPbtxt(PbtxtRegion* detail) const {
  detail->PrintI64("used_bytes", used_pages().in_bytes());
  detail->PrintI64("free_bytes", free_pages().in_bytes());
  detail->PrintI64("longest_free_range_bytes", longest_free().in_bytes());
  const HugeLength unbacked = size() - backed();
  detail->PrintI64("unbacked_bytes", unbacked.in_bytes());
  detail->PrintI64("total_unbacked_bytes", total_unbacked_.in_bytes());
  detail->PrintI64("backed_fully_free_bytes", free_backed().in_bytes());
}

inline BackingStats HugeRegion::stats() const {
  BackingStats s;
  s.system_bytes = location_.len().in_bytes();
  s.free_bytes = free_pages().in_bytes();
  s.unmapped_bytes = unmapped_pages().in_bytes();
  return s;
}

inline void HugeRegion::Inc(PageId p, Length n, bool* from_released) {
  bool should_back = false;
  const int64_t now = absl::base_internal::CycleClock::Now();
  while (n > Length(0)) {
    const HugePage hp = HugePageContaining(p);
    const size_t i = (hp - location_.start()) / NHugePages(1);
    const PageId lim = (hp + NHugePages(1)).first_page();
    Length here = std::min(n, lim - p);
    if (pages_used_[i] == Length(0) && !backed_[i]) {
      backed_[i] = true;
      should_back = true;
      ++nbacked_;
      last_touched_[i] = now;
    }
    pages_used_[i] += here;
    TC_ASSERT_LE(pages_used_[i], kPagesPerHugePage);
    p += here;
    n -= here;
  }
  *from_released = should_back;
}

inline void HugeRegion::Dec(PageId p, Length n, bool release) {
  const int64_t now = absl::base_internal::CycleClock::Now();
  bool should_unback[kNumHugePages] = {};
  while (n > Length(0)) {
    const HugePage hp = HugePageContaining(p);
    const size_t i = (hp - location_.start()) / NHugePages(1);
    const PageId lim = (hp + NHugePages(1)).first_page();
    Length here = std::min(n, lim - p);
    TC_ASSERT_GT(here, Length(0));
    TC_ASSERT_GE(pages_used_[i], here);
    TC_ASSERT(backed_[i]);
    last_touched_[i] = AverageWhens(
        here, now, kPagesPerHugePage - pages_used_[i], last_touched_[i]);
    pages_used_[i] -= here;
    if (pages_used_[i] == Length(0)) {
      should_unback[i] = true;
    }
    p += here;
    n -= here;
  }
  if (release) {
    UnbackHugepages(should_unback);
  }
}

inline HugeLength HugeRegion::UnbackHugepages(
    bool should_unback[kNumHugePages]) {
  const int64_t now = absl::base_internal::CycleClock::Now();
  HugeLength released = NHugePages(0);
  size_t i = 0;
  while (i < kNumHugePages) {
    if (!should_unback[i]) {
      i++;
      continue;
    }
    size_t j = i;
    while (j < kNumHugePages && should_unback[j]) {
      j++;
    }

    HugeLength hl = NHugePages(j - i);
    HugePage p = location_.start() + NHugePages(i);
    if (ABSL_PREDICT_TRUE(unback_(p.start_addr(), hl.in_bytes()))) {
      nbacked_ -= hl;
      total_unbacked_ += hl;

      for (size_t k = i; k < j; k++) {
        TC_ASSERT(should_unback[k]);
        backed_[k] = false;
        last_touched_[k] = now;
      }

      released += hl;
    }
    i = j;
  }

  return released;
}

template <typename Region>
inline Length HugeRegionSet<Region>::GetDesiredReleasablePages(
    Length desired, SkipSubreleaseIntervals intervals) {
  if (!intervals.SkipSubreleaseEnabled()) {
    return desired;
  }
  UpdateStatsTracker();

  Length required_pages;
  // There are two ways to calculate the demand requirement. We give priority to
  // using the peak if peak_interval is set.
  if (intervals.IsPeakIntervalSet()) {
    required_pages =
        regionstats_tracker_.GetRecentPeak(intervals.peak_interval);
  } else {
    required_pages = regionstats_tracker_.GetRecentDemand(
        intervals.short_interval, intervals.long_interval);
  }

  Length current_pages = used_pages() + free_pages();

  if (required_pages != Length(0)) {
    Length new_desired;
    if (required_pages < current_pages) {
      new_desired = current_pages - required_pages;
    }

    // Because we currently release pages from fully backed and free hugepages,
    // make sure that the realized fragmentation in HugeRegion is at least equal
    // to kPagesPerHugePage. Otherwise, return zero to make sure we do not
    // release any pages.
    if (new_desired < kPagesPerHugePage) {
      new_desired = Length(0);
    }

    if (new_desired >= desired) {
      return desired;
    }

    // Compute the number of releasable pages from HugeRegion. We do not
    // subrelease pages yet. Instead, we only release hugepages that are fully
    // free but backed. Note: the remaining target should always be smaller or
    // equal to the number of free pages according to the mechanism (recent peak
    // is always larger or equal to current used_pages), however, we still
    // calculate allowed release using the minimum of the two to avoid relying
    // on that assumption.
    Length free_backed_pages = free_backed().in_pages();
    Length releasable_pages = std::min(free_backed_pages, new_desired);

    // Reports the amount of memory that we didn't release due to this
    // mechanism, but never more than skipped free pages. In other words,
    // skipped_pages is zero if all free pages are allowed to be released by
    // this mechanism. Note, only free pages in the smaller of the two
    // (current_pages and required_pages) are skipped, the rest are allowed to
    // be subreleased.
    Length skipped_pages = std::min((free_backed_pages - releasable_pages),
                                    (desired - new_desired));

    regionstats_tracker_.ReportSkippedSubreleasePages(
        skipped_pages, std::min(current_pages, required_pages));
    return new_desired;
  }

  return desired;
}

template <typename Region>
inline void HugeRegionSet<Region>::UpdateStatsTracker() {
  regionstats_tracker_.Report(GetSubreleaseStats());
  subrelease_stats_.reset();
}

// If available, return a range of n free pages, setting *from_released =
// true iff the returned range is currently unbacked.
// Returns false if no range available.
template <typename Region>
inline bool HugeRegionSet<Region>::MaybeGet(Length n, PageId* page,
                                            bool* from_released) {
  for (Region* region : list_) {
    if (region->MaybeGet(n, page, from_released)) {
      Fix(region);
      UpdateStatsTracker();
      return true;
    }
  }
  return false;
}

// Return an allocation to a region (if one matches!)
template <typename Region>
inline bool HugeRegionSet<Region>::MaybePut(PageId p, Length n) {
  // When HugeRegionMoreOften experiment is enabled, we do not release
  // free-but-backed hugepages when we deallocate pages, but we do that
  // periodically on the background thread.
  const bool release = !UseHugeRegionMoreOften();
  for (Region* region : list_) {
    if (region->contains(p)) {
      region->Put(p, n, release);
      Fix(region);
      UpdateStatsTracker();
      return true;
    }
  }

  return false;
}

// Add region to the set.
template <typename Region>
inline void HugeRegionSet<Region>::Contribute(Region* region) {
  n_++;
  AddToList(region);
  UpdateStatsTracker();
}

template <typename Region>
inline Length HugeRegionSet<Region>::ReleasePagesByPeakDemand(
    Length desired, SkipSubreleaseIntervals intervals, bool hit_limit) {
  // Only reduce desired if skip subrelease is on.
  //
  // Additionally, if we hit the limit, we should not be applying skip
  // subrelease.  OOM may be imminent.
  if (intervals.SkipSubreleaseEnabled() && !hit_limit) {
    desired = GetDesiredReleasablePages(desired, intervals);
  }

  subrelease_stats_.set_limit_hit(hit_limit);

  Length released;
  if (desired != Length(0)) {
    for (Region* region : list_) {
      released += region->Release(desired - released).in_pages();
      if (released >= desired) break;
    }
  }

  subrelease_stats_.num_pages_subreleased += released;

  // Keep separate stats if the on going release is triggered by reaching
  // tcmalloc limit.
  if (subrelease_stats_.limit_hit()) {
    subrelease_stats_.total_pages_subreleased_due_to_limit += released;
  }

  return released;
}

template <typename Region>
inline Length HugeRegionSet<Region>::ReleasePages(double release_fraction) {
  const Length free_yet_backed = free_backed().in_pages();
  const size_t to_release =
      free_yet_backed.raw_num() * std::clamp<double>(release_fraction, 0, 1);
  const Length to_release_pages = Length(to_release);

  Length released;
  for (Region* region : list_) {
    released += region->Release(to_release_pages - released).in_pages();
    if (released >= to_release_pages) return released;
  }
  return released;
}

template <typename Region>
inline void HugeRegionSet<Region>::Print(Printer* out) const {
  out->printf("HugeRegionSet: 1 MiB+ allocations best-fit into %zu MiB slabs\n",
              Region::size().in_bytes() / 1024 / 1024);
  out->printf("HugeRegionSet: %zu total regions\n", n_);
  Length total_free;
  HugeLength total_backed = NHugePages(0);
  HugeLength total_free_backed = NHugePages(0);

  for (Region* region : list_) {
    region->Print(out);
    total_free += region->free_pages();
    total_backed += region->backed();
    total_free_backed += region->free_backed();
  }

  out->printf(
      "HugeRegionSet: %zu hugepages backed, %zu backed and free, "
      "out of %zu total\n",
      total_backed.raw_num(), total_free_backed.raw_num(),
      Region::size().raw_num() * n_);

  const Length in_pages = total_backed.in_pages();
  out->printf("HugeRegionSet: %zu pages free in backed region, %.4f free\n",
              total_free.raw_num(),
              in_pages > Length(0) ? static_cast<double>(total_free.raw_num()) /
                                         static_cast<double>(in_pages.raw_num())
                                   : 0.0);

  // Subrelease telemetry.
  out->printf(
      "HugeRegion: Since startup, %zu pages subreleased, %zu hugepages "
      "broken, (%zu pages, %zu hugepages due to reaching tcmalloc limit)\n",
      subrelease_stats_.total_pages_subreleased.raw_num(),
      subrelease_stats_.total_hugepages_broken.raw_num(),
      subrelease_stats_.total_pages_subreleased_due_to_limit.raw_num(),
      subrelease_stats_.total_hugepages_broken_due_to_limit.raw_num());

  regionstats_tracker_.Print(out, "HugeRegion");
}

template <typename Region>
inline void HugeRegionSet<Region>::PrintInPbtxt(PbtxtRegion* hpaa) const {
  hpaa->PrintI64("min_huge_region_alloc_size", 1024 * 1024);
  hpaa->PrintI64("huge_region_size", Region::size().in_bytes());
  for (Region* region : list_) {
    auto detail = hpaa->CreateSubRegion("huge_region_details");
    region->PrintInPbtxt(&detail);
  }

  hpaa->PrintI64("region_num_pages_subreleased",
                 subrelease_stats_.total_pages_subreleased.raw_num());
  hpaa->PrintI64(
      "region_num_pages_subreleased_due_to_limit",
      subrelease_stats_.total_pages_subreleased_due_to_limit.raw_num());

  regionstats_tracker_.PrintSubreleaseStatsInPbtxt(hpaa,
                                                   "region_skipped_subrelease");
  regionstats_tracker_.PrintTimeseriesStatsInPbtxt(hpaa,
                                                   "region_stats_timeseries");
}

template <typename Region>
inline void HugeRegionSet<Region>::AddSpanStats(SmallSpanStats* small,
                                                LargeSpanStats* large) const {
  for (Region* region : list_) {
    region->AddSpanStats(small, large);
  }
}

template <typename Region>
inline size_t HugeRegionSet<Region>::ActiveRegions() const {
  return n_;
}

template <typename Region>
inline BackingStats HugeRegionSet<Region>::stats() const {
  BackingStats stats;
  for (Region* region : list_) {
    stats += region->stats();
  }

  return stats;
}

template <typename Region>
inline HugeLength HugeRegionSet<Region>::free_backed() const {
  HugeLength pages;
  for (Region* region : list_) {
    pages += region->free_backed();
  }

  return pages;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_HUGE_REGION_H_
