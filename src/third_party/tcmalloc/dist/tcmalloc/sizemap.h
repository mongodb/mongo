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

#ifndef TCMALLOC_SIZEMAP_H_
#define TCMALLOC_SIZEMAP_H_

#include <stddef.h>
#include <stdint.h>

#include "absl/base/attributes.h"
#include "absl/base/dynamic_annotations.h"
#include "absl/base/optimization.h"
#include "absl/numeric/bits.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/size_class_info.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Definition of size class that is set in size_classes.cc
extern const SizeClasses kSizeClasses;

// Experimental size classes:
extern const SizeClasses kExperimentalPow2SizeClasses;
extern const SizeClasses kLegacySizeClasses;
extern const SizeClasses kLowFragSizeClasses;

// Size-class information + mapping
class SizeMap {
 public:
  // Min allocation size for cold. Once more applications can provide cold hints
  // with PGHO, we can consider adding more size classes for cold to increase
  // cold coverage fleet-wide.
  static constexpr size_t kMinAllocSizeForCold = 4096;
  static constexpr int kLargeSize = 1024;
  static constexpr int kLargeSizeAlignment = 128;

 private:
  // Shifts the provided value right by `n` bits.
  //
  // TODO(b/281517865): the LLVM codegen for ClassIndexMaybe() doesn't use
  // an immediate shift for the `>> 3` and `>> 7` operations due to a missed
  // optimization / miscompile in clang, resulting in this codegen:
  //
  //   mov        $0x3,%ecx
  //   mov        $0x7,%eax
  //   add        %edi,%eax
  //   shr        %cl,%rax
  //
  // Immediate shift has latency 1 vs 3 for cl shift, and also the `add` can
  // be far more efficient, which we force into inline assembly here:
  //
  //   lea        0x7(%rdi),%rax
  //   shr        $0x3,%rax
  //
  // Benchmark:
  // BM_new_sized_delete/1      6.51ns ± 5%   6.00ns ± 1%   -7.73%  (p=0.000)
  // BM_new_sized_delete/8      6.51ns ± 5%   6.01ns ± 1%   -7.66%  (p=0.000)
  // BM_new_sized_delete/64     6.52ns ± 5%   6.04ns ± 1%   -7.37%  (p=0.000)
  // BM_new_sized_delete/512    6.71ns ± 6%   6.21ns ± 1%   -7.40%  (p=0.000)
  template <int n>
  ABSL_ATTRIBUTE_ALWAYS_INLINE static inline size_t Shr(size_t value) {
    TC_ASSERT_LE(value, std::numeric_limits<uint32_t>::max());
#if defined(__x86_64__)
    asm("shrl %[n], %k[value]" : [value] "+r"(value) : [n] "n"(n));
    return value;
#elif defined(__aarch64__)
    size_t result;
    asm("lsr %[result], %[value], %[n]"
        : [result] "=r"(result)
        : [value] "r"(value), [n] "n"(n));
    return result;
#else
    return value >> n;
#endif
  }

  //-------------------------------------------------------------------
  // Mapping from size to size_class and vice versa
  //-------------------------------------------------------------------

  // Sizes <= 1024 have an alignment >= 8.  So for such sizes we have an
  // array indexed by ceil(size/8).  Sizes > 1024 have an alignment >= 128.
  // So for these larger sizes we have an array indexed by ceil(size/128).
  //
  // We flatten both logical arrays into one physical array and use
  // arithmetic to compute an appropriate index.  The constants used by
  // ClassIndex() were selected to make the flattening work.
  //
  // Examples:
  //   Size       Expression                      Index
  //   -------------------------------------------------------
  //   0          (0 + 7) / 8                     0
  //   1          (1 + 7) / 8                     1
  //   ...
  //   1024       (1024 + 7) / 8                  128
  //   1025       (1025 + 127 + (120<<7)) / 128   129
  //   ...
  //   32768      (32768 + 127 + (120<<7)) / 128  376
  static constexpr size_t kClassArraySize =
      ((kMaxSize + 127 + (120 << 7)) >> 7) + 1;

  // Batch size is the number of objects to move at once.
  typedef unsigned char BatchSize;

  // class_array_ is accessed on every malloc, so is very hot.  We make it the
  // first member so that it inherits the overall alignment of a SizeMap
  // instance.  In particular, if we create a SizeMap instance that's cache-line
  // aligned, this member is also aligned to the width of a cache line.
  CompactSizeClass
      class_array_[kClassArraySize * (kHasExpandedClasses ? 2 : 1)] = {0};

  // Number of objects to move between a per-thread list and a central
  // list in one shot.  We want this to be not too small so we can
  // amortize the lock overhead for accessing the central list.  Making
  // it too big may temporarily cause unnecessary memory wastage in the
  // per-thread free list until the scavenger cleans up the list.
  BatchSize num_objects_to_move_[kNumClasses] = {0};

  uint32_t max_capacity_[kNumClasses] = {0};

  // If size is no more than kMaxSize, compute index of the
  // class_array[] entry for it, putting the class index in output
  // parameter idx and returning true. Otherwise return false.
  ABSL_ATTRIBUTE_ALWAYS_INLINE static inline bool ClassIndexMaybe(size_t s,
                                                                  size_t& idx) {
    if (ABSL_PREDICT_TRUE(s <= kLargeSize)) {
      idx = Shr<3>(s + 7);
      return true;
    } else if (s <= kMaxSize) {
      idx = Shr<7>(s + 127 + (120 << 7));
      return true;
    }
    return false;
  }

  ABSL_ATTRIBUTE_ALWAYS_INLINE static inline size_t ClassIndex(size_t s) {
    size_t ret;
    TC_CHECK(ClassIndexMaybe(s, ret));
    return ret;
  }

  // Mapping from size class to number of pages to allocate at a time
  unsigned char class_to_pages_[kNumClasses] = {0};

  // Mapping from size class to max size storable in that class
  uint32_t class_to_size_[kNumClasses] = {0};

 protected:
  // Set the give size classes to be used by TCMalloc.
  bool SetSizeClasses(absl::Span<const SizeClassInfo> size_classes);

  // Check that the size classes meet all requirements.
  static bool ValidSizeClasses(absl::Span<const SizeClassInfo> size_classes);

  size_t cold_sizes_[kNumBaseClasses] = {0};
  size_t cold_sizes_count_ = 0;

 public:
  // Returns size classes to use in the current process.
  static const SizeClasses& CurrentClasses();

  // Checks assumptions used to generate the current size classes.
  // Prints any wrong assumptions to stderr.
  static void CheckAssumptions();

  // constexpr constructor to guarantee zero-initialization at compile-time.  We
  // rely on Init() to populate things.
  constexpr SizeMap() = default;

  // Initialize the mapping arrays.  Returns true on success.
  bool Init(absl::Span<const SizeClassInfo> size_classes);

  // Returns the size class for size `size` respecting the alignment
  // & access requirements of `policy`.
  //
  // Returns true on success. Returns false if either:
  // - the size exceeds the maximum size class size.
  // - the align size is greater or equal to the default page size
  // - no matching properly aligned size class is available
  //
  // Requires that policy.align() returns a non-zero power of 2.
  //
  // When policy.align() = 1 the default alignment of the size table will be
  // used. If policy.align() is constexpr 1 (e.g. when using
  // DefaultAlignPolicy) then alignment-related code will optimize away.
  //
  // TODO(b/171978365): Replace the output parameter with returning
  // absl::optional<uint32_t>.
  template <typename Policy>
  ABSL_ATTRIBUTE_ALWAYS_INLINE inline bool GetSizeClass(
      Policy policy, size_t size, size_t* size_class) const {
    const size_t align = policy.align();
    TC_ASSERT(absl::has_single_bit(align));

    if (ABSL_PREDICT_FALSE(align > kPageSize)) {
      ABSL_ANNOTATE_MEMORY_IS_UNINITIALIZED(size_class, sizeof(*size_class));
      return false;
    }

    size_t idx;
    if (ABSL_PREDICT_FALSE(!ClassIndexMaybe(size, idx))) {
      ABSL_ANNOTATE_MEMORY_IS_UNINITIALIZED(size_class, sizeof(*size_class));
      return false;
    }
    if (kHasExpandedClasses && IsColdHint(policy.access())) {
      *size_class = class_array_[idx + kClassArraySize];
    } else {
      *size_class = class_array_[idx] + policy.scaled_numa_partition();
    }

    // Don't search for suitably aligned class for operator new
    // (when alignment is statically known to be no greater than kAlignment).
    // But don't do this check at runtime when the alignment is dynamic.
    // We assume aligned allocation functions are not used with small alignment
    // most of the time (does not make much sense). And for alignment larger
    // than kAlignment, this is just an unnecessary check that always fails.
    if (__builtin_constant_p(align) &&
        align <= static_cast<size_t>(kAlignment)) {
      return true;
    }

    // Predict that size aligned allocs most often directly map to a proper
    // size class, i.e., multiples of 32, 64, etc, matching our class sizes.
    // Since alignment is <= kPageSize, we must find a suitable class
    // (at least kMaxSize is aligned on kPageSize).
    static_assert((kMaxSize % kPageSize) == 0, "the loop below won't work");
    // Profiles say we usually get the right class based on the size,
    // so avoid the loop overhead on the fast path.
    if (ABSL_PREDICT_FALSE(class_to_size(*size_class) & (align - 1))) {
      do {
        ++*size_class;
      } while (ABSL_PREDICT_FALSE(class_to_size(*size_class) & (align - 1)));
    }
    return true;
  }

  // Returns size class for given size, or 0 if this instance has not been
  // initialized yet. REQUIRES: size <= kMaxSize.
  template <typename Policy>
  ABSL_ATTRIBUTE_ALWAYS_INLINE inline size_t SizeClass(Policy policy,
                                                       size_t size) const {
    ASSUME(size <= kMaxSize);
    size_t ret = 0;
    GetSizeClass(policy, size, &ret);
    return ret;
  }

  // Get the byte-size for a specified class. REQUIRES: size_class <=
  // kNumClasses.
  ABSL_ATTRIBUTE_ALWAYS_INLINE inline size_t class_to_size(
      size_t size_class) const {
    TC_ASSERT_LT(size_class, kNumClasses);
    return class_to_size_[size_class];
  }

  // Mapping from size class to number of pages to allocate at a time
  ABSL_ATTRIBUTE_ALWAYS_INLINE inline size_t class_to_pages(
      size_t size_class) const {
    TC_ASSERT_LT(size_class, kNumClasses);
    return class_to_pages_[size_class];
  }

  // Number of objects to move between a per-thread list and a central
  // list in one shot.  We want this to be not too small so we can
  // amortize the lock overhead for accessing the central list.  Making
  // it too big may temporarily cause unnecessary memory wastage in the
  // per-thread free list until the scavenger cleans up the list.
  ABSL_ATTRIBUTE_ALWAYS_INLINE inline SizeMap::BatchSize num_objects_to_move(
      size_t size_class) const {
    TC_ASSERT_LT(size_class, kNumClasses);
    return num_objects_to_move_[size_class];
  }

  // Max per-CPU slab capacity for the default 256KB slab size.
  //
  // TODO(b/271598304): Revise this when 512KB slabs are available.
  ABSL_ATTRIBUTE_ALWAYS_INLINE size_t max_capacity(size_t size_class) const {
    TC_ASSERT_LT(size_class, kNumClasses);
    return max_capacity_[size_class];
  }

  ABSL_ATTRIBUTE_ALWAYS_INLINE absl::Span<const size_t> ColdSizeClasses()
      const {
    return {cold_sizes_, cold_sizes_count_};
  }

  static bool IsValidSizeClass(size_t size, size_t num_pages,
                               size_t num_objects_to_move);
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_SIZEMAP_H_
