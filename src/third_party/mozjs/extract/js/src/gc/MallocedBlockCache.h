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

  // Allocation and freeing.
  [[nodiscard]] PointerAndUint7 alloc(size_t size);
  void free(PointerAndUint7 blockAndListID);

  // Allows users to gradually hand blocks back to js_free, so as to avoid
  // space leaks in long-running scenarios.  The specified percentage of
  // blocks in each list is discarded.
  void preen(float percentOfBlocksToDiscard);

  // Return all blocks in the cache to js_free.
  void clear();

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

}  // namespace gc
}  // namespace js

#endif  // gc_MallocedBlockCache_h
