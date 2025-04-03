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

PointerAndUint7 MallocedBlockCache::alloc(size_t size) {
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

  // Too large to cache; go straight to js_malloc.
  if (MOZ_UNLIKELY(i >= NUM_LISTS)) {
    void* p = js_malloc(size);
    // If p is nullptr, that fact is carried into the PointerAndUint7, and the
    // caller is expected to check that.
    return PointerAndUint7(p, OVERSIZE_BLOCK_LIST_ID);
  }

  // The case we hope is common.  First, see if we can pull a block from the
  // relevant list.
  MOZ_ASSERT(i >= 1 && i < NUM_LISTS);
  // Check that i is the right list
  MOZ_ASSERT(i * STEP == size);
  if (MOZ_LIKELY(!lists[i].empty())) {
    void* block = lists[i].popCopy();
    return PointerAndUint7(block, i);
  }

  // No luck.
  void* p = js_malloc(size);
  if (MOZ_UNLIKELY(!p)) {
    return PointerAndUint7(nullptr, 0);  // OOM
  }
  return PointerAndUint7(p, i);
}

void MallocedBlockCache::free(PointerAndUint7 blockAndListID) {
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

void MallocedBlockCache::preen(float percentOfBlocksToDiscard) {
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
    for (size_t j = 0; j < list.length(); j++) {
      MOZ_ASSERT(list[j]);
      js_free(list[j]);
      list[j] = nullptr;  // for safety
    }
    list.clear();
  }
}

size_t MallocedBlockCache::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  MOZ_ASSERT(lists[OVERSIZE_BLOCK_LIST_ID].empty());
  size_t nBytes = 0;
  for (size_t listID = 0; listID < NUM_LISTS; listID++) {
    const MallocedBlockVector& list = lists[listID];
    nBytes += list.sizeOfExcludingThis(mallocSizeOf);
    // The payload size of each block in `list` is the same.  Hence, we could
    // possibly do better here (measure once and multiply by the length) if we
    // believe that the metadata size for each block is also the same.
    for (size_t i = 0; i < list.length(); i++) {
      MOZ_ASSERT(list[i]);
      nBytes += mallocSizeOf(list[i]);
    }
  }
  return nBytes;
}
