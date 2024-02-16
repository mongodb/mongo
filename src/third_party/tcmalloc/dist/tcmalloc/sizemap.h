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

#include <algorithm>
#include <limits>
#include <type_traits>

#include "absl/base/attributes.h"
#include "absl/base/dynamic_annotations.h"
#include "absl/base/macros.h"
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
extern const absl::Span<const SizeClassInfo> kSizeClasses;

// Experimental size classes:
extern const absl::Span<const SizeClassInfo> kExperimentalPow2SizeClasses;
extern const absl::Span<const SizeClassInfo> kLegacySizeClasses;

// Size-class information + mapping
class SizeMap {
 public:
  // All size classes <= 512 in all configs always have 1 page spans.
  static constexpr size_t kMultiPageSize = 512;
  // Min alignment for all size classes > kMultiPageSize in all configs.
  static constexpr size_t kMultiPageAlignment = 64;
  // log2 (kMultiPageAlignment)
  static constexpr size_t kMultiPageAlignmentShift =
      absl::bit_width(kMultiPageAlignment - 1u);

 private:
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
  static constexpr int kMaxSmallSize = 1024;
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

  // If size is no more than kMaxSize, compute index of the
  // class_array[] entry for it, putting the class index in output
  // parameter idx and returning true. Otherwise return false.
  ABSL_ATTRIBUTE_ALWAYS_INLINE static inline bool ClassIndexMaybe(
      size_t s, uint32_t* idx) {
    if (ABSL_PREDICT_TRUE(s <= kMaxSmallSize)) {
      *idx = (static_cast<uint32_t>(s) + 7) >> 3;
      return true;
    } else if (s <= kMaxSize) {
      *idx = (static_cast<uint32_t>(s) + 127 + (120 << 7)) >> 7;
      return true;
    }
    return false;
  }

  ABSL_ATTRIBUTE_ALWAYS_INLINE static inline size_t ClassIndex(size_t s) {
    uint32_t ret;
    CHECK_CONDITION(ClassIndexMaybe(s, &ret));
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
  bool ValidSizeClasses(absl::Span<const SizeClassInfo> size_classes);

  size_t cold_sizes_[12] = {0};
  size_t cold_sizes_count_ = 0;

 public:
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
      Policy policy, size_t size, uint32_t* size_class) const {
    const size_t align = policy.align();
    ASSERT(align == 0 || absl::has_single_bit(align));

    if (ABSL_PREDICT_FALSE(align > kPageSize)) {
      ABSL_ANNOTATE_MEMORY_IS_UNINITIALIZED(size_class, sizeof(*size_class));
      return false;
    }

    uint32_t idx;
    if (ABSL_PREDICT_FALSE(!ClassIndexMaybe(size, &idx))) {
      ABSL_ANNOTATE_MEMORY_IS_UNINITIALIZED(size_class, sizeof(*size_class));
      return false;
    }
    if (kHasExpandedClasses && IsColdHint(policy.access())) {
      *size_class = class_array_[idx + kClassArraySize];
    } else {
      *size_class = class_array_[idx] + policy.scaled_numa_partition();
    }
    if (align == 0) return true;

    // Predict that size aligned allocs most often directly map to a proper
    // size class, i.e., multiples of 32, 64, etc, matching our class sizes.
    const size_t mask = align - 1;
    do {
      if (ABSL_PREDICT_TRUE((class_to_size(*size_class) & mask) == 0)) {
        return true;
      }
    } while ((++*size_class % kNumBaseClasses) != 0);

    ABSL_ANNOTATE_MEMORY_IS_UNINITIALIZED(size_class, sizeof(*size_class));
    return false;
  }

  // Returns size class for given size, or 0 if this instance has not been
  // initialized yet. REQUIRES: size <= kMaxSize.
  template <typename Policy>
  ABSL_ATTRIBUTE_ALWAYS_INLINE inline size_t SizeClass(Policy policy,
                                                       size_t size) const {
    ASSERT(size <= kMaxSize);
    uint32_t ret = 0;
    GetSizeClass(policy, size, &ret);
    return ret;
  }

  // Get the byte-size for a specified class. REQUIRES: size_class <=
  // kNumClasses.
  ABSL_ATTRIBUTE_ALWAYS_INLINE inline size_t class_to_size(
      size_t size_class) const {
    ASSERT(size_class < kNumClasses);
    return class_to_size_[size_class];
  }

  // Mapping from size class to number of pages to allocate at a time
  ABSL_ATTRIBUTE_ALWAYS_INLINE inline size_t class_to_pages(
      size_t size_class) const {
    ASSERT(size_class < kNumClasses);
    return class_to_pages_[size_class];
  }

  // Number of objects to move between a per-thread list and a central
  // list in one shot.  We want this to be not too small so we can
  // amortize the lock overhead for accessing the central list.  Making
  // it too big may temporarily cause unnecessary memory wastage in the
  // per-thread free list until the scavenger cleans up the list.
  ABSL_ATTRIBUTE_ALWAYS_INLINE inline SizeMap::BatchSize num_objects_to_move(
      size_t size_class) const {
    ASSERT(size_class < kNumClasses);
    return num_objects_to_move_[size_class];
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
