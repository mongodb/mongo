/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_StoreBuffer_inl_h
#define gc_StoreBuffer_inl_h

#include "gc/StoreBuffer.h"

#include "gc/Cell.h"
#include "gc/Heap.h"

#include "gc/Heap-inl.h"

namespace js {
namespace gc {

inline /* static */ size_t ArenaCellSet::getCellIndex(const TenuredCell* cell) {
  uintptr_t cellOffset = uintptr_t(cell) & ArenaMask;
  MOZ_ASSERT(cellOffset % ArenaCellIndexBytes == 0);
  return cellOffset / ArenaCellIndexBytes;
}

inline /* static */ void ArenaCellSet::getWordIndexAndMask(size_t cellIndex,
                                                           size_t* wordp,
                                                           uint32_t* maskp) {
  BitArray<MaxArenaCellIndex>::getIndexAndMask(cellIndex, wordp, maskp);
}

inline bool ArenaCellSet::hasCell(size_t cellIndex) const {
  MOZ_ASSERT(cellIndex < MaxArenaCellIndex);
  return bits.get(cellIndex);
}

inline void ArenaCellSet::putCell(size_t cellIndex) {
  MOZ_ASSERT(cellIndex < MaxArenaCellIndex);
  MOZ_ASSERT(arena);

  bits.set(cellIndex);
  check();
}

inline void ArenaCellSet::check() const {
#ifdef DEBUG
  bool bitsZero = bits.isAllClear();
  MOZ_ASSERT(isEmpty() == bitsZero);
  MOZ_ASSERT(isEmpty() == !arena);
  if (!isEmpty()) {
    MOZ_ASSERT(IsCellPointerValid(arena));
    MOZ_ASSERT(arena->bufferedCells() == this);
    JSRuntime* runtime = arena->zone->runtimeFromMainThread();
    MOZ_ASSERT(runtime->gc.minorGCCount() == minorGCNumberAtCreation);
  }
#endif
}

inline void StoreBuffer::WholeCellBuffer::put(const Cell* cell) {
  MOZ_ASSERT(cell->isTenured());

  // BigInts don't have any children, so shouldn't show up here.
  MOZ_ASSERT(cell->getTraceKind() != JS::TraceKind::BigInt);

  Arena* arena = cell->asTenured().arena();
  ArenaCellSet* cells = arena->bufferedCells();
  if (cells->isEmpty()) {
    cells = allocateCellSet(arena);
    if (!cells) {
      return;
    }
  }

  cells->putCell(&cell->asTenured());
  cells->check();
}

inline void StoreBuffer::putWholeCell(Cell* cell) { bufferWholeCell.put(cell); }

}  // namespace gc
}  // namespace js

#endif  // gc_StoreBuffer_inl_h
