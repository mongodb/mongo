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
// Common definitions for tcmalloc code.

#ifndef TCMALLOC_COMMON_H_
#define TCMALLOC_COMMON_H_

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>

#include "absl/base/attributes.h"
#include "absl/base/dynamic_annotations.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/base/optimization.h"
#include "absl/numeric/bits.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/size_class_info.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

static_assert(sizeof(void*) == 8);

//-------------------------------------------------------------------
// Configuration
//-------------------------------------------------------------------

// There are four different models for tcmalloc which are created by defining a
// set of constant variables differently:
//
// DEFAULT:
//   The default configuration strives for good performance while trying to
//   minimize fragmentation.  It uses a smaller page size to reduce
//   fragmentation, but allocates per-thread and per-cpu capacities similar to
//   TCMALLOC_LARGE_PAGES / TCMALLOC_256K_PAGES.
//
// TCMALLOC_LARGE_PAGES:
//   Larger page sizes increase the bookkeeping granularity used by TCMalloc for
//   its allocations.  This can reduce PageMap size and traffic to the
//   innermost cache (the page heap), but can increase memory footprints.  As
//   TCMalloc will not reuse a page for a different allocation size until the
//   entire page is deallocated, this can be a source of increased memory
//   fragmentation.
//
//   Historically, larger page sizes improved lookup performance for the
//   pointer-to-size lookup in the PageMap that was part of the critical path.
//   With most deallocations leveraging C++14's sized delete feature
//   (https://isocpp.org/files/papers/n3778.html), this optimization is less
//   significant.
//
// TCMALLOC_256K_PAGES
//   This configuration uses an even larger page size (256KB) as the unit of
//   accounting granularity.
//
// TCMALLOC_SMALL_BUT_SLOW:
//   Used for situations where minimizing the memory footprint is the most
//   desirable attribute, even at the cost of performance.
//
// The constants that vary between models are:
//
//   kPageShift - Shift amount used to compute the page size.
//   kNumBaseClasses - Number of size classes serviced by bucket allocators
//   kMaxSize - Maximum size serviced by bucket allocators (thread/cpu/central)
//   kMinThreadCacheSize - The minimum size in bytes of each ThreadCache.
//   kMaxThreadCacheSize - The maximum size in bytes of each ThreadCache.
//   kDefaultOverallThreadCacheSize - The maximum combined size in bytes of all
//     ThreadCaches for an executable.
//   kStealAmount - The number of bytes one ThreadCache will steal from another
//     when the first ThreadCache is forced to Scavenge(), delaying the next
//     call to Scavenge for this thread.

// Older configurations had their own customized macros.  Convert them into
// a page-shift parameter that is checked below.

#ifndef TCMALLOC_PAGE_SHIFT
#ifdef TCMALLOC_SMALL_BUT_SLOW
#define TCMALLOC_PAGE_SHIFT 12
#define TCMALLOC_USE_PAGEMAP3
#elif defined(TCMALLOC_256K_PAGES)
#define TCMALLOC_PAGE_SHIFT 18
#elif defined(TCMALLOC_LARGE_PAGES)
#define TCMALLOC_PAGE_SHIFT 15
#else
#define TCMALLOC_PAGE_SHIFT 13
#endif
#else
#error "TCMALLOC_PAGE_SHIFT is an internal macro!"
#endif

#if TCMALLOC_PAGE_SHIFT == 12
inline constexpr size_t kPageShift = 12;
inline constexpr size_t kNumBaseClasses = 46;
inline constexpr bool kHasExpandedClasses = false;
inline constexpr size_t kMaxSize = 8 << 10;
inline constexpr size_t kMinThreadCacheSize = 4 * 1024;
inline constexpr size_t kMaxThreadCacheSize = 64 * 1024;
inline constexpr size_t kMaxCpuCacheSize = 10 * 1024;
inline constexpr size_t kDefaultOverallThreadCacheSize = kMaxThreadCacheSize;
inline constexpr size_t kStealAmount = kMinThreadCacheSize;
inline constexpr size_t kDefaultProfileSamplingRate = 1 << 19;
inline constexpr size_t kMinPages = 2;
#elif TCMALLOC_PAGE_SHIFT == 15
inline constexpr size_t kPageShift = 15;
inline constexpr size_t kNumBaseClasses = 78;
inline constexpr bool kHasExpandedClasses = true;
inline constexpr size_t kMaxSize = 256 * 1024;
inline constexpr size_t kMinThreadCacheSize = kMaxSize * 2;
inline constexpr size_t kMaxThreadCacheSize = 4 << 20;
inline constexpr size_t kMaxCpuCacheSize = 1.5 * 1024 * 1024;
inline constexpr size_t kDefaultOverallThreadCacheSize =
    8u * kMaxThreadCacheSize;
inline constexpr size_t kStealAmount = 1 << 16;
inline constexpr size_t kDefaultProfileSamplingRate = 1 << 21;
inline constexpr size_t kMinPages = 8;
#elif TCMALLOC_PAGE_SHIFT == 18
inline constexpr size_t kPageShift = 18;
inline constexpr size_t kNumBaseClasses = 89;
inline constexpr bool kHasExpandedClasses = true;
inline constexpr size_t kMaxSize = 256 * 1024;
inline constexpr size_t kMinThreadCacheSize = kMaxSize * 2;
inline constexpr size_t kMaxThreadCacheSize = 4 << 20;
inline constexpr size_t kMaxCpuCacheSize = 1.5 * 1024 * 1024;
inline constexpr size_t kDefaultOverallThreadCacheSize =
    8u * kMaxThreadCacheSize;
inline constexpr size_t kStealAmount = 1 << 16;
inline constexpr size_t kDefaultProfileSamplingRate = 1 << 21;
inline constexpr size_t kMinPages = 8;
#elif TCMALLOC_PAGE_SHIFT == 13
inline constexpr size_t kPageShift = 13;
inline constexpr size_t kNumBaseClasses = 86;
inline constexpr bool kHasExpandedClasses = true;
inline constexpr size_t kMaxSize = 256 * 1024;
inline constexpr size_t kMinThreadCacheSize = kMaxSize * 2;
inline constexpr size_t kMaxThreadCacheSize = 4 << 20;
inline constexpr size_t kMaxCpuCacheSize = 1.5 * 1024 * 1024;
inline constexpr size_t kDefaultOverallThreadCacheSize =
    8u * kMaxThreadCacheSize;
inline constexpr size_t kStealAmount = 1 << 16;
inline constexpr size_t kDefaultProfileSamplingRate = 1 << 21;
inline constexpr size_t kMinPages = 8;
#else
#error "Unsupported TCMALLOC_PAGE_SHIFT value!"
#endif

// Sanitizers constrain the memory layout which causes problems with the
// enlarged tags required to represent NUMA partitions. Disable NUMA awareness
// to avoid failing to mmap memory.
#if defined(TCMALLOC_NUMA_AWARE) && !defined(MEMORY_SANITIZER) && \
    !defined(THREAD_SANITIZER)
inline constexpr size_t kNumaPartitions = 2;
#else
inline constexpr size_t kNumaPartitions = 1;
#endif

// We have copies of kNumBaseClasses size classes for each NUMA node, followed
// by any expanded classes.
inline constexpr size_t kExpandedClassesStart =
    kNumBaseClasses * kNumaPartitions;
inline constexpr size_t kNumClasses =
    kExpandedClassesStart + (kHasExpandedClasses ? kNumBaseClasses : 0);

// Size classes are often stored as uint32_t values, but there are some
// situations where we need to store a size class with as compact a
// representation as possible (e.g. in PageMap). Here we determine the integer
// type to use in these situations - i.e. the smallest integer type large
// enough to store values in the range [0,kNumClasses).
constexpr size_t kMaxClass = kNumClasses - 1;
using CompactSizeClass =
    std::conditional_t<kMaxClass <= std::numeric_limits<uint8_t>::max(),
                       uint8_t, uint16_t>;

// ~64K classes ought to be enough for anybody, but let's be sure.
static_assert(kMaxClass <= std::numeric_limits<CompactSizeClass>::max());

// Minimum/maximum number of batches in TransferCache per size class.
// Actual numbers depends on a number of factors, see TransferCache::Init
// for details.
inline constexpr size_t kMinObjectsToMove = 2;
inline constexpr size_t kMaxObjectsToMove = 128;

inline constexpr size_t kPageSize = 1 << kPageShift;
// Verify that the page size used is at least 8x smaller than the maximum
// element size in the thread cache.  This guarantees at most 12.5% internal
// fragmentation (1/8). When page size is 256k (kPageShift == 18), the benefit
// of increasing kMaxSize to be multiple of kPageSize is unclear. Object size
// profile data indicates that the number of simultaneously live objects (of
// size >= 256k) tends to be very small. Keeping those objects as 'large'
// objects won't cause too much memory waste, while heap memory reuse can be
// improved. Increasing kMaxSize to be too large has another bad side effect --
// the thread cache pressure is increased, which will in turn increase traffic
// between central cache and thread cache, leading to performance degradation.
static_assert((kMaxSize / kPageSize) >= kMinPages || kPageShift >= 18,
              "Ratio of kMaxSize / kPageSize is too small");

inline constexpr std::align_val_t kAlignment{8};
// log2 (kAlignment)
inline constexpr size_t kAlignmentShift =
    absl::bit_width(static_cast<size_t>(kAlignment) - 1u);

// The number of times that a deallocation can cause a freelist to
// go over its max_length() before shrinking max_length().
inline constexpr int kMaxOverages = 3;

// Maximum length we allow a per-thread free-list to have before we
// move objects from it into the corresponding central free-list.  We
// want this big to avoid locking the central free-list too often.  It
// should not hurt to make this list somewhat big because the
// scavenging code will shrink it down when its contents are not in use.
inline constexpr int kMaxDynamicFreeListLength = 8192;

enum class MemoryTag : uint8_t {
  // Sampled, infrequently allocated
  kSampled = 0x0,
  // Not sampled, NUMA partition 0
  kNormalP0 = 0x1,
  // Not sampled, NUMA partition 1
  kNormalP1 = (kNumaPartitions > 1) ? 0x2 : 0xff,
  // Not sampled
  kNormal = kNormalP0,
  // Cold
  kCold = (kNumaPartitions > 1) ? 0x4 : 0x2,
};

// We make kNormal and kCold disjoint so that IsCold implies IsSampled.  This
// allows us to avoid modifying the fast delete path in any way when cold-tagged
// memory allocations are absent.  We can overload the IsSampled check and then
// do a second check for whether the possibly-sampled allocation is actually
// IsCold.
static_assert((static_cast<uint8_t>(MemoryTag::kNormal) &
               static_cast<uint8_t>(MemoryTag::kCold)) == 0,
              "kNormal and kCold should have disjoint bit patterns");

inline constexpr uintptr_t kTagShift = std::min(kAddressBits - 4, 42);
inline constexpr uintptr_t kTagMask = uintptr_t{kNumaPartitions > 1 ? 0x7 : 0x3}
                                      << kTagShift;

inline bool IsSampledMemory(const void* ptr) {
  constexpr uintptr_t kSampledNormalMask = kNumaPartitions > 1 ? 0x3 : 0x1;

  static_assert(static_cast<uintptr_t>(MemoryTag::kNormalP0) &
                kSampledNormalMask);
  static_assert(static_cast<uintptr_t>(MemoryTag::kNormalP1) &
                kSampledNormalMask);

  const uintptr_t tag =
      (reinterpret_cast<uintptr_t>(ptr) & kTagMask) >> kTagShift;
  return (tag & kSampledNormalMask) ==
         static_cast<uintptr_t>(MemoryTag::kSampled);
}

inline bool IsNormalMemory(const void* ptr) { return !IsSampledMemory(ptr); }

inline bool IsColdMemory(const void* ptr) {
  bool r = (reinterpret_cast<uintptr_t>(ptr) & kTagMask) ==
           (static_cast<uintptr_t>(MemoryTag::kCold) << kTagShift);
  // IsColdMemory(ptr) implies IsSampledMemory(ptr).  This allows us to avoid
  // introducing new branches on the delete fast path when cold memory tags are
  // not in use.
  ASSERT(!r || IsSampledMemory(ptr));
  return r;
}

inline constexpr bool ColdFeatureActive() { return kHasExpandedClasses; }

inline MemoryTag GetMemoryTag(const void* ptr) {
  return static_cast<MemoryTag>((reinterpret_cast<uintptr_t>(ptr) & kTagMask) >>
                                kTagShift);
}

absl::string_view MemoryTagToLabel(MemoryTag tag);

inline constexpr bool IsExpandedSizeClass(unsigned size_class) {
  return kHasExpandedClasses && (size_class >= kExpandedClassesStart);
}

#if !defined(TCMALLOC_SMALL_BUT_SLOW) && __SIZEOF_POINTER__ != 4
// Always allocate at least a huge page
inline constexpr size_t kMinSystemAlloc = kHugePageSize;
inline constexpr size_t kMinMmapAlloc = 1 << 30;  // mmap() in 1GiB ranges.
#else
// Allocate in units of 2MiB. This is the size of a huge page for x86, but
// not for Power.
inline constexpr size_t kMinSystemAlloc = 2 << 20;
// mmap() in units of 32MiB. This is a multiple of huge page size for
// both x86 (2MiB) and Power (16MiB)
inline constexpr size_t kMinMmapAlloc = 32 << 20;
#endif

static_assert(kMinMmapAlloc % kMinSystemAlloc == 0,
              "Minimum mmap allocation size is not a multiple of"
              " minimum system allocation size");

enum class AllocationAccess {
  kHot,
  kCold,
};

inline bool IsColdHint(hot_cold_t hint) {
  return static_cast<uint8_t>(hint) < uint8_t{128};
}

inline AllocationAccess AccessFromPointer(void* ptr) {
  if (!kHasExpandedClasses) {
    ASSERT(!IsColdMemory(ptr));
    return AllocationAccess::kHot;
  }

  return ABSL_PREDICT_FALSE(IsColdMemory(ptr)) ? AllocationAccess::kCold
                                               : AllocationAccess::kHot;
}

inline MemoryTag NumaNormalTag(size_t numa_partition) {
  switch (numa_partition) {
    case 0:
      return MemoryTag::kNormalP0;
    case 1:
      return MemoryTag::kNormalP1;
    default:
      ASSUME(false);
      __builtin_unreachable();
  }
}

inline size_t NumaPartitionFromPointer(void* ptr) {
  if constexpr (kNumaPartitions == 1) {
    return 0;
  }

  switch (GetMemoryTag(ptr)) {
    case MemoryTag::kNormalP1:
      return 1;
    default:
      return 0;
  }
}

// Linker initialized, so this lock can be accessed at any time.
// Note: `CpuCache::ResizeInfo::lock` must be taken before the `pageheap_lock`
// if both are going to be held simultaneously.
extern absl::base_internal::SpinLock pageheap_lock;

// Evaluates a/b, avoiding division by zero.
inline double safe_div(double a, double b) {
  if (b == 0) {
    return 0.;
  } else {
    return a / b;
  }
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_COMMON_H_
