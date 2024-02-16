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

#ifndef TCMALLOC_HUGE_PAGE_AWARE_ALLOCATOR_H_
#define TCMALLOC_HUGE_PAGE_AWARE_ALLOCATOR_H_

#include <stddef.h>

#include "absl/base/thread_annotations.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_allocator.h"
#include "tcmalloc/huge_cache.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/huge_region.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/lifetime_based_allocator.h"
#include "tcmalloc/page_allocator_interface.h"
#include "tcmalloc/page_heap_allocator.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/system-alloc.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

bool decide_subrelease();

enum class HugeRegionCountOption : bool {
  // This is a default behavior. We use slack to determine when to use
  // HugeRegion. When slack is greater than 64MB (to ignore small binaries), and
  // greater than the number of small allocations, we allocate large allocations
  // from HugeRegion.
  kSlack,
  // When the experiment TEST_ONLY_TCMALLOC_USE_HUGE_REGIONS_MORE_OFTEN is
  // enabled, we use number of abandoned pages in addition to slack to make a
  // decision. If the size of abandoned pages plus slack exceeds 64MB (to ignore
  // small binaries), we use HugeRegion for large allocations.
  kAbandonedCount
};

// An implementation of the PageAllocator interface that is hugepage-efficient.
// Attempts to pack allocations into full hugepages wherever possible,
// and aggressively returns empty ones to the system.
class HugePageAwareAllocator final : public PageAllocatorInterface {
 public:
  explicit HugePageAwareAllocator(MemoryTag tag);
  // For use in testing.
  HugePageAwareAllocator(MemoryTag tag,
                         HugeRegionCountOption use_huge_region_more_often);
  HugePageAwareAllocator(MemoryTag tag,
                         HugeRegionCountOption use_huge_region_more_often,
                         LifetimePredictionOptions lifetime_options);
  ~HugePageAwareAllocator() override = default;

  // Allocate a run of "n" pages.  Returns zero if out of memory.
  // Caller should not pass "n == 0" -- instead, n should have
  // been rounded up already.
  Span* New(Length n, size_t objects_per_span)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) override;

  // As New, but the returned span is aligned to a <align>-page boundary.
  // <align> must be a power of two.
  Span* NewAligned(Length n, Length align, size_t objects_per_span)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) override;

  // Delete the span "[p, p+n-1]".
  // REQUIRES: span was returned by earlier call to New() and
  //           has not yet been deleted.
  void Delete(Span* span, size_t objects_per_span)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override;

  BackingStats stats() const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override;

  void GetSmallSpanStats(SmallSpanStats* result)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override;

  void GetLargeSpanStats(LargeSpanStats* result)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override;

  // Try to release at least num_pages for reuse by the OS.  Returns
  // the actual number of pages released, which may be less than
  // num_pages if there weren't enough pages to release. The result
  // may also be larger than num_pages since page_heap might decide to
  // release one large range instead of fragmenting it into two
  // smaller released and unreleased ranges.
  Length ReleaseAtLeastNPages(Length num_pages)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override;

  Length ReleaseAtLeastNPagesBreakingHugepages(Length n)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Prints stats about the page heap to *out.
  void Print(Printer* out) ABSL_LOCKS_EXCLUDED(pageheap_lock) override;

  // Print stats to *out, excluding long/likely uninteresting things
  // unless <everything> is true.
  void Print(Printer* out, bool everything) ABSL_LOCKS_EXCLUDED(pageheap_lock);

  void PrintInPbtxt(PbtxtRegion* region)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) override;

  HugeLength DonatedHugePages() const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    return donated_huge_pages_;
  }

  // Number of pages that have been retained on huge pages by donations that did
  // not reassemble by the time the larger allocation was deallocated.
  Length AbandonedPages() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    return abandoned_pages_;
  }

  const HugeCache* cache() const { return &cache_; }

  LifetimeBasedAllocator& lifetime_based_allocator() {
    return lifetime_allocator_;
  }

  const HugeRegionSet<HugeRegion>& region() const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    return regions_;
  };

 private:
  typedef HugePageFiller<PageTracker> FillerType;
  FillerType filler_ ABSL_GUARDED_BY(pageheap_lock);

  class RegionAllocImpl final : public LifetimeBasedAllocator::RegionAlloc {
   public:
    explicit RegionAllocImpl(HugePageAwareAllocator* p) : p_(p) {}

    // We need to explicitly instantiate the destructor here so that it gets
    // placed within GOOGLE_MALLOC_SECTION.
    ~RegionAllocImpl() override {}

    HugeRegion* AllocRegion(HugeLength n, HugeRange* range) override
        ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
      if (!range->valid()) {
        *range = p_->alloc_.Get(n);
      }
      if (!range->valid()) return nullptr;
      HugeRegion* region = p_->region_allocator_.New();
      new (region) HugeRegion(*range, MemoryModifyFunction(SystemRelease));
      return region;
    }

   private:
    HugePageAwareAllocator* p_;
  };

  // Calls SystemRelease, but with dropping of pageheap_lock around the call.
  static ABSL_MUST_USE_RESULT bool UnbackWithoutLock(void* start, size_t length)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  HugeRegionSet<HugeRegion> regions_ ABSL_GUARDED_BY(pageheap_lock);

  PageHeapAllocator<FillerType::Tracker> tracker_allocator_
      ABSL_GUARDED_BY(pageheap_lock);
  PageHeapAllocator<HugeRegion> region_allocator_
      ABSL_GUARDED_BY(pageheap_lock);

  FillerType::Tracker* GetTracker(HugePage p);

  void SetTracker(HugePage p, FillerType::Tracker* pt);

  template <MemoryTag tag>
  static AddressRange AllocAndReport(size_t bytes, size_t align)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
  static void* MetaDataAlloc(size_t bytes);
  HugeAllocator alloc_ ABSL_GUARDED_BY(pageheap_lock);
  HugeCache cache_ ABSL_GUARDED_BY(pageheap_lock);

  // donated_huge_pages_ measures the number of huge pages contributed to the
  // filler from left overs of large huge page allocations.  When the large
  // allocation is deallocated, we decrement this count *if* we were able to
  // fully reassemble the address range (that is, the partial hugepage did not
  // get stuck in the filler).
  HugeLength donated_huge_pages_ ABSL_GUARDED_BY(pageheap_lock);
  // abandoned_pages_ tracks the number of pages contributed to the filler after
  // a donating allocation is deallocated but the entire huge page has not been
  // reassembled.
  Length abandoned_pages_ ABSL_GUARDED_BY(pageheap_lock);

  // Performs lifetime predictions for large objects and places short-lived
  // objects into a separate region to reduce filler contention.
  RegionAllocImpl lifetime_allocator_region_alloc_;
  LifetimeBasedAllocator lifetime_allocator_;

  // Ddetermines if the experiment is enabled. If enabled, we use
  // abandoned_count_ in addition to slack in determining when to use
  // HugeRegion.
  const HugeRegionCountOption use_huge_region_more_often_;
  bool UseHugeRegionMoreOften() const {
    return use_huge_region_more_often_ ==
           HugeRegionCountOption::kAbandonedCount;
  }

  void GetSpanStats(SmallSpanStats* small, LargeSpanStats* large,
                    PageAgeHistograms* ages)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  PageId RefillFiller(Length n, size_t num_objects, bool* from_released)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Allocate the first <n> from p, and contribute the rest to the filler.  If
  // "donated" is true, the contribution will be marked as coming from the
  // tail of a multi-hugepage alloc.  Returns the allocated section.
  PageId AllocAndContribute(HugePage p, Length n, size_t num_objects,
                            bool donated)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
  // Helpers for New().

  Span* LockAndAlloc(Length n, size_t objects_per_span, bool* from_released);

  Span* AllocSmall(Length n, size_t objects_per_span, bool* from_released)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
  Span* AllocLarge(Length n, size_t objects_per_span, bool* from_released,
                   LifetimeStats* lifetime_context)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
  Span* AllocEnormous(Length n, size_t objects_per_span, bool* from_released)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  Span* AllocRawHugepages(Length n, size_t num_objects, bool* from_released)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Allocates a span and adds a tracker. This span has to be associated with a
  // filler donation and have an associated page tracker. A tracker will only be
  // added if there is an associated lifetime prediction.
  Span* AllocRawHugepagesAndMaybeTrackLifetime(
      Length n, size_t num_objects,
      const LifetimeBasedAllocator::AllocationResult& lifetime_alloc,
      bool* from_released) ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  bool AddRegion() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  void ReleaseHugepage(FillerType::Tracker* pt)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
  // Return an allocation from a single hugepage.
  void DeleteFromHugepage(FillerType::Tracker* pt, PageId p, Length n,
                          size_t num_objects, bool might_abandon)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Finish an allocation request - give it a span and mark it in the pagemap.
  Span* Finalize(Length n, size_t num_objects, PageId page);
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_HUGE_PAGE_AWARE_ALLOCATOR_H_
