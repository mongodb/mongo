// Copyright 2022 The TCMalloc Authors
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

#include "tcmalloc/allocation_sampling.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/debugging/stacktrace.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/guarded_allocations.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/exponential_biased.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stack_trace_table.h"
#include "tcmalloc/tcmalloc_policy.h"
#include "tcmalloc/thread_cache.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

std::unique_ptr<const ProfileBase> DumpFragmentationProfile(Static& state) {
  auto profile = std::make_unique<StackTraceTable>(ProfileType::kFragmentation);
  state.sampled_allocation_recorder().Iterate(
      [&state, &profile](const SampledAllocation& sampled_allocation) {
        // Compute fragmentation to charge to this sample:
        const StackTrace& t = sampled_allocation.sampled_stack;
        if (t.proxy == nullptr) {
          // There is just one object per-span, and neighboring spans
          // can be released back to the system, so we charge no
          // fragmentation to this sampled object.
          return;
        }

        // Fetch the span on which the proxy lives so we can examine its
        // co-residents.
        const PageId p = PageIdContaining(t.proxy);
        Span* span = state.pagemap().GetDescriptor(p);
        if (span == nullptr) {
          // Avoid crashes in production mode code, but report in tests.
          TC_ASSERT_NE(span, nullptr);
          return;
        }

        const double frag = span->Fragmentation(t.allocated_size);
        if (frag > 0) {
          // Associate the memory warmth with the actual object, not the proxy.
          // The residency information (t.span_start_address) is likely not very
          // useful, but we might as well pass it along.
          profile->AddTrace(frag, t);
        }
      });
  return profile;
}

std::unique_ptr<const ProfileBase> DumpHeapProfile(Static& state) {
  auto profile = std::make_unique<StackTraceTable>(ProfileType::kHeap);
  state.sampled_allocation_recorder().Iterate(
      [&](const SampledAllocation& sampled_allocation) {
        profile->AddTrace(1.0, sampled_allocation.sampled_stack);
      });
  return profile;
}

template <typename State>
ABSL_ATTRIBUTE_NOINLINE static inline void FreeProxyObject(State& state,
                                                           void* ptr,
                                                           size_t size_class) {
  if (ABSL_PREDICT_TRUE(UsePerCpuCache(state))) {
    state.cpu_cache().Deallocate(ptr, size_class);
  } else if (ThreadCache* cache = ThreadCache::GetCacheIfPresent();
             ABSL_PREDICT_TRUE(cache)) {
    cache->Deallocate(ptr, size_class);
  } else {
    // This thread doesn't have thread-cache yet or already. Delete directly
    // into transfer cache.
    state.transfer_cache().InsertRange(size_class, absl::Span<void*>(&ptr, 1));
  }
}

// Performs sampling for already occurred allocation of object.
//
// For very small object sizes, object is used as 'proxy' and full
// page with sampled marked is allocated instead.
//
// For medium-sized objects that have single instance per span,
// they're simply freed and fresh page span is allocated to represent
// sampling.
//
// For large objects (i.e. allocated with do_malloc_pages) they are
// also fully reused and their span is marked as sampled.
//
// Note that do_free_with_size assumes sampled objects have
// page-aligned addresses. Please change both functions if need to
// invalidate the assumption.
//
// Note that size_class might not match requested_size in case of
// memalign. I.e. when larger than requested allocation is done to
// satisfy alignment constraint.
//
// In case of out-of-memory condition when allocating span or
// stacktrace struct, this function simply cheats and returns original
// object. As if no sampling was requested.
sized_ptr_t SampleifyAllocation(Static& state, size_t requested_size,
                                size_t align, size_t weight, size_t size_class,
                                hot_cold_t access_hint, bool size_returning,
                                void* obj, Span* span) {
  TC_CHECK((size_class != 0 && obj != nullptr && span == nullptr) ||
           (size_class == 0 && obj == nullptr && span != nullptr));

  StackTrace stack_trace;
  stack_trace.proxy = nullptr;
  stack_trace.requested_size = requested_size;
  // Grab the stack trace outside the heap lock.
  stack_trace.depth = absl::GetStackTrace(stack_trace.stack, kMaxStackDepth, 0);

  // requested_alignment = 1 means 'small size table alignment was used'
  // Historically this is reported as requested_alignment = 0
  stack_trace.requested_alignment = align;
  if (stack_trace.requested_alignment == 1) {
    stack_trace.requested_alignment = 0;
  }

  stack_trace.requested_size_returning = size_returning;
  stack_trace.access_hint = static_cast<uint8_t>(access_hint);
  stack_trace.weight = weight;

  GuardedAllocWithStatus alloc_with_status{
      nullptr, Profile::Sample::GuardedStatus::NotAttempted};

  size_t capacity = 0;
  if (size_class != 0) {
    TC_ASSERT_EQ(size_class, state.pagemap().sizeclass(PageIdContaining(obj)));

    stack_trace.allocated_size = state.sizemap().class_to_size(size_class);
    stack_trace.cold_allocated = IsExpandedSizeClass(size_class);

    Length num_pages = BytesToLengthCeil(stack_trace.allocated_size);
    alloc_with_status = state.guardedpage_allocator().TrySample(
        requested_size, stack_trace.requested_alignment, num_pages,
        stack_trace);
    if (alloc_with_status.status == Profile::Sample::GuardedStatus::Guarded) {
      TC_CHECK(false, "Mongo check: Allocated from GuardedPageAllocator despite it not being enabled.");
      TC_ASSERT(!IsNormalMemory(alloc_with_status.alloc));
      const PageId p = PageIdContaining(alloc_with_status.alloc);
      PageHeapSpinLockHolder l;
      span = Span::New(p, num_pages);
      state.pagemap().Set(p, span);
      // If we report capacity back from a size returning allocation, we can not
      // report the stack_trace.allocated_size, as we guard the size to
      // 'requested_size', and we maintain the invariant that GetAllocatedSize()
      // must match the returned size from size returning allocations. So in
      // that case, we report the requested size for both capacity and
      // GetAllocatedSize().
      if (size_returning) {
        stack_trace.allocated_size = requested_size;
      }
      capacity = requested_size;
    } else if ((span = state.page_allocator().New(
                    num_pages, {1, AccessDensityPrediction::kSparse},
                    MemoryTag::kSampled)) == nullptr) {
      capacity = stack_trace.allocated_size;
      return {obj, capacity};
    } else {
      capacity = stack_trace.allocated_size;
    }

    size_t span_size =
        Length(state.sizemap().class_to_pages(size_class)).in_bytes();
    size_t objects_per_span = span_size / stack_trace.allocated_size;

    if (objects_per_span != 1) {
      TC_ASSERT_GT(objects_per_span, 1);
      stack_trace.proxy = obj;
      obj = nullptr;
    }
  } else {
    // Set stack_trace.allocated_size to the exact size for a page allocation.
    // NOTE: if we introduce gwp-asan sampling / guarded allocations
    // for page allocations, then we need to revisit do_malloc_pages as
    // the current assumption is that only class sized allocs are sampled
    // for gwp-asan.
    stack_trace.allocated_size = span->bytes_in_span();
    stack_trace.cold_allocated =
        GetMemoryTag(span->start_address()) == MemoryTag::kCold;
    capacity = stack_trace.allocated_size;
  }

  // A span must be provided or created by this point.
  TC_ASSERT_NE(span, nullptr);
  
  // Mongo check: the span cannot be inside the GuardedPageAllocator, because we do not enable it.
  TC_CHECK(!tc_globals.guardedpage_allocator().PointerIsMine(span->start_address()));

  stack_trace.sampled_alloc_handle =
      state.sampled_alloc_handle_generator.fetch_add(
          1, std::memory_order_relaxed) +
      1;
  stack_trace.span_start_address = span->start_address();
  stack_trace.allocation_time = absl::Now();
  stack_trace.guarded_status = static_cast<int>(alloc_with_status.status);

  // How many allocations does this sample represent, given the sampling
  // frequency (weight) and its size.
  const double allocation_estimate =
      static_cast<double>(weight) / (requested_size + 1);

  // Adjust our estimate of internal fragmentation.
  TC_ASSERT_LE(requested_size, stack_trace.allocated_size);
  if (requested_size < stack_trace.allocated_size) {
    state.sampled_internal_fragmentation_.Add(
        allocation_estimate * (stack_trace.allocated_size - requested_size));
  }

  state.allocation_samples.ReportMalloc(stack_trace);

  state.deallocation_samples.ReportMalloc(stack_trace);

  // The SampledAllocation object is visible to readers after this. Readers only
  // care about its various metadata (e.g. stack trace, weight) to generate the
  // heap profile, and won't need any information from Span::Sample() next.
  SampledAllocation* sampled_allocation =
      state.sampled_allocation_recorder().Register(std::move(stack_trace));
  // No pageheap_lock required. The span is freshly allocated and no one else
  // can access it. It is visible after we return from this allocation path.
  span->Sample(sampled_allocation);

  state.peak_heap_tracker().MaybeSaveSample();

  if (obj != nullptr) {
    // We are not maintaining precise statistics on malloc hit/miss rates at our
    // cache tiers.  We can deallocate into our ordinary cache.
    TC_ASSERT_NE(size_class, 0);
    FreeProxyObject(state, obj, size_class);
  }
  TC_ASSERT_EQ(state.pagemap().sizeclass(span->first_page()), 0);
  return {(alloc_with_status.alloc != nullptr) ? alloc_with_status.alloc
                                               : span->start_address(),
          capacity};
}

ABSL_ATTRIBUTE_NOINLINE
static void ReportMismatchedDelete(SampledAllocation& alloc, size_t size,
                                   size_t requested_size,
                                   std::optional<size_t> allocated_size) {
  TC_LOG("*** GWP-ASan (https://google.github.io/tcmalloc/gwp-asan.html) has detected a memory error ***");
  TC_LOG("Error originates from memory allocated at:");
  PrintStackTrace(alloc.sampled_stack.stack, alloc.sampled_stack.depth);

  if (allocated_size.value_or(requested_size) != requested_size) {
    TC_LOG("Mismatched-size-delete of %v bytes (expected %v - %v bytes) at:",
           size, requested_size, *allocated_size);
  } else {
    TC_LOG("Mismatched-size-delete of %v bytes (expected %v bytes) at:", size,
           requested_size);
  }
  static void* stack[kMaxStackDepth];
  const size_t depth = absl::GetStackTrace(stack, kMaxStackDepth, 1);
  PrintStackTrace(stack, depth);

  RecordCrash("GWP-ASan", "mismatched-size-delete");
  abort();
}

void MaybeUnsampleAllocation(Static& state, void* ptr,
                             std::optional<size_t> size, Span* span) {
  // No pageheap_lock required. The sampled span should be unmarked and have its
  // state cleared only once. External synchronization when freeing is required;
  // otherwise, concurrent writes here would likely report a double-free.
  if (SampledAllocation* sampled_allocation = span->Unsample()) {
    TC_ASSERT_EQ(state.pagemap().sizeclass(PageIdContaining(ptr)), 0);

    void* const proxy = sampled_allocation->sampled_stack.proxy;
    const size_t weight = sampled_allocation->sampled_stack.weight;
    const size_t requested_size =
        sampled_allocation->sampled_stack.requested_size;
    const size_t allocated_size =
        sampled_allocation->sampled_stack.allocated_size;
    if (size.has_value()) {
      if (sampled_allocation->sampled_stack.requested_size_returning) {
        if (ABSL_PREDICT_FALSE(
                !(requested_size <= *size && *size <= allocated_size))) {
          ReportMismatchedDelete(*sampled_allocation, *size, requested_size,
                                 allocated_size);
        }
      } else if (ABSL_PREDICT_FALSE(size != requested_size)) {
        ReportMismatchedDelete(*sampled_allocation, *size, requested_size,
                               std::nullopt);
      }
    }
    // SampleifyAllocation turns alignment 1 into 0, turn it back for
    // SizeMap::SizeClass.
    const size_t alignment =
        sampled_allocation->sampled_stack.requested_alignment != 0
            ? sampled_allocation->sampled_stack.requested_alignment
            : 1;
    // How many allocations does this sample represent, given the sampling
    // frequency (weight) and its size.
    const double allocation_estimate =
        static_cast<double>(weight) / (requested_size + 1);
    AllocHandle sampled_alloc_handle =
        sampled_allocation->sampled_stack.sampled_alloc_handle;
    state.sampled_allocation_recorder().Unregister(sampled_allocation);

    // Adjust our estimate of internal fragmentation.
    TC_ASSERT_LE(requested_size, allocated_size);
    if (requested_size < allocated_size) {
      const size_t sampled_fragmentation =
          allocation_estimate * (allocated_size - requested_size);

      // Check against wraparound
      TC_ASSERT_GE(state.sampled_internal_fragmentation_.value(),
                   sampled_fragmentation);
      state.sampled_internal_fragmentation_.Add(-sampled_fragmentation);
    }

    state.deallocation_samples.ReportFree(sampled_alloc_handle);

    if (proxy) {
      const auto policy = CppPolicy().InSameNumaPartitionAs(proxy);
      size_t size_class;
      if (AccessFromPointer(proxy) == AllocationAccess::kCold) {
        size_class = state.sizemap().SizeClass(
            policy.AccessAsCold().AlignAs(alignment), allocated_size);
      } else {
        size_class = state.sizemap().SizeClass(
            policy.AccessAsHot().AlignAs(alignment), allocated_size);
      }
      TC_ASSERT_EQ(size_class,
                   state.pagemap().sizeclass(PageIdContaining(proxy)));
      FreeProxyObject(state, proxy, size_class);
    }
  }
}

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END
