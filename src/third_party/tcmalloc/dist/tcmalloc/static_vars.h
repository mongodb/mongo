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
//
// Static variables shared by multiple classes.

#ifndef TCMALLOC_STATIC_VARS_H_
#define TCMALLOC_STATIC_VARS_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "tcmalloc/allocation_sample.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/deallocation_profiler.h"
#include "tcmalloc/explicitly_constructed.h"
#include "tcmalloc/guarded_page_allocator.h"
#include "tcmalloc/internal/atomic_stats_counter.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/page_allocator.h"
#include "tcmalloc/page_heap_allocator.h"
#include "tcmalloc/peak_heap_tracker.h"
#include "tcmalloc/sampled_allocation.h"
#include "tcmalloc/sampled_allocation_allocator.h"
#include "tcmalloc/sampled_allocation_recorder.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stack_trace_table.h"
#include "tcmalloc/transfer_cache.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

class CpuCache;
class PageMap;
class ThreadCache;

using SampledAllocationRecorder =
    ::tcmalloc::tcmalloc_internal::SampleRecorder<SampledAllocation,
                                                  SampledAllocationAllocator>;

class Static final {
 public:
  constexpr Static() = default;

  // True if InitIfNecessary() has run to completion.
  static bool IsInited();
  // Must be called before calling any of the accessors below.
  // Safe to call multiple times.
  static void InitIfNecessary();

  // Central cache.
  static CentralFreeList& central_freelist(int size_class) {
    return transfer_cache().central_freelist(size_class);
  }
  // Central cache -- an array of free-lists, one per size-class.
  // We have a separate lock per free-list to reduce contention.
  static TransferCacheManager& transfer_cache() { return transfer_cache_; }

  // A per-cache domain TransferCache.
  static ShardedTransferCacheManager& sharded_transfer_cache() {
    return sharded_transfer_cache_;
  }

  static SizeMap& sizemap() { return sizemap_; }

  static CpuCache& cpu_cache() { return cpu_cache_; }

  static PeakHeapTracker& peak_heap_tracker() { return peak_heap_tracker_; }

  static NumaTopology<kNumaPartitions, kNumBaseClasses>& numa_topology() {
    return numa_topology_;
  }

  //////////////////////////////////////////////////////////////////////
  // In addition to the explicit initialization comment, the variables below
  // must be protected by pageheap_lock.

  static Arena& arena() { return arena_; }

  // Page-level allocator.
  static PageAllocator& page_allocator() {
    return *reinterpret_cast<PageAllocator*>(page_allocator_.memory);
  }

  static PageMap& pagemap() { return pagemap_; }

  static GuardedPageAllocator& guardedpage_allocator() {
    return guardedpage_allocator_;
  }

  static SampledAllocationAllocator& sampledallocation_allocator() {
    return sampledallocation_allocator_;
  }

  static PageHeapAllocator<Span>& span_allocator() { return span_allocator_; }

  static PageHeapAllocator<ThreadCache>& threadcache_allocator() {
    return threadcache_allocator_;
  }

  static SampledAllocationRecorder& sampled_allocation_recorder() {
    return sampled_allocation_recorder_.get_mutable();
  }

  // State kept for sampled allocations (/heapz support). No pageheap_lock
  // required when reading/writing the counters.
  ABSL_CONST_INIT static tcmalloc_internal::StatsCounter sampled_objects_size_;
  // sampled_internal_fragmentation estimates the amount of memory overhead from
  // allocation sizes being rounded up to size class/page boundaries.
  ABSL_CONST_INIT static tcmalloc_internal::StatsCounter
      sampled_internal_fragmentation_;
  // total_sampled_count_ tracks the total number of allocations that are
  // sampled.
  ABSL_CONST_INIT static tcmalloc_internal::StatsCounter total_sampled_count_;

  ABSL_CONST_INIT static AllocationSampleList allocation_samples;

  ABSL_CONST_INIT static deallocationz::DeallocationProfilerList
      deallocation_samples;

  // MallocHook::AllocHandle is a simple 64-bit int, and is not dependent on
  // other data.
  ABSL_CONST_INIT static std::atomic<AllocHandle>
      sampled_alloc_handle_generator;

  static PageHeapAllocator<StackTraceTable::LinkedSample>&
  linked_sample_allocator() {
    return linked_sample_allocator_;
  }

  static bool ABSL_ATTRIBUTE_ALWAYS_INLINE CpuCacheActive() {
    return cpu_cache_active_.load(std::memory_order_acquire);
  }
  static void ActivateCpuCache() {
    cpu_cache_active_.store(true, std::memory_order_release);
  }

  static bool ABSL_ATTRIBUTE_ALWAYS_INLINE IsOnFastPath() {
    return
        // These boolean operations do not require short-circuiting from &&.
        // Bitwise AND of booleans triggers -Wbitwise-instead-of-logical, as
        // this can be a common source of bugs.  Suppress this by casting to
        // int first.

#ifndef TCMALLOC_DEPRECATED_PERTHREAD
        // When the per-cpu cache is enabled, and the thread's current cpu
        // variable is initialized we will try to allocate from the per-cpu
        // cache. If something fails, we bail out to the full malloc.
        // Checking the current cpu variable here allows us to remove it from
        // the fast-path, since we will fall back to the slow path until this
        // variable is initialized.
        static_cast<int>(CpuCacheActive()) &
        static_cast<int>(subtle::percpu::IsFastNoInit());
#else
        !CpuCacheActive();
#endif
  }

  static size_t metadata_bytes() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // The root of the pagemap is potentially a large poorly utilized
  // structure, so figure out how much of it is actually resident.
  static size_t pagemap_residence();

 private:
#if defined(__clang__)
  __attribute__((preserve_most))
#endif
  static void
  SlowInitIfNecessary();

  // These static variables require explicit initialization.  We cannot
  // count on their constructors to do any initialization because other
  // static variables may try to allocate memory before these variables
  // can run their constructors.

  ABSL_CONST_INIT static Arena arena_;
  static SizeMap sizemap_;
  TCMALLOC_ATTRIBUTE_NO_DESTROY ABSL_CONST_INIT static TransferCacheManager
      transfer_cache_;
  ABSL_CONST_INIT static ShardedTransferCacheManager sharded_transfer_cache_;
  static CpuCache cpu_cache_;
  ABSL_CONST_INIT static GuardedPageAllocator guardedpage_allocator_;
  static SampledAllocationAllocator sampledallocation_allocator_;
  static PageHeapAllocator<Span> span_allocator_;
  static PageHeapAllocator<ThreadCache> threadcache_allocator_;
  static PageHeapAllocator<StackTraceTable::LinkedSample>
      linked_sample_allocator_;
  ABSL_CONST_INIT static std::atomic<bool> inited_;
  ABSL_CONST_INIT static std::atomic<bool> cpu_cache_active_;
  ABSL_CONST_INIT static PeakHeapTracker peak_heap_tracker_;
  ABSL_CONST_INIT static NumaTopology<kNumaPartitions, kNumBaseClasses>
      numa_topology_;

  // PageHeap uses a constructor for initialization.  Like the members above,
  // we can't depend on initialization order, so pageheap is new'd
  // into this buffer.
  union PageAllocatorStorage {
    constexpr PageAllocatorStorage() : extra(0) {}

    char memory[sizeof(PageAllocator)];
    uintptr_t extra;  // To force alignment
  };

  static PageAllocatorStorage page_allocator_;
  static PageMap pagemap_;

  // Manages sampled allocations and allows iteration over samples free from
  // the global pageheap_lock.
  static ExplicitlyConstructed<SampledAllocationRecorder>
      sampled_allocation_recorder_;
};

ABSL_CONST_INIT extern Static tc_globals;

inline bool Static::IsInited() {
  return inited_.load(std::memory_order_acquire);
}

inline void Static::InitIfNecessary() {
  if (ABSL_PREDICT_FALSE(!IsInited())) {
    SlowInitIfNecessary();
  }
}

// Why are these functions here? Because we want to inline them, but they
// need access to Static::span_allocator. Putting them in span.h would lead
// to nasty dependency loops.  Since anything that needs them certainly
// includes static_vars.h, this is a perfectly good compromise.
// TODO(b/134687001): move span_allocator to Span, getting rid of the need for
// this.
inline Span* Span::New(PageId p, Length len) {
  Span* result = Static::span_allocator().New();
  result->Init(p, len);
  return result;
}

inline void Span::Delete(Span* span) {
#ifndef NDEBUG
  // In debug mode, trash the contents of deleted Spans
  memset(static_cast<void*>(span), 0x3f, sizeof(*span));
#endif
  Static::span_allocator().Delete(span);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_STATIC_VARS_H_
