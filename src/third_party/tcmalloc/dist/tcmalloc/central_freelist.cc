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

#include "tcmalloc/central_freelist.h"

#include <stdint.h>

#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/prefetch.h"
#include "tcmalloc/page_heap.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace central_freelist_internal {

static MemoryTag MemoryTagFromSizeClass(size_t size_class) {
  if (IsExpandedSizeClass(size_class)) {
    return MemoryTag::kCold;
  }
  if (!tc_globals.numa_topology().numa_aware()) {
    return MemoryTag::kNormal;
  }
  return NumaNormalTag(size_class / kNumBaseClasses);
}

size_t StaticForwarder::class_to_size(int size_class) {
  return tc_globals.sizemap().class_to_size(size_class);
}

Length StaticForwarder::class_to_pages(int size_class) {
  return Length(tc_globals.sizemap().class_to_pages(size_class));
}

Span* StaticForwarder::MapObjectToSpan(const void* object) {
  const PageId p = PageIdContaining(object);
  Span* span = tc_globals.pagemap().GetExistingDescriptor(p);
  return span;
}

Span* StaticForwarder::AllocateSpan(int size_class, size_t objects_per_span,
                                    Length pages_per_span) {
  const MemoryTag tag = MemoryTagFromSizeClass(size_class);
  Span* span =
      tc_globals.page_allocator().New(pages_per_span, objects_per_span, tag);
  if (ABSL_PREDICT_FALSE(span == nullptr)) {
    return nullptr;
  }
  ASSERT(tag == GetMemoryTag(span->start_address()));
  ASSERT(span->num_pages() == pages_per_span);

  tc_globals.pagemap().RegisterSizeClass(span, size_class);
  return span;
}

static void ReturnSpansToPageHeap(MemoryTag tag, absl::Span<Span*> free_spans,
                                  size_t objects_per_span)
    ABSL_LOCKS_EXCLUDED(pageheap_lock) {
  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  for (Span* const free_span : free_spans) {
    ASSERT(tag == GetMemoryTag(free_span->start_address()));
    tc_globals.page_allocator().Delete(free_span, objects_per_span, tag);
  }
}

void StaticForwarder::DeallocateSpans(int size_class, size_t objects_per_span,
                                      absl::Span<Span*> free_spans) {
  // Unregister size class doesn't require holding any locks.
  for (Span* const free_span : free_spans) {
    ASSERT(IsNormalMemory(free_span->start_address()) ||
           IsColdMemory(free_span->start_address()));
    tc_globals.pagemap().UnregisterSizeClass(free_span);

    // Before taking pageheap_lock, prefetch the PageTrackers these spans are
    // on.
    //
    // Small-but-slow does not use the HugePageAwareAllocator (by default), so
    // do not prefetch on this config.
#ifndef TCMALLOC_SMALL_BUT_SLOW
    const PageId p = free_span->first_page();

    // In huge_page_filler.h, we static_assert that PageTracker's key elements
    // for deallocation are within the first two cachelines.
    void* pt = tc_globals.pagemap().GetHugepage(p);
    // Prefetch for writing, as we will issue stores to the PageTracker
    // instance.
    PrefetchW(pt);
    PrefetchW(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pt) +
                                      ABSL_CACHELINE_SIZE));
#endif  // TCMALLOC_SMALL_BUT_SLOW
  }

  const MemoryTag tag = MemoryTagFromSizeClass(size_class);
  ReturnSpansToPageHeap(tag, free_spans, objects_per_span);
}

}  // namespace central_freelist_internal
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
