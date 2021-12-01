/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_Bitmap_h
#define ds_Bitmap_h

#include <algorithm>

#include "js/AllocPolicy.h"
#include "js/HashTable.h"
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

class DenseBitmap
{
    typedef Vector<uintptr_t, 0, SystemAllocPolicy> Data;
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

    void copyBitsFrom(size_t wordStart, size_t numWords, uintptr_t* source) {
        MOZ_ASSERT(wordStart + numWords <= data.length());
        mozilla::PodCopy(&data[wordStart], source, numWords);
    }

    void bitwiseOrRangeInto(size_t wordStart, size_t numWords, uintptr_t* target) const {
        for (size_t i = 0; i < numWords; i++)
            target[i] |= data[wordStart + i];
    }
};

class SparseBitmap
{
    // The number of words of bits to use for each block mainly affects the
    // memory usage of the bitmap. To minimize overhead, bitmaps which are
    // expected to be fairly dense should have a large block size, and bitmaps
    // which are expected to be very sparse should have a small block size.
    static const size_t WordsInBlock = 4096 / sizeof(uintptr_t);

    typedef mozilla::Array<uintptr_t, WordsInBlock> BitBlock;
    typedef HashMap<size_t, BitBlock*, DefaultHasher<size_t>, SystemAllocPolicy> Data;
    Data data;

    static size_t blockStartWord(size_t word) {
        return word & ~(WordsInBlock - 1);
    }

    // Return the number of words in a BitBlock starting at |blockWord| which
    // are in |other|.
    static size_t wordIntersectCount(size_t blockWord, const DenseBitmap& other) {
        long count = other.numWords() - blockWord;
        return std::min<size_t>((size_t)WordsInBlock, std::max<long>(count, 0));
    }

    BitBlock& createBlock(Data::AddPtr p, size_t blockId);

    MOZ_ALWAYS_INLINE BitBlock* getBlock(size_t blockId) const {
        Data::Ptr p = data.lookup(blockId);
        return p ? p->value() : nullptr;
    }

    MOZ_ALWAYS_INLINE BitBlock& getOrCreateBlock(size_t blockId) {
        Data::AddPtr p = data.lookupForAdd(blockId);
        if (p)
            return *p->value();
        return createBlock(p, blockId);
    }

  public:
    bool init() { return data.init(); }
    ~SparseBitmap();

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf);

    MOZ_ALWAYS_INLINE void setBit(size_t bit) {
        size_t word = bit / JS_BITS_PER_WORD;
        size_t blockWord = blockStartWord(word);
        BitBlock& block = getOrCreateBlock(blockWord / WordsInBlock);
        block[word - blockWord] |= uintptr_t(1) << (bit % JS_BITS_PER_WORD);
    }

    bool getBit(size_t bit) const;

    void bitwiseAndWith(const DenseBitmap& other);
    void bitwiseOrWith(const SparseBitmap& other);
    void bitwiseOrInto(DenseBitmap& other) const;

    // Currently, this API only supports a range of words that is in a single bit block.
    void bitwiseOrRangeInto(size_t wordStart, size_t numWords, uintptr_t* target) const;
};

} // namespace js

#endif // ds_Bitmap_h
