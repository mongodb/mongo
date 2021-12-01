/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ds/LifoAlloc.h"

#include "mozilla/MathAlgorithms.h"

using namespace js;

using mozilla::RoundUpPow2;
using mozilla::tl::BitSize;

namespace js {
namespace detail {

/* static */
UniquePtr<BumpChunk>
BumpChunk::newWithCapacity(size_t size)
{
    MOZ_ASSERT(RoundUpPow2(size) == size);
    MOZ_ASSERT(size >= sizeof(BumpChunk));
    void* mem = js_malloc(size);
    if (!mem)
        return nullptr;

    UniquePtr<BumpChunk> result(new (mem) BumpChunk(size));

    // We assume that the alignment of LIFO_ALLOC_ALIGN is less than that of the
    // underlying memory allocator -- creating a new BumpChunk should always
    // satisfy the LIFO_ALLOC_ALIGN alignment constraint.
    MOZ_ASSERT(AlignPtr(result->begin()) == result->begin());
    return result;
}

bool
BumpChunk::canAlloc(size_t n)
{
    uint8_t* aligned = AlignPtr(bump_);
    uint8_t* newBump = aligned + n;
    // bump_ <= newBump, is necessary to catch overflow.
    return bump_ <= newBump && newBump <= capacity_;
}

} // namespace detail
} // namespace js

void
LifoAlloc::freeAll()
{
    while (!chunks_.empty()) {
        BumpChunk bc = mozilla::Move(chunks_.popFirst());
        decrementCurSize(bc->computedSizeOfIncludingThis());
    }
    while (!unused_.empty()) {
        BumpChunk bc = mozilla::Move(unused_.popFirst());
        decrementCurSize(bc->computedSizeOfIncludingThis());
    }

    // Nb: maintaining curSize_ correctly isn't easy.  Fortunately, this is an
    // excellent sanity check.
    MOZ_ASSERT(curSize_ == 0);
}

LifoAlloc::BumpChunk
LifoAlloc::newChunkWithCapacity(size_t n)
{
    MOZ_ASSERT(fallibleScope_, "[OOM] Cannot allocate a new chunk in an infallible scope.");

    // Compute the size which should be requested in order to be able to fit |n|
    // bytes in the newly allocated chunk, or default the |defaultChunkSize_|.
    size_t defaultChunkFreeSpace = defaultChunkSize_ - detail::BumpChunk::reservedSpace;
    size_t chunkSize;
    if (n > defaultChunkFreeSpace) {
        MOZ_ASSERT(defaultChunkFreeSpace < defaultChunkSize_);
        size_t allocSizeWithCanaries = n + (defaultChunkSize_ - defaultChunkFreeSpace);

        // Guard for overflow.
        if (allocSizeWithCanaries < n ||
            (allocSizeWithCanaries & (size_t(1) << (BitSize<size_t>::value - 1))))
        {
            return nullptr;
        }

        chunkSize = RoundUpPow2(allocSizeWithCanaries);
    } else {
        chunkSize = defaultChunkSize_;
    }

    // Create a new BumpChunk, and allocate space for it.
    BumpChunk result = detail::BumpChunk::newWithCapacity(chunkSize);
    if (!result)
        return nullptr;
    MOZ_ASSERT(result->computedSizeOfIncludingThis() == chunkSize);
    return result;
}

bool
LifoAlloc::getOrCreateChunk(size_t n)
{
    // Look for existing unused BumpChunks to satisfy the request, and pick the
    // first one which is large enough, and move it into the list of used
    // chunks.
    if (!unused_.empty()) {
        if (unused_.begin()->canAlloc(n)) {
            chunks_.append(mozilla::Move(unused_.popFirst()));
            return true;
        }

        BumpChunkList::Iterator e(unused_.end());
        for (BumpChunkList::Iterator i(unused_.begin()); i->next() != e.get(); ++i) {
            detail::BumpChunk* elem = i->next();
            MOZ_ASSERT(elem->empty());
            if (elem->canAlloc(n)) {
                BumpChunkList temp = mozilla::Move(unused_.splitAfter(i.get()));
                chunks_.append(mozilla::Move(temp.popFirst()));
                unused_.appendAll(mozilla::Move(temp));
                return true;
            }
        }
    }

    // Allocate a new BumpChunk with enough space for the next allocation.
    BumpChunk newChunk = newChunkWithCapacity(n);
    if (!newChunk)
        return false;
    size_t size = newChunk->computedSizeOfIncludingThis();
    chunks_.append(mozilla::Move(newChunk));
    incrementCurSize(size);
    return true;
}

void
LifoAlloc::transferFrom(LifoAlloc* other)
{
    MOZ_ASSERT(!markCount);
    MOZ_ASSERT(!other->markCount);

    incrementCurSize(other->curSize_);
    appendUnused(mozilla::Move(other->unused_));
    appendUsed(mozilla::Move(other->chunks_));
    other->curSize_ = 0;
}

void
LifoAlloc::transferUnusedFrom(LifoAlloc* other)
{
    MOZ_ASSERT(!markCount);

    size_t size = 0;
    for (detail::BumpChunk& bc : other->unused_)
        size += bc.computedSizeOfIncludingThis();

    appendUnused(mozilla::Move(other->unused_));
    incrementCurSize(size);
    other->decrementCurSize(size);
}
