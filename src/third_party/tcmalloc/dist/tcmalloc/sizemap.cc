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

#include "tcmalloc/sizemap.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>

#include "absl/base/macros.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_page_aware_allocator.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/size_class_info.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

const SizeClasses& SizeMap::CurrentClasses() {
  switch (Static::size_class_configuration()) {
    case SizeClassConfiguration::kPow2Below64:
      return kSizeClasses;
    case SizeClassConfiguration::kPow2Only:
      return kExperimentalPow2SizeClasses;
    case SizeClassConfiguration::kLowFrag:
      return kLowFragSizeClasses;
    case SizeClassConfiguration::kLegacy:
      // TODO(b/242710633): remove this opt out.
      return kLegacySizeClasses;
  }
  TC_BUG("unreachable");
}

void SizeMap::CheckAssumptions() {
  bool failed = false;
  auto a = CurrentClasses().assumptions;
  if (a.has_expanded_classes != kHasExpandedClasses) {
    fprintf(stderr, "kHasExpandedClasses: assumed %d, actual %d\n",
            a.has_expanded_classes, kHasExpandedClasses);
    failed = true;
  }
  if (a.span_size != sizeof(Span)) {
    fprintf(stderr, "sizeof(Span): assumed %zu, actual %zu\n", a.span_size,
            sizeof(Span));
    failed = true;
  }
  if (a.sampling_rate != kDefaultProfileSamplingRate) {
    fprintf(stderr, "kDefaultProfileSamplingRate: assumed %zu, actual %zu\n",
            a.sampling_rate, kDefaultProfileSamplingRate);
    failed = true;
  }
  if (a.large_size != SizeMap::kLargeSize) {
    fprintf(stderr, "SizeMap::kLargeSize: assumed %zu, actual %u\n",
            a.large_size, SizeMap::kLargeSize);
    failed = true;
  }
  if (a.large_size_alignment != SizeMap::kLargeSizeAlignment) {
    fprintf(stderr, "SizeMap::kLargeSizeAlignment: assumed %zu, actual %u\n",
            a.large_size_alignment, SizeMap::kLargeSizeAlignment);
    failed = true;
  }
  if (failed) {
    fprintf(stderr, "*************************************\n");
    fprintf(stderr, "* MISMATCHED SIZE CLASS ASSUMPTIONS *\n");
    fprintf(stderr, "*************************************\n");
  }
}

extern "C" void TCMallocInternalCheckSizeClassAssumptions() {
  SizeMap::CheckAssumptions();
}

bool SizeMap::IsValidSizeClass(size_t size, size_t pages,
                               size_t num_objects_to_move) {
  if (size == 0) {
    TC_LOG("size class is 0");
    return false;
  }
  if (size > kMaxSize) {
    TC_LOG("size %v class too big %v", size, kMaxSize);
    return false;
  }
  // Verify Span does not use intrusive list which triggers memory accesses
  // for sizes suitable for cold classes.
  if (size >= kMinAllocSizeForCold && !Span::IsNonIntrusive(size)) {
    TC_LOG("size %v is suitable for cold classes but is intrusive", size);
    return false;
  }
  // Check required alignment
  const size_t alignment = size > SizeMap::kLargeSize
                               ? kLargeSizeAlignment
                               : static_cast<size_t>(kAlignment);
  if ((size & (alignment - 1)) != 0) {
    TC_LOG("%v not aligned properly %v", size, alignment);
    return false;
  }
  if (pages == 0) {
    TC_LOG("pages should not be 0");
    return false;
  }
  if (pages >= 255) {
    TC_LOG("pages %v limited to 254", pages);
    return false;
  }
  const size_t objects_per_span = Length(pages).in_bytes() / size;
  if (objects_per_span < 1) {
    TC_LOG("each span must have at least one object");
    return false;
  }
  if (!Span::IsValidSizeClass(size, pages)) {
    TC_LOG("%v span size class assumptions are broken: pages=%v objs=%v", size,
           pages, objects_per_span);
    return false;
  }
  if (!HugePageAwareAllocator::IsValidSizeClass(size, pages)) {
    TC_LOG("%v hpaa size class assumptions are broken: pages=%v objs=%v", size,
           pages, objects_per_span);
    return false;
  }
  if (num_objects_to_move < 2) {
    TC_LOG("num objects to move %v too small (<2)", num_objects_to_move);
    return false;
  }
  if (num_objects_to_move > kMaxObjectsToMove) {
    TC_LOG("num objects to move %v too large %v", num_objects_to_move,
           kMaxObjectsToMove);
    return false;
  }

  return true;
}

bool SizeMap::SetSizeClasses(absl::Span<const SizeClassInfo> size_classes) {
  const int num_classes = size_classes.size();
  if (!ValidSizeClasses(size_classes)) {
    return false;
  }

  class_to_size_[0] = 0;
  class_to_pages_[0] = 0;
  num_objects_to_move_[0] = 0;

  int curr = 1;
  for (int c = 1; c < num_classes; c++) {
    class_to_size_[curr] = size_classes[c].size;
    class_to_pages_[curr] = size_classes[c].pages;
    num_objects_to_move_[curr] = size_classes[c].num_to_move;
    max_capacity_[curr] = size_classes[c].max_capacity;
    ++curr;
  }

  // Fill any unspecified size classes with 0.
  for (int x = curr; x < kNumBaseClasses; x++) {
    class_to_size_[x] = 0;
    class_to_pages_[x] = 0;
    num_objects_to_move_[x] = 0;
  }

  // Copy selected size classes into the upper registers.
  for (int i = 1; i < (kNumClasses / kNumBaseClasses); i++) {
    std::copy(&class_to_size_[0], &class_to_size_[kNumBaseClasses],
              &class_to_size_[kNumBaseClasses * i]);
    std::copy(&class_to_pages_[0], &class_to_pages_[kNumBaseClasses],
              &class_to_pages_[kNumBaseClasses * i]);
    std::copy(&num_objects_to_move_[0], &num_objects_to_move_[kNumBaseClasses],
              &num_objects_to_move_[kNumBaseClasses * i]);
  }

  return true;
}

// Return true if all size classes meet the requirements for alignment
// ordering and min and max values.
bool SizeMap::ValidSizeClasses(absl::Span<const SizeClassInfo> size_classes) {
  if (size_classes.empty()) {
    return false;
  }
  int num_classes = size_classes.size();
  if (kHasExpandedClasses && num_classes > kNumBaseClasses) {
    num_classes = kNumBaseClasses;
  }

  if (size_classes[0].size != 0 || size_classes[0].pages != 0 ||
      size_classes[0].num_to_move != 0) {
    return false;
  }

  for (int c = 1; c < num_classes; c++) {
    size_t class_size = size_classes[c].size;
    size_t pages = size_classes[c].pages;
    size_t num_objects_to_move = size_classes[c].num_to_move;
    // Each size class must be larger than the previous size class.
    if (class_size <= size_classes[c - 1].size) {
      TC_LOG("Non-increasing size class %v: prev=%v next=%v", c,
             size_classes[c - 1].size, class_size);
      return false;
    }
    if (!IsValidSizeClass(class_size, pages, num_objects_to_move)) {
      return false;
    }
  }
  // Last size class must be kMaxSize.  This is not strictly
  // class_to_size_[kNumBaseClasses - 1] because several size class
  // configurations populate fewer distinct size classes and fill the tail of
  // the array with zeroes.
  if (size_classes[num_classes - 1].size != kMaxSize) {
    TC_LOG("last class %v size %v doesn't cover kMaxSize %v", num_classes - 1,
           size_classes[num_classes - 1].size, kMaxSize);
    return false;
  }
  return true;
}

// Initialize the mapping arrays
bool SizeMap::Init(absl::Span<const SizeClassInfo> size_classes) {
  // Do some sanity checking on add_amount[]/shift_amount[]/class_array[]
  TC_CHECK_EQ(ClassIndex(0), 0);
  TC_CHECK_LT(ClassIndex(kMaxSize), sizeof(class_array_));
  static_assert(kAlignment <= std::align_val_t{16}, "kAlignment is too large");

  if (!SetSizeClasses(size_classes)) {
    return false;
  }

  int next_size = 0;
  for (int c = 1; c < kNumClasses; c++) {
    const int max_size_in_class = class_to_size_[c];

    for (int s = next_size; s <= max_size_in_class;
         s += static_cast<size_t>(kAlignment)) {
      class_array_[ClassIndex(s)] = c;
    }
    next_size = max_size_in_class + static_cast<size_t>(kAlignment);
    if (next_size > kMaxSize) {
      break;
    }
  }

  if (!ColdFeatureActive()) {
    return true;
  }

  memset(cold_sizes_, 0, sizeof(cold_sizes_));
  cold_sizes_count_ = 0;
  // Point all lookups in the upper register of class_array_ (allocations
  // seeking cold memory) to the lower size classes.  This gives us an easy
  // fallback for sizes that are too small for moving to cold memory (due to
  // intrusive span metadata).
  std::copy(&class_array_[0], &class_array_[kClassArraySize],
            &class_array_[kClassArraySize]);

  for (int c = kExpandedClassesStart; c < kNumClasses; c++) {
    size_t max_size_in_class = class_to_size_[c];
    if (max_size_in_class == 0 || max_size_in_class < kMinAllocSizeForCold) {
      // Resetting next_size to the last size class before
      // kMinAllocSizeForCold + kAlignment.
      next_size = max_size_in_class + static_cast<size_t>(kAlignment);
      continue;
    }

    TC_CHECK(Span::IsNonIntrusive(max_size_in_class), "size=%v",
             max_size_in_class);
    cold_sizes_[cold_sizes_count_] = c;
    ++cold_sizes_count_;

    for (int s = next_size; s <= max_size_in_class;
         s += static_cast<size_t>(kAlignment)) {
      class_array_[ClassIndex(s) + kClassArraySize] = c;
    }
    next_size = max_size_in_class + static_cast<size_t>(kAlignment);
    if (next_size > kMaxSize) {
      break;
    }
  }
  return true;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
