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

#ifndef TCMALLOC_ARENA_H_
#define TCMALLOC_ARENA_H_

#include <stddef.h>
#include <stdint.h>

#include <new>

#include "absl/base/attributes.h"
#include "absl/base/thread_annotations.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

struct ArenaStats {
  // The number of bytes allocated and in-use by calls to Alloc().
  size_t bytes_allocated;
  // The number of bytes currently reserved for future calls to Alloc().
  size_t bytes_unallocated;
  // The number of bytes lost and unavailable to calls to Alloc() due to
  // inefficiencies in Arena.
  size_t bytes_unavailable;
  // The number of allocated bytes that have subsequently become non-resident,
  // e.g. due to the slab being resized. Note that these bytes are disjoint from
  // the ones counted in `bytes_allocated`.
  size_t bytes_nonresident;

  // The number of blocks allocated by the Arena.
  size_t blocks;
};

// Arena allocation; designed for use by tcmalloc internal data structures like
// spans, profiles, etc.  Always expands.
class Arena {
 public:
  constexpr Arena() {}

  // Returns a properly aligned byte array of length "bytes".  Crashes if
  // allocation fails.  Requires pageheap_lock is held.
  ABSL_ATTRIBUTE_RETURNS_NONNULL void* Alloc(
      size_t bytes, std::align_val_t alignment = kAlignment)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Updates the stats for allocated and non-resident bytes.
  void UpdateAllocatedAndNonresident(int64_t allocated, int64_t nonresident)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    TC_ASSERT_GE(static_cast<int64_t>(bytes_allocated_) + allocated, 0);
    bytes_allocated_ += allocated;
    TC_ASSERT_GE(static_cast<int64_t>(bytes_nonresident_) + nonresident, 0);
    bytes_nonresident_ += nonresident;
  }

  // Returns statistics about memory allocated and managed by this Arena.
  ArenaStats stats() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    ArenaStats s;
    s.bytes_allocated = bytes_allocated_;
    s.bytes_unallocated = free_avail_;
    s.bytes_unavailable = bytes_unavailable_;
    s.bytes_nonresident = bytes_nonresident_;
    s.blocks = blocks_;
    return s;
  }

 private:
  // How much to allocate from system at a time
  static constexpr int kAllocIncrement = 128 << 10;

  // Free area from which to carve new objects
  char* free_area_ ABSL_GUARDED_BY(pageheap_lock) = nullptr;
  size_t free_avail_ ABSL_GUARDED_BY(pageheap_lock) = 0;

  // Total number of bytes allocated from this arena
  size_t bytes_allocated_ ABSL_GUARDED_BY(pageheap_lock) = 0;
  // The number of bytes that are unused and unavailable for future allocations
  // because they are at the end of a discarded arena block.
  size_t bytes_unavailable_ ABSL_GUARDED_BY(pageheap_lock) = 0;
  // The number of bytes on the arena that have been MADV_DONTNEEDed away. Note
  // that these bytes are disjoint from the ones counted in `bytes_allocated`.
  size_t bytes_nonresident_ ABSL_GUARDED_BY(pageheap_lock) = 0;
  // Total number of blocks/free areas managed by this Arena.
  size_t blocks_ ABSL_GUARDED_BY(pageheap_lock) = 0;

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_ARENA_H_
