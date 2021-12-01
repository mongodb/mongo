/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ds/Bitmap.h"

using namespace js;

SparseBitmap::~SparseBitmap()
{
    if (data.initialized()) {
        for (Data::Range r(data.all()); !r.empty(); r.popFront())
            js_delete(r.front().value());
    }
}

size_t
SparseBitmap::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf)
{
    size_t size = data.sizeOfExcludingThis(mallocSizeOf);
    for (Data::Range r(data.all()); !r.empty(); r.popFront())
        size += mallocSizeOf(r.front().value());
    return size;
}

SparseBitmap::BitBlock&
SparseBitmap::createBlock(Data::AddPtr p, size_t blockId)
{
    MOZ_ASSERT(!p);
    AutoEnterOOMUnsafeRegion oomUnsafe;
    BitBlock* block = js_new<BitBlock>();
    if (!block || !data.add(p, blockId, block))
        oomUnsafe.crash("Bitmap OOM");
    PodZero(block);
    return *block;
}

bool
SparseBitmap::getBit(size_t bit) const
{
    size_t word = bit / JS_BITS_PER_WORD;
    size_t blockWord = blockStartWord(word);

    BitBlock* block = getBlock(blockWord / WordsInBlock);
    if (block)
        return (*block)[word - blockWord] & (uintptr_t(1) << (bit % JS_BITS_PER_WORD));
    return false;
}

void
SparseBitmap::bitwiseAndWith(const DenseBitmap& other)
{
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

void
SparseBitmap::bitwiseOrWith(const SparseBitmap& other)
{
    for (Data::Range r(other.data.all()); !r.empty(); r.popFront()) {
        const BitBlock& otherBlock = *r.front().value();
        BitBlock& block = getOrCreateBlock(r.front().key());
        for (size_t i = 0; i < WordsInBlock; i++)
            block[i] |= otherBlock[i];
    }
}

void
SparseBitmap::bitwiseOrInto(DenseBitmap& other) const
{
    for (Data::Range r(data.all()); !r.empty(); r.popFront()) {
        BitBlock& block = *r.front().value();
        size_t blockWord = r.front().key() * WordsInBlock;
        size_t numWords = wordIntersectCount(blockWord, other);
#ifdef DEBUG
        // Any words out of range in other should be zero in this bitmap.
        for (size_t i = numWords; i < WordsInBlock; i++)
            MOZ_ASSERT(!block[i]);
#endif
        for (size_t i = 0; i < numWords; i++)
            other.word(blockWord + i) |= block[i];
    }
}

void
SparseBitmap::bitwiseOrRangeInto(size_t wordStart, size_t numWords, uintptr_t* target) const
{
    size_t blockWord = blockStartWord(wordStart);

    // We only support using a single bit block in this API.
    MOZ_ASSERT(numWords && (blockWord == blockStartWord(wordStart + numWords - 1)));

    BitBlock* block = getBlock(blockWord / WordsInBlock);
    if (block) {
        for (size_t i = 0; i < numWords; i++)
            target[i] |= (*block)[wordStart - blockWord + i];
    }
}
