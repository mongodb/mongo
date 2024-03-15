/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ds/LifoAlloc.h"

#include "mozilla/Likely.h"
#include "mozilla/MathAlgorithms.h"

#include <algorithm>

#ifdef LIFO_CHUNK_PROTECT
#  include "gc/Memory.h"
#endif

using namespace js;

using mozilla::RoundUpPow2;
using mozilla::tl::BitSize;

namespace js {
namespace detail {

/* static */
UniquePtr<BumpChunk> BumpChunk::newWithCapacity(size_t size) {
  MOZ_DIAGNOSTIC_ASSERT(size >= sizeof(BumpChunk));
  void* mem = js_malloc(size);
  if (!mem) {
    return nullptr;
  }

  UniquePtr<BumpChunk> result(new (mem) BumpChunk(size));

  // We assume that the alignment of LIFO_ALLOC_ALIGN is less than that of the
  // underlying memory allocator -- creating a new BumpChunk should always
  // satisfy the LIFO_ALLOC_ALIGN alignment constraint.
  MOZ_ASSERT(AlignPtr(result->begin()) == result->begin());
  return result;
}

#ifdef LIFO_CHUNK_PROTECT

static uint8_t* AlignPtrUp(uint8_t* ptr, uintptr_t align) {
  MOZ_ASSERT(mozilla::IsPowerOfTwo(align));
  uintptr_t uptr = uintptr_t(ptr);
  uintptr_t diff = uptr & (align - 1);
  diff = (align - diff) & (align - 1);
  uptr = uptr + diff;
  return (uint8_t*)uptr;
}

static uint8_t* AlignPtrDown(uint8_t* ptr, uintptr_t align) {
  MOZ_ASSERT(mozilla::IsPowerOfTwo(align));
  uintptr_t uptr = uintptr_t(ptr);
  uptr = uptr & ~(align - 1);
  return (uint8_t*)uptr;
}

void BumpChunk::setReadOnly() {
  uintptr_t pageSize = gc::SystemPageSize();
  // The allocated chunks might not be aligned on page boundaries. This code
  // is used to ensure that we are changing the memory protection of pointers
  // which are within the range of the BumpChunk, or that the range formed by
  // [b .. e] is empty.
  uint8_t* b = base();
  uint8_t* e = capacity_;
  b = AlignPtrUp(b, pageSize);
  e = AlignPtrDown(e, pageSize);
  if (e <= b) {
    return;
  }
  gc::MakePagesReadOnly(b, e - b);
}

void BumpChunk::setReadWrite() {
  uintptr_t pageSize = gc::SystemPageSize();
  // The allocated chunks might not be aligned on page boundaries. This code
  // is used to ensure that we are changing the memory protection of pointers
  // which are within the range of the BumpChunk, or that the range formed by
  // [b .. e] is empty.
  uint8_t* b = base();
  uint8_t* e = capacity_;
  b = AlignPtrUp(b, pageSize);
  e = AlignPtrDown(e, pageSize);
  if (e <= b) {
    return;
  }
  gc::UnprotectPages(b, e - b);
}

#endif

}  // namespace detail
}  // namespace js

void LifoAlloc::reset(size_t defaultChunkSize) {
  MOZ_ASSERT(mozilla::IsPowerOfTwo(defaultChunkSize));

  while (!chunks_.empty()) {
    chunks_.popFirst();
  }
  while (!oversize_.empty()) {
    oversize_.popFirst();
  }
  while (!unused_.empty()) {
    unused_.popFirst();
  }
  defaultChunkSize_ = defaultChunkSize;
  oversizeThreshold_ = defaultChunkSize;
  markCount = 0;
  curSize_ = 0;
  smallAllocsSize_ = 0;
}

void LifoAlloc::freeAll() {
  // When free-ing all chunks, we can no longer determine which chunks were
  // transferred and which were not, so simply clear the heuristic to zero
  // right away.
  smallAllocsSize_ = 0;

  while (!chunks_.empty()) {
    UniqueBumpChunk bc = chunks_.popFirst();
    decrementCurSize(bc->computedSizeOfIncludingThis());
  }
  while (!oversize_.empty()) {
    UniqueBumpChunk bc = oversize_.popFirst();
    decrementCurSize(bc->computedSizeOfIncludingThis());
  }
  while (!unused_.empty()) {
    UniqueBumpChunk bc = unused_.popFirst();
    decrementCurSize(bc->computedSizeOfIncludingThis());
  }

  // Nb: maintaining curSize_ correctly isn't easy.  Fortunately, this is an
  // excellent sanity check.
  MOZ_ASSERT(curSize_ == 0);
}

// Round at the same page granularity used by malloc.
static size_t MallocGoodSize(size_t aSize) {
#if defined(MOZ_MEMORY)
  return malloc_good_size(aSize);
#else
  return aSize;
#endif
}

// Heuristic to choose the size of the next BumpChunk for small allocations.
// `start` is the size of the first chunk. `used` is the total size of all
// BumpChunks in this LifoAlloc so far.
static size_t NextSize(size_t start, size_t used) {
  // Double the size, up to 1 MB.
  const size_t mb = 1 * 1024 * 1024;
  if (used < mb) {
    return std::max(start, used);
  }

  // After 1 MB, grow more gradually, to waste less memory.
  // The sequence (in megabytes) begins:
  // 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 5, ...
  return RoundUp(used / 8, mb);
}

LifoAlloc::UniqueBumpChunk LifoAlloc::newChunkWithCapacity(size_t n,
                                                           bool oversize) {
  MOZ_ASSERT(fallibleScope_,
             "[OOM] Cannot allocate a new chunk in an infallible scope.");

  // Compute the size which should be requested in order to be able to fit |n|
  // bytes in a newly allocated chunk, or default to |defaultChunkSize_|.

  size_t minSize;
  if (MOZ_UNLIKELY(!detail::BumpChunk::allocSizeWithRedZone(n, &minSize) ||
                   (minSize & (size_t(1) << (BitSize<size_t>::value - 1))))) {
    return nullptr;
  }

  // Note: When computing chunkSize growth, we only are interested in chunks
  // used for small allocations. This excludes unused chunks, oversized chunks,
  // and chunks transferred in from another LifoAlloc.
  MOZ_ASSERT(curSize_ >= smallAllocsSize_);
  const size_t chunkSize = (oversize || minSize > defaultChunkSize_)
                               ? MallocGoodSize(minSize)
                               : NextSize(defaultChunkSize_, smallAllocsSize_);

  // Create a new BumpChunk, and allocate space for it.
  UniqueBumpChunk result = detail::BumpChunk::newWithCapacity(chunkSize);
  if (!result) {
    return nullptr;
  }
  MOZ_ASSERT(result->computedSizeOfIncludingThis() == chunkSize);
  return result;
}

LifoAlloc::UniqueBumpChunk LifoAlloc::getOrCreateChunk(size_t n) {
  // Look for existing unused BumpChunks to satisfy the request, and pick the
  // first one which is large enough, and move it into the list of used
  // chunks.
  if (!unused_.empty()) {
    if (unused_.begin()->canAlloc(n)) {
      return unused_.popFirst();
    }

    BumpChunkList::Iterator e(unused_.end());
    for (BumpChunkList::Iterator i(unused_.begin()); i->next() != e.get();
         ++i) {
      detail::BumpChunk* elem = i->next();
      MOZ_ASSERT(elem->empty());
      if (elem->canAlloc(n)) {
        BumpChunkList temp = unused_.splitAfter(i.get());
        UniqueBumpChunk newChunk = temp.popFirst();
        unused_.appendAll(std::move(temp));
        return newChunk;
      }
    }
  }

  // Allocate a new BumpChunk with enough space for the next allocation.
  UniqueBumpChunk newChunk = newChunkWithCapacity(n, false);
  if (!newChunk) {
    return newChunk;
  }
  incrementCurSize(newChunk->computedSizeOfIncludingThis());
  return newChunk;
}

void* LifoAlloc::allocImplColdPath(size_t n) {
  void* result;
  UniqueBumpChunk newChunk = getOrCreateChunk(n);
  if (!newChunk) {
    return nullptr;
  }

  // This new chunk is about to be used for small allocations.
  smallAllocsSize_ += newChunk->computedSizeOfIncludingThis();

  // Since we just created a large enough chunk, this can't fail.
  chunks_.append(std::move(newChunk));
  result = chunks_.last()->tryAlloc(n);
  MOZ_ASSERT(result);
  return result;
}

void* LifoAlloc::allocImplOversize(size_t n) {
  void* result;
  UniqueBumpChunk newChunk = newChunkWithCapacity(n, true);
  if (!newChunk) {
    return nullptr;
  }
  incrementCurSize(newChunk->computedSizeOfIncludingThis());

  // Since we just created a large enough chunk, this can't fail.
  oversize_.append(std::move(newChunk));
  result = oversize_.last()->tryAlloc(n);
  MOZ_ASSERT(result);
  return result;
}

bool LifoAlloc::ensureUnusedApproximateColdPath(size_t n, size_t total) {
  for (detail::BumpChunk& bc : unused_) {
    total += bc.unused();
    if (total >= n) {
      return true;
    }
  }

  UniqueBumpChunk newChunk = newChunkWithCapacity(n, false);
  if (!newChunk) {
    return false;
  }
  incrementCurSize(newChunk->computedSizeOfIncludingThis());
  unused_.pushFront(std::move(newChunk));
  return true;
}

LifoAlloc::Mark LifoAlloc::mark() {
  markCount++;
  Mark res;
  if (!chunks_.empty()) {
    res.chunk = chunks_.last()->mark();
  }
  if (!oversize_.empty()) {
    res.oversize = oversize_.last()->mark();
  }
  return res;
}

void LifoAlloc::release(Mark mark) {
  markCount--;
#ifdef DEBUG
  auto assertIsContained = [](const detail::BumpChunk::Mark& m,
                              BumpChunkList& list) {
    if (m.markedChunk()) {
      bool contained = false;
      for (const detail::BumpChunk& chunk : list) {
        if (&chunk == m.markedChunk() && chunk.contains(m)) {
          contained = true;
          break;
        }
      }
      MOZ_ASSERT(contained);
    }
  };
  assertIsContained(mark.chunk, chunks_);
  assertIsContained(mark.oversize, oversize_);
#endif

  BumpChunkList released;
  auto cutAtMark = [&released](const detail::BumpChunk::Mark& m,
                               BumpChunkList& list) {
    // Move the blocks which are after the mark to the set released chunks.
    if (!m.markedChunk()) {
      released = std::move(list);
    } else {
      released = list.splitAfter(m.markedChunk());
    }

    // Release everything which follows the mark in the last chunk.
    if (!list.empty()) {
      list.last()->release(m);
    }
  };

  // Release the content of all the blocks which are after the marks, and keep
  // blocks as unused.
  cutAtMark(mark.chunk, chunks_);
  for (detail::BumpChunk& bc : released) {
    bc.release();

    // Chunks moved from (after a mark) in chunks_ to unused_ are no longer
    // considered small allocations.
    smallAllocsSize_ -= bc.computedSizeOfIncludingThis();
  }
  unused_.appendAll(std::move(released));

  // Free the content of all the blocks which are after the marks.
  cutAtMark(mark.oversize, oversize_);
  while (!released.empty()) {
    UniqueBumpChunk bc = released.popFirst();
    decrementCurSize(bc->computedSizeOfIncludingThis());
  }
}

void LifoAlloc::steal(LifoAlloc* other) {
  MOZ_ASSERT(!other->markCount);
  MOZ_DIAGNOSTIC_ASSERT(unused_.empty());
  MOZ_DIAGNOSTIC_ASSERT(chunks_.empty());
  MOZ_DIAGNOSTIC_ASSERT(oversize_.empty());

  // Copy everything from |other| to |this| except for |peakSize_|, which
  // requires some care.
  chunks_ = std::move(other->chunks_);
  oversize_ = std::move(other->oversize_);
  unused_ = std::move(other->unused_);
  markCount = other->markCount;
  defaultChunkSize_ = other->defaultChunkSize_;
  oversizeThreshold_ = other->oversizeThreshold_;
  curSize_ = other->curSize_;
  peakSize_ = std::max(peakSize_, other->peakSize_);
  smallAllocsSize_ = other->smallAllocsSize_;
#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
  fallibleScope_ = other->fallibleScope_;
#endif

  other->reset(defaultChunkSize_);
}

void LifoAlloc::transferFrom(LifoAlloc* other) {
  MOZ_ASSERT(!markCount);
  MOZ_ASSERT(!other->markCount);

  // Transferred chunks are not counted as part of |smallAllocsSize| as this
  // could introduce bias in the |NextSize| heuristics, leading to
  // over-allocations in *this* LifoAlloc. As well, to avoid interference with
  // small allocations made with |this|, the last chunk of the |chunks_| list
  // should remain the last chunk. Therefore, the transferred chunks are
  // prepended to the |chunks_| list.
  incrementCurSize(other->curSize_);

  appendUnused(std::move(other->unused_));
  chunks_.prependAll(std::move(other->chunks_));
  oversize_.prependAll(std::move(other->oversize_));
  other->curSize_ = 0;
  other->smallAllocsSize_ = 0;
}

void LifoAlloc::transferUnusedFrom(LifoAlloc* other) {
  MOZ_ASSERT(!markCount);

  size_t size = 0;
  for (detail::BumpChunk& bc : other->unused_) {
    size += bc.computedSizeOfIncludingThis();
  }

  appendUnused(std::move(other->unused_));
  incrementCurSize(size);
  other->decrementCurSize(size);
}

#ifdef LIFO_CHUNK_PROTECT
void LifoAlloc::setReadOnly() {
  for (detail::BumpChunk& bc : chunks_) {
    bc.setReadOnly();
  }
  for (detail::BumpChunk& bc : oversize_) {
    bc.setReadOnly();
  }
  for (detail::BumpChunk& bc : unused_) {
    bc.setReadOnly();
  }
}

void LifoAlloc::setReadWrite() {
  for (detail::BumpChunk& bc : chunks_) {
    bc.setReadWrite();
  }
  for (detail::BumpChunk& bc : oversize_) {
    bc.setReadWrite();
  }
  for (detail::BumpChunk& bc : unused_) {
    bc.setReadWrite();
  }
}
#endif
