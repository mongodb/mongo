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

#ifndef TCMALLOC_PAGE_ALLOCATOR_TEST_UTIL_H_
#define TCMALLOC_PAGE_ALLOCATOR_TEST_UTIL_H_

#include <cstddef>
#include <tuple>
#include <utility>

#include "absl/types/span.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"

// TODO(b/116000878): Remove dependency on common.h if it causes ODR issues.
#include "tcmalloc/common.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// AddressRegion that adds some padding on either side of each
// allocation.  This prevents multiple PageAllocators in the system
// from noticing one another's presence in the pagemap.
class ExtraRegion : public AddressRegion {
 public:
  explicit ExtraRegion(AddressRegion* under) : under_(under) {}

  std::pair<void*, size_t> Alloc(size_t size, size_t alignment) override {
    size_t big = size + alignment + alignment;
    // Can't pad if allocation is within 2 * alignment of region size.
    if (big > kMinMmapAlloc) {
      return under_->Alloc(size, alignment);
    }
    void* ptr;
    size_t actual_size;
    std::tie(ptr, actual_size) = under_->Alloc(big, alignment);
    if (!ptr) return {nullptr, 0};
    actual_size = actual_size - alignment * 2;
    return {static_cast<char*>(ptr) + alignment, actual_size};
  }

 private:
  AddressRegion* under_;
};

class ExtraRegionFactory : public AddressRegionFactory {
 public:
  explicit ExtraRegionFactory(AddressRegionFactory* under) : under_(under) {}

  AddressRegion* Create(void* start, size_t size, UsageHint hint) override {
    AddressRegion* underlying_region = under_->Create(start, size, hint);
    TC_CHECK(underlying_region);
    void* region_space = MallocInternal(sizeof(ExtraRegion));
    TC_CHECK(region_space);
    return new (region_space) ExtraRegion(underlying_region);
  }

  size_t GetStats(absl::Span<char> buffer) override {
    return under_->GetStats(buffer);
  }

 private:
  AddressRegionFactory* under_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_PAGE_ALLOCATOR_TEST_UTIL_H_
