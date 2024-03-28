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

#ifndef TCMALLOC_CENTRAL_FREELIST_H_
#define TCMALLOC_CENTRAL_FREELIST_H_

#include <stddef.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/numeric/bits.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/hinted_tracker_lists.h"
#include "tcmalloc/internal/atomic_stats_counter.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/span.h"
#include "tcmalloc/span_stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

namespace central_freelist_internal {

// StaticForwarder provides access to the PageMap and page heap.
//
// This is a class, rather than namespaced globals, so that it can be mocked for
// testing.
class StaticForwarder {
 public:
  static uint32_t max_span_cache_size() {
    return Parameters::max_span_cache_size();
  }

  static size_t class_to_size(int size_class);
  static Length class_to_pages(int size_class);
  static void MapObjectsToSpans(absl::Span<void*> batch, Span** spans);
  static Span* AllocateSpan(int size_class, SpanAllocInfo span_alloc_info,
                            Length pages_per_span)
      ABSL_LOCKS_EXCLUDED(pageheap_lock);
  static void DeallocateSpans(int size_class, size_t objects_per_span,
                              absl::Span<Span*> free_spans)
      ABSL_LOCKS_EXCLUDED(pageheap_lock);
};

// Specifies number of nonempty_ lists that keep track of non-empty spans.
static constexpr size_t kNumLists = 8;

// Specifies the threshold for number of objects per span. The threshold is
// used to consider a span sparsely- vs. densely-accessed.
static constexpr size_t kFewObjectsAllocMaxLimit = 16;

// Data kept per size-class in central cache.
template <typename ForwarderT>
class CentralFreeList {
 public:
  using Forwarder = ForwarderT;

  constexpr CentralFreeList()
      : lock_(absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY),
        size_class_(0),
        object_size_(0),
        objects_per_span_(0),
        first_nonempty_index_(0),
        pages_per_span_(0),
        nonempty_(),
        use_all_buckets_for_few_object_spans_(false) {}

  CentralFreeList(const CentralFreeList&) = delete;
  CentralFreeList& operator=(const CentralFreeList&) = delete;

  void Init(size_t size_class, bool use_all_buckets_for_few_object_spans)
      ABSL_LOCKS_EXCLUDED(lock_);

  // These methods all do internal locking.

  // Insert batch into the central freelist.
  // REQUIRES: batch.size() > 0 && batch.size() <= kMaxObjectsToMove.
  void InsertRange(absl::Span<void*> batch) ABSL_LOCKS_EXCLUDED(lock_);

  // Fill a prefix of batch[0..N-1] with up to N elements removed from central
  // freelist.  Return the number of elements removed.
  ABSL_MUST_USE_RESULT int RemoveRange(void** batch, int N)
      ABSL_LOCKS_EXCLUDED(lock_);

  // Returns the number of free objects in cache.
  size_t length() const { return static_cast<size_t>(counter_.value()); }

  // Returns the memory overhead (internal fragmentation) attributable
  // to the freelist.  This is memory lost when the size of elements
  // in a freelist doesn't exactly divide the page-size (an 8192-byte
  // page full of 5-byte objects would have 2 bytes memory overhead).
  size_t OverheadBytes() const;

  // Returns number of live spans currently in the nonempty_[n] list.
  // REQUIRES: n >= 0 && n < kNumLists.
  size_t NumSpansInList(int n) ABSL_LOCKS_EXCLUDED(lock_);
  SpanStats GetSpanStats() const;

  // Reports span utilization histogram stats.
  void PrintSpanUtilStats(Printer* out);
  void PrintSpanUtilStatsInPbtxt(PbtxtRegion* region);

  // Get number of spans in the histogram bucket. We record spans in the
  // histogram indexed by absl::bit_width(allocated). So, instead of using the
  // absolute number of allocated objects, it uses absl::bit_width(allocated),
  // passed as <bitwidth>, to index and return the number of spans in the
  // histogram.
  size_t NumSpansWith(uint16_t bitwidth) const;

  Forwarder& forwarder() { return forwarder_; }

 private:
  // Release an object to spans.
  // Returns object's span if it become completely free.
  Span* ReleaseToSpans(void* object, Span* span, size_t object_size,
                       uint32_t size_reciprocal, uint32_t max_span_cache_size)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // Populate cache by fetching from the page heap.
  // May temporarily release lock_.
  // Fill a prefix of batch[0..N-1] with up to N elements removed from central
  // freelist. Returns the number of elements removed.
  int Populate(void** batch, int N) ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // Allocate a span from the forwarder.
  Span* AllocateSpan();

  // Parses nonempty_ lists and returns span from the list with the lowest
  // possible index.
  // Returns the span if one exists in the nonempty_ lists. Else, returns
  // nullptr.
  Span* FirstNonEmptySpan() ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // Returns first index to the nonempty_ lists that may record spans.
  uint8_t GetFirstNonEmptyIndex() const;

  // Returns index into nonempty_ based on the number of allocated objects for
  // the span. Depending on the number of objects per span, either the absolute
  // number of allocated objects or the absl::bit_width(allocated), passed as
  // bitwidth, is used to to calculate the list index.
  uint8_t IndexFor(uint16_t allocated, uint8_t bitwidth);

  // Records span utilization in objects_to_span_ map. Instead of using the
  // absolute number of allocated objects, it uses absl::bit_width(allocated),
  // passed as <bitwidth>, to index this map.
  //
  // If increase is set to true, includes the span by incrementing the count
  // in the map. Otherwise, removes the span by decrementing the count in
  // the map.
  void RecordSpanUtil(uint8_t bitwidth, bool increase)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    ASSUME(bitwidth > 0);
    // Updates to objects_to_span_ are guarded by lock_, so writes may be
    // performed using LossyAdd.
    objects_to_spans_[bitwidth - 1].LossyAdd(increase ? 1 : -1);
  }

  // This lock protects all the mutable data members.
  absl::base_internal::SpinLock lock_;

  size_t size_class_;  // My size class (immutable after Init())
  size_t object_size_;
  size_t objects_per_span_;
  // Size reciprocal is used to replace division with multiplication when
  // computing object indices in the Span bitmap.
  uint32_t size_reciprocal_ = 0;
  // Hint used for parsing through the nonempty_ lists. This prevents us from
  // parsing the lists with an index starting zero, if the lowest possible index
  // is higher than that.
  size_t first_nonempty_index_;
  Length pages_per_span_;

  size_t num_spans() const {
    size_t requested = num_spans_requested_.value();
    size_t returned = num_spans_returned_.value();
    if (requested < returned) return 0;
    return (requested - returned);
  }

  void RecordSpanAllocated() ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    counter_.LossyAdd(objects_per_span_);
    num_spans_requested_.LossyAdd(1);
  }

  void RecordMultiSpansDeallocated(size_t num_spans_returned)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    counter_.LossyAdd(-num_spans_returned * objects_per_span_);
    num_spans_returned_.LossyAdd(num_spans_returned);
  }

  void UpdateObjectCounts(int num) ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    counter_.LossyAdd(num);
  }

  // The followings are kept as a StatsCounter so that they can read without
  // acquiring a lock. Updates to these variables are guarded by lock_
  // so writes are performed using LossyAdd for speed, the lock still
  // guarantees accuracy.

  // Num free objects in cache entry
  StatsCounter counter_;

  StatsCounter num_spans_requested_;
  StatsCounter num_spans_returned_;

  // Records histogram of span utilization.
  //
  // Each bucket in the histogram records number of live spans with
  // corresponding number of allocated objects. Instead of using the absolute
  // value of number of allocated objects, we use absl::bit_width(allocated) to
  // index this map. A bucket in the histogram corresponds to power-of-two
  // number of objects. That is, bucket N tracks number of spans with allocated
  // objects < 2^(N+1). For instance, objects_to_spans_ map tracks number of
  // spans with allocated objects in the range [a,b), indexed as: [1,2) in
  // objects_to_spans_[0], [2,4) in objects_to_spans_[1], [4, 8) in
  // objects_to_spans_[2] and so on. We can query the objects_to_spans_ map
  // using NumSpansWith(bitwidth) to obtain the number of spans associated
  // with the corresponding bucket in the histogram.
  //
  // As the actual value of objects_per_span_ is not known at compile time, we
  // use maximum value that it can be to initialize this hashmap, and
  // kSpanUtilBucketCapacity determines this value. We also check during Init
  // that absl::bit_width(objects_per_span_) is indeed less than or equal to
  // kSpanUtilBucketCapacity.
  //
  // We disable collection of histogram stats for TCMalloc small-but-slow due to
  // performance issues. See b/227362263.
  static constexpr size_t kSpanUtilBucketCapacity = 16;
  StatsCounter objects_to_spans_[kSpanUtilBucketCapacity];

  // Non-empty lists that distinguish spans based on the number of objects
  // allocated from them. As we prioritize spans, spans may be added to any of
  // the kNumLists nonempty_ lists based on their allocated objects. If span
  // prioritization is disabled, we add spans to the nonempty_[kNumlists-1]
  // list, leaving other lists unused.
  //
  // We do not enable multiple nonempty lists for small-but-slow yet due to
  // performance issues. See b/227362263.
#ifdef TCMALLOC_INTERNAL_SMALL_BUT_SLOW
  SpanList nonempty_ ABSL_GUARDED_BY(lock_);
#else
  HintedTrackerLists<Span, kNumLists> nonempty_ ABSL_GUARDED_BY(lock_);
#endif
  bool use_all_buckets_for_few_object_spans_;

  ABSL_ATTRIBUTE_NO_UNIQUE_ADDRESS Forwarder forwarder_;
};

// Like a constructor and hence we disable thread safety analysis.
template <class Forwarder>
inline void CentralFreeList<Forwarder>::Init(
    size_t size_class,
    bool use_all_buckets_for_few_object_spans) ABSL_NO_THREAD_SAFETY_ANALYSIS {
  size_class_ = size_class;
  object_size_ = forwarder_.class_to_size(size_class);
  if (object_size_ == 0) {
    return;
  }
  pages_per_span_ = forwarder_.class_to_pages(size_class);
  objects_per_span_ =
      pages_per_span_.in_bytes() / (object_size_ ? object_size_ : 1);
  size_reciprocal_ = Span::CalcReciprocal(object_size_);
  use_all_buckets_for_few_object_spans_ =
      use_all_buckets_for_few_object_spans &&
      objects_per_span_ <= 2 * kNumLists;

  // Records nonempty_ list index associated with the span with
  // objects_per_span_ number of allocated objects. Refer to the comment in
  // IndexFor(...) below for a detailed description.
  first_nonempty_index_ =
      use_all_buckets_for_few_object_spans_
          ? (kNumLists + 1 >= objects_per_span_
                 ? kNumLists + 1 - objects_per_span_
                 : 0)
          : kNumLists -
                std::min<size_t>(absl::bit_width(objects_per_span_), kNumLists);

  TC_ASSERT(absl::bit_width(objects_per_span_) <= kSpanUtilBucketCapacity);
}

template <class Forwarder>
inline Span* CentralFreeList<Forwarder>::ReleaseToSpans(
    void* object, Span* span, size_t object_size, uint32_t size_reciprocal,
    uint32_t max_span_cache_size) {
  if (ABSL_PREDICT_FALSE(span->FreelistEmpty(object_size))) {
#ifdef TCMALLOC_INTERNAL_SMALL_BUT_SLOW
    nonempty_.prepend(span);
#else
    const uint8_t index = GetFirstNonEmptyIndex();
    nonempty_.Add(span, index);
    span->set_nonempty_index(index);
#endif
  }

#ifdef TCMALLOC_INTERNAL_SMALL_BUT_SLOW
  // We maintain a single nonempty list for small-but-slow. Also, we do not
  // collect histogram stats due to performance issues.
  if (ABSL_PREDICT_TRUE(span->FreelistPush(object, object_size, size_reciprocal,
                                           max_span_cache_size))) {
    return nullptr;
  }
  nonempty_.remove(span);
  return span;
#else
  const uint8_t prev_index = span->nonempty_index();
  const uint16_t prev_allocated = span->Allocated();
  const uint8_t prev_bitwidth = absl::bit_width(prev_allocated);
  if (ABSL_PREDICT_FALSE(!span->FreelistPush(
          object, object_size, size_reciprocal, max_span_cache_size))) {
    // Update the histogram as the span is full and will be removed from the
    // nonempty_ list.
    RecordSpanUtil(prev_bitwidth, /*increase=*/false);
    nonempty_.Remove(span, prev_index);
    return span;
  }
  // As the objects are being added to the span, its utilization might change.
  // We remove the stale utilization from the histogram and add the new
  // utilization to the histogram after we release objects to the span.
  uint16_t cur_allocated = prev_allocated - 1;
  TC_ASSERT_EQ(cur_allocated, span->Allocated());
  const uint8_t cur_bitwidth = absl::bit_width(cur_allocated);
  if (cur_bitwidth != prev_bitwidth) {
    RecordSpanUtil(prev_bitwidth, /*increase=*/false);
    RecordSpanUtil(cur_bitwidth, /*increase=*/true);
  }
  // If span allocation changes so that it moved to a different nonempty_ list,
  // we remove it from the previous list and add it to the desired list indexed
  // by cur_index.
  const uint8_t cur_index = IndexFor(cur_allocated, cur_bitwidth);
  if (cur_index != prev_index) {
    nonempty_.Remove(span, prev_index);
    nonempty_.Add(span, cur_index);
    span->set_nonempty_index(cur_index);
  }
  return nullptr;
#endif
}

template <class Forwarder>
inline Span* CentralFreeList<Forwarder>::FirstNonEmptySpan() {
  // Scan nonempty_ lists in the range [first_nonempty_index_, kNumLists) and
  // return the span from a non-empty list if one exists. If all the lists are
  // empty, return nullptr.
#ifdef TCMALLOC_INTERNAL_SMALL_BUT_SLOW
  if (ABSL_PREDICT_FALSE(nonempty_.empty())) {
    return nullptr;
  }
  return nonempty_.first();
#else
  return nonempty_.PeekLeast(GetFirstNonEmptyIndex());
#endif
}

template <class Forwarder>
inline uint8_t CentralFreeList<Forwarder>::GetFirstNonEmptyIndex() const {
  return first_nonempty_index_;
}

template <class Forwarder>
inline uint8_t CentralFreeList<Forwarder>::IndexFor(uint16_t allocated,
                                                    uint8_t bitwidth) {
  // We would like to index into the nonempty_ list based on the number of
  // allocated objects from the span. Given a span with fewer allocated objects
  // (i.e. when it is more likely to be freed), we would like to map it to a
  // higher index in the nonempty_ list.
  //
  // The number of objects per span is less than or equal to 2 * kNumlists.
  // We index such spans by just the number of allocated objects.  When the
  // allocated objects are in the range [1, 8], then we map the spans to buckets
  // 7, 6, ... 0 respectively.  When the allocated objects are more than
  // kNumlists, then we map the span to bucket 0.
  ASSUME(allocated > 0);
  if (use_all_buckets_for_few_object_spans_) {
    if (allocated <= kNumLists) {
      return kNumLists - allocated;
    }
    return 0;
  }
  // Depending on the number of kNumLists and the number of objects per span, we
  // may have to clamp multiple buckets in index 0. It should be ok to do that
  // because it is less beneficial to differentiate between spans that have 128
  // vs 256 allocated objects, compared to those that have 16 vs 32 allocated
  // objects.
  //
  // Consider objects_per_span = 1024 and kNumLists = 8. The following examples
  // show spans with allocated objects in the range [a, b) indexed to the
  // nonempty_[idx] list using a notation [a, b) -> idx.
  // [1, 2) -> 7, [2, 4) -> 6, [4, 8) -> 5, [8, 16) -> 4, [16, 32) -> 3, [32,
  // 64) -> 2, [64, 128) -> 1, [128, 1024) -> 0.
  ASSUME(bitwidth > 0);
  const uint8_t offset = std::min<size_t>(bitwidth, kNumLists);
  const uint8_t index = kNumLists - offset;
  ASSUME(index < kNumLists);
  return index;
}

template <class Forwarder>
inline size_t CentralFreeList<Forwarder>::NumSpansInList(int n) {
  ASSUME(n >= 0);
  ASSUME(n < kNumLists);
  absl::base_internal::SpinLockHolder h(&lock_);
#ifdef TCMALLOC_INTERNAL_SMALL_BUT_SLOW
  return nonempty_.length();
#else
  return nonempty_.SizeOfList(n);
#endif
}

template <class Forwarder>
inline void CentralFreeList<Forwarder>::InsertRange(absl::Span<void*> batch) {
  TC_CHECK(!batch.empty());
  TC_CHECK_LE(batch.size(), kMaxObjectsToMove);
  Span* spans[kMaxObjectsToMove];
  // First, map objects to spans and prefetch spans outside of our mutex
  // (to reduce critical section size and cache misses).
  forwarder_.MapObjectsToSpans(batch, spans);

  if (objects_per_span_ == 1) {
    // If there is only 1 object per span, skip CentralFreeList entirely.
    forwarder_.DeallocateSpans(size_class_, objects_per_span_,
                               {spans, batch.size()});
    return;
  }

  // Safe to store free spans into freed up space in span array.
  const uint32_t max_span_cache_size = forwarder_.max_span_cache_size();
  Span** free_spans = spans;
  int free_count = 0;

  // Then, release all individual objects into spans under our mutex
  // and collect spans that become completely free.
  {
    // Use local copy of variables to ensure that they are not reloaded.
    size_t object_size = object_size_;
    uint32_t size_reciprocal = size_reciprocal_;
    absl::base_internal::SpinLockHolder h(&lock_);
    for (int i = 0; i < batch.size(); ++i) {
      Span* span = ReleaseToSpans(batch[i], spans[i], object_size,
                                  size_reciprocal, max_span_cache_size);
      if (ABSL_PREDICT_FALSE(span)) {
        free_spans[free_count] = span;
        free_count++;
      }
    }

    RecordMultiSpansDeallocated(free_count);
    UpdateObjectCounts(batch.size());
  }

  // Then, release all free spans into page heap under its mutex.
  if (ABSL_PREDICT_FALSE(free_count)) {
    forwarder_.DeallocateSpans(size_class_, objects_per_span_,
                               absl::MakeSpan(free_spans, free_count));
  }
}

template <class Forwarder>
inline int CentralFreeList<Forwarder>::RemoveRange(void** batch, int N) {
  ASSUME(N > 0);

  if (objects_per_span_ == 1) {
    // If there is only 1 object per span, skip CentralFreeList entirely.
    Span* span = AllocateSpan();
    if (ABSL_PREDICT_FALSE(span == nullptr)) {
      return 0;
    }
    batch[0] = span->start_address();
    return 1;
  }

  // Use local copy of variable to ensure that it is not reloaded.
  size_t object_size = object_size_;
  int result = 0;
  absl::base_internal::SpinLockHolder h(&lock_);

  do {
    Span* span = FirstNonEmptySpan();
    if (ABSL_PREDICT_FALSE(!span)) {
      result += Populate(batch + result, N - result);
      break;
    }

#ifdef TCMALLOC_INTERNAL_SMALL_BUT_SLOW
    // We do not collect histogram stats for small-but-slow.
    int here = span->FreelistPopBatch(batch + result, N - result, object_size);
    TC_ASSERT_GT(here, 0);
    if (span->FreelistEmpty(object_size)) {
      nonempty_.remove(span);
    }
#else
    const uint16_t prev_allocated = span->Allocated();
    const uint8_t prev_bitwidth = absl::bit_width(prev_allocated);
    const uint8_t prev_index = span->nonempty_index();
    int here = span->FreelistPopBatch(batch + result, N - result, object_size);
    TC_ASSERT_GT(here, 0);
    // As the objects are being popped from the span, its utilization might
    // change. So, we remove the stale utilization from the histogram here and
    // add it again once we pop the objects.
    const uint16_t cur_allocated = prev_allocated + here;
    TC_ASSERT_EQ(cur_allocated, span->Allocated());
    const uint8_t cur_bitwidth = absl::bit_width(cur_allocated);
    if (cur_bitwidth != prev_bitwidth) {
      RecordSpanUtil(prev_bitwidth, /*increase=*/false);
      RecordSpanUtil(cur_bitwidth, /*increase=*/true);
    }
    if (span->FreelistEmpty(object_size)) {
      nonempty_.Remove(span, prev_index);
    } else {
      // If span allocation changes so that it must be moved to a different
      // nonempty_ list, we remove it from the previous list and add it to the
      // desired list indexed by cur_index.
      const uint8_t cur_index = IndexFor(cur_allocated, cur_bitwidth);
      if (cur_index != prev_index) {
        nonempty_.Remove(span, prev_index);
        nonempty_.Add(span, cur_index);
        span->set_nonempty_index(cur_index);
      }
    }
#endif
    result += here;
  } while (result < N);
  UpdateObjectCounts(-result);
  return result;
}

// Fetch memory from the system and add to the central cache freelist.
template <class Forwarder>
inline int CentralFreeList<Forwarder>::Populate(void** batch, int N)
    ABSL_NO_THREAD_SAFETY_ANALYSIS {
  // Release central list lock while operating on pageheap
  // Note, this could result in multiple calls to populate each allocating
  // a new span and the pushing those partially full spans onto nonempty.
  lock_.Unlock();

  Span* span = AllocateSpan();
  if (ABSL_PREDICT_FALSE(span == nullptr)) {
    return 0;
  }

  int result = span->BuildFreelist(object_size_, objects_per_span_, batch, N,
                                   forwarder_.max_span_cache_size());
  TC_ASSERT_GT(result, 0);
  // This is a cheaper check than using FreelistEmpty().
  bool span_empty = result == objects_per_span_;

  lock_.Lock();

#ifdef TCMALLOC_INTERNAL_SMALL_BUT_SLOW
  // We do not collect histogram stats for small-but-slow. Moreover, we maintain
  // a single nonempty list to which we prepend the span.
  if (!span_empty) {
    nonempty_.prepend(span);
  }
#else
  // Update the histogram once we populate the span.
  const uint16_t allocated = result;
  TC_ASSERT_EQ(allocated, span->Allocated());
  const uint8_t bitwidth = absl::bit_width(allocated);
  RecordSpanUtil(bitwidth, /*increase=*/true);
  if (!span_empty) {
    const uint8_t index = IndexFor(allocated, bitwidth);
    nonempty_.Add(span, index);
    span->set_nonempty_index(index);
  }
#endif
  RecordSpanAllocated();
  return result;
}

template <class Forwarder>
Span* CentralFreeList<Forwarder>::AllocateSpan() {
  // Use number of objects per span as a proxy for estimating access density of
  // the span. If number of objects per span is higher than
  // kFewObjectsAllocMaxLimit threshold, we assume that the span would be
  // long-lived.
  const AccessDensityPrediction density =
      objects_per_span_ > kFewObjectsAllocMaxLimit
          ? AccessDensityPrediction::kDense
          : AccessDensityPrediction::kSparse;

  SpanAllocInfo info = {.objects_per_span = objects_per_span_,
                        .density = density};
  Span* span = forwarder_.AllocateSpan(size_class_, info, pages_per_span_);
  if (ABSL_PREDICT_FALSE(span == nullptr)) {
    TC_LOG("tcmalloc: allocation failed %v", pages_per_span_);
  }
  return span;
}

template <class Forwarder>
inline size_t CentralFreeList<Forwarder>::OverheadBytes() const {
  if (ABSL_PREDICT_FALSE(object_size_ == 0)) {
    return 0;
  }
  const size_t overhead_per_span = pages_per_span_.in_bytes() % object_size_;
  return num_spans() * overhead_per_span;
}

template <class Forwarder>
inline SpanStats CentralFreeList<Forwarder>::GetSpanStats() const {
  SpanStats stats;
  if (ABSL_PREDICT_FALSE(objects_per_span_ == 0)) {
    return stats;
  }
  stats.num_spans_requested = static_cast<size_t>(num_spans_requested_.value());
  stats.num_spans_returned = static_cast<size_t>(num_spans_returned_.value());
  stats.obj_capacity = stats.num_live_spans() * objects_per_span_;
  return stats;
}

template <class Forwarder>
inline size_t CentralFreeList<Forwarder>::NumSpansWith(
    uint16_t bitwidth) const {
  TC_ASSERT_GT(bitwidth, 0);
  const int bucket = bitwidth - 1;
  return objects_to_spans_[bucket].value();
}

template <class Forwarder>
inline void CentralFreeList<Forwarder>::PrintSpanUtilStats(Printer* out) {
  out->printf("class %3d [ %8zu bytes ] : ", size_class_, object_size_);
  for (size_t i = 1; i <= kSpanUtilBucketCapacity; ++i) {
    out->printf("%6zu < %zu", NumSpansWith(i), 1 << i);
    if (i < kSpanUtilBucketCapacity) {
      out->printf(",");
    }
  }
  out->printf("\n");
  out->printf("class %3d [ %8zu bytes ] : ", size_class_, object_size_);
  for (size_t i = 0; i < kNumLists; ++i) {
    out->printf("%6zu: %zu", i, NumSpansInList(i));
    if (i < kNumLists - 1) {
      out->printf(",");
    }
  }
  out->printf("\n");
}

template <class Forwarder>
inline void CentralFreeList<Forwarder>::PrintSpanUtilStatsInPbtxt(
    PbtxtRegion* region) {
  for (size_t i = 1; i <= kSpanUtilBucketCapacity; ++i) {
    PbtxtRegion histogram = region->CreateSubRegion("span_util_histogram");
    histogram.PrintI64("lower_bound", 1 << (i - 1));
    histogram.PrintI64("upper_bound", 1 << i);
    histogram.PrintI64("value", NumSpansWith(i));
  }

  for (size_t i = 0; i < kNumLists; ++i) {
    PbtxtRegion occupancy =
        region->CreateSubRegion("prioritization_list_occupancy");
    occupancy.PrintI64("list_index", i);
    occupancy.PrintI64("value", NumSpansInList(i));
  }
}

}  // namespace central_freelist_internal

using CentralFreeList = central_freelist_internal::CentralFreeList<
    central_freelist_internal::StaticForwarder>;

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_CENTRAL_FREELIST_H_
