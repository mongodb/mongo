/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Heap_inl_h
#define gc_Heap_inl_h

#include "gc/Heap.h"

#include "gc/StoreBuffer.h"
#include "gc/Zone.h"
#include "util/Poison.h"
#include "vm/Runtime.h"

inline void js::gc::Arena::init(GCRuntime* gc, JS::Zone* zone, AllocKind kind) {
  MOZ_ASSERT(zone);
  MOZ_ASSERT(IsValidAllocKind(kind));

  MOZ_MAKE_MEM_UNDEFINED(this, ArenaSize);

  allocKind = kind;
  zone_ = zone;
  next = nullptr;
  isNewlyCreated_ = 1;
  onDelayedMarkingList_ = 0;
  hasDelayedBlackMarking_ = 0;
  hasDelayedGrayMarking_ = 0;
  nextDelayedMarkingArena_ = 0;
  if (zone_->isAtomsZone()) {
    atomBitmapStart() = gc->atomMarking.allocateIndex(gc);
  } else {
    bufferedCells() = &ArenaCellSet::Empty;
  }

  setAsFullyUnused();  // Initializes firstFreeSpan.

#ifdef DEBUG
  checkNoMarkedCells();
#endif
}

inline void js::gc::Arena::freeAtomMarkingBitmapIndex(GCRuntime* gc,
                                                      const AutoLockGC& lock) {
  MOZ_ASSERT(zone_->isAtomsZone());
  gc->atomMarking.freeIndex(atomBitmapStart(), lock);
#ifdef DEBUG
  atomBitmapStart() = 0;  // Also zeroed by write to bufferedCells_ in release.
#endif
}

inline void js::gc::Arena::release() {
  MOZ_ASSERT(allocated());

  // Clients should call freeAtomMarkingBitmapIndex() if necessary.
  MOZ_ASSERT_IF(zone_->isAtomsZone(), atomBitmapStart_ == 0);

  // Poison zone pointer to highlight UAF on released arenas in crash data.
  AlwaysPoison(&zone_, JS_FREED_ARENA_PATTERN, sizeof(zone_),
               MemCheckKind::MakeNoAccess);

  firstFreeSpan.initAsEmpty();
  allocKind = AllocKind::LIMIT;
  onDelayedMarkingList_ = 0;
  hasDelayedBlackMarking_ = 0;
  hasDelayedGrayMarking_ = 0;
  nextDelayedMarkingArena_ = 0;
  bufferedCells_ = nullptr;

  MOZ_ASSERT(!allocated());
}

inline js::gc::ArenaCellSet*& js::gc::Arena::bufferedCells() {
  MOZ_ASSERT(zone_ && !zone_->isAtomsZone());
  return bufferedCells_;
}

inline size_t& js::gc::Arena::atomBitmapStart() {
  MOZ_ASSERT(zone_ && zone_->isAtomsZone());
  return atomBitmapStart_;
}

// Mark bitmap API:

// Unless noted otherwise, the following methods that update the mark bits are
// not thread safe and must not be called in parallel with each other.
//
// They use separate read and write operations to avoid an unnecessarily strict
// atomic update on the marking bitmap.
//
// They may be called in parallel with read operations on the mark bitmap where
// there is no required ordering between the operations. This happens when gray
// unmarking occurs in parallel with background sweeping.

// The return value indicates if the cell went from unmarked to marked.
template <size_t BytesPerMarkBit, size_t FirstThingOffset>
MOZ_ALWAYS_INLINE bool
js::gc::MarkBitmap<BytesPerMarkBit, FirstThingOffset>::markIfUnmarked(
    const void* cell, MarkColor color) {
  MarkBitmapWord* word;
  uintptr_t mask;
  getMarkWordAndMask(cell, ColorBit::BlackBit, &word, &mask);
  if (*word & mask) {
    return false;
  }
  if (color == MarkColor::Black) {
    uintptr_t bits = *word;
    *word = bits | mask;
  } else {
    // We use getMarkWordAndMask to recalculate both mask and word as doing just
    // mask << color may overflow the mask.
    getMarkWordAndMask(cell, ColorBit::GrayOrBlackBit, &word, &mask);
    if (*word & mask) {
      return false;
    }
    uintptr_t bits = *word;
    *word = bits | mask;
  }
  return true;
}

// This version of the method is safe in the face of concurrent writes to the
// mark bitmap but if two threads attempt to mark the same cell at the same time
// then both calls can succeed and return true.
//
// This method is used for parallel marking where the extra synchronization
// required to avoid this results in worse performance overall.
template <size_t BytesPerMarkBit, size_t FirstThingOffset>
MOZ_ALWAYS_INLINE bool
js::gc::MarkBitmap<BytesPerMarkBit, FirstThingOffset>::markIfUnmarkedThreadSafe(
    const void* cell, MarkColor color) {
  MarkBitmapWord* word;
  uintptr_t mask;
  getMarkWordAndMask(cell, ColorBit::BlackBit, &word, &mask);
  if (*word & mask) {
    return false;
  }
  if (color == MarkColor::Black) {
    *word |= mask;
  } else {
    // We use getMarkWordAndMask to recalculate both mask and word as doing just
    // mask << color may overflow the mask.
    getMarkWordAndMask(cell, ColorBit::GrayOrBlackBit, &word, &mask);
    if (*word & mask) {
      return false;
    }
    *word |= mask;
  }
  return true;
}

template <size_t BytesPerMarkBit, size_t FirstThingOffset>
MOZ_ALWAYS_INLINE void
js::gc::MarkBitmap<BytesPerMarkBit, FirstThingOffset>::markBlack(
    const void* cell) {
  MarkBitmapWord* word;
  uintptr_t mask;
  getMarkWordAndMask(cell, ColorBit::BlackBit, &word, &mask);
  uintptr_t bits = *word;
  *word = bits | mask;
}

template <size_t BytesPerMarkBit, size_t FirstThingOffset>
MOZ_ALWAYS_INLINE void
js::gc::MarkBitmap<BytesPerMarkBit, FirstThingOffset>::markBlackAtomic(
    const void* cell) {
  MarkBitmapWord* word;
  uintptr_t mask;
  getMarkWordAndMask(cell, ColorBit::BlackBit, &word, &mask);
  *word |= mask;
}

template <size_t BytesPerMarkBit, size_t FirstThingOffset>
MOZ_ALWAYS_INLINE void
js::gc::MarkBitmap<BytesPerMarkBit, FirstThingOffset>::copyMarkBit(
    TenuredCell* dst, const TenuredCell* src, ColorBit colorBit) {
  ArenaChunkBase* srcChunk = detail::GetCellChunkBase(src);
  MarkBitmapWord* srcWord;
  uintptr_t srcMask;
  srcChunk->markBits.getMarkWordAndMask(src, colorBit, &srcWord, &srcMask);

  MarkBitmapWord* dstWord;
  uintptr_t dstMask;
  getMarkWordAndMask(dst, colorBit, &dstWord, &dstMask);

  uintptr_t bits = *dstWord;
  bits &= ~dstMask;
  if (*srcWord & srcMask) {
    bits |= dstMask;
  }
  *dstWord = bits;
}

template <size_t BytesPerMarkBit, size_t FirstThingOffset>
MOZ_ALWAYS_INLINE void
js::gc::MarkBitmap<BytesPerMarkBit, FirstThingOffset>::unmark(
    const void* cell) {
  unmarkOneBit(cell, ColorBit::BlackBit);
  unmarkOneBit(cell, ColorBit::GrayOrBlackBit);
}

template <size_t BytesPerMarkBit, size_t FirstThingOffset>
MOZ_ALWAYS_INLINE void
js::gc::MarkBitmap<BytesPerMarkBit, FirstThingOffset>::unmarkOneBit(
    const void* cell, ColorBit colorBit) {
  MarkBitmapWord* word;
  uintptr_t mask;
  uintptr_t bits;
  getMarkWordAndMask(cell, colorBit, &word, &mask);
  bits = *word;
  *word = bits & ~mask;
}

template <size_t BytesPerMarkBit, size_t FirstThingOffset>
inline js::gc::MarkBitmapWord*
js::gc::MarkBitmap<BytesPerMarkBit, FirstThingOffset>::arenaBits(Arena* arena) {
  static_assert(
      ArenaBitmapBits == ArenaBitmapWords * JS_BITS_PER_WORD,
      "We assume that the part of the bitmap corresponding to the arena "
      "has the exact number of words so we do not need to deal with a word "
      "that covers bits from two arenas.");

  MarkBitmapWord* word;
  uintptr_t unused;
  getMarkWordAndMask(arena, ColorBit::BlackBit, &word, &unused);
  return word;
}

template <size_t BytesPerMarkBit, size_t FirstThingOffset>
void js::gc::MarkBitmap<BytesPerMarkBit, FirstThingOffset>::copyFrom(
    const MarkBitmap& other) {
  for (size_t i = 0; i < WordCount; i++) {
    bitmap[i] = uintptr_t(other.bitmap[i]);
  }
}

template <size_t BytesPerMarkBit, size_t FirstThingOffset>
void js::gc::MarkBitmap<BytesPerMarkBit, FirstThingOffset>::clear() {
  for (size_t i = 0; i < WordCount; i++) {
    bitmap[i] = 0;
  }
}

bool js::gc::TenuredCell::markIfUnmarked(MarkColor color /* = Black */) const {
  return chunk()->markBits.markIfUnmarked(this, color);
}

bool js::gc::TenuredCell::markIfUnmarkedThreadSafe(MarkColor color) const {
  return chunk()->markBits.markIfUnmarkedThreadSafe(this, color);
}

void js::gc::TenuredCell::markBlack() const {
  chunk()->markBits.markBlack(this);
}

void js::gc::TenuredCell::markBlackAtomic() const {
  chunk()->markBits.markBlackAtomic(this);
}

void js::gc::TenuredCell::copyMarkBitsFrom(const TenuredCell* src) {
  ChunkMarkBitmap& markBits = chunk()->markBits;
  markBits.copyMarkBit(this, src, ColorBit::BlackBit);
  markBits.copyMarkBit(this, src, ColorBit::GrayOrBlackBit);
}

void js::gc::TenuredCell::unmark() { chunk()->markBits.unmark(this); }

#endif
