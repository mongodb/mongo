/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sw=2 et tw=80:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_MallocedBlockCache_h
#define gc_MallocedBlockCache_h

#include "ds/PointerAndUint7.h"
#include "js/AllocPolicy.h"
#include "js/Vector.h"
#include "util/Poison.h"

namespace js {
namespace gc {

// MallocedBlockCache implements a lightweight wrapper around js_malloc/js_free.
//
// Blocks are requested by ::alloc and must be returned with ::free, at which
// point the cache may decide to hold on to the block rather than hand it back
// to js_free.  Subsequent ::alloc calls may be satisfied from the cached
// blocks rather than calling js_malloc.  The mechanism is designed to be much
// cheaper than calling js_malloc/js_free directly.  One consequence is that
// there is no locking; it is essential therefore to use each cache only from
// a single thread.
//
// The intended use is for lightweight management of OOL (malloc'd) storage
// associated with WasmStructObject and WasmArrayObject.  The mechanism is
// general and potentially has other uses.  Blocks of size STEP * NUM_LISTS
// and larger are never cached, though.
//
// Request sizes are rounded up to a multiple of STEP.  There are NUM_LISTS-1
// free lists, with a "List ID" indicating the multiple of STEP stored on the
// list.  So for example, blocks of size 3 * STEP (after rounding up) are
// stored on the list with ID 3.  List ID 0 indicates blocks which are too
// large to live on any freelist.  With the default settings, this gives
// separate freelists for blocks of size 16, 32, 48, .. 496.  Blocks of size
// zero are not supported, and `lists[0]` will always be empty.
//
// Allocation of a block produces not only the block's address but also its
// List ID.  When freeing, both values must be presented, because there is
// otherwise no way for ::free to know the size of the original allocation,
// and hence which freelist it should go on.  For this reason, the ::alloc and
// ::free methods produce and take a `PointerAndUint7`, not a `void*`.
//
// Resizing of blocks is not supported.

class MallocedBlockCache {
 public:
  static const size_t STEP = 16;

  static const size_t NUM_LISTS = 32;
  // This limitation exists because allocation returns a PointerAndUint7, and
  // a List-ID value (viz, 0 .. NUM_LISTS-1) is stored in the Uint7 part.
  static_assert(NUM_LISTS <= (1 << 7));

  // list[0] must always remain empty.  List ID 0 indicates a block which
  // cannot participate in the freelist machinery because it is too large.
  //
  // list[i], for 1 <= i < NUM_LISTS, holds blocks of size i * STEP only.
  // All requests are rounded up to multiple of SIZE.
  //
  // We do not expect to be required to issue or accept blocks of size zero.
  static const size_t OVERSIZE_BLOCK_LIST_ID = 0;
  using MallocedBlockVector = Vector<void*, 0, SystemAllocPolicy>;

  MallocedBlockVector lists[NUM_LISTS];

  ~MallocedBlockCache();

  static inline size_t listIDForSize(size_t size);

  // Allocation and freeing.  Use `alloc` to allocate.  `allocSlow` is
  // `alloc`s fallback path.  Do not call it directly, since it doesn't handle
  // all cases by itself.
  [[nodiscard]] inline PointerAndUint7 alloc(size_t size);
  [[nodiscard]] MOZ_NEVER_INLINE PointerAndUint7 allocSlow(size_t size);

  inline void free(PointerAndUint7 blockAndListID);

  // Allows users to gradually hand blocks back to js_free, so as to avoid
  // space leaks in long-running scenarios.  The specified percentage of
  // blocks in each list is discarded.
  void preen(double percentOfBlocksToDiscard);

  // Return all blocks in the cache to js_free.
  void clear();

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

/* static */
inline size_t MallocedBlockCache::listIDForSize(size_t size) {
  // Figure out which free list can give us a block of size `size`, after it
  // has been rounded up to a multiple of `step`.
  //
  // Example mapping for STEP = 16 and NUM_LISTS = 8, after rounding up:
  //   0 never holds any blocks (denotes "too large")
  //   1 holds blocks of size  16
  //   2 holds blocks of size  32
  //   3 holds blocks of size  48
  //   4 holds blocks of size  64
  //   5 holds blocks of size  80
  //   6 holds blocks of size  96
  //   7 holds blocks of size 112
  //
  // For a request of size n:
  // * if n == 0, fail
  // * else
  //      round n up to a multiple of STEP
  //      let i = n / STEP
  //      if i >= NUM_LISTS
  //         alloc direct from js_malloc, and return listID = 0
  //      if lists[i] is nonempty, use lists[i] and return listID = i.
  //      else
  //         let p = js_malloc(n)
  //         return p and listID = i.

  // We're never expected to handle zero-sized blocks.
  MOZ_ASSERT(size > 0);

  size = js::RoundUp(size, STEP);
  size_t i = size / STEP;
  MOZ_ASSERT(i > 0);

  if (i >= NUM_LISTS) {
    return OVERSIZE_BLOCK_LIST_ID;
  }

  return i;
}

inline PointerAndUint7 MallocedBlockCache::alloc(size_t size) {
  size_t i = listIDForSize(size);
  MOZ_ASSERT(i < NUM_LISTS);

  // Fast path: try to pull a block from the relevant list.
  if (MOZ_LIKELY(
          i != OVERSIZE_BLOCK_LIST_ID &&  // "block is small enough to cache"
          !lists[i].empty())) {           // "a cached block is available"
    // Check that i is the right list
    MOZ_ASSERT(i * STEP == js::RoundUp(size, STEP));
    void* block = lists[i].popCopy();
    return PointerAndUint7(block, i);
  }

  // Fallback path for all other cases.
  return allocSlow(size);
}

inline void MallocedBlockCache::free(PointerAndUint7 blockAndListID) {
  // This is a whole lot simpler than the ::alloc case, since we are given the
  // listId and don't have to compute it (not that we have any way to).
  void* block = blockAndListID.pointer();
  uint32_t listID = blockAndListID.uint7();
  MOZ_ASSERT(block);
  MOZ_ASSERT(listID < NUM_LISTS);
  if (MOZ_UNLIKELY(listID == OVERSIZE_BLOCK_LIST_ID)) {
    // It was too large for recycling; go straight to js_free.
    js_free(block);
    return;
  }

  // Put it back on list `listId`, first poisoning it for safety.
  memset(block, JS_NOTINUSE_TRAILER_PATTERN, listID * STEP);
  MOZ_MAKE_MEM_UNDEFINED(block, listID * STEP);
  if (MOZ_UNLIKELY(!lists[listID].append(block))) {
    // OOM'd while doing admin.  Hand it off to js_free and forget about the
    // OOM.
    js_free(block);
  }
}

}  // namespace gc
}  // namespace js

#endif  // gc_MallocedBlockCache_h
