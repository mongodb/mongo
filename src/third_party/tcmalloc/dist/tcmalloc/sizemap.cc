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
#include <new>

#include "tcmalloc/experiment.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/sampler.h"
#include "tcmalloc/span.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

bool SizeMap::IsValidSizeClass(size_t size, size_t pages,
                               size_t num_objects_to_move) {
  if (size == 0) {
    Log(kLog, __FILE__, __LINE__, "size class is 0", size);
    return false;
  }
  if (size > kMaxSize) {
    Log(kLog, __FILE__, __LINE__, "size class too big", size, kMaxSize);
    return false;
  }
  // Check required alignment
  size_t alignment = 128;
  if (size <= kMultiPageSize) {
    alignment = static_cast<size_t>(kAlignment);
  } else if (size <= SizeMap::kMaxSmallSize) {
    alignment = kMultiPageAlignment;
  }
  if ((size & (alignment - 1)) != 0) {
    Log(kLog, __FILE__, __LINE__, "Not aligned properly", size, alignment);
    return false;
  }
  if (size <= kMultiPageSize && pages != 1) {
    Log(kLog, __FILE__, __LINE__, "Multiple pages not allowed", size, pages,
        kMultiPageSize);
    return false;
  }
  if (pages == 0) {
    Log(kLog, __FILE__, __LINE__, "pages should not be 0", pages);
    return false;
  }
  if (pages >= 256) {
    Log(kLog, __FILE__, __LINE__, "pages limited to 255", pages);
    return false;
  }
  const size_t objects_per_span = Length(pages).in_bytes() / size;
  if (objects_per_span < 1) {
    Log(kLog, __FILE__, __LINE__, "each span must have at least one object");
    return false;
  } else if (size >= kBitmapMinObjectSize && objects_per_span > 64) {
    Log(kLog, __FILE__, __LINE__, "too many objects for bitmap representation",
        size, objects_per_span);
    return false;
  }
  if (num_objects_to_move < 2) {
    Log(kLog, __FILE__, __LINE__, "num objects to move too small (<2)",
        num_objects_to_move);
    return false;
  }
  if (num_objects_to_move > kMaxObjectsToMove) {
    Log(kLog, __FILE__, __LINE__, "num objects to move too large",
        num_objects_to_move, kMaxObjectsToMove);
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
      Log(kLog, __FILE__, __LINE__, "Non-increasing size class", c,
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
    Log(kLog, __FILE__, __LINE__, "last class doesn't cover kMaxSize",
        num_classes - 1, size_classes[num_classes - 1].size, kMaxSize);
    return false;
  }
  return true;
}

// Initialize the mapping arrays
bool SizeMap::Init(absl::Span<const SizeClassInfo> size_classes) {
  // Do some sanity checking on add_amount[]/shift_amount[]/class_array[]
  if (ClassIndex(0) != 0) {
    Crash(kCrash, __FILE__, __LINE__, "Invalid class index for size 0",
          ClassIndex(0));
  }
  if (ClassIndex(kMaxSize) >= sizeof(class_array_)) {
    Crash(kCrash, __FILE__, __LINE__, "Invalid class index for kMaxSize",
          ClassIndex(kMaxSize));
  }

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

  if (!kHasExpandedClasses) {
    return true;
  }

  memset(cold_sizes_, 0, sizeof(cold_sizes_));
  cold_sizes_count_ = 0;

  if (!ColdFeatureActive()) {
    std::copy(&class_array_[0], &class_array_[kClassArraySize],
              &class_array_[kClassArraySize]);
    return true;
  }

  // TODO(b/123523202): Systematically identify candidates for cold allocation
  // and include them explicitly in size_classes.cc.
  static constexpr size_t kColdCandidates[] = {
      2048,  4096,  6144,  7168,  8192,   16384,
      20480, 32768, 40960, 65536, 131072, 262144,
  };
  static_assert(ABSL_ARRAYSIZE(kColdCandidates) <= ABSL_ARRAYSIZE(cold_sizes_),
                "kColdCandidates is too large.");

  // Point all lookups in the upper register of class_array_ (allocations
  // seeking cold memory) to the lower size classes.  This gives us an easy
  // fallback for sizes that are too small for moving to cold memory (due to
  // intrusive span metadata).
  std::copy(&class_array_[0], &class_array_[kClassArraySize],
            &class_array_[kClassArraySize]);

  for (size_t max_size_in_class : kColdCandidates) {
    ASSERT(max_size_in_class != 0);

    // Find the size class.  Some of our kColdCandidates may not map to actual
    // size classes in our current configuration.
    bool found = false;
    int c;
    for (c = kExpandedClassesStart; c < kNumClasses; c++) {
      if (class_to_size_[c] == max_size_in_class) {
        found = true;
        break;
      }
    }

    if (!found) {
      continue;
    }

    // Verify the candidate can fit into a single span's kCacheSize, otherwise,
    // we use an intrusive freelist which triggers memory accesses.
    if (Length(class_to_pages_[c]).in_bytes() / max_size_in_class >
        Span::kCacheSize) {
      continue;
    }

    cold_sizes_[cold_sizes_count_] = c;
    cold_sizes_count_++;

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
