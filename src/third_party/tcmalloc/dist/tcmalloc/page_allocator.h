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

#ifndef TCMALLOC_PAGE_ALLOCATOR_H_
#define TCMALLOC_PAGE_ALLOCATOR_H_

#include <stddef.h>

#include <array>
#include <cstdint>
#include <limits>

#include "absl/base/attributes.h"
#include "absl/base/thread_annotations.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_page_aware_allocator.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/page_allocator_interface.h"
#include "tcmalloc/page_heap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

class PageAllocator {
 public:
  PageAllocator();
  ~PageAllocator() = delete;
  // Allocate a run of "n" pages.  Returns zero if out of memory.
  // Caller should not pass "n == 0" -- instead, n should have
  // been rounded up already.
  //
  // Any address in the returned Span is guaranteed to satisfy
  // GetMemoryTag(addr) == "tag".
  Span* New(Length n, SpanAllocInfo span_alloc_info, MemoryTag tag)
      ABSL_LOCKS_EXCLUDED(pageheap_lock);

  // As New, but the returned span is aligned to a <align>-page boundary.
  // <align> must be a power of two.
  Span* NewAligned(Length n, Length align, SpanAllocInfo span_alloc_info,
                   MemoryTag tag) ABSL_LOCKS_EXCLUDED(pageheap_lock);

  // Delete the span "[p, p+n-1]".
  // REQUIRES: span was returned by earlier call to New() with the same value of
  //           "tag" and has not yet been deleted.
  void Delete(Span* span, size_t objects_per_span, MemoryTag tag)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  BackingStats stats() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  void GetSmallSpanStats(SmallSpanStats* result)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  void GetLargeSpanStats(LargeSpanStats* result)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Try to release at least num_pages for reuse by the OS.  Returns
  // the actual number of pages released, which may be less than
  // num_pages if there weren't enough pages to release. The result
  // may also be larger than num_pages since page_heap might decide to
  // release one large range instead of fragmenting it into two
  // smaller released and unreleased ranges.
  Length ReleaseAtLeastNPages(Length num_pages)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Prints stats about the page heap to *out.
  void Print(Printer* out, MemoryTag tag) ABSL_LOCKS_EXCLUDED(pageheap_lock);
  void PrintInPbtxt(PbtxtRegion* region, MemoryTag tag)
      ABSL_LOCKS_EXCLUDED(pageheap_lock);

  enum LimitKind { kSoft, kHard, kNumLimits };
  void set_limit(size_t limit, LimitKind limit_kind)
      ABSL_LOCKS_EXCLUDED(pageheap_lock);
  int64_t limit(LimitKind limit_kind) const ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    TC_ASSERT_LT(limit_kind, kNumLimits);
    PageHeapSpinLockHolder h;
    return limits_[limit_kind];
  }

  int64_t limit_hits(LimitKind limit_kind) const
      ABSL_LOCKS_EXCLUDED(pageheap_lock);

  int64_t successful_shrinks_after_limit_hit(LimitKind limit_kind) const
      ABSL_LOCKS_EXCLUDED(pageheap_lock);

  // If we have a usage limit set, ensure we're not violating it from our latest
  // allocation.
  void ShrinkToUsageLimit(Length n)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  const PageAllocInfo& info(MemoryTag tag) const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  enum Algorithm {
    PAGE_HEAP = 0,
    HPAA = 1,
  };

  Algorithm algorithm() const { return alg_; }

  struct PeakStats {
    size_t backed_bytes;
    size_t sampled_application_bytes;
  };

  PeakStats peak_stats() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    return PeakStats{peak_backed_bytes_, peak_sampled_application_bytes_};
  }

 private:
  bool ShrinkHardBy(Length page, LimitKind limit_kind)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  using Interface =
      std::conditional<huge_page_allocator_internal::kUnconditionalHPAA,
                       HugePageAwareAllocator, PageAllocatorInterface>::type;

  ABSL_ATTRIBUTE_RETURNS_NONNULL Interface* impl(MemoryTag tag) const;

  size_t active_numa_partitions() const;

  static constexpr size_t kNumHeaps = kNumaPartitions + 2;

  union Choices {
    Choices() : dummy(0) {}
    ~Choices() {}
    int dummy;
    PageHeap ph;
    HugePageAwareAllocator hpaa;
  } choices_[kNumHeaps];
  std::array<Interface*, kNumaPartitions> normal_impl_;
  Interface* sampled_impl_;
  Interface* cold_impl_;
  Algorithm alg_;
  bool has_cold_impl_;

  // Max size of backed spans we will attempt to maintain.
  // Crash if we can't maintain below limits_[kHard], which is guaranteed to be
  // higher than limits_[kSoft].
  size_t limits_[kNumLimits] = {std::numeric_limits<size_t>::max(),
                                std::numeric_limits<size_t>::max()};

  // The number of times the limit has been hit.
  int64_t limit_hits_[kNumLimits]{0};
  // Number of times we succeeded in shrinking the memory usage to be less than
  // or at the limit.
  int64_t successful_shrinks_after_limit_hit_[kNumLimits]{0};

  // peak_backed_bytes_ tracks the maximum number of pages backed (with physical
  // memory) in the page heap and metadata.
  //
  // peak_sampled_application_bytes_ is a snapshot of
  // tc_globals.sampled_objects_size_ at the time of the most recent
  // peak_backed_bytes_ high water mark.  While this is an estimate of true
  // in-use by application demand, it is generally accurate at scale and
  // requires minimal work to compute.
  size_t peak_backed_bytes_{0};
  size_t peak_sampled_application_bytes_{0};
};

inline PageAllocator::Interface* PageAllocator::impl(MemoryTag tag) const {
  if constexpr (huge_page_allocator_internal::kUnconditionalHPAA) {
    TC_ASSERT_EQ(alg_, HPAA);
  }

  switch (tag) {
    case MemoryTag::kNormalP0:
      return normal_impl_[0];
    case MemoryTag::kNormalP1:
      return normal_impl_[1];
    case MemoryTag::kSampled:
      return sampled_impl_;
    case MemoryTag::kCold:
      return cold_impl_;
    default:
      ASSUME(false);
      __builtin_unreachable();
  }
}

inline Span* PageAllocator::New(Length n, SpanAllocInfo span_alloc_info,
                                MemoryTag tag) {
  return impl(tag)->New(n, span_alloc_info);
}

inline Span* PageAllocator::NewAligned(Length n, Length align,
                                       SpanAllocInfo span_alloc_info,
                                       MemoryTag tag) {
  return impl(tag)->NewAligned(n, align, span_alloc_info);
}

inline void PageAllocator::Delete(Span* span, size_t objects_per_span,
                                  MemoryTag tag) {
  impl(tag)->Delete(span, objects_per_span);
}

inline BackingStats PageAllocator::stats() const {
  BackingStats ret = normal_impl_[0]->stats();
  for (int partition = 1; partition < active_numa_partitions(); partition++) {
    ret += normal_impl_[partition]->stats();
  }
  ret += sampled_impl_->stats();
  if (has_cold_impl_) {
    ret += cold_impl_->stats();
  }
  return ret;
}

inline void PageAllocator::GetSmallSpanStats(SmallSpanStats* result) {
  SmallSpanStats normal, sampled;
  for (int partition = 0; partition < active_numa_partitions(); partition++) {
    SmallSpanStats part_stats;
    normal_impl_[partition]->GetSmallSpanStats(&part_stats);
    normal += part_stats;
  }
  sampled_impl_->GetSmallSpanStats(&sampled);
  *result = normal + sampled;
  if (has_cold_impl_) {
    SmallSpanStats cold;
    cold_impl_->GetSmallSpanStats(&cold);
    *result += cold;
  }
}

inline void PageAllocator::GetLargeSpanStats(LargeSpanStats* result) {
  LargeSpanStats normal, sampled;
  for (int partition = 0; partition < active_numa_partitions(); partition++) {
    LargeSpanStats part_stats;
    normal_impl_[partition]->GetLargeSpanStats(&part_stats);
    normal += part_stats;
  }
  sampled_impl_->GetLargeSpanStats(&sampled);
  *result = normal + sampled;
  if (has_cold_impl_) {
    LargeSpanStats cold;
    cold_impl_->GetLargeSpanStats(&cold);
    *result = *result + cold;
  }
}

inline Length PageAllocator::ReleaseAtLeastNPages(Length num_pages) {
  Length released;
  // TODO(ckennelly): Refine this policy.  Cold data should be the most
  // resilient to not being on huge pages.
  if (has_cold_impl_) {
    released = cold_impl_->ReleaseAtLeastNPages(num_pages);
  }
  for (int partition = 0; partition < active_numa_partitions(); partition++) {
    released += normal_impl_[partition]->ReleaseAtLeastNPages(
        num_pages > released ? num_pages - released : Length(0));
  }

  released += sampled_impl_->ReleaseAtLeastNPages(
      num_pages > released ? num_pages - released : Length(0));
  return released;
}

inline void PageAllocator::Print(Printer* out, MemoryTag tag) {
  if (tag == MemoryTag::kCold && !has_cold_impl_) {
    return;
  }

  const absl::string_view label = MemoryTagToLabel(tag);
  if (tag != MemoryTag::kNormal) {
    out->printf("\n>>>>>>> Begin %s page allocator <<<<<<<\n", label);
  }
  impl(tag)->Print(out);
  if (tag != MemoryTag::kNormal) {
    out->printf(">>>>>>> End %s page allocator <<<<<<<\n", label);
  }
}

inline void PageAllocator::PrintInPbtxt(PbtxtRegion* region, MemoryTag tag) {
  if (tag == MemoryTag::kCold && !has_cold_impl_) {
    return;
  }

  PbtxtRegion pa = region->CreateSubRegion("page_allocator");
  pa.PrintRaw("tag", MemoryTagToLabel(tag));
  impl(tag)->PrintInPbtxt(&pa);
}

inline void PageAllocator::set_limit(size_t limit, LimitKind limit_kind) {
  PageHeapSpinLockHolder h;
  limits_[limit_kind] = limit;
  if (limits_[kHard] < limits_[kSoft]) {
    // Soft limit can not be higher than hard limit.
    limits_[kSoft] = limits_[kHard];
  }
  // Attempt to shed memory to get below the new limit.
  ShrinkToUsageLimit(Length(0));
}

inline int64_t PageAllocator::limit_hits(LimitKind limit_kind) const {
  TC_ASSERT_LT(limit_kind, kNumLimits);
  PageHeapSpinLockHolder l;
  return limit_hits_[limit_kind];
}

inline int64_t PageAllocator::successful_shrinks_after_limit_hit(
    LimitKind limit_kind) const {
  TC_ASSERT_LT(limit_kind, kNumLimits);
  PageHeapSpinLockHolder l;
  return successful_shrinks_after_limit_hit_[limit_kind];
}

inline const PageAllocInfo& PageAllocator::info(MemoryTag tag) const {
  return impl(tag)->info();
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_PAGE_ALLOCATOR_H_
