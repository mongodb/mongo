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

#include "absl/base/internal/cycleclock.h"
#include "tcmalloc/huge_allocator.h"
#include "tcmalloc/huge_page_filler.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

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
  HugeRegion(HugeRange r, MemoryModifyFunction unback);
  HugeRegion() = delete;

  // If available, return a range of n free pages, setting *from_released =
  // true iff the returned range is currently unbacked.
  // Returns false if no range available.
  bool MaybeGet(Length n, PageId* p, bool* from_released);

  // Return [p, p + n) for new allocations.
  // If release=true, release any hugepages made empty as a result.
  // REQUIRES: [p, p + n) was the result of a previous MaybeGet.
  void Put(PageId p, Length n, bool release);

  // Release any hugepages that are unused but backed.
  HugeLength Release();

  // Is p located in this region?
  bool contains(PageId p) { return location_.contains(p); }

  // Stats
  Length used_pages() const { return Length(tracker_.used()); }
  Length free_pages() const {
    return size().in_pages() - unmapped_pages() - used_pages();
  }
  Length unmapped_pages() const { return (size() - nbacked_).in_pages(); }

  void AddSpanStats(SmallSpanStats* small, LargeSpanStats* large,
                    PageAgeHistograms* ages) const;

  HugeLength backed() const;

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

  void UnbackHugepages(bool should_unback[kNumHugePages]);

  // How many pages are used in each hugepage?
  Length pages_used_[kNumHugePages];
  // Is this hugepage backed?
  bool backed_[kNumHugePages];
  HugeLength nbacked_;
  int64_t last_touched_[kNumHugePages];
  HugeLength total_unbacked_{NHugePages(0)};

  MemoryModifyFunction unback_;
};

// Manage a set of regions from which we allocate.
// Strategy: Allocate from the most fragmented region that fits.
template <typename Region>
class HugeRegionSet {
 public:
  HugeRegionSet() : n_(0) {}

  // If available, return a range of n free pages, setting *from_released =
  // true iff the returned range is currently unbacked.
  // Returns false if no range available.
  bool MaybeGet(Length n, PageId* page, bool* from_released);

  // Return an allocation to a region (if one matches!)
  bool MaybePut(PageId p, Length n);

  // Add region to the set.
  void Contribute(Region* region);

  void Print(Printer* out) const;
  void PrintInPbtxt(PbtxtRegion* hpaa) const;
  void AddSpanStats(SmallSpanStats* small, LargeSpanStats* large,
                    PageAgeHistograms* ages) const;
  BackingStats stats() const;
  size_t ActiveRegions() const;

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

  size_t n_;
  // Sorted by longest_free increasing.
  TList<Region> list_;
};

// REQUIRES: r.len() == size(); r unbacked.
inline HugeRegion::HugeRegion(HugeRange r, MemoryModifyFunction unback)
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

// Release any hugepages that are unused but backed.
inline HugeLength HugeRegion::Release() {
  HugeLength r = NHugePages(0);
  bool should_unback_[kNumHugePages] = {};
  for (size_t i = 0; i < kNumHugePages; ++i) {
    if (backed_[i] && pages_used_[i] == Length(0)) {
      should_unback_[i] = true;
      ++r;
    }
  }
  UnbackHugepages(should_unback_);
  return r;
}

inline void HugeRegion::AddSpanStats(SmallSpanStats* small,
                                     LargeSpanStats* large,
                                     PageAgeHistograms* ages) const {
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
      ASSERT(i < kNumHugePages || n == 0);
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

    if (ages != nullptr) {
      ages->RecordRange(Length(n), released, when);
    }
    index += n;
  }
  CHECK_CONDITION(f == free_pages());
  CHECK_CONDITION(u == unmapped_pages());
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
    ASSERT(pages_used_[i] <= kPagesPerHugePage);
    p += here;
    n -= here;
  }
  *from_released = should_back;
}

inline void HugeRegion::Dec(PageId p, Length n, bool release) {
  const int64_t now = absl::base_internal::CycleClock::Now();
  bool should_unback_[kNumHugePages] = {};
  while (n > Length(0)) {
    const HugePage hp = HugePageContaining(p);
    const size_t i = (hp - location_.start()) / NHugePages(1);
    const PageId lim = (hp + NHugePages(1)).first_page();
    Length here = std::min(n, lim - p);
    ASSERT(here > Length(0));
    ASSERT(pages_used_[i] >= here);
    ASSERT(backed_[i]);
    last_touched_[i] = AverageWhens(
        here, now, kPagesPerHugePage - pages_used_[i], last_touched_[i]);
    pages_used_[i] -= here;
    if (pages_used_[i] == Length(0)) {
      should_unback_[i] = true;
    }
    p += here;
    n -= here;
  }
  if (release) {
    UnbackHugepages(should_unback_);
  }
}

inline void HugeRegion::UnbackHugepages(bool should_unback[kNumHugePages]) {
  const int64_t now = absl::base_internal::CycleClock::Now();
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
        ASSERT(should_unback[k]);
        backed_[k] = false;
        last_touched_[k] = now;
      }
    }
    i = j;
  }
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
      return true;
    }
  }
  return false;
}

// Return an allocation to a region (if one matches!)
template <typename Region>
inline bool HugeRegionSet<Region>::MaybePut(PageId p, Length n) {
  for (Region* region : list_) {
    if (region->contains(p)) {
      region->Put(p, n, true);
      Fix(region);
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
}

template <typename Region>
inline void HugeRegionSet<Region>::Print(Printer* out) const {
  out->printf("HugeRegionSet: 1 MiB+ allocations best-fit into %zu MiB slabs\n",
              Region::size().in_bytes() / 1024 / 1024);
  out->printf("HugeRegionSet: %zu total regions\n", n_);
  Length total_free;
  HugeLength total_backed = NHugePages(0);

  for (Region* region : list_) {
    region->Print(out);
    total_free += region->free_pages();
    total_backed += region->backed();
  }

  out->printf("HugeRegionSet: %zu hugepages backed out of %zu total\n",
              total_backed.raw_num(), Region::size().raw_num() * n_);

  const Length in_pages = total_backed.in_pages();
  out->printf("HugeRegionSet: %zu pages free in backed region, %.4f free\n",
              total_free.raw_num(),
              in_pages > Length(0) ? static_cast<double>(total_free.raw_num()) /
                                         static_cast<double>(in_pages.raw_num())
                                   : 0.0);
}

template <typename Region>
inline void HugeRegionSet<Region>::PrintInPbtxt(PbtxtRegion* hpaa) const {
  hpaa->PrintI64("min_huge_region_alloc_size", 1024 * 1024);
  hpaa->PrintI64("huge_region_size", Region::size().in_bytes());
  for (Region* region : list_) {
    auto detail = hpaa->CreateSubRegion("huge_region_details");
    region->PrintInPbtxt(&detail);
  }
}

template <typename Region>
inline void HugeRegionSet<Region>::AddSpanStats(SmallSpanStats* small,
                                                LargeSpanStats* large,
                                                PageAgeHistograms* ages) const {
  for (Region* region : list_) {
    region->AddSpanStats(small, large, ages);
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

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_HUGE_REGION_H_
