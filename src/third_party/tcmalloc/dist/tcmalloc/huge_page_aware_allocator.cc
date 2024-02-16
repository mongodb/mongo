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

#include <stdint.h>
#include <string.h>

#include <new>

#include "absl/base/internal/cycleclock.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/experiment_config.h"
#include "tcmalloc/huge_allocator.h"
#include "tcmalloc/huge_page_filler.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/lifetime_predictions.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/prefetch.h"
#include "tcmalloc/lifetime_based_allocator.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/system-alloc.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

bool decide_want_hpaa();
ABSL_ATTRIBUTE_WEAK int default_want_hpaa();
ABSL_ATTRIBUTE_WEAK int default_subrelease();

bool decide_subrelease() {
  if (!decide_want_hpaa()) {
    // Subrelease is off if HPAA is off.
    return false;
  }

  const char* e = thread_safe_getenv("TCMALLOC_HPAA_CONTROL");
  if (e) {
    switch (e[0]) {
      case '0':
        if (default_want_hpaa != nullptr) {
          int default_hpaa = default_want_hpaa();
          if (default_hpaa < 0) {
            return false;
          }
        }

        Log(kLog, __FILE__, __LINE__,
            "Runtime opt-out from HPAA requires building with "
            "//tcmalloc:want_no_hpaa."
        );
        break;
      case '1':
        return false;
      case '2':
        return true;
      default:
        Crash(kCrash, __FILE__, __LINE__, "bad env var", e);
        return false;
    }
  }

  if (default_subrelease != nullptr) {
    const int decision = default_subrelease();
    if (decision != 0) {
      return decision > 0;
    }
  }

  return true;
}

FillerPartialRerelease decide_partial_rerelease() {
  const char* e = thread_safe_getenv("TCMALLOC_PARTIAL_RELEASE_CONTROL");
  if (e) {
    if (e[0] == '0') {
      return FillerPartialRerelease::Return;
    }
    if (e[0] == '1') {
      return FillerPartialRerelease::Retain;
    }
    Crash(kCrash, __FILE__, __LINE__, "bad env var", e);
  }

  return FillerPartialRerelease::Retain;
}

LifetimePredictionOptions decide_lifetime_predictions() {
  // See LifetimePredictionOptions::FromFlag for a description of the format.
  const char* e = tcmalloc::tcmalloc_internal::thread_safe_getenv(
      "TCMALLOC_LIFETIMES_CONTROL");

  if (e != nullptr) {
    return LifetimePredictionOptions::FromFlag(e);
  }

  return LifetimePredictionOptions::Default();
}

HugeRegionCountOption use_huge_region_for_often() {
  return (IsExperimentActive(
              Experiment::TEST_ONLY_TCMALLOC_USE_HUGE_REGIONS_MORE_OFTEN) ||
          IsExperimentActive(Experiment::TCMALLOC_USE_HUGE_REGIONS_MORE_OFTEN))
             ? HugeRegionCountOption::kAbandonedCount
             : HugeRegionCountOption::kSlack;
}

// Some notes: locking discipline here is a bit funny, because
// we want to *not* hold the pageheap lock while backing memory.

// We have here a collection of slightly different allocators each
// optimized for slightly different purposes.  This file has two main purposes:
// - pick the right one for a given allocation
// - provide enough data to figure out what we picked last time!

HugePageAwareAllocator::HugePageAwareAllocator(MemoryTag tag)
    : HugePageAwareAllocator(tag, use_huge_region_for_often(),
                             decide_lifetime_predictions()) {}

HugePageAwareAllocator::HugePageAwareAllocator(
    MemoryTag tag, HugeRegionCountOption use_huge_region_more_often)
    : HugePageAwareAllocator(tag, use_huge_region_more_often,
                             decide_lifetime_predictions()) {}

HugePageAwareAllocator::HugePageAwareAllocator(
    MemoryTag tag, HugeRegionCountOption use_huge_region_more_often,
    LifetimePredictionOptions lifetime_options)
    : PageAllocatorInterface("HugePageAware", tag),
      filler_(decide_partial_rerelease(),
              Parameters::separate_allocs_for_few_and_many_objects_spans(),
              MemoryModifyFunction(SystemRelease)),
      alloc_(
          [](MemoryTag tag) {
            // TODO(ckennelly): Remove the template parameter.
            switch (tag) {
              case MemoryTag::kNormal:
                return AllocAndReport<MemoryTag::kNormal>;
              case MemoryTag::kNormalP1:
                return AllocAndReport<MemoryTag::kNormalP1>;
              case MemoryTag::kSampled:
                return AllocAndReport<MemoryTag::kSampled>;
              case MemoryTag::kCold:
                return AllocAndReport<MemoryTag::kCold>;
              default:
                ASSUME(false);
                __builtin_unreachable();
            }
          }(tag),
          MetaDataAlloc),
      cache_(HugeCache{&alloc_, MetaDataAlloc,
                       MemoryModifyFunction(UnbackWithoutLock)}),
      lifetime_allocator_region_alloc_(this),
      lifetime_allocator_(lifetime_options, &lifetime_allocator_region_alloc_),
      use_huge_region_more_often_(use_huge_region_more_often) {
  tracker_allocator_.Init(&tc_globals.arena());
  region_allocator_.Init(&tc_globals.arena());
}

HugePageAwareAllocator::FillerType::Tracker* HugePageAwareAllocator::GetTracker(
    HugePage p) {
  void* v = tc_globals.pagemap().GetHugepage(p.first_page());
  FillerType::Tracker* pt = reinterpret_cast<FillerType::Tracker*>(v);
  ASSERT(pt == nullptr || pt->location() == p);
  return pt;
}

void HugePageAwareAllocator::SetTracker(
    HugePage p, HugePageAwareAllocator::FillerType::Tracker* pt) {
  tc_globals.pagemap().SetHugepage(p.first_page(), pt);
}

PageId HugePageAwareAllocator::AllocAndContribute(HugePage p, Length n,
                                                  size_t num_objects,
                                                  bool donated) {
  CHECK_CONDITION(p.start_addr() != nullptr);
  FillerType::Tracker* pt = tracker_allocator_.New();
  new (pt)
      FillerType::Tracker(p, absl::base_internal::CycleClock::Now(), donated);
  ASSERT(pt->longest_free_range() >= n);
  ASSERT(pt->was_donated() == donated);
  // if the page was donated, we track its size so that we can potentially
  // measure it in abandoned_count_ once this large allocation gets deallocated.
  if (pt->was_donated()) {
    pt->set_abandoned_count(n);
  }
  PageId page = pt->Get(n).page;
  ASSERT(page == p.first_page());
  SetTracker(p, pt);
  filler_.Contribute(pt, donated, num_objects);
  ASSERT(pt->was_donated() == donated);
  return page;
}

PageId HugePageAwareAllocator::RefillFiller(Length n, size_t num_objects,
                                            bool* from_released) {
  HugeRange r = cache_.Get(NHugePages(1), from_released);
  if (!r.valid()) return PageId{0};
  // This is duplicate to Finalize, but if we need to break up
  // hugepages to get to our usage limit it would be very bad to break
  // up what's left of r after we allocate from there--while r is
  // mostly empty, clearly what's left in the filler is too fragmented
  // to be very useful, and we would rather release those
  // pages. Otherwise, we're nearly guaranteed to release r (if n
  // isn't very large), and the next allocation will just repeat this
  // process.
  tc_globals.page_allocator().ShrinkToUsageLimit(n);
  return AllocAndContribute(r.start(), n, num_objects, /*donated=*/false);
}

Span* HugePageAwareAllocator::Finalize(Length n, size_t num_objects,
                                       PageId page)
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
  ASSERT(page != PageId{0});
  Span* ret = Span::New(page, n);
  tc_globals.pagemap().Set(page, ret);
  ASSERT(!ret->sampled());
  info_.RecordAlloc(page, n, num_objects);
  tc_globals.page_allocator().ShrinkToUsageLimit(n);
  return ret;
}

// For anything <= half a huge page, we will unconditionally use the filler
// to pack it into a single page.  If we need another page, that's fine.
Span* HugePageAwareAllocator::AllocSmall(Length n, size_t objects_per_span,
                                         bool* from_released) {
  auto [pt, page] = filler_.TryGet(n, objects_per_span);
  if (ABSL_PREDICT_TRUE(pt != nullptr)) {
    *from_released = false;
    return Finalize(n, objects_per_span, page);
  }

  page = RefillFiller(n, objects_per_span, from_released);
  if (ABSL_PREDICT_FALSE(page == PageId{0})) {
    return nullptr;
  }
  return Finalize(n, objects_per_span, page);
}

Span* HugePageAwareAllocator::AllocLarge(Length n, size_t objects_per_span,
                                         bool* from_released,
                                         LifetimeStats* lifetime_context) {
  // If it's an exact page multiple, just pull it from pages directly.
  HugeLength hl = HLFromPages(n);
  if (hl.in_pages() == n) {
    return AllocRawHugepages(n, objects_per_span, from_released);
  }

  PageId page;
  // If we fit in a single hugepage, try the Filler first.
  if (n < kPagesPerHugePage) {
    auto [pt, page] = filler_.TryGet(n, objects_per_span);
    if (ABSL_PREDICT_TRUE(pt != nullptr)) {
      *from_released = false;
      return Finalize(n, objects_per_span, page);
    }
  }

  // Try to perform a lifetime-based allocation.
  LifetimeBasedAllocator::AllocationResult lifetime =
      lifetime_allocator_.MaybeGet(n, from_released, lifetime_context);

  // TODO(mmaas): Implement tracking if this is subsequently put into a
  // conventional region (currently ignored).

  // Was an object allocated in the lifetime region? If so, we return it.
  if (lifetime.TryGetAllocation(&page)) {
    return Finalize(n, objects_per_span, page);
  }

  // If we're using regions in this binary (see below comment), is
  // there currently available space there?
  if (regions_.MaybeGet(n, &page, from_released)) {
    return Finalize(n, objects_per_span, page);
  }

  // We have two choices here: allocate a new region or go to
  // hugepages directly (hoping that slack will be filled by small
  // allocation.) The second strategy is preferrable, as it's
  // typically faster and usually more space efficient, but it's sometimes
  // catastrophic.
  //
  // See https://github.com/google/tcmalloc/tree/master/docs/regions-are-not-optional.md
  //
  // So test directly if we're in the bad case--almost no binaries are.
  // If not, just fall back to direct allocation (and hope we do hit that case!)
  const Length slack = info_.slack();
  const Length donated =
      UseHugeRegionMoreOften() ? abandoned_pages_ + slack : slack;
  // Don't bother at all until the binary is reasonably sized.
  if (donated < HLFromBytes(64 * 1024 * 1024).in_pages()) {
    return AllocRawHugepagesAndMaybeTrackLifetime(n, objects_per_span, lifetime,
                                                  from_released);
  }

  // In the vast majority of binaries, we have many small allocations which
  // will nicely fill slack.  (Fleetwide, the average ratio is 15:1; only
  // a handful of binaries fall below 1:1.)
  //
  // If we enable an experiment that tries to use huge regions more frequently,
  // we skip the check.
  const Length small = info_.small();
  if (slack < small && !UseHugeRegionMoreOften()) {
    return AllocRawHugepagesAndMaybeTrackLifetime(n, objects_per_span, lifetime,
                                                  from_released);
  }

  // We couldn't allocate a new region. They're oversized, so maybe we'd get
  // lucky with a smaller request?
  if (!AddRegion()) {
    return AllocRawHugepagesAndMaybeTrackLifetime(n, objects_per_span, lifetime,
                                                  from_released);
  }

  CHECK_CONDITION(regions_.MaybeGet(n, &page, from_released));
  return Finalize(n, objects_per_span, page);
}

Span* HugePageAwareAllocator::AllocEnormous(Length n, size_t objects_per_span,
                                            bool* from_released) {
  return AllocRawHugepages(n, objects_per_span, from_released);
}

Span* HugePageAwareAllocator::AllocRawHugepages(Length n, size_t num_objects,
                                                bool* from_released) {
  HugeLength hl = HLFromPages(n);

  HugeRange r = cache_.Get(hl, from_released);
  if (!r.valid()) return nullptr;

  // We now have a huge page range that covers our request.  There
  // might be some slack in it if n isn't a multiple of
  // kPagesPerHugePage. Add the hugepage with slack to the filler,
  // pretending the non-slack portion is a smaller allocation.
  Length total = hl.in_pages();
  Length slack = total - n;
  HugePage first = r.start();
  SetTracker(first, nullptr);
  HugePage last = first + r.len() - NHugePages(1);
  if (slack == Length(0)) {
    SetTracker(last, nullptr);
    return Finalize(total, num_objects, r.start().first_page());
  }

  ++donated_huge_pages_;

  Length here = kPagesPerHugePage - slack;
  ASSERT(here > Length(0));
  AllocAndContribute(last, here, num_objects, /*donated=*/true);
  Span* span = Finalize(n, num_objects, r.start().first_page());
  span->set_donated(/*value=*/true);
  return span;
}

Span* HugePageAwareAllocator::AllocRawHugepagesAndMaybeTrackLifetime(
    Length n, size_t num_objects,
    const LifetimeBasedAllocator::AllocationResult& lifetime_alloc,
    bool* from_released) ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
  Span* result = AllocRawHugepages(n, num_objects, from_released);

  if (result != nullptr) {
    // If this is an object with a lifetime prediction and led to a donation,
    // add it to the tracker so that we can track its lifetime.
    HugePage hp = HugePageContaining(result->last_page());
    FillerType::Tracker* pt = GetTracker(hp);
    ASSERT(pt != nullptr);

    // The allocator may shrink the heap in response to allocations, which may
    // cause the page to be subreleased and not donated anymore once we get
    // here. If it still is, we attach a lifetime tracker (if enabled).
    if (ABSL_PREDICT_TRUE(pt->donated())) {
      lifetime_allocator_.MaybeAddTracker(lifetime_alloc,
                                          pt->lifetime_tracker());
    }
  }

  return result;
}

static void BackSpan(Span* span) {
  SystemBack(span->start_address(), span->bytes_in_span());
}

// public
Span* HugePageAwareAllocator::New(Length n, size_t objects_per_span) {
  CHECK_CONDITION(n > Length(0));
  bool from_released;
  Span* s = LockAndAlloc(n, objects_per_span, &from_released);
  if (s) {
    // Prefetch for writing, as we anticipate using the memory soon.
    PrefetchW(s->start_address());
    // TODO(b/256233439):  Improve accuracy of from_released value.  The filler
    // may have subreleased pages and is returning them now.
    if (from_released) BackSpan(s);
  }
  ASSERT(!s || GetMemoryTag(s->start_address()) == tag_);
  return s;
}

Span* HugePageAwareAllocator::LockAndAlloc(Length n, size_t objects_per_span,
                                           bool* from_released) {
  // Check whether we may perform lifetime-based allocation, and if so, collect
  // the allocation context without holding the lock.
  LifetimeStats* lifetime_ctx = lifetime_allocator_.CollectLifetimeContext(n);

  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  // Our policy depends on size.  For small things, we will pack them
  // into single hugepages.
  if (n <= kPagesPerHugePage / 2) {
    return AllocSmall(n, objects_per_span, from_released);
  }

  // For anything too big for the filler, we use either a direct hugepage
  // allocation, or possibly the regions if we are worried about slack.
  if (n <= HugeRegion::size().in_pages()) {
    return AllocLarge(n, objects_per_span, from_released, lifetime_ctx);
  }

  // In the worst case, we just fall back to directly allocating a run
  // of hugepages.
  return AllocEnormous(n, objects_per_span, from_released);
}

// public
Span* HugePageAwareAllocator::NewAligned(Length n, Length align,
                                         size_t objects_per_span) {
  if (align <= Length(1)) {
    return New(n, objects_per_span);
  }

  // we can do better than this, but...
  // TODO(b/134690769): support higher align.
  CHECK_CONDITION(align <= kPagesPerHugePage);
  bool from_released;
  Span* s;
  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    s = AllocRawHugepages(n, objects_per_span, &from_released);
  }
  if (s && from_released) BackSpan(s);
  ASSERT(!s || GetMemoryTag(s->start_address()) == tag_);
  return s;
}

void HugePageAwareAllocator::DeleteFromHugepage(FillerType::Tracker* pt,
                                                PageId p, Length n,
                                                size_t num_objects,
                                                bool might_abandon) {
  if (ABSL_PREDICT_TRUE(filler_.Put(pt, p, n, num_objects) == nullptr)) {
    // If this allocation had resulted in a donation to the filler, we record
    // these pages as abandoned.
    if (ABSL_PREDICT_FALSE(might_abandon)) {
      ASSERT(pt->was_donated());
      abandoned_pages_ += pt->abandoned_count();
      pt->set_abandoned(true);
    }
    return;
  }
  if (pt->was_donated()) {
    --donated_huge_pages_;
    if (pt->abandoned()) {
      abandoned_pages_ -= pt->abandoned_count();
      pt->set_abandoned(false);
    }
  } else {
    ASSERT(pt->abandoned_count() == Length(0));
  }
  lifetime_allocator_.MaybePutTracker(pt->lifetime_tracker(), n);
  ReleaseHugepage(pt);
}

bool HugePageAwareAllocator::AddRegion() {
  HugeRange r = alloc_.Get(HugeRegion::size());
  if (!r.valid()) return false;
  HugeRegion* region = region_allocator_.New();
  new (region) HugeRegion(r, MemoryModifyFunction(SystemRelease));
  regions_.Contribute(region);
  return true;
}

void HugePageAwareAllocator::Delete(Span* span, size_t objects_per_span) {
  ASSERT(!span || GetMemoryTag(span->start_address()) == tag_);
  PageId p = span->first_page();
  HugePage hp = HugePageContaining(p);
  Length n = span->num_pages();
  info_.RecordFree(p, n, objects_per_span);

  bool might_abandon = span->donated();
  Span::Delete(span);
  // Clear the descriptor of the page so a second pass through the same page
  // could trigger the check on `span != nullptr` in do_free_pages.
  tc_globals.pagemap().Set(p, nullptr);

  // The tricky part, as with so many allocators: where did we come from?
  // There are several possibilities.
  FillerType::Tracker* pt = GetTracker(hp);
  // a) We got packed by the filler onto a single hugepage - return our
  //    allocation to that hugepage in the filler.
  if (ABSL_PREDICT_TRUE(pt != nullptr)) {
    ASSERT(hp == HugePageContaining(p + n - Length(1)));
    DeleteFromHugepage(pt, p, n, objects_per_span, might_abandon);
    return;
  }

  // b) We got put into a region, possibly crossing hugepages -
  //    return our allocation to the region.
  if (regions_.MaybePut(p, n)) return;
  if (lifetime_allocator_.MaybePut(p, n)) return;

  // c) we came straight from the HugeCache - return straight there.  (We
  //    might have had slack put into the filler - if so, return that virtual
  //    allocation to the filler too!)
  ASSERT(n >= kPagesPerHugePage);
  HugeLength hl = HLFromPages(n);
  HugePage last = hp + hl - NHugePages(1);
  Length slack = hl.in_pages() - n;
  if (slack == Length(0)) {
    ASSERT(GetTracker(last) == nullptr);
  } else {
    pt = GetTracker(last);
    lifetime_allocator_.MaybePutTracker(pt->lifetime_tracker(), n);
    CHECK_CONDITION(pt != nullptr);
    ASSERT(pt->was_donated());
    // We put the slack into the filler (see AllocEnormous.)
    // Handle this page separately as a virtual allocation
    // onto the last hugepage.
    PageId virt = last.first_page();
    Length virt_len = kPagesPerHugePage - slack;
    // We may have used the slack, which would prevent us from returning
    // the entire range now.  If filler returned a Tracker, we are fully empty.
    if (filler_.Put(pt, virt, virt_len, objects_per_span) == nullptr) {
      // Last page isn't empty -- pretend the range was shorter.
      --hl;

      // Note that we abandoned virt_len pages with pt.  These can be reused for
      // other allocations, but this can contribute to excessive slack in the
      // filler.
      abandoned_pages_ += pt->abandoned_count();
      pt->set_abandoned(true);
    } else {
      // Last page was empty - but if we sub-released it, we still
      // have to split it off and release it independently.)
      //
      // We were able to reclaim the donated slack.
      --donated_huge_pages_;
      ASSERT(!pt->abandoned());

      if (pt->released()) {
        --hl;
        ReleaseHugepage(pt);
      } else {
        // Get rid of the tracker *object*, but not the *hugepage* (which is
        // still part of our range.)
        SetTracker(pt->location(), nullptr);
        ASSERT(!pt->lifetime_tracker()->is_tracked());
        tracker_allocator_.Delete(pt);
      }
    }
  }
  cache_.Release({hp, hl});
}

void HugePageAwareAllocator::ReleaseHugepage(FillerType::Tracker* pt) {
  ASSERT(pt->used_pages() == Length(0));
  HugeRange r = {pt->location(), NHugePages(1)};
  SetTracker(pt->location(), nullptr);

  if (pt->released()) {
    cache_.ReleaseUnbacked(r);
  } else {
    cache_.Release(r);
  }

  ASSERT(!pt->lifetime_tracker()->is_tracked());
  tracker_allocator_.Delete(pt);
}

// public
BackingStats HugePageAwareAllocator::stats() const {
  BackingStats stats = alloc_.stats();
  const auto actual_system = stats.system_bytes;
  stats += cache_.stats();
  stats += filler_.stats();
  stats += regions_.stats();
  stats += lifetime_allocator_.GetRegionStats().value_or(BackingStats());
  // the "system" (total managed) byte count is wildly double counted,
  // since it all comes from HugeAllocator but is then managed by
  // cache/regions/filler. Adjust for that.
  stats.system_bytes = actual_system;
  return stats;
}

// public
void HugePageAwareAllocator::GetSmallSpanStats(SmallSpanStats* result) {
  GetSpanStats(result, nullptr, nullptr);
}

// public
void HugePageAwareAllocator::GetLargeSpanStats(LargeSpanStats* result) {
  GetSpanStats(nullptr, result, nullptr);
}

void HugePageAwareAllocator::GetSpanStats(SmallSpanStats* small,
                                          LargeSpanStats* large,
                                          PageAgeHistograms* ages) {
  if (small != nullptr) {
    *small = SmallSpanStats();
  }
  if (large != nullptr) {
    *large = LargeSpanStats();
  }

  alloc_.AddSpanStats(small, large, ages);
  filler_.AddSpanStats(small, large, ages);
  regions_.AddSpanStats(small, large, ages);
  cache_.AddSpanStats(small, large, ages);
}

// public
Length HugePageAwareAllocator::ReleaseAtLeastNPages(Length num_pages) {
  Length released;
  released += cache_.ReleaseCachedPages(HLFromPages(num_pages)).in_pages();

  // This is our long term plan but in current state will lead to insufficient
  // THP coverage. It is however very useful to have the ability to turn this on
  // for testing.
  // TODO(b/134690769): make this work, remove the flag guard.
  if (Parameters::hpaa_subrelease()) {
    if (released < num_pages) {
      released += filler_.ReleasePages(
          num_pages - released,
          SkipSubreleaseIntervals{
              .peak_interval = Parameters::filler_skip_subrelease_interval(),
              .short_interval =
                  Parameters::filler_skip_subrelease_short_interval(),
              .long_interval =
                  Parameters::filler_skip_subrelease_long_interval()},
          /*hit_limit*/ false);
    }
  }

  // TODO(b/134690769):
  // - perhaps release region?
  // - refuse to release if we're too close to zero?
  info_.RecordRelease(num_pages, released);
  return released;
}

static double BytesToMiB(size_t bytes) {
  const double MiB = 1048576.0;
  return bytes / MiB;
}

static void BreakdownStats(Printer* out, const BackingStats& s,
                           const char* label) {
  out->printf("%s %6.1f MiB used, %6.1f MiB free, %6.1f MiB unmapped\n", label,
              BytesToMiB(s.system_bytes - s.free_bytes - s.unmapped_bytes),
              BytesToMiB(s.free_bytes), BytesToMiB(s.unmapped_bytes));
}

static void BreakdownStatsInPbtxt(PbtxtRegion* hpaa, const BackingStats& s,
                                  const char* key) {
  auto usage = hpaa->CreateSubRegion(key);
  usage.PrintI64("used", s.system_bytes - s.free_bytes - s.unmapped_bytes);
  usage.PrintI64("free", s.free_bytes);
  usage.PrintI64("unmapped", s.unmapped_bytes);
}

// public
void HugePageAwareAllocator::Print(Printer* out) { Print(out, true); }

void HugePageAwareAllocator::Print(Printer* out, bool everything) {
  SmallSpanStats small;
  LargeSpanStats large;
  BackingStats bstats;
  PageAgeHistograms ages(absl::base_internal::CycleClock::Now());
  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  bstats = stats();
  GetSpanStats(&small, &large, &ages);
  PrintStats("HugePageAware", out, bstats, small, large, everything);
  out->printf(
      "\nHuge page aware allocator components:\n"
      "------------------------------------------------\n");
  out->printf("HugePageAware: breakdown of used / free / unmapped space:\n");

  auto fstats = filler_.stats();
  BreakdownStats(out, fstats, "HugePageAware: filler  ");

  auto rstats = regions_.stats();
  BreakdownStats(out, rstats, "HugePageAware: region  ");

  // Report short-lived region allocations when enabled.
  auto lstats = lifetime_allocator_.GetRegionStats();
  if (lstats.has_value()) {
    BreakdownStats(out, lstats.value(), "HugePageAware: lifetime");
  }

  auto cstats = cache_.stats();
  // Everything in the filler came from the cache -
  // adjust the totals so we see the amount used by the mutator.
  cstats.system_bytes -= fstats.system_bytes;
  BreakdownStats(out, cstats, "HugePageAware: cache   ");

  auto astats = alloc_.stats();
  // Everything in *all* components came from here -
  // so again adjust the totals.
  astats.system_bytes -=
      (fstats + rstats + lstats.value_or(BackingStats()) + cstats).system_bytes;
  BreakdownStats(out, astats, "HugePageAware: alloc   ");
  out->printf("\n");

  out->printf(
      "HugePageAware: filler donations %zu (%zu pages from abandoned "
      "donations)\n",
      donated_huge_pages_.raw_num(), abandoned_pages_.raw_num());

  // Component debug output
  // Filler is by far the most important; print (some) of it
  // unconditionally.
  filler_.Print(out, everything);
  out->printf("\n");
  if (everything) {
    regions_.Print(out);
    out->printf("\n");
    cache_.Print(out);
    lifetime_allocator_.Print(out);
    out->printf("\n");
    alloc_.Print(out);
    out->printf("\n");

    // Use statistics
    info_.Print(out);

    // and age tracking.
    ages.Print("HugePageAware", out);
  }

  out->printf("PARAMETER hpaa_subrelease %d\n",
              Parameters::hpaa_subrelease() ? 1 : 0);
}

void HugePageAwareAllocator::PrintInPbtxt(PbtxtRegion* region) {
  SmallSpanStats small;
  LargeSpanStats large;
  PageAgeHistograms ages(absl::base_internal::CycleClock::Now());
  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  GetSpanStats(&small, &large, &ages);
  PrintStatsInPbtxt(region, small, large, ages);
  {
    auto hpaa = region->CreateSubRegion("huge_page_allocator");
    hpaa.PrintBool("using_hpaa", true);
    hpaa.PrintBool("using_hpaa_subrelease", Parameters::hpaa_subrelease());

    // Fill HPAA Usage
    auto fstats = filler_.stats();
    BreakdownStatsInPbtxt(&hpaa, fstats, "filler_usage");

    auto rstats = regions_.stats();
    BreakdownStatsInPbtxt(&hpaa, rstats, "region_usage");

    auto cstats = cache_.stats();
    // Everything in the filler came from the cache -
    // adjust the totals so we see the amount used by the mutator.
    cstats.system_bytes -= fstats.system_bytes;
    BreakdownStatsInPbtxt(&hpaa, cstats, "cache_usage");

    auto astats = alloc_.stats();
    // Everything in *all* components came from here -
    // so again adjust the totals.
    astats.system_bytes -= (fstats + rstats + cstats).system_bytes;

    auto lstats = lifetime_allocator_.GetRegionStats();
    if (lstats.has_value()) {
      astats.system_bytes -= lstats.value().system_bytes;
      BreakdownStatsInPbtxt(&hpaa, lstats.value(), "lifetime_region_usage");
    }

    BreakdownStatsInPbtxt(&hpaa, astats, "alloc_usage");

    filler_.PrintInPbtxt(&hpaa);
    regions_.PrintInPbtxt(&hpaa);
    cache_.PrintInPbtxt(&hpaa);
    alloc_.PrintInPbtxt(&hpaa);
    lifetime_allocator_.PrintInPbtxt(&hpaa);

    // Use statistics
    info_.PrintInPbtxt(&hpaa, "hpaa_stat");

    hpaa.PrintI64("filler_donated_huge_pages", donated_huge_pages_.raw_num());
    hpaa.PrintI64("filler_abandoned_pages", abandoned_pages_.raw_num());
  }
}

template <MemoryTag tag>
AddressRange HugePageAwareAllocator::AllocAndReport(size_t bytes,
                                                    size_t align) {
  auto ret = SystemAlloc(bytes, align, tag);
  if (ret.ptr == nullptr) return ret;
  const PageId page = PageIdContaining(ret.ptr);
  const Length page_len = BytesToLengthFloor(ret.bytes);
  tc_globals.pagemap().Ensure(page, page_len);
  return ret;
}

void* HugePageAwareAllocator::MetaDataAlloc(size_t bytes)
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
  return tc_globals.arena().Alloc(bytes);
}

Length HugePageAwareAllocator::ReleaseAtLeastNPagesBreakingHugepages(Length n) {
  // We desperately need to release memory, and are willing to
  // compromise on hugepage usage. That means releasing from the filler.
  return filler_.ReleasePages(n, SkipSubreleaseIntervals{},
                              /*hit_limit*/ true);
}

bool HugePageAwareAllocator::UnbackWithoutLock(void* start, size_t length) {
  pageheap_lock.Unlock();
  const bool ret = SystemRelease(start, length);
  pageheap_lock.Lock();
  return ret;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
