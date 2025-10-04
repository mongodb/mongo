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
// Specification of Size classes
#ifndef TCMALLOC_SIZE_CLASS_INFO_H_
#define TCMALLOC_SIZE_CLASS_INFO_H_

#include <stddef.h>
#include <stdint.h>

#include "absl/types/span.h"
#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Precomputed size class parameters.
struct SizeClassInfo {
  // Max size storable in that class
  uint32_t size;

  // Number of pages to allocate at a time
  uint8_t pages;

  // Number of objects to move between a per-thread list and a central list in
  // one shot.  We want this to be not too small so we can amortize the lock
  // overhead for accessing the central list.  Making it too big may temporarily
  // cause unnecessary memory wastage in the per-thread free list until the
  // scavenger cleans up the list.
  uint8_t num_to_move;

  // Max per-CPU slab capacity for the default 256KB slab size.
  // Scaled up/down for larger/smaller slab sizes.
  uint32_t max_capacity;
};

struct SizeClassAssumptions {
  bool has_expanded_classes;    // kHasExpandedClasses
  size_t span_size;             // sizeof(Span)
  size_t sampling_rate;         // kDefaultProfileSamplingRate
  size_t large_size;            // SizeMap::kLargeSize
  size_t large_size_alignment;  // SizeMap::kLargeSizeAlignment
};

struct SizeClasses {
  absl::Span<const SizeClassInfo> classes;
  SizeClassAssumptions assumptions;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_SIZE_CLASS_INFO_H_
