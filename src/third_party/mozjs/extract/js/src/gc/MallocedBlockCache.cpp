/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sw=2 et tw=80:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/MallocedBlockCache.h"
#include "mozilla/MemoryChecking.h"

using js::PointerAndUint7;
using js::gc::MallocedBlockCache;

MallocedBlockCache::~MallocedBlockCache() { clear(); }

// This is the fallback path for MallocedBlockCache::alloc.  Do not call this
// directly, since it doesn't handle all cases by itself.  See ::alloc for
// further comments.
PointerAndUint7 MallocedBlockCache::allocSlow(size_t size) {
  // We're never expected to handle zero-sized blocks.
  MOZ_ASSERT(size > 0);

  size = js::RoundUp(size, STEP);
  size_t i = size / STEP;
  MOZ_ASSERT(i > 0);

  // Too large to cache; go straight to js_malloc.
  if (MOZ_UNLIKELY(i >= NUM_LISTS)) {
    void* p = js_malloc(size);
    // If p is nullptr, that fact is carried into the PointerAndUint7, and the
    // caller is expected to check that.
    return PointerAndUint7(p, OVERSIZE_BLOCK_LIST_ID);
  }

  // The block is of cacheable size, but we expect the relevant list to be
  // empty, because ::alloc will have handled the case where it wasn't.
  MOZ_ASSERT(i >= 1 && i < NUM_LISTS);
  // Check that i is the right list
  MOZ_ASSERT(i * STEP == size);
  MOZ_RELEASE_ASSERT(lists[i].empty());

  // And so we have to hand the request off to js_malloc.
  void* p = js_malloc(size);
  if (MOZ_UNLIKELY(!p)) {
    return PointerAndUint7(nullptr, 0);  // OOM
  }
  return PointerAndUint7(p, i);
}

void MallocedBlockCache::preen(double percentOfBlocksToDiscard) {
  MOZ_ASSERT(percentOfBlocksToDiscard >= 0.0 &&
             percentOfBlocksToDiscard <= 100.0);
  MOZ_ASSERT(lists[OVERSIZE_BLOCK_LIST_ID].empty());
  for (size_t listID = 1; listID < NUM_LISTS; listID++) {
    MallocedBlockVector& list = lists[listID];
    size_t numToFree =
        size_t(float(list.length()) * (percentOfBlocksToDiscard / 100.0));
    MOZ_RELEASE_ASSERT(numToFree <= list.length());
    while (numToFree > 0) {
      void* block = list.popCopy();
      MOZ_ASSERT(block);
      js_free(block);
      numToFree--;
    }
  }
}

void MallocedBlockCache::clear() {
  MOZ_ASSERT(lists[OVERSIZE_BLOCK_LIST_ID].empty());
  for (size_t i = 1; i < NUM_LISTS; i++) {
    MallocedBlockVector& list = lists[i];
    for (void*& block : list) {
      MOZ_ASSERT(block);
      js_free(block);
      block = nullptr;  // for safety
    }
    list.clear();
  }
}

size_t MallocedBlockCache::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  MOZ_ASSERT(lists[OVERSIZE_BLOCK_LIST_ID].empty());
  size_t nBytes = 0;
  for (const MallocedBlockVector& list : lists) {
    nBytes += list.sizeOfExcludingThis(mallocSizeOf);
    // The payload size of each block in `list` is the same.  Hence, we could
    // possibly do better here (measure once and multiply by the length) if we
    // believe that the metadata size for each block is also the same.
    for (void* block : list) {
      MOZ_ASSERT(block);
      nBytes += mallocSizeOf(block);
    }
  }
  return nBytes;
}
