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

#include "tcmalloc/page_heap.h"

#include <stddef.h>

#include <limits>

#include "absl/base/internal/cycleclock.h"
#include "absl/base/internal/spinlock.h"
#include "absl/numeric/bits.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/page_heap_allocator.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/system-alloc.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Helper function to record span address into pageheap
void PageHeap::RecordSpan(Span* span) {
  pagemap_->Set(span->first_page(), span);
  if (span->num_pages() > Length(1)) {
    pagemap_->Set(span->last_page(), span);
  }
}

PageHeap::PageHeap(MemoryTag tag) : PageHeap(&tc_globals.pagemap(), tag) {}

PageHeap::PageHeap(PageMap* map, MemoryTag tag)
    : PageAllocatorInterface("PageHeap", map, tag),
      // Start scavenging at kMaxPages list
      release_index_(kMaxPages.raw_num()) {}

Span* PageHeap::SearchFreeAndLargeLists(Length n, bool* from_returned) {
  ASSERT(Check());
  ASSERT(n > Length(0));

  // Find first size >= n that has a non-empty list
  for (Length s = n; s < kMaxPages; ++s) {
    SpanList* ll = &free_[s.raw_num()].normal;
    // If we're lucky, ll is non-empty, meaning it has a suitable span.
    if (!ll->empty()) {
      ASSERT(ll->first()->location() == Span::ON_NORMAL_FREELIST);
      *from_returned = false;
      return Carve(ll->first(), n);
    }
    // Alternatively, maybe there's a usable returned span.
    ll = &free_[s.raw_num()].returned;
    if (!ll->empty()) {
      ASSERT(ll->first()->location() == Span::ON_RETURNED_FREELIST);
      *from_returned = true;
      return Carve(ll->first(), n);
    }
  }
  // No luck in free lists, our last chance is in a larger class.
  return AllocLarge(n, from_returned);  // May be NULL
}

Span* PageHeap::AllocateSpan(Length n, bool* from_returned) {
  ASSERT(Check());
  Span* result = SearchFreeAndLargeLists(n, from_returned);
  if (result != nullptr) return result;

  // Grow the heap and try again.
  if (!GrowHeap(n)) {
    ASSERT(Check());
    return nullptr;
  }

  result = SearchFreeAndLargeLists(n, from_returned);
  // our new memory should be unbacked
  ASSERT(*from_returned);
  return result;
}

Span* PageHeap::New(Length n, size_t objects_per_span) {
  ASSERT(n > Length(0));
  bool from_returned;
  Span* result;
  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    result = AllocateSpan(n, &from_returned);
    if (result) tc_globals.page_allocator().ShrinkToUsageLimit(n);
    if (result)
      info_.RecordAlloc(result->first_page(), result->num_pages(),
                        objects_per_span);
  }

  if (result != nullptr && from_returned) {
    SystemBack(result->start_address(), result->bytes_in_span());
  }

  ASSERT(!result || GetMemoryTag(result->start_address()) == tag_);
  return result;
}

static bool IsSpanBetter(Span* span, Span* best, Length n) {
  if (span->num_pages() < n) {
    return false;
  }
  if (best == nullptr) {
    return true;
  }
  if (span->num_pages() < best->num_pages()) {
    return true;
  }
  if (span->num_pages() > best->num_pages()) {
    return false;
  }
  return span->first_page() < best->first_page();
}

// We could do slightly more efficient things here (we do some
// unnecessary Carves in New) but it's not anywhere
// close to a fast path, and is going to be replaced soon anyway, so
// don't bother.
Span* PageHeap::NewAligned(Length n, Length align, size_t objects_per_span) {
  ASSERT(n > Length(0));
  ASSERT(absl::has_single_bit(align.raw_num()));

  if (align <= Length(1)) {
    return New(n, objects_per_span);
  }

  bool from_returned;
  Span* span;
  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    Length extra = align - Length(1);
    span = AllocateSpan(n + extra, &from_returned);
    if (span == nullptr) return nullptr;
    // <span> certainly contains an appropriately aligned region; find it
    // and chop off the rest.
    PageId p = span->first_page();
    const Length mask = align - Length(1);
    PageId aligned = PageId{(p.index() + mask.raw_num()) & ~mask.raw_num()};
    ASSERT(aligned.index() % align.raw_num() == 0);
    ASSERT(p <= aligned);
    ASSERT(aligned + n <= p + span->num_pages());
    // we have <extra> too many pages now, possible all before, possibly all
    // after, maybe both
    Length before = aligned - p;
    Length after = extra - before;
    span->set_first_page(aligned);
    span->set_num_pages(n);
    RecordSpan(span);

    const Span::Location loc =
        from_returned ? Span::ON_RETURNED_FREELIST : Span::ON_NORMAL_FREELIST;
    if (before > Length(0)) {
      Span* extra = Span::New(p, before);
      extra->set_location(loc);
      RecordSpan(extra);
      MergeIntoFreeList(extra);
    }

    if (after > Length(0)) {
      Span* extra = Span::New(aligned + n, after);
      extra->set_location(loc);
      RecordSpan(extra);
      MergeIntoFreeList(extra);
    }

    info_.RecordAlloc(aligned, n, objects_per_span);
  }

  if (span != nullptr && from_returned) {
    SystemBack(span->start_address(), span->bytes_in_span());
  }

  ASSERT(!span || GetMemoryTag(span->start_address()) == tag_);
  return span;
}

Span* PageHeap::AllocLarge(Length n, bool* from_returned) {
  // find the best span (closest to n in size).
  // The following loops implements address-ordered best-fit.
  Span* best = nullptr;

  // Search through normal list
  for (Span* span : large_.normal) {
    ASSERT(span->location() == Span::ON_NORMAL_FREELIST);
    if (IsSpanBetter(span, best, n)) {
      best = span;
      *from_returned = false;
    }
  }

  // Search through released list in case it has a better fit
  for (Span* span : large_.returned) {
    ASSERT(span->location() == Span::ON_RETURNED_FREELIST);
    if (IsSpanBetter(span, best, n)) {
      best = span;
      *from_returned = true;
    }
  }

  return best == nullptr ? nullptr : Carve(best, n);
}

Span* PageHeap::Carve(Span* span, Length n) {
  ASSERT(n > Length(0));
  ASSERT(span->location() != Span::IN_USE);
  const Span::Location old_location = span->location();
  RemoveFromFreeList(span);
  span->set_location(Span::IN_USE);

  const Length extra = span->num_pages() - n;
  if (extra > Length(0)) {
    Span* leftover = nullptr;
    // Check if this span has another span on the right but not on the left.
    // There is one special case we want to handle: if heap grows down (as it is
    // usually happens with mmap allocator) and user allocates lots of large
    // persistent memory blocks (namely, kMinSystemAlloc + epsilon), then we
    // want to return the last part of the span to user and push the beginning
    // to the freelist.
    // Otherwise system allocator would allocate 2 * kMinSystemAlloc, we return
    // the first kMinSystemAlloc + epsilon to user and add the remaining
    // kMinSystemAlloc - epsilon to the freelist. The remainder is not large
    // enough to satisfy the next allocation request, so we allocate
    // another 2 * kMinSystemAlloc from system and the process repeats wasting
    // half of memory.
    // If we return the last part to user, then the remainder will be merged
    // with the next system allocation which will result in dense packing.
    // There are no other known cases where span splitting strategy matters,
    // so in other cases we return beginning to user.
    if (pagemap_->GetDescriptor(span->first_page() - Length(1)) == nullptr &&
        pagemap_->GetDescriptor(span->last_page() + Length(1)) != nullptr) {
      leftover = Span::New(span->first_page(), extra);
      span->set_first_page(span->first_page() + extra);
      pagemap_->Set(span->first_page(), span);
    } else {
      leftover = Span::New(span->first_page() + n, extra);
    }
    leftover->set_location(old_location);
    RecordSpan(leftover);
    PrependToFreeList(leftover);  // Skip coalescing - no candidates possible
    leftover->set_freelist_added_time(span->freelist_added_time());
    span->set_num_pages(n);
    pagemap_->Set(span->last_page(), span);
  }
  ASSERT(Check());
  return span;
}

void PageHeap::Delete(Span* span, size_t objects_per_span) {
  ASSERT(GetMemoryTag(span->start_address()) == tag_);
  info_.RecordFree(span->first_page(), span->num_pages(), objects_per_span);
  ASSERT(Check());
  ASSERT(span->location() == Span::IN_USE);
  ASSERT(!span->sampled());
  ASSERT(span->num_pages() > Length(0));
  ASSERT(pagemap_->GetDescriptor(span->first_page()) == span);
  ASSERT(pagemap_->GetDescriptor(span->last_page()) == span);
  span->set_location(Span::ON_NORMAL_FREELIST);
  MergeIntoFreeList(span);  // Coalesces if possible
  ASSERT(Check());
}

void PageHeap::MergeIntoFreeList(Span* span) {
  ASSERT(span->location() != Span::IN_USE);
  span->set_freelist_added_time(absl::base_internal::CycleClock::Now());

  // Coalesce -- we guarantee that "p" != 0, so no bounds checking
  // necessary.  We do not bother resetting the stale pagemap
  // entries for the pieces we are merging together because we only
  // care about the pagemap entries for the boundaries.
  //
  // Note that only similar spans are merged together.  For example,
  // we do not coalesce "returned" spans with "normal" spans.
  const PageId p = span->first_page();
  const Length n = span->num_pages();
  Span* prev = pagemap_->GetDescriptor(p - Length(1));
  if (prev != nullptr && prev->location() == span->location()) {
    // Merge preceding span into this span
    ASSERT(prev->last_page() + Length(1) == p);
    const Length len = prev->num_pages();
    span->AverageFreelistAddedTime(prev);
    RemoveFromFreeList(prev);
    Span::Delete(prev);
    span->set_first_page(span->first_page() - len);
    span->set_num_pages(span->num_pages() + len);
    pagemap_->Set(span->first_page(), span);
  }
  Span* next = pagemap_->GetDescriptor(p + n);
  if (next != nullptr && next->location() == span->location()) {
    // Merge next span into this span
    ASSERT(next->first_page() == p + n);
    const Length len = next->num_pages();
    span->AverageFreelistAddedTime(next);
    RemoveFromFreeList(next);
    Span::Delete(next);
    span->set_num_pages(span->num_pages() + len);
    pagemap_->Set(span->last_page(), span);
  }

  PrependToFreeList(span);
}

void PageHeap::PrependToFreeList(Span* span) {
  ASSERT(span->location() != Span::IN_USE);
  SpanListPair* list = (span->num_pages() < kMaxPages)
                           ? &free_[span->num_pages().raw_num()]
                           : &large_;
  if (span->location() == Span::ON_NORMAL_FREELIST) {
    stats_.free_bytes += span->bytes_in_span();
    list->normal.prepend(span);
  } else {
    stats_.unmapped_bytes += span->bytes_in_span();
    list->returned.prepend(span);
  }
}

void PageHeap::RemoveFromFreeList(Span* span) {
  ASSERT(span->location() != Span::IN_USE);
  SpanListPair* list = (span->num_pages() < kMaxPages)
                           ? &free_[span->num_pages().raw_num()]
                           : &large_;
  if (span->location() == Span::ON_NORMAL_FREELIST) {
    stats_.free_bytes -= span->bytes_in_span();
    list->normal.remove(span);
  } else {
    stats_.unmapped_bytes -= span->bytes_in_span();
    list->returned.remove(span);
  }
}

Length PageHeap::ReleaseLastNormalSpan(SpanListPair* slist) {
  Span* s = slist->normal.last();
  ASSERT(s->location() == Span::ON_NORMAL_FREELIST);
  RemoveFromFreeList(s);

  // We're dropping very important and otherwise contended pageheap_lock around
  // call to potentially very slow syscall to release pages. Those syscalls can
  // be slow even with "advanced" things such as MADV_FREE{,ABLE} because they
  // have to walk actual page tables, and we sometimes deal with large spans,
  // which sometimes takes lots of time. Plus Linux grabs per-address space
  // mm_sem lock which could be extremely contended at times. So it is best if
  // we avoid holding one contended lock while waiting for another.
  //
  // Note, we set span location to in-use, because our span could be found via
  // pagemap in e.g. MergeIntoFreeList while we're not holding the lock. By
  // marking it in-use we prevent this possibility. So span is removed from free
  // list and marked "unmergable" and that guarantees safety during unlock-ful
  // release.
  //
  // Taking the span off the free list will make our stats reporting wrong if
  // another thread happens to try to measure memory usage during the release,
  // so we fix up the stats during the unlocked period.
  stats_.free_bytes += s->bytes_in_span();
  s->set_location(Span::IN_USE);
  pageheap_lock.Unlock();

  const Length n = s->num_pages();
  bool success = SystemRelease(s->start_address(), s->bytes_in_span());

  pageheap_lock.Lock();
  if (ABSL_PREDICT_TRUE(success)) {
    stats_.free_bytes -= s->bytes_in_span();
    s->set_location(Span::ON_RETURNED_FREELIST);
  } else {
    s->set_location(Span::ON_NORMAL_FREELIST);
  }
  MergeIntoFreeList(s);  // Coalesces if possible.
  return n;
}

Length PageHeap::ReleaseAtLeastNPages(Length num_pages) {
  Length released_pages;
  Length prev_released_pages = Length::max() + Length(1);

  // Round robin through the lists of free spans, releasing the last
  // span in each list.  Stop after releasing at least num_pages.
  while (released_pages < num_pages) {
    if (released_pages == prev_released_pages) {
      // Last iteration of while loop made no progress.
      break;
    }
    prev_released_pages = released_pages;

    for (int i = 0; i < kMaxPages.raw_num() + 1 && released_pages < num_pages;
         i++, release_index_++) {
      if (release_index_ > kMaxPages.raw_num()) release_index_ = 0;
      SpanListPair* slist = (release_index_ == kMaxPages.raw_num())
                                ? &large_
                                : &free_[release_index_];
      if (!slist->normal.empty()) {
        Length released_len = ReleaseLastNormalSpan(slist);
        released_pages += released_len;
      }
    }
  }
  info_.RecordRelease(num_pages, released_pages);
  return released_pages;
}

void PageHeap::GetSmallSpanStats(SmallSpanStats* result) {
  for (int s = 0; s < kMaxPages.raw_num(); s++) {
    result->normal_length[s] = free_[s].normal.length();
    result->returned_length[s] = free_[s].returned.length();
  }
}

void PageHeap::GetLargeSpanStats(LargeSpanStats* result) {
  result->spans = 0;
  result->normal_pages = Length(0);
  result->returned_pages = Length(0);
  for (Span* s : large_.normal) {
    result->normal_pages += s->num_pages();
    result->spans++;
  }
  for (Span* s : large_.returned) {
    result->returned_pages += s->num_pages();
    result->spans++;
  }
}

bool PageHeap::GrowHeap(Length n) {
  if (n > Length::max()) return false;
  auto [ptr, actual_size] = SystemAlloc(n.in_bytes(), kPageSize, tag_);
  if (ptr == nullptr) return false;
  n = BytesToLengthFloor(actual_size);

  stats_.system_bytes += actual_size;
  const PageId p = PageIdContaining(ptr);
  ASSERT(p > PageId{0});

  // If we have already a lot of pages allocated, just pre allocate a bunch of
  // memory for the page map. This prevents fragmentation by pagemap metadata
  // when a program keeps allocating and freeing large blocks.

  // Make sure pagemap has entries for all of the new pages.
  // Plus ensure one before and one after so coalescing code
  // does not need bounds-checking.
  if (ABSL_PREDICT_TRUE(pagemap_->Ensure(p - Length(1), n + Length(2)))) {
    // Pretend the new area is allocated and then return it to cause
    // any necessary coalescing to occur.
    Span* span = Span::New(p, n);
    RecordSpan(span);
    span->set_location(Span::ON_RETURNED_FREELIST);
    MergeIntoFreeList(span);
    ASSERT(Check());
    return true;
  } else {
    // We could not allocate memory within the pagemap.
    // Note the following leaks virtual memory, but at least it gets rid of
    // the underlying physical memory.  If SystemRelease fails, there's little
    // we can do (we couldn't allocate for Ensure), but we have the consolation
    // that the memory has not been touched (so it is likely not populated).
    (void)SystemRelease(ptr, actual_size);
    return false;
  }
}

bool PageHeap::Check() {
  ASSERT(free_[0].normal.empty());
  ASSERT(free_[0].returned.empty());
  return true;
}

void PageHeap::PrintInPbtxt(PbtxtRegion* region) {
  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  SmallSpanStats small;
  GetSmallSpanStats(&small);
  LargeSpanStats large;
  GetLargeSpanStats(&large);

  struct Helper {
    static void RecordAges(PageAgeHistograms* ages, const SpanListPair& pair) {
      for (const Span* s : pair.normal) {
        ages->RecordRange(s->num_pages(), false, s->freelist_added_time());
      }

      for (const Span* s : pair.returned) {
        ages->RecordRange(s->num_pages(), true, s->freelist_added_time());
      }
    }
  };

  PageAgeHistograms ages(absl::base_internal::CycleClock::Now());
  for (int s = 0; s < kMaxPages.raw_num(); ++s) {
    Helper::RecordAges(&ages, free_[s]);
  }
  Helper::RecordAges(&ages, large_);
  PrintStatsInPbtxt(region, small, large, ages);
  // We do not collect info_.PrintInPbtxt for now.
}

void PageHeap::Print(Printer* out) {
  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  SmallSpanStats small;
  GetSmallSpanStats(&small);
  LargeSpanStats large;
  GetLargeSpanStats(&large);
  PrintStats("PageHeap", out, stats_, small, large, true);

  struct Helper {
    static void RecordAges(PageAgeHistograms* ages, const SpanListPair& pair) {
      for (const Span* s : pair.normal) {
        ages->RecordRange(s->num_pages(), false, s->freelist_added_time());
      }

      for (const Span* s : pair.returned) {
        ages->RecordRange(s->num_pages(), true, s->freelist_added_time());
      }
    }
  };

  PageAgeHistograms ages(absl::base_internal::CycleClock::Now());
  for (int s = 0; s < kMaxPages.raw_num(); ++s) {
    Helper::RecordAges(&ages, free_[s]);
  }
  Helper::RecordAges(&ages, large_);
  ages.Print("PageHeap", out);

  info_.Print(out);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
