/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_Bitmap_h
#define ds_Bitmap_h

#include "mozilla/Array.h"
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/MemoryChecking.h"

#include <algorithm>
#include <stddef.h>
#include <stdint.h>

#include "js/AllocPolicy.h"
#include "js/HashTable.h"
#include "js/HeapAPI.h"
#include "js/Vector.h"

// This file provides two classes for representing bitmaps.
//
// DenseBitmap is an array of words of bits, with size linear in the maximum
// bit which has been set on it.
//
// SparseBitmap provides a reasonably simple, reasonably efficient (in time and
// space) implementation of a sparse bitmap. The basic representation is a hash
// table whose entries are fixed length malloc'ed blocks of bits.

namespace js {

class DenseBitmap {
  using Data = Vector<uintptr_t, 0, SystemAllocPolicy>;

  Data data;

 public:
  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) {
    return data.sizeOfExcludingThis(mallocSizeOf);
  }

  bool ensureSpace(size_t numWords) {
    MOZ_ASSERT(data.empty());
    return data.appendN(0, numWords);
  }

  size_t numWords() const { return data.length(); }
  uintptr_t word(size_t i) const { return data[i]; }
  uintptr_t& word(size_t i) { return data[i]; }

  template <typename T>
  typename std::enable_if_t<std::is_convertible_v<T, uintptr_t>, void>
  copyBitsFrom(size_t wordStart, size_t numWords, T* source) {
    MOZ_ASSERT(wordStart + numWords <= data.length());
    for (size_t i = 0; i < numWords; i++) {
      data[wordStart + i] = source[i];
    }
  }

  template <typename T>
  typename std::enable_if_t<std::is_convertible_v<T, uintptr_t>, void>
  bitwiseOrRangeInto(size_t wordStart, size_t numWords, T* target) const {
    for (size_t i = 0; i < numWords; i++) {
      target[i] |= data[wordStart + i];
    }
  }
};

class SparseBitmap {
  // The number of words of bits to use for each block mainly affects the
  // memory usage of the bitmap. To minimize overhead, bitmaps which are
  // expected to be fairly dense should have a large block size, and bitmaps
  // which are expected to be very sparse should have a small block size.
  static const size_t WordsInBlock = 4096 / sizeof(uintptr_t);

  using BitBlock = mozilla::Array<uintptr_t, WordsInBlock>;
  using Data =
      HashMap<size_t, BitBlock*, DefaultHasher<size_t>, SystemAllocPolicy>;

  Data data;

  MOZ_ALWAYS_INLINE static size_t blockStartWord(size_t word) {
    return word & ~(WordsInBlock - 1);
  }

  MOZ_ALWAYS_INLINE static uintptr_t bitMask(size_t bit) {
    return uintptr_t(1) << (bit % JS_BITS_PER_WORD);
  }

  // Return the number of words in a BitBlock starting at |blockWord| which
  // are in |other|.
  static size_t wordIntersectCount(size_t blockWord, const DenseBitmap& other) {
    long count = other.numWords() - blockWord;
    return static_cast<size_t>(std::clamp(count, 0l, (long)WordsInBlock));
  }

  BitBlock& createBlock(Data::AddPtr p, size_t blockId,
                        AutoEnterOOMUnsafeRegion& oomUnsafe);

  BitBlock* createBlock(Data::AddPtr p, size_t blockId);

  MOZ_ALWAYS_INLINE BitBlock* getBlock(size_t blockId) const {
    Data::Ptr p = data.lookup(blockId);
    return p ? p->value() : nullptr;
  }

  MOZ_ALWAYS_INLINE const BitBlock* readonlyThreadsafeGetBlock(
      size_t blockId) const {
    Data::Ptr p = data.readonlyThreadsafeLookup(blockId);
    return p ? p->value() : nullptr;
  }

  MOZ_ALWAYS_INLINE BitBlock& getOrCreateBlock(size_t blockId) {
    // The lookupForAdd() needs protection against injected OOMs, as does
    // the add() within createBlock().
    AutoEnterOOMUnsafeRegion oomUnsafe;
    Data::AddPtr p = data.lookupForAdd(blockId);
    if (p) {
      return *p->value();
    }
    return createBlock(p, blockId, oomUnsafe);
  }

  MOZ_ALWAYS_INLINE BitBlock* getOrCreateBlockFallible(size_t blockId) {
    Data::AddPtr p = data.lookupForAdd(blockId);
    if (p) {
      return p->value();
    }
    return createBlock(p, blockId);
  }

 public:
  ~SparseBitmap();

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf);

  MOZ_ALWAYS_INLINE void setBit(size_t bit) {
    size_t word = bit / JS_BITS_PER_WORD;
    size_t blockWord = blockStartWord(word);
    BitBlock& block = getOrCreateBlock(blockWord / WordsInBlock);
    block[word - blockWord] |= bitMask(bit);
  }

  MOZ_ALWAYS_INLINE bool setBitFallible(size_t bit) {
    size_t word = bit / JS_BITS_PER_WORD;
    size_t blockWord = blockStartWord(word);
    BitBlock* block = getOrCreateBlockFallible(blockWord / WordsInBlock);
    if (!block) {
      return false;
    }
    (*block)[word - blockWord] |= bitMask(bit);
    return true;
  }

  bool getBit(size_t bit) const;
  bool readonlyThreadsafeGetBit(size_t bit) const;

  void bitwiseAndWith(const DenseBitmap& other);
  void bitwiseOrWith(const SparseBitmap& other);
  void bitwiseOrInto(DenseBitmap& other) const;

  // Currently, the following APIs only supports a range of words that is in a
  // single bit block.

  template <typename T>
  typename std::enable_if_t<std::is_convertible_v<T, uintptr_t>, void>
  bitwiseAndRangeWith(size_t wordStart, size_t numWords, T* source) {
    size_t blockWord = blockStartWord(wordStart);

    // We only support using a single bit block in this API.
    MOZ_ASSERT(numWords &&
               (blockWord == blockStartWord(wordStart + numWords - 1)));

    BitBlock* block = getBlock(blockWord / WordsInBlock);
    if (block) {
      for (size_t i = 0; i < numWords; i++) {
        (*block)[wordStart - blockWord + i] &= source[i];
      }
    }
  }

  template <typename T>
  typename std::enable_if_t<std::is_convertible_v<T, uintptr_t>, void>
  bitwiseOrRangeInto(size_t wordStart, size_t numWords, T* target) const {
    size_t blockWord = blockStartWord(wordStart);

    // We only support using a single bit block in this API.
    MOZ_ASSERT(numWords &&
               (blockWord == blockStartWord(wordStart + numWords - 1)));

    BitBlock* block = getBlock(blockWord / WordsInBlock);
    if (block) {
      for (size_t i = 0; i < numWords; i++) {
        target[i] |= (*block)[wordStart - blockWord + i];
      }
    }
  }
};

}  // namespace js

#endif  // ds_Bitmap_h
