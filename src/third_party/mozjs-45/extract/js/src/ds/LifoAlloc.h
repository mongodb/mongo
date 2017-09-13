/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_LifoAlloc_h
#define ds_LifoAlloc_h

#include "mozilla/Attributes.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/MemoryChecking.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/PodOperations.h"
#include "mozilla/TemplateLib.h"
#include "mozilla/TypeTraits.h"

// This data structure supports stacky LIFO allocation (mark/release and
// LifoAllocScope). It does not maintain one contiguous segment; instead, it
// maintains a bunch of linked memory segments. In order to prevent malloc/free
// thrashing, unused segments are deallocated when garbage collection occurs.

#include "jsutil.h"

namespace js {

namespace detail {

static const size_t LIFO_ALLOC_ALIGN = 8;

MOZ_ALWAYS_INLINE
char*
AlignPtr(void* orig)
{
    static_assert(mozilla::tl::FloorLog2<LIFO_ALLOC_ALIGN>::value ==
                  mozilla::tl::CeilingLog2<LIFO_ALLOC_ALIGN>::value,
                  "LIFO_ALLOC_ALIGN must be a power of two");

    char* result = (char*) ((uintptr_t(orig) + (LIFO_ALLOC_ALIGN - 1)) & (~LIFO_ALLOC_ALIGN + 1));
    MOZ_ASSERT(uintptr_t(result) % LIFO_ALLOC_ALIGN == 0);
    return result;
}

// Header for a chunk of memory wrangled by the LifoAlloc.
class BumpChunk
{
    char*       bump;          // start of the available data
    char*       limit;         // end of the data
    BumpChunk*  next_;         // the next BumpChunk
    size_t      bumpSpaceSize;  // size of the data area

    char* headerBase() { return reinterpret_cast<char*>(this); }
    char* bumpBase() const { return limit - bumpSpaceSize; }

    explicit BumpChunk(size_t bumpSpaceSize)
      : bump(reinterpret_cast<char*>(this) + sizeof(BumpChunk)),
        limit(bump + bumpSpaceSize),
        next_(nullptr), bumpSpaceSize(bumpSpaceSize)
    {
        MOZ_ASSERT(bump == AlignPtr(bump));
    }

    void setBump(void* ptr) {
        MOZ_ASSERT(bumpBase() <= ptr);
        MOZ_ASSERT(ptr <= limit);
#if defined(DEBUG) || defined(MOZ_HAVE_MEM_CHECKS)
        char* prevBump = bump;
#endif
        bump = static_cast<char*>(ptr);
#ifdef DEBUG
        MOZ_ASSERT(contains(prevBump));

        // Clobber the now-free space.
        if (prevBump > bump)
            memset(bump, 0xcd, prevBump - bump);
#endif

        // Poison/Unpoison memory that we just free'd/allocated.
#if defined(MOZ_HAVE_MEM_CHECKS)
        if (prevBump > bump)
            MOZ_MAKE_MEM_NOACCESS(bump, prevBump - bump);
        else if (bump > prevBump)
            MOZ_MAKE_MEM_UNDEFINED(prevBump, bump - prevBump);
#endif
    }

  public:
    BumpChunk* next() const { return next_; }
    void setNext(BumpChunk* succ) { next_ = succ; }

    size_t used() const { return bump - bumpBase(); }

    void* start() const { return bumpBase(); }
    void* end() const { return limit; }

    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) {
        return mallocSizeOf(this);
    }

    size_t computedSizeOfIncludingThis() {
        return limit - headerBase();
    }

    void resetBump() {
        setBump(headerBase() + sizeof(BumpChunk));
    }

    void* mark() const { return bump; }

    void release(void* mark) {
        MOZ_ASSERT(contains(mark));
        MOZ_ASSERT(mark <= bump);
        setBump(mark);
    }

    bool contains(void* mark) const {
        return bumpBase() <= mark && mark <= limit;
    }

    bool canAlloc(size_t n);

    size_t unused() {
        return limit - AlignPtr(bump);
    }

    // Try to perform an allocation of size |n|, return null if not possible.
    MOZ_ALWAYS_INLINE
    void* tryAlloc(size_t n) {
        char* aligned = AlignPtr(bump);
        char* newBump = aligned + n;

        if (newBump > limit)
            return nullptr;

        // Check for overflow.
        if (MOZ_UNLIKELY(newBump < bump))
            return nullptr;

        MOZ_ASSERT(canAlloc(n)); // Ensure consistency between "can" and "try".
        setBump(newBump);
        return aligned;
    }

    static BumpChunk* new_(size_t chunkSize);
    static void delete_(BumpChunk* chunk);
};

} // namespace detail

// LIFO bump allocator: used for phase-oriented and fast LIFO allocations.
//
// Note: |latest| is not necessary "last". We leave BumpChunks latent in the
// chain after they've been released to avoid thrashing before a GC.
class LifoAlloc
{
    typedef detail::BumpChunk BumpChunk;

    BumpChunk*  first;
    BumpChunk*  latest;
    BumpChunk*  last;
    size_t      markCount;
    size_t      defaultChunkSize_;
    size_t      curSize_;
    size_t      peakSize_;

    void operator=(const LifoAlloc&) = delete;
    LifoAlloc(const LifoAlloc&) = delete;

    // Return a BumpChunk that can perform an allocation of at least size |n|
    // and add it to the chain appropriately.
    //
    // Side effect: if retval is non-null, |first| and |latest| are initialized
    // appropriately.
    BumpChunk* getOrCreateChunk(size_t n);

    void reset(size_t defaultChunkSize) {
        MOZ_ASSERT(mozilla::RoundUpPow2(defaultChunkSize) == defaultChunkSize);
        first = latest = last = nullptr;
        defaultChunkSize_ = defaultChunkSize;
        markCount = 0;
        curSize_ = 0;
    }

    // Append unused chunks to the end of this LifoAlloc.
    void appendUnused(BumpChunk* start, BumpChunk* end) {
        MOZ_ASSERT(start && end);
        if (last)
            last->setNext(start);
        else
            first = latest = start;
        last = end;
    }

    // Append used chunks to the end of this LifoAlloc. We act as if all the
    // chunks in |this| are used, even if they're not, so memory may be wasted.
    void appendUsed(BumpChunk* otherFirst, BumpChunk* otherLatest, BumpChunk* otherLast) {
        MOZ_ASSERT(otherFirst && otherLatest && otherLast);
        if (last)
            last->setNext(otherFirst);
        else
            first = otherFirst;
        latest = otherLatest;
        last = otherLast;
    }

    void incrementCurSize(size_t size) {
        curSize_ += size;
        if (curSize_ > peakSize_)
            peakSize_ = curSize_;
    }
    void decrementCurSize(size_t size) {
        MOZ_ASSERT(curSize_ >= size);
        curSize_ -= size;
    }

    MOZ_ALWAYS_INLINE
    void* allocImpl(size_t n) {
        void* result;
        if (latest && (result = latest->tryAlloc(n)))
            return result;

        if (!getOrCreateChunk(n))
            return nullptr;

        // Since we just created a large enough chunk, this can't fail.
        result = latest->tryAlloc(n);
        MOZ_ASSERT(result);
        return result;
    }

  public:
    explicit LifoAlloc(size_t defaultChunkSize)
      : peakSize_(0)
    {
        reset(defaultChunkSize);
    }

    // Steal allocated chunks from |other|.
    void steal(LifoAlloc* other) {
        MOZ_ASSERT(!other->markCount);
        MOZ_ASSERT(!latest);

        // Copy everything from |other| to |this| except for |peakSize_|, which
        // requires some care.
        size_t oldPeakSize = peakSize_;
        mozilla::PodAssign(this, other);
        peakSize_ = Max(oldPeakSize, curSize_);

        other->reset(defaultChunkSize_);
    }

    // Append all chunks from |other|. They are removed from |other|.
    void transferFrom(LifoAlloc* other);

    // Append unused chunks from |other|. They are removed from |other|.
    void transferUnusedFrom(LifoAlloc* other);

    ~LifoAlloc() { freeAll(); }

    size_t defaultChunkSize() const { return defaultChunkSize_; }

    // Frees all held memory.
    void freeAll();

    static const unsigned HUGE_ALLOCATION = 50 * 1024 * 1024;
    void freeAllIfHugeAndUnused() {
        if (markCount == 0 && curSize_ > HUGE_ALLOCATION)
            freeAll();
    }

    MOZ_ALWAYS_INLINE
    void* alloc(size_t n) {
        JS_OOM_POSSIBLY_FAIL();
        return allocImpl(n);
    }

    MOZ_ALWAYS_INLINE
    void* allocInfallibleOrAssert(size_t n) {
        void* result = allocImpl(n);
        MOZ_RELEASE_ASSERT(result, "[OOM] Is it really infallible?");
        return result;
    }

    MOZ_ALWAYS_INLINE
    void* allocInfallibleOrCrash(size_t n) {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (void* result = allocImpl(n))
            return result;
        oomUnsafe.crash("LifoAlloc::allocInfallible");
        return nullptr;
    }

    MOZ_ALWAYS_INLINE
    void* allocInfallible(size_t n) {
        return allocInfallibleOrCrash(n);
    }

    // Ensures that enough space exists to satisfy N bytes worth of
    // allocation requests, not necessarily contiguous. Note that this does
    // not guarantee a successful single allocation of N bytes.
    MOZ_ALWAYS_INLINE
    bool ensureUnusedApproximate(size_t n) {
        size_t total = 0;
        for (BumpChunk* chunk = latest; chunk; chunk = chunk->next()) {
            total += chunk->unused();
            if (total >= n)
                return true;
        }
        BumpChunk* latestBefore = latest;
        if (!getOrCreateChunk(n))
            return false;
        if (latestBefore)
            latest = latestBefore;
        return true;
    }

    template <typename T>
    T* newArray(size_t count) {
        static_assert(mozilla::IsPod<T>::value,
                      "T must be POD so that constructors (and destructors, "
                      "when the LifoAlloc is freed) need not be called");
        return newArrayUninitialized<T>(count);
    }

    // Create an array with uninitialized elements of type |T|.
    // The caller is responsible for initialization.
    template <typename T>
    T* newArrayUninitialized(size_t count) {
        size_t bytes;
        if (MOZ_UNLIKELY(!CalculateAllocSize<T>(count, &bytes)))
            return nullptr;
        return static_cast<T*>(alloc(bytes));
    }

    class Mark {
        BumpChunk* chunk;
        void* markInChunk;
        friend class LifoAlloc;
        Mark(BumpChunk* chunk, void* markInChunk) : chunk(chunk), markInChunk(markInChunk) {}
      public:
        Mark() : chunk(nullptr), markInChunk(nullptr) {}
    };

    Mark mark() {
        markCount++;
        return latest ? Mark(latest, latest->mark()) : Mark();
    }

    void release(Mark mark) {
        markCount--;
        if (!mark.chunk) {
            latest = first;
            if (latest)
                latest->resetBump();
        } else {
            latest = mark.chunk;
            latest->release(mark.markInChunk);
        }
    }

    void releaseAll() {
        MOZ_ASSERT(!markCount);
        latest = first;
        if (latest)
            latest->resetBump();
    }

    // Get the total "used" (occupied bytes) count for the arena chunks.
    size_t used() const {
        size_t accum = 0;
        for (BumpChunk* chunk = first; chunk; chunk = chunk->next()) {
            accum += chunk->used();
            if (chunk == latest)
                break;
        }
        return accum;
    }

    // Return true if the LifoAlloc does not currently contain any allocations.
    bool isEmpty() const {
        return !latest || !latest->used();
    }

    // Return the number of bytes remaining to allocate in the current chunk.
    // e.g. How many bytes we can allocate before needing a new block.
    size_t availableInCurrentChunk() const {
        if (!latest)
            return 0;
        return latest->unused();
    }

    // Get the total size of the arena chunks (including unused space).
    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        size_t n = 0;
        for (BumpChunk* chunk = first; chunk; chunk = chunk->next())
            n += chunk->sizeOfIncludingThis(mallocSizeOf);
        return n;
    }

    // Get the total size of the arena chunks (including unused space).
    size_t computedSizeOfExcludingThis() const {
        size_t n = 0;
        for (BumpChunk* chunk = first; chunk; chunk = chunk->next())
            n += chunk->computedSizeOfIncludingThis();
        return n;
    }

    // Like sizeOfExcludingThis(), but includes the size of the LifoAlloc itself.
    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
    }

    // Get the peak size of the arena chunks (including unused space and
    // bookkeeping space).
    size_t peakSizeOfExcludingThis() const { return peakSize_; }

    // Doesn't perform construction; useful for lazily-initialized POD types.
    template <typename T>
    MOZ_ALWAYS_INLINE
    T* pod_malloc() {
        return static_cast<T*>(alloc(sizeof(T)));
    }

    JS_DECLARE_NEW_METHODS(new_, alloc, MOZ_ALWAYS_INLINE)
    JS_DECLARE_NEW_METHODS(newInfallible, allocInfallible, MOZ_ALWAYS_INLINE)

    // A mutable enumeration of the allocated data.
    class Enum
    {
        friend class LifoAlloc;
        friend class detail::BumpChunk;

        LifoAlloc* alloc_;  // The LifoAlloc being traversed.
        BumpChunk* chunk_;  // The current chunk.
        char* position_;    // The current position (must be within chunk_).

        // If there is not enough room in the remaining block for |size|,
        // advance to the next block and update the position.
        void ensureSpaceAndAlignment(size_t size) {
            MOZ_ASSERT(!empty());
            char* aligned = detail::AlignPtr(position_);
            if (aligned + size > chunk_->end()) {
                chunk_ = chunk_->next();
                position_ = static_cast<char*>(chunk_->start());
            } else {
                position_ = aligned;
            }
            MOZ_ASSERT(uintptr_t(position_) + size <= uintptr_t(chunk_->end()));
        }

      public:
        explicit Enum(LifoAlloc& alloc)
          : alloc_(&alloc),
            chunk_(alloc.first),
            position_(static_cast<char*>(alloc.first ? alloc.first->start() : nullptr))
        {}

        // Return true if there are no more bytes to enumerate.
        bool empty() {
            return !chunk_ || (chunk_ == alloc_->latest && position_ >= chunk_->mark());
        }

        // Move the read position forward by the size of one T.
        template <typename T>
        void popFront() {
            popFront(sizeof(T));
        }

        // Move the read position forward by |size| bytes.
        void popFront(size_t size) {
            ensureSpaceAndAlignment(size);
            position_ = position_ + size;
        }

        // Update the bytes at the current position with a new value.
        template <typename T>
        void updateFront(const T& t) {
            ensureSpaceAndAlignment(sizeof(T));
            memmove(position_, &t, sizeof(T));
        }

        // Return a pointer to the item at the current position. This
        // returns a pointer to the inline storage, not a copy.
        template <typename T>
        T* get(size_t size = sizeof(T)) {
            ensureSpaceAndAlignment(size);
            return reinterpret_cast<T*>(position_);
        }

        // Return a Mark at the current position of the Enum.
        Mark mark() {
            alloc_->markCount++;
            return Mark(chunk_, position_);
        }
    };
};

class MOZ_NON_TEMPORARY_CLASS LifoAllocScope
{
    LifoAlloc*      lifoAlloc;
    LifoAlloc::Mark mark;
    bool            shouldRelease;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

  public:
    explicit LifoAllocScope(LifoAlloc* lifoAlloc
                            MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : lifoAlloc(lifoAlloc),
        mark(lifoAlloc->mark()),
        shouldRelease(true)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    ~LifoAllocScope() {
        if (shouldRelease)
            lifoAlloc->release(mark);
    }

    LifoAlloc& alloc() {
        return *lifoAlloc;
    }

    void releaseEarly() {
        MOZ_ASSERT(shouldRelease);
        lifoAlloc->release(mark);
        shouldRelease = false;
    }
};

enum Fallibility {
    Fallible,
    Infallible
};

template <Fallibility fb>
class LifoAllocPolicy
{
    LifoAlloc& alloc_;

  public:
    MOZ_IMPLICIT LifoAllocPolicy(LifoAlloc& alloc)
      : alloc_(alloc)
    {}
    template <typename T>
    T* maybe_pod_malloc(size_t numElems) {
        size_t bytes;
        if (MOZ_UNLIKELY(!CalculateAllocSize<T>(numElems, &bytes)))
            return nullptr;
        void* p = fb == Fallible ? alloc_.alloc(bytes) : alloc_.allocInfallible(bytes);
        return static_cast<T*>(p);
    }
    template <typename T>
    T* maybe_pod_calloc(size_t numElems) {
        T* p = maybe_pod_malloc<T>(numElems);
        if (MOZ_UNLIKELY(!p))
            return nullptr;
        memset(p, 0, numElems * sizeof(T));
        return p;
    }
    template <typename T>
    T* maybe_pod_realloc(T* p, size_t oldSize, size_t newSize) {
        T* n = maybe_pod_malloc<T>(newSize);
        if (MOZ_UNLIKELY(!n))
            return nullptr;
        MOZ_ASSERT(!(oldSize & mozilla::tl::MulOverflowMask<sizeof(T)>::value));
        memcpy(n, p, Min(oldSize * sizeof(T), newSize * sizeof(T)));
        return n;
    }
    template <typename T>
    T* pod_malloc(size_t numElems) {
        return maybe_pod_malloc<T>(numElems);
    }
    template <typename T>
    T* pod_calloc(size_t numElems) {
        return maybe_pod_calloc<T>(numElems);
    }
    template <typename T>
    T* pod_realloc(T* p, size_t oldSize, size_t newSize) {
        return maybe_pod_realloc<T>(p, oldSize, newSize);
    }
    void free_(void* p) {
    }
    void reportAllocOverflow() const {
    }
    bool checkSimulatedOOM() const {
        return fb == Infallible || !js::oom::ShouldFailWithOOM();
    }
};

} // namespace js

#endif /* ds_LifoAlloc_h */
