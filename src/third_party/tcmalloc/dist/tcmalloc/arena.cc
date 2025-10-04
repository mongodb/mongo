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

#include "tcmalloc/arena.h"

#include <cstddef>
#include <cstdint>
#include <new>

#include "absl/base/optimization.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/system-alloc.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

void* Arena::Alloc(size_t bytes, std::align_val_t alignment) {
  size_t align = static_cast<size_t>(alignment);
  TC_ASSERT_GT(align, 0);
  {  // First we need to move up to the correct alignment.
    const int misalignment = reinterpret_cast<uintptr_t>(free_area_) % align;
    const int alignment_bytes = misalignment != 0 ? align - misalignment : 0;
    free_area_ += alignment_bytes;
    free_avail_ -= alignment_bytes;
    bytes_allocated_ += alignment_bytes;
  }
  char* result;
  if (free_avail_ < bytes) {
    size_t ask = bytes > kAllocIncrement ? bytes : kAllocIncrement;
    auto [ptr, actual_size] = SystemAlloc(ask, kPageSize, MemoryTag::kMetadata);
    free_area_ = reinterpret_cast<char*>(ptr);
    if (ABSL_PREDICT_FALSE(free_area_ == nullptr)) {
      TC_BUG(
          "FATAL ERROR: Out of memory trying to allocate internal tcmalloc "
          "data (bytes=%v, object-size=%v); is something preventing mmap from "
          "succeeding (sandbox, VSS limitations)?",
          kAllocIncrement, bytes);
    }
    SystemBack(free_area_, actual_size);

    // We've discarded the previous free_area_, so any bytes that were
    // unallocated are effectively inaccessible to future allocations.
    bytes_unavailable_ += free_avail_;
    blocks_++;

    free_avail_ = actual_size;
  }

  TC_ASSERT_EQ(reinterpret_cast<uintptr_t>(free_area_) % align, 0);
  result = free_area_;
  free_area_ += bytes;
  free_avail_ -= bytes;
  bytes_allocated_ += bytes;
  return reinterpret_cast<void*>(result);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
