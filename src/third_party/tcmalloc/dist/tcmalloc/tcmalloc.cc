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
// tcmalloc is a fast malloc implementation.  See
// https://github.com/google/tcmalloc/tree/master/docs/design.md for a high-level description of
// how this malloc works.
//
// SYNCHRONIZATION
//  1. The thread-/cpu-specific lists are accessed without acquiring any locks.
//     This is safe because each such list is only accessed by one thread/cpu at
//     a time.
//  2. We have a lock per central free-list, and hold it while manipulating
//     the central free list for a particular size.
//  3. The central page allocator is protected by "pageheap_lock".
//  4. The pagemap (which maps from page-number to descriptor),
//     can be read without holding any locks, and written while holding
//     the "pageheap_lock".
//
//     This multi-threaded access to the pagemap is safe for fairly
//     subtle reasons.  We basically assume that when an object X is
//     allocated by thread A and deallocated by thread B, there must
//     have been appropriate synchronization in the handoff of object
//     X from thread A to thread B.
//
// PAGEMAP
// -------
// Page map contains a mapping from page id to Span.
//
// If Span s occupies pages [p..q],
//      pagemap[p] == s
//      pagemap[q] == s
//      pagemap[p+1..q-1] are undefined
//      pagemap[p-1] and pagemap[q+1] are defined:
//         NULL if the corresponding page is not yet in the address space.
//         Otherwise it points to a Span.  This span may be free
//         or allocated.  If free, it is in one of pageheap's freelist.

#include "tcmalloc/tcmalloc.h"

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/config.h"
#include "absl/base/const_init.h"
#include "absl/base/dynamic_annotations.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/base/macros.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/debugging/stacktrace.h"
#include "absl/memory/memory.h"
#include "absl/numeric/bits.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/strip.h"
#include "tcmalloc/allocation_sample.h"
#include "tcmalloc/allocation_sampling.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/deallocation_profiler.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/global_stats.h"
#include "tcmalloc/guarded_page_allocator.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/overflow.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal_malloc_extension.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/malloc_tracing_extension.h"
#include "tcmalloc/new_extension.h"
#include "tcmalloc/page_allocator.h"
#include "tcmalloc/page_heap.h"
#include "tcmalloc/page_heap_allocator.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/sampled_allocation.h"
#include "tcmalloc/sampler.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stack_trace_table.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/system-alloc.h"
#include "tcmalloc/tcmalloc_policy.h"
#include "tcmalloc/thread_cache.h"
#include "tcmalloc/transfer_cache.h"
#include "tcmalloc/transfer_cache_stats.h"

#if defined(TCMALLOC_HAVE_STRUCT_MALLINFO) || \
    defined(TCMALLOC_HAVE_STRUCT_MALLINFO2)
#include <malloc.h>
#endif

#if !defined(__x86_64__) && !defined(__aarch64__) && !defined(__riscv)
#error "Unsupported architecture."
#endif

#if defined(__android__) || defined(__APPLE__)
#error "Unsupported platform."
#endif

#ifndef __linux__
#error "Unsupported platform."
#endif

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Gets a human readable description of the current state of the malloc data
// structures. Returns the actual written size.
// [buffer, buffer+result] will contain NUL-terminated output string.
//
// REQUIRES: buffer_length > 0.
extern "C" ABSL_ATTRIBUTE_UNUSED int MallocExtension_Internal_GetStatsInPbtxt(
    char* buffer, int buffer_length) {
  ASSERT(buffer_length > 0);
  Printer printer(buffer, buffer_length);

  // Print level one stats unless lots of space is available
  if (buffer_length < 10000) {
    DumpStatsInPbtxt(&printer, 1);
  } else {
    DumpStatsInPbtxt(&printer, 2);
  }

  size_t required = printer.SpaceRequired();

  if (buffer_length > required) {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    required += GetRegionFactory()->GetStatsInPbtxt(
        absl::Span<char>(buffer + required, buffer_length - required));
  }

  return required;
}

static void PrintStats(int level) {
  const int kBufferSize = 64 << 10;
  char* buffer = new char[kBufferSize];
  Printer printer(buffer, kBufferSize);
  DumpStats(&printer, level);
  (void)write(STDERR_FILENO, buffer, strlen(buffer));
  delete[] buffer;
}

extern "C" void MallocExtension_Internal_GetStats(std::string* ret) {
  size_t shift = std::max<size_t>(18, absl::bit_width(ret->capacity()) - 1);
  for (; shift < 22; shift++) {
    const size_t size = 1 << shift;
    // Double ret's size until we succeed in writing the buffer without
    // truncation.
    //
    // TODO(b/142931922):  printer only writes data and does not read it.
    // Leverage https://wg21.link/P1072 when it is standardized.
    ret->resize(size - 1);

    size_t written_size = TCMalloc_Internal_GetStats(&*ret->begin(), size - 1);
    if (written_size < size - 1) {
      // We did not truncate.
      ret->resize(written_size);
      break;
    }
  }
}

extern "C" size_t TCMalloc_Internal_GetStats(char* buffer,
                                             size_t buffer_length) {
  Printer printer(buffer, buffer_length);
  if (buffer_length < 10000) {
    DumpStats(&printer, 1);
  } else {
    DumpStats(&printer, 2);
  }

  printer.printf("\nLow-level allocator stats:\n");
  printer.printf("Memory Release Failures: %d\n", SystemReleaseErrors());

  size_t n = printer.SpaceRequired();

  size_t bytes_remaining = buffer_length > n ? buffer_length - n : 0;
  if (bytes_remaining > 0) {
    n += GetRegionFactory()->GetStats(
        absl::Span<char>(buffer + n, bytes_remaining));
  }

  return n;
}

extern "C" const ProfileBase* MallocExtension_Internal_SnapshotCurrent(
    ProfileType type) {
  switch (type) {
    case ProfileType::kHeap:
      return DumpHeapProfile(tc_globals).release();
    case ProfileType::kFragmentation:
      return DumpFragmentationProfile(tc_globals).release();
    case ProfileType::kPeakHeap:
      return tc_globals.peak_heap_tracker().DumpSample().release();
    default:
      return nullptr;
  }
}

extern "C" AllocationProfilingTokenBase*
MallocExtension_Internal_StartAllocationProfiling() {
  return new AllocationSample(&tc_globals.allocation_samples, absl::Now());
}

extern "C" tcmalloc_internal::AllocationProfilingTokenBase*
MallocExtension_Internal_StartLifetimeProfiling() {
  return new deallocationz::DeallocationSample(
      &tc_globals.deallocation_samples);
}

MallocExtension::Ownership GetOwnership(const void* ptr) {
  const PageId p = PageIdContaining(ptr);
  return tc_globals.pagemap().GetDescriptor(p)
             ? MallocExtension::Ownership::kOwned
             : MallocExtension::Ownership::kNotOwned;
}

extern "C" bool MallocExtension_Internal_GetNumericProperty(
    const char* name_data, size_t name_size, size_t* value) {
  return GetNumericProperty(name_data, name_size, value);
}

extern "C" void MallocExtension_Internal_GetMemoryLimit(
    MallocExtension::MemoryLimit* limit) {
  ASSERT(limit != nullptr);

  std::tie(limit->limit, limit->hard) = tc_globals.page_allocator().limit();
}

extern "C" void MallocExtension_Internal_SetMemoryLimit(
    const MallocExtension::MemoryLimit* limit) {
  ASSERT(limit != nullptr);

  if (!limit->hard) {
    Parameters::set_heap_size_hard_limit(0);
    tc_globals.page_allocator().set_limit(limit->limit, false /* !hard */);
  } else {
    Parameters::set_heap_size_hard_limit(limit->limit);
  }
}

extern "C" void MallocExtension_Internal_MarkThreadIdle() {
  ThreadCache::BecomeIdle();
}

extern "C" AddressRegionFactory* MallocExtension_Internal_GetRegionFactory() {
  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  return GetRegionFactory();
}

extern "C" void MallocExtension_Internal_SetRegionFactory(
    AddressRegionFactory* factory) {
  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  SetRegionFactory(factory);
}

// ReleaseMemoryToSystem drops the page heap lock while actually calling to
// kernel to release pages. To avoid confusing ourselves with
// extra_bytes_released handling, lets do separate lock just for release.
ABSL_CONST_INIT static absl::base_internal::SpinLock release_lock(
    absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY);

extern "C" size_t MallocExtension_Internal_ReleaseMemoryToSystem(
    size_t num_bytes) {
  // ReleaseMemoryToSystem() might release more than the requested bytes because
  // the page heap releases at the span granularity, and spans are of wildly
  // different sizes.  This keeps track of the extra bytes bytes released so
  // that the app can periodically call ReleaseMemoryToSystem() to release
  // memory at a constant rate.
  ABSL_CONST_INIT static size_t extra_bytes_released;

  absl::base_internal::SpinLockHolder rh(&release_lock);

  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  if (num_bytes <= extra_bytes_released) {
    // We released too much on a prior call, so don't release any
    // more this time.
    extra_bytes_released = extra_bytes_released - num_bytes;
    num_bytes = 0;
  } else {
    num_bytes = num_bytes - extra_bytes_released;
  }

  Length num_pages;
  if (num_bytes > 0) {
    // A sub-page size request may round down to zero.  Assume the caller wants
    // some memory released.
    num_pages = BytesToLengthCeil(num_bytes);
    ASSERT(num_pages > Length(0));
  } else {
    num_pages = Length(0);
  }
  size_t bytes_released =
      tc_globals.page_allocator().ReleaseAtLeastNPages(num_pages).in_bytes();
  if (bytes_released > num_bytes) {
    extra_bytes_released = bytes_released - num_bytes;
    return num_bytes;
  }
  // The PageHeap wasn't able to release num_bytes.  Don't try to compensate
  // with a big release next time.
  extra_bytes_released = 0;
  return bytes_released;
}

// nallocx slow path.
// Moved to a separate function because size_class_with_alignment is not inlined
// which would cause nallocx to become non-leaf function with stack frame and
// stack spills. ABSL_ATTRIBUTE_ALWAYS_INLINE does not work on
// size_class_with_alignment, compiler barks that it can't inline the function
// somewhere.
static ABSL_ATTRIBUTE_NOINLINE size_t nallocx_slow(size_t size, int flags) {
  tc_globals.InitIfNecessary();
  size_t align = static_cast<size_t>(1ull << (flags & 0x3f));
  uint32_t size_class;
  if (ABSL_PREDICT_TRUE(tc_globals.sizemap().GetSizeClass(
          CppPolicy().AlignAs(align), size, &size_class))) {
    ASSERT(size_class != 0);
    return tc_globals.sizemap().class_to_size(size_class);
  } else {
    return BytesToLengthCeil(size).in_bytes();
  }
}

// The nallocx function allocates no memory, but it performs the same size
// computation as the malloc function, and returns the real size of the
// allocation that would result from the equivalent malloc function call.
// nallocx is a malloc extension originally implemented by jemalloc:
// http://www.unix.com/man-page/freebsd/3/nallocx/
extern "C" size_t nallocx(size_t size, int flags) noexcept {
  if (ABSL_PREDICT_FALSE(!tc_globals.IsInited() || flags != 0)) {
    return nallocx_slow(size, flags);
  }
  uint32_t size_class;
  if (ABSL_PREDICT_TRUE(
          tc_globals.sizemap().GetSizeClass(CppPolicy(), size, &size_class))) {
    ASSERT(size_class != 0);
    return tc_globals.sizemap().class_to_size(size_class);
  } else {
    return BytesToLengthCeil(size).in_bytes();
  }
}

extern "C" MallocExtension::Ownership MallocExtension_Internal_GetOwnership(
    const void* ptr) {
  return GetOwnership(ptr);
}

extern "C" void MallocExtension_Internal_GetProperties(
    std::map<std::string, MallocExtension::Property>* result) {
  TCMallocStats stats;
  // We do not enquire about residency of TCMalloc's metadata since accuracy is
  // not important for applications invoking this function.  The data reported
  // would always suffer from time-of-check vs time-of-use problem.
  ExtractTCMallocStats(&stats, /*report_residence=*/false);

  const uint64_t virtual_memory_used = VirtualMemoryUsed(stats);
  const uint64_t physical_memory_used = PhysicalMemoryUsed(stats);
  const uint64_t bytes_in_use_by_app = InUseByApp(stats);

  result->clear();
  // Virtual Memory Used
  (*result)["generic.virtual_memory_used"].value = virtual_memory_used;
  // Physical Memory used
  (*result)["generic.physical_memory_used"].value = physical_memory_used;
  // Bytes in use By App
  (*result)["generic.current_allocated_bytes"].value = bytes_in_use_by_app;
  (*result)["generic.bytes_in_use_by_app"].value = bytes_in_use_by_app;
  (*result)["generic.heap_size"].value = HeapSizeBytes(stats.pageheap);
  (*result)["generic.peak_memory_usage"].value =
      static_cast<uint64_t>(stats.peak_stats.sampled_application_bytes);
  (*result)["generic.realized_fragmentation"].value = static_cast<uint64_t>(
      100. * safe_div(stats.peak_stats.backed_bytes -
                          stats.peak_stats.sampled_application_bytes,
                      stats.peak_stats.sampled_application_bytes));
  // Page Heap Free
  (*result)["tcmalloc.page_heap_free"].value = stats.pageheap.free_bytes;
  (*result)["tcmalloc.pageheap_free_bytes"].value = stats.pageheap.free_bytes;
  // Metadata Bytes
  (*result)["tcmalloc.metadata_bytes"].value = stats.metadata_bytes;
  // Heaps in Use
  (*result)["tcmalloc.thread_cache_count"].value = stats.tc_stats.in_use;
  // Central Cache Free List
  (*result)["tcmalloc.central_cache_free"].value = stats.central_bytes;
  // Transfer Cache Free List
  (*result)["tcmalloc.transfer_cache_free"].value = stats.transfer_bytes;
  // Per CPU Cache Free List
  (*result)["tcmalloc.cpu_free"].value = stats.per_cpu_bytes;
  (*result)["tcmalloc.sharded_transfer_cache_free"].value =
      stats.sharded_transfer_bytes;
  (*result)["tcmalloc.per_cpu_caches_active"].value =
      tc_globals.CpuCacheActive();
  // Thread Cache Free List
  (*result)["tcmalloc.current_total_thread_cache_bytes"].value =
      stats.thread_bytes;
  (*result)["tcmalloc.thread_cache_free"].value = stats.thread_bytes;
  (*result)["tcmalloc.local_bytes"].value = LocalBytes(stats);

  size_t overall_thread_cache_size;
  {
    absl::base_internal::SpinLockHolder l(&pageheap_lock);
    overall_thread_cache_size = ThreadCache::overall_thread_cache_size();
  }
  (*result)["tcmalloc.max_total_thread_cache_bytes"].value =
      overall_thread_cache_size;

  // Page Unmapped
  (*result)["tcmalloc.pageheap_unmapped_bytes"].value =
      stats.pageheap.unmapped_bytes;
  // Arena non-resident bytes aren't on the page heap, but they are unmapped.
  (*result)["tcmalloc.page_heap_unmapped"].value =
      stats.pageheap.unmapped_bytes + stats.arena.bytes_nonresident;
  (*result)["tcmalloc.sampled_internal_fragmentation"].value =
      tc_globals.sampled_internal_fragmentation_.value();

  (*result)["tcmalloc.page_algorithm"].value =
      tc_globals.page_allocator().algorithm();

  (*result)["tcmalloc.external_fragmentation_bytes"].value =
      ExternalBytes(stats);
  (*result)["tcmalloc.required_bytes"].value = RequiredBytes(stats);
  (*result)["tcmalloc.slack_bytes"].value = SlackBytes(stats.pageheap);

  size_t amount;
  bool is_hard;
  std::tie(amount, is_hard) = tc_globals.page_allocator().limit();
  if (is_hard) {
    (*result)["tcmalloc.hard_usage_limit_bytes"].value = amount;
    (*result)["tcmalloc.desired_usage_limit_bytes"].value =
        std::numeric_limits<size_t>::max();
  } else {
    (*result)["tcmalloc.hard_usage_limit_bytes"].value =
        std::numeric_limits<size_t>::max();
    (*result)["tcmalloc.desired_usage_limit_bytes"].value = amount;
  }

  WalkExperiments([&](absl::string_view name, bool active) {
    (*result)[absl::StrCat("tcmalloc.experiment.", name)].value = active;
  });
}

extern "C" size_t MallocExtension_Internal_ReleaseCpuMemory(int cpu) {
  size_t bytes = 0;
  if (tc_globals.CpuCacheActive()) {
    bytes = tc_globals.cpu_cache().Reclaim(cpu);
  }
  return bytes;
}

//-------------------------------------------------------------------
// Helpers for the exported routines below
//-------------------------------------------------------------------

enum class Hooks { RUN, NO };

static void FreeSmallSlow(void* ptr, size_t size_class);

namespace {

// Sets `*psize` to `size`,
inline void SetCapacity(size_t size, std::nullptr_t) {}
inline void SetCapacity(size_t size, size_t* psize) { *psize = size; }

// Sets `*psize` to the size for the size class in `size_class`,
inline void SetClassCapacity(size_t size, std::nullptr_t) {}
inline void SetClassCapacity(uint32_t size_class, size_t* psize) {
  *psize = tc_globals.sizemap().class_to_size(size_class);
}

// Sets `*psize` to the size for the size class in `size_class` if `ptr` is not
// null, else `*psize` is set to 0. This method is overloaded for `nullptr_t`
// below, allowing the compiler to optimize code between regular and size
// returning allocation operations.
inline void SetClassCapacity(const void*, uint32_t, std::nullptr_t) {}
inline void SetClassCapacity(const void* ptr, uint32_t size_class,
                             size_t* psize) {
  if (ABSL_PREDICT_TRUE(ptr != nullptr)) {
    *psize = tc_globals.sizemap().class_to_size(size_class);
  } else {
    *psize = 0;
  }
}

// Sets `*psize` to the size in pages corresponding to the requested size in
// `size` if `ptr` is not null, else `*psize` is set to 0. This method is
// overloaded for `nullptr_t` below, allowing the compiler to optimize code
// between regular and size returning allocation operations.
inline void SetPagesCapacity(const void*, Length, std::nullptr_t) {}
inline void SetPagesCapacity(const void* ptr, Length size, size_t* psize) {
  if (ABSL_PREDICT_TRUE(ptr != nullptr)) {
    *psize = size.in_bytes();
  } else {
    *psize = 0;
  }
}

}  // namespace

// In free fast-path we handle delete hooks by delegating work to slower
// function that both performs delete hooks calls and does free. This is done so
// that free fast-path only does tail calls, which allow compiler to avoid
// generating costly prologue/epilogue for fast-path.
template <void F(void*, size_t), Hooks hooks_state>
static ABSL_ATTRIBUTE_SECTION(google_malloc) void invoke_delete_hooks_and_free(
    void* ptr, size_t t) {
  // Refresh the fast path state.
  GetThreadSampler()->UpdateFastPathState();
  return F(ptr, t);
}

template <void F(void*, PageId), Hooks hooks_state>
static ABSL_ATTRIBUTE_SECTION(google_malloc) void invoke_delete_hooks_and_free(
    void* ptr, PageId p) {
  // Refresh the fast path state.
  GetThreadSampler()->UpdateFastPathState();
  return F(ptr, p);
}

// Helper for do_free_with_size_class
template <Hooks hooks_state>
static inline ABSL_ATTRIBUTE_ALWAYS_INLINE void FreeSmall(void* ptr,
                                                          size_t size_class) {
  if (!IsExpandedSizeClass(size_class)) {
    ASSERT(IsNormalMemory(ptr));
  } else {
    ASSERT(IsColdMemory(ptr));
  }
  if (ABSL_PREDICT_FALSE(!GetThreadSampler()->IsOnFastPath())) {
    // Take the slow path.
    invoke_delete_hooks_and_free<FreeSmallSlow, hooks_state>(ptr, size_class);
    return;
  }

#ifndef TCMALLOC_DEPRECATED_PERTHREAD
  // The CPU Cache is enabled, so we're able to take the fastpath.
  ASSERT(tc_globals.CpuCacheActive());
  ASSERT(subtle::percpu::IsFastNoInit());

  tc_globals.cpu_cache().Deallocate(ptr, size_class);
#else   // TCMALLOC_DEPRECATED_PERTHREAD
  ThreadCache* cache = ThreadCache::GetCacheIfPresent();

  // IsOnFastPath does not track whether or not we have an active ThreadCache on
  // this thread, so we need to check cache for nullptr.
  if (ABSL_PREDICT_FALSE(cache == nullptr)) {
    FreeSmallSlow(ptr, size_class);
    return;
  }

  cache->Deallocate(ptr, size_class);
#endif  // TCMALLOC_DEPRECATED_PERTHREAD
}

// this helper function is used when FreeSmall (defined above) hits
// the case of thread state not being in per-cpu mode or hitting case
// of no thread cache. This happens when thread state is not yet
// properly initialized with real thread cache or with per-cpu mode,
// or when thread state is already destroyed as part of thread
// termination.
//
// We explicitly prevent inlining it to keep it out of fast-path, so
// that fast-path only has tail-call, so that fast-path doesn't need
// function prologue/epilogue.
ABSL_ATTRIBUTE_NOINLINE
static void FreeSmallSlow(void* ptr, size_t size_class) {
  if (ABSL_PREDICT_TRUE(UsePerCpuCache(tc_globals))) {
    tc_globals.cpu_cache().Deallocate(ptr, size_class);
  } else if (ThreadCache* cache = ThreadCache::GetCacheIfPresent();
             ABSL_PREDICT_TRUE(cache)) {
    cache->Deallocate(ptr, size_class);
  } else {
    // This thread doesn't have thread-cache yet or already. Delete directly
    // into central cache.
    tc_globals.transfer_cache().InsertRange(size_class,
                                            absl::Span<void*>(&ptr, 1));
  }
}

namespace {

template <typename Policy, typename CapacityPtr = std::nullptr_t>
inline void* do_malloc_pages(Policy policy, size_t size, int num_objects,
                             CapacityPtr capacity = nullptr) {
  // Page allocator does not deal well with num_pages = 0.
  Length num_pages = std::max<Length>(BytesToLengthCeil(size), Length(1));

  MemoryTag tag = MemoryTag::kNormal;
  if (IsColdHint(policy.access())) {
    tag = MemoryTag::kCold;
  } else if (tc_globals.numa_topology().numa_aware()) {
    tag = NumaNormalTag(policy.numa_partition());
  }
  Span* span = tc_globals.page_allocator().NewAligned(
      num_pages, BytesToLengthCeil(policy.align()), num_objects, tag);

  if (span == nullptr) {
    SetPagesCapacity(nullptr, Length(0), capacity);
    return nullptr;
  }

  void* result = span->start_address();
  ASSERT(!ColdFeatureActive() || tag == GetMemoryTag(span->start_address()));

  // Set capacity to the exact size for a page allocation.  This needs to be
  // revisited if we introduce gwp-asan sampling / guarded allocations to
  // do_malloc_pages().
  SetPagesCapacity(result, num_pages, capacity);

  if (size_t weight = ShouldSampleAllocation(size)) {
    CHECK_CONDITION(result == SampleLargeAllocation(tc_globals, policy, size,
                                                    weight, span, capacity));
  }

  return result;
}

template <typename Policy, typename CapacityPtr>
inline void* ABSL_ATTRIBUTE_ALWAYS_INLINE AllocSmall(Policy policy,
                                                     size_t size_class,
                                                     size_t size,
                                                     CapacityPtr capacity) {
  ASSERT(size_class != 0);
  void* result;

  if (UsePerCpuCache(tc_globals)) {
    result = tc_globals.cpu_cache().Allocate<Policy::handle_oom>(size_class);
  } else {
    result = ThreadCache::GetCache()->Allocate<Policy::handle_oom>(size_class);
  }

  if (!Policy::can_return_nullptr()) {
    ASSUME(result != nullptr);
  }

  if (ABSL_PREDICT_FALSE(result == nullptr)) {
    SetCapacity(0, capacity);
    return nullptr;
  }
  size_t weight;
  if (ABSL_PREDICT_FALSE(weight = ShouldSampleAllocation(size))) {
    return SampleSmallAllocation(tc_globals, policy, size, weight, size_class,
                                 result, capacity);
  }
  SetClassCapacity(size_class, capacity);
  return result;
}

// Handles freeing object that doesn't have size class, i.e. which
// is either large or sampled. We explicitly prevent inlining it to
// keep it out of fast-path. This helps avoid expensive
// prologue/epilogue for fast-path freeing functions.
ABSL_ATTRIBUTE_NOINLINE
static void do_free_pages(void* ptr, const PageId p) {
  Span* span = tc_globals.pagemap().GetExistingDescriptor(p);
  CHECK_CONDITION(span != nullptr && "Possible double free detected");
  // Prefetch now to avoid a stall accessing *span while under the lock.
  span->Prefetch();

  MaybeUnsampleAllocation(tc_globals, ptr, span);

  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    ASSERT(span->first_page() == p);
    if (IsSampledMemory(ptr)) {
      if (tc_globals.guardedpage_allocator().PointerIsMine(ptr)) {
        // Release lock while calling Deallocate() since it does a system call.
        pageheap_lock.Unlock();
        tc_globals.guardedpage_allocator().Deallocate(ptr);
        pageheap_lock.Lock();
        Span::Delete(span);
      } else if (IsColdMemory(ptr)) {
        ASSERT(reinterpret_cast<uintptr_t>(ptr) % kPageSize == 0);
        tc_globals.page_allocator().Delete(span, 1, MemoryTag::kCold);
      } else {
        ASSERT(reinterpret_cast<uintptr_t>(ptr) % kPageSize == 0);
        tc_globals.page_allocator().Delete(span, 1, MemoryTag::kSampled);
      }
    } else if (kNumaPartitions != 1) {
      ASSERT(reinterpret_cast<uintptr_t>(ptr) % kPageSize == 0);
      tc_globals.page_allocator().Delete(span, 1, GetMemoryTag(ptr));
    } else {
      ASSERT(reinterpret_cast<uintptr_t>(ptr) % kPageSize == 0);
      tc_globals.page_allocator().Delete(span, 1, MemoryTag::kNormal);
    }
  }
}

#ifndef NDEBUG
static size_t GetSizeClass(void* ptr) {
  const PageId p = PageIdContaining(ptr);
  return tc_globals.pagemap().sizeclass(p);
}
#endif

// Helper for the object deletion (free, delete, etc.).  Inputs:
//   ptr is object to be freed
//   size_class is the size class of that object, or 0 if it's unknown
//   have_size_class is true iff size_class is known and is non-0.
//
// Note that since have_size_class is compile-time constant, genius compiler
// would not need it. Since it would be able to somehow infer that
// GetSizeClass never produces 0 size_class, and so it
// would know that places that call this function with explicit 0 is
// "have_size_class-case" and others are "!have_size_class-case". But we
// certainly don't have such compiler. See also do_free_with_size below.
template <bool have_size_class, Hooks hooks_state>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void do_free_with_size_class(
    void* ptr, size_t size_class) {
  // !have_size_class -> size_class == 0
  ASSERT(have_size_class || size_class == 0);

  const PageId p = PageIdContaining(ptr);

  // if we have_size_class, then we've excluded ptr == nullptr case. See
  // comment in do_free_with_size. Thus we only bother testing nullptr
  // in non-sized case.
  //
  // Thus: ptr == nullptr -> !have_size_class
  ASSERT(ptr != nullptr || !have_size_class);
  if (!have_size_class && ABSL_PREDICT_FALSE(ptr == nullptr)) {
    return;
  }

  // ptr must be a result of a previous malloc/memalign/... call, and
  // therefore static initialization must have already occurred.
  ASSERT(tc_globals.IsInited());

  if (!have_size_class) {
    size_class = tc_globals.pagemap().sizeclass(p);
  }
  if (have_size_class || ABSL_PREDICT_TRUE(size_class != 0)) {
    ASSERT(size_class == GetSizeClass(ptr));
    ASSERT(ptr != nullptr);
    ASSERT(!tc_globals.pagemap().GetExistingDescriptor(p)->sampled());
    FreeSmall<hooks_state>(ptr, size_class);
  } else {
    invoke_delete_hooks_and_free<do_free_pages, hooks_state>(ptr, p);
  }
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void do_free(void* ptr) {
  return do_free_with_size_class<false, Hooks::RUN>(ptr, 0);
}

void do_free_no_hooks(void* ptr) {
  return do_free_with_size_class<false, Hooks::NO>(ptr, 0);
}

template <typename AlignPolicy>
bool CorrectSize(void* ptr, size_t size, AlignPolicy align);

bool CorrectAlignment(void* ptr, std::align_val_t alignment);

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void FreePages(void* ptr) {
  const PageId p = PageIdContaining(ptr);
  invoke_delete_hooks_and_free<do_free_pages, Hooks::RUN>(ptr, p);
}

template <typename AlignPolicy>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void do_free_with_size(void* ptr,
                                                           size_t size,
                                                           AlignPolicy align) {
  ASSERT(CorrectSize(ptr, size, align));
  ASSERT(CorrectAlignment(ptr, static_cast<std::align_val_t>(align.align())));

  // This is an optimized path that may be taken if the binary is compiled
  // with -fsized-delete. We attempt to discover the size class cheaply
  // without any cache misses by doing a plain computation that
  // maps from size to size-class.
  //
  // The optimized path doesn't work with sampled objects, whose deletions
  // trigger more operations and require to visit metadata.
  if (ABSL_PREDICT_FALSE(IsSampledMemory(ptr))) {
    // IsColdMemory(ptr) implies IsSampledMemory(ptr).
    if (!IsColdMemory(ptr)) {
      // we don't know true class size of the ptr
      if (ptr == nullptr) return;
      return FreePages(ptr);
    } else {
      // This code is redundant with the outer !IsSampledMemory path below, but
      // this avoids putting the IsSampledMemory&&IsColdMemory check on the
      // critical path of the size lookup.
      //
      // As of October 2022, this is faster than unifying the path and using
      // AccessAs().
      ASSERT(ptr != nullptr);

      uint32_t size_class;
      if (ABSL_PREDICT_FALSE(!tc_globals.sizemap().GetSizeClass(
              CppPolicy().AlignAs(align.align()).AccessAsCold(), size,
              &size_class))) {
        // We couldn't calculate the size class, which means size > kMaxSize.
        ASSERT(size > kMaxSize || align.align() > alignof(std::max_align_t));
        static_assert(kMaxSize >= kPageSize,
                      "kMaxSize must be at least kPageSize");
        return FreePages(ptr);
      }

      return do_free_with_size_class<true, Hooks::RUN>(ptr, size_class);
    }
  }

  // At this point, since ptr's tag bit is 1, it means that it
  // cannot be nullptr either. Thus all code below may rely on ptr !=
  // nullptr. And particularly, since we're only caller of
  // do_free_with_size_class with have_size_class == true, it means
  // have_size_class implies ptr != nullptr.
  ASSERT(ptr != nullptr);

  uint32_t size_class;
  if (ABSL_PREDICT_FALSE(!tc_globals.sizemap().GetSizeClass(
          CppPolicy().AlignAs(align.align()).InSameNumaPartitionAs(ptr), size,
          &size_class))) {
    // We couldn't calculate the size class, which means size > kMaxSize.
    ASSERT(size > kMaxSize || align.align() > alignof(std::max_align_t));
    static_assert(kMaxSize >= kPageSize, "kMaxSize must be at least kPageSize");
    return FreePages(ptr);
  }

  return do_free_with_size_class<true, Hooks::RUN>(ptr, size_class);
}

inline size_t GetSize(const void* ptr) {
  if (ptr == nullptr) return 0;
  const PageId p = PageIdContaining(ptr);
  size_t size_class = tc_globals.pagemap().sizeclass(p);
  if (size_class != 0) {
    return tc_globals.sizemap().class_to_size(size_class);
  } else {
    const Span* span = tc_globals.pagemap().GetExistingDescriptor(p);
    if (span->sampled()) {
      if (tc_globals.guardedpage_allocator().PointerIsMine(ptr)) {
        return tc_globals.guardedpage_allocator().GetRequestedSize(ptr);
      }
      return span->sampled_allocation()->sampled_stack.allocated_size;
    } else {
      return span->bytes_in_span();
    }
  }
}

// Checks that an asserted object size for <ptr> is valid.
template <typename AlignPolicy>
bool CorrectSize(void* ptr, size_t size, AlignPolicy align) {
  // size == 0 means we got no hint from sized delete, so we certainly don't
  // have an incorrect one.
  if (size == 0) return true;
  if (ptr == nullptr) return true;
  uint32_t size_class = 0;
  // Round-up passed in size to how much tcmalloc allocates for that size.
  if (tc_globals.guardedpage_allocator().PointerIsMine(ptr)) {
    size = tc_globals.guardedpage_allocator().GetRequestedSize(ptr);
  } else if (tc_globals.sizemap().GetSizeClass(
                 CppPolicy().AlignAs(align.align()), size, &size_class)) {
    size = tc_globals.sizemap().class_to_size(size_class);
  } else {
    size = BytesToLengthCeil(size).in_bytes();
  }
  size_t actual = GetSize(ptr);
  if (ABSL_PREDICT_TRUE(actual == size)) return true;
  // We might have had a cold size class, so actual > size.  If we did not use
  // size returning new, the caller may not know this occurred.
  //
  // Nonetheless, it is permitted to pass a size anywhere in [requested, actual]
  // to sized delete.
  if (actual > size && IsSampledMemory(ptr)) {
    if (tc_globals.sizemap().GetSizeClass(
            CppPolicy().AlignAs(align.align()).AccessAsCold(), size,
            &size_class)) {
      size = tc_globals.sizemap().class_to_size(size_class);
      if (actual == size) {
        return true;
      }
    }
  }
  Log(kLog, __FILE__, __LINE__, "size check failed", actual, size, size_class);
  return false;
}

// Checks that an asserted object <ptr> has <align> alignment.
bool CorrectAlignment(void* ptr, std::align_val_t alignment) {
  size_t align = static_cast<size_t>(alignment);
  ASSERT(absl::has_single_bit(align));
  return ((reinterpret_cast<uintptr_t>(ptr) & (align - 1)) == 0);
}

// Helpers for use by exported routines below or inside debugallocation.cc:

inline void do_malloc_stats() { PrintStats(1); }

inline int do_malloc_trim(size_t pad) {
  // We ignore pad for now and just do a best effort release of pages.
  static_cast<void>(pad);
  return MallocExtension_Internal_ReleaseMemoryToSystem(0) != 0 ? 1 : 0;
}

inline int do_mallopt(int cmd, int value) {
  return 1;  // Indicates error
}

#ifdef TCMALLOC_HAVE_STRUCT_MALLINFO
inline struct mallinfo do_mallinfo() {
  TCMallocStats stats;
  ExtractTCMallocStats(&stats, false);

  // Just some of the fields are filled in.
  struct mallinfo info;
  memset(&info, 0, sizeof(info));

  // Unfortunately, the struct contains "int" field, so some of the
  // size values will be truncated.
  info.arena = static_cast<int>(stats.pageheap.system_bytes);
  info.fsmblks = static_cast<int>(stats.thread_bytes + stats.central_bytes +
                                  stats.transfer_bytes);
  info.fordblks = static_cast<int>(stats.pageheap.free_bytes +
                                   stats.pageheap.unmapped_bytes);
  info.uordblks = static_cast<int>(InUseByApp(stats));

  return info;
}
#endif  // TCMALLOC_HAVE_STRUCT_MALLINFO

#ifdef TCMALLOC_HAVE_STRUCT_MALLINFO2
inline struct mallinfo2 do_mallinfo2() {
  TCMallocStats stats;
  ExtractTCMallocStats(&stats, false);

  // Just some of the fields are filled in.
  struct mallinfo2 info;
  memset(&info, 0, sizeof(info));

  info.arena = static_cast<size_t>(stats.pageheap.system_bytes);
  info.fsmblks = static_cast<size_t>(stats.thread_bytes + stats.central_bytes +
                                     stats.transfer_bytes);
  info.fordblks = static_cast<size_t>(stats.pageheap.free_bytes +
                                      stats.pageheap.unmapped_bytes);
  info.uordblks = static_cast<size_t>(InUseByApp(stats));

  return info;
}
#endif

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

using tcmalloc::tcmalloc_internal::AllocSmall;
using tcmalloc::tcmalloc_internal::CppPolicy;
using tcmalloc::tcmalloc_internal::do_free_no_hooks;
#ifdef TCMALLOC_HAVE_STRUCT_MALLINFO
using tcmalloc::tcmalloc_internal::do_mallinfo;
#endif
#ifdef TCMALLOC_HAVE_STRUCT_MALLINFO2
using tcmalloc::tcmalloc_internal::do_mallinfo2;
#endif
using tcmalloc::tcmalloc_internal::do_malloc_pages;
using tcmalloc::tcmalloc_internal::do_malloc_stats;
using tcmalloc::tcmalloc_internal::do_malloc_trim;
using tcmalloc::tcmalloc_internal::do_mallopt;
using tcmalloc::tcmalloc_internal::GetThreadSampler;
using tcmalloc::tcmalloc_internal::MallocPolicy;
using tcmalloc::tcmalloc_internal::SetClassCapacity;
using tcmalloc::tcmalloc_internal::tc_globals;
using tcmalloc::tcmalloc_internal::UsePerCpuCache;

#ifdef TCMALLOC_DEPRECATED_PERTHREAD
using tcmalloc::tcmalloc_internal::ThreadCache;
#endif  // TCMALLOC_DEPRECATED_PERTHREAD

// Slow path implementation.
// This function is used by `fast_alloc` if the allocation requires page sized
// allocations or some complex logic is required such as initialization,
// invoking new/delete hooks, sampling, etc.
//
// TODO(b/130771275):  This function is marked as static, rather than appearing
// in the anonymous namespace, to workaround incomplete heapz filtering.
template <typename Policy, typename CapacityPtr = std::nullptr_t>
static void* ABSL_ATTRIBUTE_SECTION(google_malloc)
    slow_alloc(Policy policy, size_t size, CapacityPtr capacity = nullptr) {
  tc_globals.InitIfNecessary();
  GetThreadSampler()->UpdateFastPathState();
  void* p;
  uint32_t size_class;
  bool is_small = tc_globals.sizemap().GetSizeClass(policy, size, &size_class);
  if (ABSL_PREDICT_TRUE(is_small)) {
    p = AllocSmall(policy, size_class, size, capacity);
  } else {
    p = do_malloc_pages(policy, size, 1, capacity);
    if (ABSL_PREDICT_FALSE(p == nullptr)) {
      return Policy::handle_oom(size);
    }
  }
  if (Policy::invoke_hooks()) {
  }
  return p;
}

template <typename Policy, typename CapacityPtr = std::nullptr_t>
static inline void* ABSL_ATTRIBUTE_ALWAYS_INLINE
fast_alloc(Policy policy, size_t size, CapacityPtr capacity = nullptr) {
  // If size is larger than kMaxSize, it's not fast-path anymore. In
  // such case, GetSizeClass will return false, and we'll delegate to the slow
  // path. If malloc is not yet initialized, we may end up with size_class == 0
  // (regardless of size), but in this case should also delegate to the slow
  // path by the fast path check further down.
  uint32_t size_class;
  bool is_small = tc_globals.sizemap().GetSizeClass(policy, size, &size_class);
  if (ABSL_PREDICT_FALSE(!is_small)) {
    return slow_alloc(policy, size, capacity);
  }

  // When using per-thread caches, we have to check for the presence of the
  // cache for this thread before we try to sample, as slow_alloc will
  // also try to sample the allocation.
#ifdef TCMALLOC_DEPRECATED_PERTHREAD
  ThreadCache* const cache = ThreadCache::GetCacheIfPresent();
  if (ABSL_PREDICT_FALSE(cache == nullptr)) {
    return slow_alloc(policy, size, capacity);
  }
#endif
  // TryRecordAllocationFast() returns true if no extra logic is required, e.g.:
  // - this allocation does not need to be sampled
  // - no new/delete hooks need to be invoked
  // - no need to initialize thread globals, data or caches.
  // The method updates 'bytes until next sample' thread sampler counters.
  if (ABSL_PREDICT_FALSE(!GetThreadSampler()->TryRecordAllocationFast(size))) {
    return slow_alloc(policy, size, capacity);
  }

  // Fast path implementation for allocating small size memory.
  // This code should only be reached if all of the below conditions are met:
  // - the size does not exceed the maximum size (size class > 0)
  // - cpu / thread cache data has been initialized.
  // - the allocation is not subject to sampling / gwp-asan.
  // - no new/delete hook is installed and required to be called.
  ASSERT(size_class != 0);
  void* ret;
#ifndef TCMALLOC_DEPRECATED_PERTHREAD
  // The CPU cache should be ready.
  ret = tc_globals.cpu_cache().Allocate<Policy::handle_oom>(size_class);
#else   // !defined(TCMALLOC_DEPRECATED_PERTHREAD)
  // The ThreadCache should be ready.
  ASSERT(cache != nullptr);
  ret = cache->Allocate<Policy::handle_oom>(size_class);
#endif  // TCMALLOC_DEPRECATED_PERTHREAD
  if (!Policy::can_return_nullptr()) {
    ASSUME(ret != nullptr);
  }
  SetClassCapacity(ret, size_class, capacity);
  return ret;
}

using tcmalloc::tcmalloc_internal::GetOwnership;
using tcmalloc::tcmalloc_internal::GetSize;

extern "C" size_t MallocExtension_Internal_GetAllocatedSize(const void* ptr) {
  ASSERT(!ptr ||
         GetOwnership(ptr) != tcmalloc::MallocExtension::Ownership::kNotOwned);
  return GetSize(ptr);
}

extern "C" void MallocExtension_Internal_MarkThreadBusy() {
  // Allocate to force the creation of a thread cache, but avoid
  // invoking any hooks.
  tc_globals.InitIfNecessary();

  if (UsePerCpuCache(tc_globals)) {
    return;
  }

  do_free_no_hooks(slow_alloc(CppPolicy().Nothrow().WithoutHooks(), 0));
}

absl::StatusOr<tcmalloc::malloc_tracing_extension::AllocatedAddressRanges>
MallocTracingExtension_Internal_GetAllocatedAddressRanges() {
  tcmalloc::malloc_tracing_extension::AllocatedAddressRanges
      allocated_address_ranges;
  constexpr float kAllocatedSpansSizeReserveFactor = 1.2;
  constexpr int kMaxAttempts = 10;
  for (int i = 0; i < kMaxAttempts; i++) {
    int estimated_span_count;
    {
      absl::base_internal::SpinLockHolder l(
          &tcmalloc::tcmalloc_internal::pageheap_lock);
      estimated_span_count = tc_globals.span_allocator().stats().total;
    }
    // We need to avoid allocation events during GetAllocatedSpans, as that may
    // cause a deadlock on pageheap_lock. To this end, we ensure that the result
    // vector already has a capacity greater than the current total span count.
    allocated_address_ranges.spans.reserve(estimated_span_count *
                                           kAllocatedSpansSizeReserveFactor);
    int actual_span_count =
        tc_globals.pagemap().GetAllocatedSpans(allocated_address_ranges.spans);
    if (allocated_address_ranges.spans.size() == actual_span_count) {
      return allocated_address_ranges;
    }
    allocated_address_ranges.spans.clear();
  }
  return absl::InternalError(
      "Could not fetch all Spans due to insufficient reserved capacity in the "
      "output vector.");
}

//-------------------------------------------------------------------
// Exported routines
//-------------------------------------------------------------------

using tcmalloc::tcmalloc_internal::AlignAsPolicy;
using tcmalloc::tcmalloc_internal::CorrectAlignment;
using tcmalloc::tcmalloc_internal::DefaultAlignPolicy;
using tcmalloc::tcmalloc_internal::do_free;
using tcmalloc::tcmalloc_internal::do_free_with_size;
using tcmalloc::tcmalloc_internal::GetPageSize;
using tcmalloc::tcmalloc_internal::MallocAlignPolicy;
using tcmalloc::tcmalloc_internal::MultiplyOverflow;

// depends on TCMALLOC_HAVE_STRUCT_MALLINFO, so needs to come after that.
#include "tcmalloc/libc_override.h"

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalMalloc(
    size_t size) noexcept {
  return fast_alloc(MallocPolicy(), size);
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalNew(size_t size) {
  return fast_alloc(CppPolicy(), size);
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalNewNothrow(
    size_t size, const std::nothrow_t&) noexcept {
  return fast_alloc(CppPolicy().Nothrow(), size);
}

extern "C" ABSL_CACHELINE_ALIGNED ABSL_ATTRIBUTE_SECTION(google_malloc)
    tcmalloc::sized_ptr_t tcmalloc_size_returning_operator_new(size_t size) {
  size_t capacity;
  void* p = fast_alloc(CppPolicy(), size, &capacity);
  return {p, capacity};
}

extern "C" ABSL_CACHELINE_ALIGNED ABSL_ATTRIBUTE_SECTION(
    google_malloc) tcmalloc::sized_ptr_t
    tcmalloc_size_returning_operator_new_aligned(size_t size,
                                                 std::align_val_t alignment) {
  ASSERT(absl::has_single_bit(static_cast<size_t>(alignment)));
  size_t capacity;
  void* p = fast_alloc(CppPolicy().AlignAs(alignment), size, &capacity);
  return {p, capacity};
}

extern "C" ABSL_CACHELINE_ALIGNED ABSL_ATTRIBUTE_SECTION(google_malloc)
    tcmalloc::sized_ptr_t tcmalloc_size_returning_operator_new_hot_cold(
        size_t size, tcmalloc::hot_cold_t hot_cold) {
  size_t capacity;
  void* p = static_cast<uint8_t>(hot_cold) >= uint8_t{128}
                ? fast_alloc(CppPolicy().AccessAsHot(), size, &capacity)
                : fast_alloc(CppPolicy().AccessAsCold(), size, &capacity);
  return {p, capacity};
}

extern "C" ABSL_CACHELINE_ALIGNED ABSL_ATTRIBUTE_SECTION(google_malloc)
    tcmalloc::sized_ptr_t tcmalloc_size_returning_operator_new_aligned_hot_cold(
        size_t size, std::align_val_t alignment,
        tcmalloc::hot_cold_t hot_cold) {
  ASSERT(absl::has_single_bit(static_cast<size_t>(alignment)));
  size_t capacity;
  void* p = static_cast<uint8_t>(hot_cold) >= uint8_t{128}
                ? fast_alloc(CppPolicy().AlignAs(alignment).AccessAsHot(), size,
                             &capacity)
                : fast_alloc(CppPolicy().AlignAs(alignment).AccessAsCold(),
                             size, &capacity);
  return {p, capacity};
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalMemalign(
    size_t align, size_t size) noexcept {
  ASSERT(absl::has_single_bit(align));
  return fast_alloc(MallocPolicy().AlignAs(align), size);
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalNewAligned(
    size_t size, std::align_val_t alignment) {
  ASSERT(absl::has_single_bit(static_cast<size_t>(alignment)));
  return fast_alloc(CppPolicy().AlignAs(alignment), size);
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalNewAlignedNothrow(
    size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  ASSERT(absl::has_single_bit(static_cast<size_t>(alignment)));
  return fast_alloc(CppPolicy().Nothrow().AlignAs(alignment), size);
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalCalloc(
    size_t n, size_t elem_size) noexcept {
  size_t size;
  if (ABSL_PREDICT_FALSE(MultiplyOverflow(n, elem_size, &size))) {
    return MallocPolicy::handle_oom(std::numeric_limits<size_t>::max());
  }
  void* result = fast_alloc(MallocPolicy(), size);
  if (ABSL_PREDICT_TRUE(result != nullptr)) {
    memset(result, 0, size);
  }
  return result;
}

static inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* do_realloc(void* old_ptr,
                                                            size_t new_size) {
  tc_globals.InitIfNecessary();
  // Get the size of the old entry
  const size_t old_size = GetSize(old_ptr);

  // Reallocate if the new size is larger than the old size,
  // or if the new size is significantly smaller than the old size.
  // We do hysteresis to avoid resizing ping-pongs:
  //    . If we need to grow, grow to max(new_size, old_size * 1.X)
  //    . Don't shrink unless new_size < old_size * 0.Y
  // X and Y trade-off time for wasted space.  For now we do 1.25 and 0.5.
  const size_t min_growth = std::min(
      old_size / 4,
      std::numeric_limits<size_t>::max() - old_size);  // Avoid overflow.
  const size_t lower_bound_to_grow = old_size + min_growth;
  const size_t upper_bound_to_shrink = old_size / 2;
  if ((new_size > old_size) || (new_size < upper_bound_to_shrink)) {
    // Need to reallocate.
    void* new_ptr = nullptr;

    if (new_size > old_size && new_size < lower_bound_to_grow) {
      // Avoid fast_alloc() reporting a hook with the lower bound size
      // as we the expectation for pointer returning allocation functions
      // is that malloc hooks are invoked with the requested_size.
      new_ptr = fast_alloc(MallocPolicy().Nothrow().WithoutHooks(),
                           lower_bound_to_grow);
      if (new_ptr != nullptr) {
      }
    }
    if (new_ptr == nullptr) {
      // Either new_size is not a tiny increment, or last do_malloc failed.
      new_ptr = fast_alloc(MallocPolicy(), new_size);
    }
    if (new_ptr == nullptr) {
      return nullptr;
    }
    memcpy(new_ptr, old_ptr, ((old_size < new_size) ? old_size : new_size));
    // We could use a variant of do_free() that leverages the fact
    // that we already know the sizeclass of old_ptr.  The benefit
    // would be small, so don't bother.
    do_free(old_ptr);
    return new_ptr;
  } else {
    return old_ptr;
  }
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalRealloc(
    void* ptr, size_t size) noexcept {
  if (ptr == nullptr) {
    return fast_alloc(MallocPolicy(), size);
  }
  if (size == 0) {
    do_free(ptr);
    return nullptr;
  }
  return do_realloc(ptr, size);
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalReallocArray(
    void* ptr, size_t n, size_t elem_size) noexcept {
  size_t size;
  if (ABSL_PREDICT_FALSE(MultiplyOverflow(n, elem_size, &size))) {
    return MallocPolicy::handle_oom(std::numeric_limits<size_t>::max());
  }
  if (ptr == nullptr) {
    return fast_alloc(MallocPolicy(), size);
  }
  if (size == 0) {
    do_free(ptr);
    return nullptr;
  }
  return do_realloc(ptr, size);
}

extern "C" ABSL_CACHELINE_ALIGNED ABSL_ATTRIBUTE_SECTION(
    google_malloc) tcmalloc::sized_ptr_t
    tcmalloc_size_returning_operator_new_nothrow(size_t size) noexcept {
  size_t capacity;
  void* p = fast_alloc(CppPolicy().Nothrow(), size, &capacity);
  return {p, capacity};
}

extern "C" ABSL_CACHELINE_ALIGNED ABSL_ATTRIBUTE_SECTION(google_malloc)
    tcmalloc::sized_ptr_t tcmalloc_size_returning_operator_new_aligned_nothrow(
        size_t size, std::align_val_t alignment) noexcept {
  ASSERT(absl::has_single_bit(static_cast<size_t>(alignment)));
  size_t capacity;
  void* p =
      fast_alloc(CppPolicy().AlignAs(alignment).Nothrow(), size, &capacity);
  return {p, capacity};
}

extern "C" ABSL_CACHELINE_ALIGNED ABSL_ATTRIBUTE_SECTION(google_malloc)
    tcmalloc::sized_ptr_t tcmalloc_size_returning_operator_new_hot_cold_nothrow(
        size_t size, tcmalloc::hot_cold_t hot_cold) noexcept {
  size_t capacity;
  void* p =
      static_cast<uint8_t>(hot_cold) >= uint8_t{128}
          ? fast_alloc(CppPolicy().AccessAsHot().Nothrow(), size, &capacity)
          : fast_alloc(CppPolicy().AccessAsCold().Nothrow(), size, &capacity);
  return {p, capacity};
}

extern "C" ABSL_CACHELINE_ALIGNED ABSL_ATTRIBUTE_SECTION(
    google_malloc) tcmalloc::sized_ptr_t
    tcmalloc_size_returning_operator_new_aligned_hot_cold_nothrow(
        size_t size, std::align_val_t alignment,
        tcmalloc::hot_cold_t hot_cold) noexcept {
  ASSERT(absl::has_single_bit(static_cast<size_t>(alignment)));
  size_t capacity;
  void* p =
      static_cast<uint8_t>(hot_cold) >= uint8_t{128}
          ? fast_alloc(CppPolicy().AlignAs(alignment).AccessAsHot().Nothrow(),
                       size, &capacity)
          : fast_alloc(CppPolicy().AlignAs(alignment).AccessAsCold().Nothrow(),
                       size, &capacity);
  return {p, capacity};
}

extern "C" ABSL_CACHELINE_ALIGNED void TCMallocInternalFree(
    void* ptr) noexcept {
  do_free(ptr);
}

extern "C" ABSL_CACHELINE_ALIGNED void TCMallocInternalFreeSized(
    void* ptr, size_t size) noexcept {
  do_free_with_size(ptr, size, MallocAlignPolicy());
}

extern "C" ABSL_CACHELINE_ALIGNED void TCMallocInternalFreeAlignedSized(
    void* ptr, size_t align, size_t size) noexcept {
  ASSERT(absl::has_single_bit(align));
  do_free_with_size(ptr, size, AlignAsPolicy(align));
}

extern "C" void TCMallocInternalCfree(void* ptr) noexcept
    TCMALLOC_ALIAS(TCMallocInternalFree);

extern "C" ABSL_CACHELINE_ALIGNED void TCMallocInternalSdallocx(
    void* ptr, size_t size, int flags) noexcept {
  size_t alignment = alignof(std::max_align_t);

  if (ABSL_PREDICT_FALSE(flags != 0)) {
    ASSERT((flags & ~0x3f) == 0);
    alignment = static_cast<size_t>(1ull << (flags & 0x3f));
  }

  return do_free_with_size(ptr, size, AlignAsPolicy(alignment));
}

extern "C" void TCMallocInternalDelete(void* p) noexcept
    TCMALLOC_ALIAS(TCMallocInternalFree);

extern "C" void TCMallocInternalDeleteAligned(
    void* p, std::align_val_t alignment) noexcept
#if defined(NDEBUG)
    TCMALLOC_ALIAS(TCMallocInternalDelete);
#else
{
  // Note: The aligned delete/delete[] implementations differ slightly from
  // their respective aliased implementations to take advantage of checking the
  // passed-in alignment.
  ASSERT(CorrectAlignment(p, alignment));
  return TCMallocInternalDelete(p);
}
#endif

extern "C" ABSL_CACHELINE_ALIGNED void TCMallocInternalDeleteSized(
    void* p, size_t size) noexcept {
  do_free_with_size(p, size, DefaultAlignPolicy());
}

extern "C" ABSL_CACHELINE_ALIGNED void TCMallocInternalDeleteSizedAligned(
    void* p, size_t t, std::align_val_t alignment) noexcept {
  ASSERT(absl::has_single_bit(static_cast<size_t>(alignment)));
  return do_free_with_size(p, t, AlignAsPolicy(alignment));
}

extern "C" void TCMallocInternalDeleteArraySized(void* p, size_t size) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteSized);

extern "C" void TCMallocInternalDeleteArraySizedAligned(
    void* p, size_t t, std::align_val_t alignment) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteSizedAligned);

// Standard C++ library implementations define and use this
// (via ::operator delete(ptr, nothrow)).
// But it's really the same as normal delete, so we just do the same thing.
extern "C" void TCMallocInternalDeleteNothrow(void* p,
                                              const std::nothrow_t&) noexcept
    TCMALLOC_ALIAS(TCMallocInternalFree);

extern "C" void TCMallocInternalDeleteAlignedNothrow(
    void* p, std::align_val_t alignment, const std::nothrow_t&) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteAligned);

extern "C" void* TCMallocInternalNewArray(size_t size)
    TCMALLOC_ALIAS(TCMallocInternalNew);

extern "C" void* TCMallocInternalNewArrayAligned(size_t size,
                                                 std::align_val_t alignment)
    TCMALLOC_ALIAS(TCMallocInternalNewAligned);

extern "C" void* TCMallocInternalNewArrayNothrow(
    size_t size, const std::nothrow_t& nt) noexcept
    TCMALLOC_ALIAS(TCMallocInternalNewNothrow);

extern "C" void* TCMallocInternalNewArrayAlignedNothrow(
    size_t size, std::align_val_t alignment, const std::nothrow_t& nt) noexcept
    TCMALLOC_ALIAS(TCMallocInternalNewAlignedNothrow);

extern "C" void TCMallocInternalDeleteArray(void* p) noexcept
    TCMALLOC_ALIAS(TCMallocInternalFree);

extern "C" void TCMallocInternalDeleteArrayAligned(
    void* p, std::align_val_t alignment) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteAligned);

extern "C" void TCMallocInternalDeleteArrayNothrow(
    void* p, const std::nothrow_t& nt) noexcept
    TCMALLOC_ALIAS(TCMallocInternalFree);

extern "C" void TCMallocInternalDeleteArrayAlignedNothrow(
    void* p, std::align_val_t alignment, const std::nothrow_t& nt) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteAligned);

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalAlignedAlloc(
    size_t align, size_t size) noexcept {
  // See https://www.open-std.org/jtc1/sc22/wg14/www/docs/summary.htm#dr_460.
  // The standard was updated to say that if align is not supported by the
  // implementation, a null pointer should be returned. We require alignment to
  // be greater than 0 and a power of 2.
  if (ABSL_PREDICT_FALSE(align == 0 || !absl::has_single_bit(align))) {
    // glibc, FreeBSD, and NetBSD manuals all document aligned_alloc() as
    // returning EINVAL if align is not a power of 2. We do the same.
    errno = EINVAL;
    return nullptr;
  }
  return fast_alloc(MallocPolicy().AlignAs(align), size);
}

extern "C" ABSL_CACHELINE_ALIGNED int TCMallocInternalPosixMemalign(
    void** result_ptr, size_t align, size_t size) noexcept {
  ASSERT(result_ptr != nullptr);
  if (ABSL_PREDICT_FALSE(((align % sizeof(void*)) != 0) ||
                         !absl::has_single_bit(align))) {
    return EINVAL;
  }
  void* result = fast_alloc(MallocPolicy().AlignAs(align), size);
  if (ABSL_PREDICT_FALSE(result == nullptr)) {
    return ENOMEM;
  }
  *result_ptr = result;
  return 0;
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalValloc(
    size_t size) noexcept {
  // Allocate page-aligned object of length >= size bytes
  return fast_alloc(MallocPolicy().AlignAs(GetPageSize()), size);
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalPvalloc(
    size_t size) noexcept {
  // Round up size to a multiple of pagesize
  size_t page_size = GetPageSize();
  if (size == 0) {    // pvalloc(0) should allocate one page, according to
    size = page_size;  // http://man.free4web.biz/man3/libmpatrol.3.html
  }
  size = (size + page_size - 1) & ~(page_size - 1);
  return fast_alloc(MallocPolicy().AlignAs(page_size), size);
}

extern "C" void TCMallocInternalMallocStats(void) noexcept {
  do_malloc_stats();
}

extern "C" int TCMallocInternalMallocTrim(size_t pad) noexcept {
  return do_malloc_trim(pad);
}

extern "C" int TCMallocInternalMallOpt(int cmd, int value) noexcept {
  return do_mallopt(cmd, value);
}

#ifdef TCMALLOC_HAVE_STRUCT_MALLINFO
extern "C" struct mallinfo TCMallocInternalMallInfo(void) noexcept {
  return do_mallinfo();
}
#endif

#ifdef TCMALLOC_HAVE_STRUCT_MALLINFO2
extern "C" struct mallinfo2 TCMallocInternalMallInfo2(void) noexcept {
  return do_mallinfo2();
}
#endif

extern "C" int TCMallocInternalMallocInfo(int opts ABSL_ATTRIBUTE_UNUSED,
                                          FILE* fp) noexcept {
  fputs("<malloc></malloc>\n", fp);
  return 0;
}

extern "C" size_t TCMallocInternalMallocSize(void* ptr) noexcept {
  ASSERT(GetOwnership(ptr) != tcmalloc::MallocExtension::Ownership::kNotOwned);
  return GetSize(ptr);
}

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// The constructor allocates an object to ensure that initialization
// runs before main(), and therefore we do not have a chance to become
// multi-threaded before initialization.  We also create the TSD key
// here.  Presumably by the time this constructor runs, glibc is in
// good enough shape to handle pthread_key_create().
//
// The destructor prints stats when the program exits.
class TCMallocGuard {
 public:
  TCMallocGuard() {
    TCMallocInternalFree(TCMallocInternalMalloc(1));
    ThreadCache::InitTSD();
    TCMallocInternalFree(TCMallocInternalMalloc(1));
  }
};

static TCMallocGuard module_enter_exit_hook;

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

ABSL_CACHELINE_ALIGNED void* operator new(
    size_t size, tcmalloc::hot_cold_t hot_cold) noexcept(false) {
  if (static_cast<uint8_t>(hot_cold) >= uint8_t{128}) {
    return fast_alloc(CppPolicy().AccessAsHot(), size, nullptr);
  } else {
    return fast_alloc(CppPolicy().AccessAsCold(), size, nullptr);
  }
}

ABSL_CACHELINE_ALIGNED void* operator new(
    size_t size, std::nothrow_t, tcmalloc::hot_cold_t hot_cold) noexcept {
  if (static_cast<uint8_t>(hot_cold) >= uint8_t{128}) {
    return fast_alloc(CppPolicy().Nothrow().AccessAsHot(), size, nullptr);
  } else {
    return fast_alloc(CppPolicy().Nothrow().AccessAsCold(), size, nullptr);
  }
}

ABSL_CACHELINE_ALIGNED void* operator new(
    size_t size, std::align_val_t align,
    tcmalloc::hot_cold_t hot_cold) noexcept(false) {
  ASSERT(absl::has_single_bit(static_cast<size_t>(align)));
  if (static_cast<uint8_t>(hot_cold) >= uint8_t{128}) {
    return fast_alloc(CppPolicy().AlignAs(align).AccessAsHot(), size, nullptr);
  } else {
    return fast_alloc(CppPolicy().AlignAs(align).AccessAsCold(), size, nullptr);
  }
}

ABSL_CACHELINE_ALIGNED void* operator new(
    size_t size, std::align_val_t align, std::nothrow_t,
    tcmalloc::hot_cold_t hot_cold) noexcept {
  ASSERT(absl::has_single_bit(static_cast<size_t>(align)));
  if (static_cast<uint8_t>(hot_cold) >= uint8_t{128}) {
    return fast_alloc(CppPolicy().Nothrow().AlignAs(align).AccessAsHot(), size,
                      nullptr);
  } else {
    return fast_alloc(CppPolicy().Nothrow().AlignAs(align).AccessAsCold(), size,
                      nullptr);
  }
}

ABSL_CACHELINE_ALIGNED void* operator new[](
    size_t size, tcmalloc::hot_cold_t hot_cold) noexcept(false) {
  if (static_cast<uint8_t>(hot_cold) >= uint8_t{128}) {
    return fast_alloc(CppPolicy().AccessAsHot(), size, nullptr);
  } else {
    return fast_alloc(CppPolicy().AccessAsCold(), size, nullptr);
  }
}

ABSL_CACHELINE_ALIGNED void* operator new[](
    size_t size, std::nothrow_t, tcmalloc::hot_cold_t hot_cold) noexcept {
  if (static_cast<uint8_t>(hot_cold) >= uint8_t{128}) {
    return fast_alloc(CppPolicy().Nothrow().AccessAsHot(), size, nullptr);
  } else {
    return fast_alloc(CppPolicy().Nothrow().AccessAsCold(), size, nullptr);
  }
}

ABSL_CACHELINE_ALIGNED void* operator new[](
    size_t size, std::align_val_t align,
    tcmalloc::hot_cold_t hot_cold) noexcept(false) {
  ASSERT(absl::has_single_bit(static_cast<size_t>(align)));
  if (static_cast<uint8_t>(hot_cold) >= uint8_t{128}) {
    return fast_alloc(CppPolicy().AlignAs(align).AccessAsHot(), size, nullptr);
  } else {
    return fast_alloc(CppPolicy().AlignAs(align).AccessAsCold(), size, nullptr);
  }
}

ABSL_CACHELINE_ALIGNED void* operator new[](
    size_t size, std::align_val_t align, std::nothrow_t,
    tcmalloc::hot_cold_t hot_cold) noexcept {
  ASSERT(absl::has_single_bit(static_cast<size_t>(align)));
  if (static_cast<uint8_t>(hot_cold) >= uint8_t{128}) {
    return fast_alloc(CppPolicy().Nothrow().AlignAs(align).AccessAsHot(), size,
                      nullptr);
  } else {
    return fast_alloc(CppPolicy().Nothrow().AlignAs(align).AccessAsCold(), size,
                      nullptr);
  }
}
