/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ds/Bitmap.h"

#include <algorithm>

#include "js/UniquePtr.h"

using namespace js;

SparseBitmap::~SparseBitmap() {
  for (Data::Range r(data.all()); !r.empty(); r.popFront()) {
    js_delete(r.front().value());
  }
}

size_t SparseBitmap::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) {
  size_t size = data.shallowSizeOfExcludingThis(mallocSizeOf);
  for (Data::Range r(data.all()); !r.empty(); r.popFront()) {
    size += mallocSizeOf(r.front().value());
  }
  return size;
}

SparseBitmap::BitBlock* SparseBitmap::createBlock(Data::AddPtr p,
                                                  size_t blockId) {
  MOZ_ASSERT(!p);
  auto block = js::MakeUnique<BitBlock>();
  if (!block || !data.add(p, blockId, block.get())) {
    return nullptr;
  }
  std::fill(block->begin(), block->end(), 0);
  return block.release();
}

SparseBitmap::BitBlock& SparseBitmap::createBlock(
    Data::AddPtr p, size_t blockId, AutoEnterOOMUnsafeRegion& oomUnsafe) {
  BitBlock* block = createBlock(p, blockId);
  if (!block) {
    oomUnsafe.crash("Bitmap OOM");
  }
  return *block;
}

bool SparseBitmap::getBit(size_t bit) const {
  size_t word = bit / JS_BITS_PER_WORD;
  size_t blockWord = blockStartWord(word);

  BitBlock* block = getBlock(blockWord / WordsInBlock);
  if (block) {
    return (*block)[word - blockWord] &
           (uintptr_t(1) << (bit % JS_BITS_PER_WORD));
  }
  return false;
}

void SparseBitmap::bitwiseAndWith(const DenseBitmap& other) {
  for (Data::Enum e(data); !e.empty(); e.popFront()) {
    BitBlock& block = *e.front().value();
    size_t blockWord = e.front().key() * WordsInBlock;
    bool anySet = false;
    size_t numWords = wordIntersectCount(blockWord, other);
    for (size_t i = 0; i < numWords; i++) {
      block[i] &= other.word(blockWord + i);
      anySet |= !!block[i];
    }
    if (!anySet) {
      js_delete(&block);
      e.removeFront();
    }
  }
}

void SparseBitmap::bitwiseOrWith(const SparseBitmap& other) {
  for (Data::Range r(other.data.all()); !r.empty(); r.popFront()) {
    const BitBlock& otherBlock = *r.front().value();
    BitBlock& block = getOrCreateBlock(r.front().key());
    for (size_t i = 0; i < WordsInBlock; i++) {
      block[i] |= otherBlock[i];
    }
  }
}

void SparseBitmap::bitwiseOrInto(DenseBitmap& other) const {
  for (Data::Range r(data.all()); !r.empty(); r.popFront()) {
    BitBlock& block = *r.front().value();
    size_t blockWord = r.front().key() * WordsInBlock;
    size_t numWords = wordIntersectCount(blockWord, other);
#ifdef DEBUG
    // Any words out of range in other should be zero in this bitmap.
    for (size_t i = numWords; i < WordsInBlock; i++) {
      MOZ_ASSERT(!block[i]);
    }
#endif
    for (size_t i = 0; i < numWords; i++) {
      other.word(blockWord + i) |= block[i];
    }
  }
}
