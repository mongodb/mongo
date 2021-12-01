/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_LifoAlloc_h
#define ds_LifoAlloc_h

#include "mozilla/Attributes.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/MemoryChecking.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Move.h"
#include "mozilla/PodOperations.h"
#include "mozilla/TemplateLib.h"
#include "mozilla/TypeTraits.h"

// This data structure supports stacky LIFO allocation (mark/release and
// LifoAllocScope). It does not maintain one contiguous segment; instead, it
// maintains a bunch of linked memory segments. In order to prevent malloc/free
// thrashing, unused segments are deallocated when garbage collection occurs.

#include "jsutil.h"

#include "js/UniquePtr.h"

namespace js {

namespace detail {

template <typename T>
class SingleLinkedList;

template <typename T>
class SingleLinkedListElement
{
    friend class SingleLinkedList<T>;
    js::UniquePtr<T> next_;

  public:
    SingleLinkedListElement()
      : next_(nullptr)
    {}
    ~SingleLinkedListElement() {
        MOZ_ASSERT(!next_);
    }

    T* next() const { return next_.get(); }
};

// Single linked list which is using UniquePtr to hold the next pointers.
// UniquePtr are used to ensure that none of the elements are used
// silmutaneously in 2 different list.
template <typename T>
class SingleLinkedList
{
  private:
    // First element of the list which owns the next element, and ensure that
    // that this list is the only owner of the element.
    UniquePtr<T> head_;

    // Weak pointer to the last element of the list.
    T* last_;

    void assertInvariants() {
        MOZ_ASSERT(bool(head_) == bool(last_));
        MOZ_ASSERT_IF(last_, !last_->next_);
    }

  public:
    SingleLinkedList()
      : head_(nullptr), last_(nullptr)
    {
        assertInvariants();
    }

    SingleLinkedList(SingleLinkedList&& other)
      : head_(mozilla::Move(other.head_)), last_(other.last_)
    {
        other.last_ = nullptr;
        assertInvariants();
        other.assertInvariants();
    }

    ~SingleLinkedList() {
        MOZ_ASSERT(!head_);
        MOZ_ASSERT(!last_);
    }

    // Move the elements of the |other| list in the current one, and implicitly
    // remove all the elements of the current list.
    SingleLinkedList& operator=(SingleLinkedList&& other) {
        head_ = mozilla::Move(other.head_);
        last_ = other.last_;
        other.last_ = nullptr;
        assertInvariants();
        other.assertInvariants();
        return *this;
    }

    bool empty() const { return !last_; }

    // Iterates over the elements of the list, this is used to avoid raw
    // manipulation of pointers as much as possible.
    class Iterator {
        T* current_;

      public:
        explicit Iterator(T* current) : current_(current) {}

        T& operator *() const { return *current_; }
        T* operator ->() const { return current_; }
        T* get() const { return current_; }

        const Iterator& operator++() {
            current_ = current_->next();
            return *this;
        }

        bool operator!=(Iterator& other) const {
            return current_ != other.current_;
        }
        bool operator==(Iterator& other) const {
            return current_ == other.current_;
        }
    };

    Iterator begin() const { return Iterator(head_.get()); }
    Iterator end() const { return Iterator(nullptr); }
    Iterator last() const { return Iterator(last_); }

    // Split the list in 2 single linked lists after the element given as
    // argument.  The front of the list remains in the current list, while the
    // back goes in the newly create linked list.
    //
    // This is used for example to extract one element from a list. (see
    // LifoAlloc::getOrCreateChunk)
    SingleLinkedList splitAfter(T* newLast) {
        MOZ_ASSERT(newLast);
        SingleLinkedList result;
        if (newLast->next_) {
            result.head_ = mozilla::Move(newLast->next_);
            result.last_ = last_;
            last_ = newLast;
        }
        assertInvariants();
        result.assertInvariants();
        return result;
    }

    void pushFront(UniquePtr<T>&& elem) {
        if (!last_)
            last_ = elem.get();
        elem->next_ = mozilla::Move(head_);
        head_ = mozilla::Move(elem);
        assertInvariants();
    }

    void append(UniquePtr<T>&& elem) {
        if (last_) {
            last_->next_ = mozilla::Move(elem);
            last_ = last_->next_.get();
        } else {
            head_ = mozilla::Move(elem);
            last_ = head_.get();
        }
        assertInvariants();
    }
    void appendAll(SingleLinkedList&& list) {
        if (list.empty())
            return;
        if (last_)
            last_->next_ = mozilla::Move(list.head_);
        else
            head_ = mozilla::Move(list.head_);
        last_ = list.last_;
        list.last_ = nullptr;
        assertInvariants();
        list.assertInvariants();
    }
    UniquePtr<T> popFirst() {
        MOZ_ASSERT(head_);
        UniquePtr<T> result = mozilla::Move(head_);
        head_ = mozilla::Move(result->next_);
        if (!head_)
            last_ = nullptr;
        assertInvariants();
        return result;
    }
};

static const size_t LIFO_ALLOC_ALIGN = 8;

MOZ_ALWAYS_INLINE
uint8_t*
AlignPtr(uint8_t* orig) {
    static_assert(mozilla::tl::FloorLog2<LIFO_ALLOC_ALIGN>::value ==
                  mozilla::tl::CeilingLog2<LIFO_ALLOC_ALIGN>::value,
                  "LIFO_ALLOC_ALIGN must be a power of two");

    uint8_t* result = (uint8_t*) AlignBytes(uintptr_t(orig), LIFO_ALLOC_ALIGN);
    MOZ_ASSERT(uintptr_t(result) % LIFO_ALLOC_ALIGN == 0);
    return result;
}

// A Chunk represent a single memory allocation made with the system
// allocator. As the owner of the memory, it is responsible for the allocation
// and the deallocation.
//
// This structure is only move-able, but not copyable.
class BumpChunk : public SingleLinkedListElement<BumpChunk>
{
  private:
    // Pointer to the last byte allocated in this chunk.
    uint8_t* bump_;
    // Pointer to the last byte available in this chunk.
    const uint8_t* capacity_;

#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
    // Magic number used to check against poisoned values.
    const uintptr_t magic_;
    static constexpr uintptr_t magicNumber =
        sizeof(uintptr_t) == 4 ? uintptr_t(0x4c69666f) : uintptr_t(0x4c69666f42756d70);
#endif

    // Poison the memory with memset, in order to catch errors due to
    // use-after-free, with undefinedChunkMemory pattern, or to catch
    // use-before-init with uninitializedChunkMemory.
#if defined(DEBUG)
# define LIFO_HAVE_MEM_CHECKS 1

    // Byte used for poisoning unused memory after releasing memory.
    static constexpr int undefinedChunkMemory = 0xcd;
    // Byte used for poisoning uninitialized memory after reserving memory.
    static constexpr int uninitializedChunkMemory = 0xce;

# define LIFO_MAKE_MEM_NOACCESS(addr, size)      \
    do {                                         \
        uint8_t* base = (addr);                  \
        size_t sz = (size);                      \
        memset(base, undefinedChunkMemory, sz);  \
        MOZ_MAKE_MEM_NOACCESS(base, sz);         \
    } while (0)

# define LIFO_MAKE_MEM_UNDEFINED(addr, size)         \
    do {                                             \
        uint8_t* base = (addr);                      \
        size_t sz = (size);                          \
        MOZ_MAKE_MEM_UNDEFINED(base, sz);            \
        memset(base, uninitializedChunkMemory, sz);  \
        MOZ_MAKE_MEM_UNDEFINED(base, sz);            \
    } while(0)

#elif defined(MOZ_HAVE_MEM_CHECKS)
# define LIFO_HAVE_MEM_CHECKS 1
# define LIFO_MAKE_MEM_NOACCESS(addr, size) MOZ_MAKE_MEM_NOACCESS((addr), (size))
# define LIFO_MAKE_MEM_UNDEFINED(addr, size) MOZ_MAKE_MEM_UNDEFINED((addr), (size))
#endif

    void assertInvariants() {
        MOZ_DIAGNOSTIC_ASSERT(magic_ == magicNumber);
        MOZ_ASSERT(begin() <= end());
        MOZ_ASSERT(end() <= capacity_);
    }

    BumpChunk& operator=(const BumpChunk&) = delete;
    BumpChunk(const BumpChunk&) = delete;

    explicit BumpChunk(uintptr_t capacity)
      : bump_(begin()),
        capacity_(base() + capacity)
#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
      , magic_(magicNumber)
#endif
    {
        // We cannot bake this value inside the BumpChunk class, because
        // sizeof(BumpChunk) can only be computed after the closing brace of the
        // BumpChunk class, or within one of its methods. As a work-around, the
        // reservedSpace value is baked in, and we check that it indeed matches
        // with the space taken by the data of the BumpChunk class, and the
        // alignment of a pointer.
        MOZ_ASSERT(BumpChunk::reservedSpace == AlignBytes(sizeof(BumpChunk), LIFO_ALLOC_ALIGN),
                   "Checked that the baked-in value correspond to computed value");

        assertInvariants();
#if defined(LIFO_HAVE_MEM_CHECKS)
        // The memory is freshly allocated and marked as undefined by the
        // allocator of the BumpChunk. Instead, we mark this memory as
        // no-access, as it has not been allocated within the BumpChunk.
        LIFO_MAKE_MEM_NOACCESS(bump_, capacity_ - bump_);
#endif
    }

    // Cast |this| into a uint8_t* pointer.
    //
    // Warning: Are you sure you do not want to use begin() instead?
    const uint8_t* base() const { return reinterpret_cast<const uint8_t*>(this); }
    uint8_t* base() { return reinterpret_cast<uint8_t*>(this); }

    // Update the bump pointer to any value contained in this chunk, which is
    // above the private fields of this chunk.
    //
    // The memory is poisoned and flagged as no-access when the bump pointer is
    // moving backward, such as when memory is released, or when a Mark is used
    // to unwind previous allocations.
    //
    // The memory is flagged as undefined when the bump pointer is moving
    // forward.
    void setBump(uint8_t* newBump) {
        assertInvariants();
        MOZ_ASSERT(begin() <= newBump);
        MOZ_ASSERT(newBump <= capacity_);
#if defined(LIFO_HAVE_MEM_CHECKS)
        // Poison/Unpoison memory that we just free'd/allocated.
        if (bump_ > newBump)
            LIFO_MAKE_MEM_NOACCESS(newBump, bump_ - newBump);
        else if (newBump > bump_)
            LIFO_MAKE_MEM_UNDEFINED(bump_, newBump - bump_);
#endif
        bump_ = newBump;
    }

  public:
    ~BumpChunk() {
        release();
    }

    // Space reserved for the BumpChunk internal data, and the alignment of the
    // first allocation content.  This can be used to ensure there is enough
    // space for the next allocation. (see LifoAlloc::newChunkWithCapacity)
    static constexpr size_t reservedSpace = 4 * sizeof(uintptr_t);

    // Returns true if this chunk contains no allocated content.
    bool empty() const { return end() == begin(); }

    // Returns the size in bytes of the number of allocated space. This includes
    // the size consumed by the alignment of the allocations.
    size_t used() const { return end() - begin(); }

    // These are used for manipulating a chunk as if it was a vector of bytes,
    // and used for iterating over the content of the buffer (see
    // LifoAlloc::Enum)
    const uint8_t* begin() const { return base() + reservedSpace; }
    uint8_t* begin() { return base() + reservedSpace; }
    uint8_t* end() const { return bump_; }

    // This function is the only way to allocate and construct a chunk. It
    // returns a UniquePtr to the newly allocated chunk.  The size given as
    // argument includes the space needed for the header of the chunk.
    static UniquePtr<BumpChunk> newWithCapacity(size_t size);

    // Report allocation.
    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return mallocSizeOf(this);
    }

    // Report allocation size.
    size_t computedSizeOfIncludingThis() const {
        return capacity_ - base();
    }

    // Opaque type used to carry a pointer to the current location of the bump_
    // pointer, within a BumpChunk.
    class Mark
    {
        // Chunk which owns the pointer.
        BumpChunk* chunk_;
        // Recorded of the bump_ pointer of the BumpChunk.
        uint8_t* bump_;

        friend class BumpChunk;
        explicit Mark(BumpChunk* chunk, uint8_t* bump)
          : chunk_(chunk), bump_(bump)
        {}

      public:
        Mark() : chunk_(nullptr), bump_(nullptr) {}

        BumpChunk* markedChunk() const { return chunk_; }
    };

    // Return a mark to be able to unwind future allocations with the release
    // function. (see LifoAllocScope)
    Mark mark() {
        return Mark(this, end());
    }

    // Check if a pointer is part of the allocated data of this chunk.
    bool contains(void* ptr) const {
        // Note: We cannot check "ptr < end()" because the mark have a 0-size
        // length.
        return begin() <= ptr && ptr <= end();
    }

    // Check if a mark is part of the allocated data of this chunk.
    bool contains(Mark m) const {
        MOZ_ASSERT(m.chunk_ == this);
        return contains(m.bump_);
    }

    // Release the memory allocated in this chunk. This function does not call
    // any of the destructors.
    void release() {
        setBump(begin());
    }

    // Release the memory allocated in this chunk since the corresponding mark
    // got created. This function does not call any of the destructors.
    void release(Mark m) {
        MOZ_RELEASE_ASSERT(contains(m));
        setBump(m.bump_);
    }

    // Returns true, if the unused space is large enough for an allocation of
    // |n| bytes.
    bool canAlloc(size_t n);

    // Space remaining in the current chunk.
    size_t unused() const {
        uint8_t* aligned = AlignPtr(end());
        if (aligned < capacity_)
            return capacity_ - aligned;
        return 0;
    }

    // Try to perform an allocation of size |n|, returns nullptr if not possible.
    MOZ_ALWAYS_INLINE
    void* tryAlloc(size_t n) {
        uint8_t* aligned = AlignPtr(end());
        uint8_t* newBump = aligned + n;

        if (newBump > capacity_)
            return nullptr;

        // Check for overflow.
        if (MOZ_UNLIKELY(newBump < bump_))
            return nullptr;

        MOZ_ASSERT(canAlloc(n)); // Ensure consistency between "can" and "try".
        setBump(newBump);
        return aligned;
    }
};

} // namespace detail

// LIFO bump allocator: used for phase-oriented and fast LIFO allocations.
//
// Note: We leave BumpChunks latent in the set of unused chunks after they've
// been released to avoid thrashing before a GC.
class LifoAlloc
{
    using BumpChunk = js::UniquePtr<detail::BumpChunk>;
    using BumpChunkList = detail::SingleLinkedList<detail::BumpChunk>;

    // List of chunks containing allocated data. In the common case, the last
    // chunk of this list is always used to perform the allocations. When the
    // allocation cannot be performed, we move a Chunk from the unused set to
    // the list of used chunks.
    BumpChunkList chunks_;

    // Set of unused chunks, which can be reused for future allocations.
    BumpChunkList unused_;

    size_t      markCount;
    size_t      defaultChunkSize_;
    size_t      curSize_;
    size_t      peakSize_;
#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
    bool        fallibleScope_;
#endif

    void operator=(const LifoAlloc&) = delete;
    LifoAlloc(const LifoAlloc&) = delete;

    // Return a BumpChunk that can perform an allocation of at least size |n|.
    BumpChunk newChunkWithCapacity(size_t n);

    // Reuse or allocate a BumpChunk that can perform an allocation of at least
    // size |n|, if successful it is placed at the end the list of |chunks_|.
    MOZ_MUST_USE bool getOrCreateChunk(size_t n);

    void reset(size_t defaultChunkSize) {
        MOZ_ASSERT(mozilla::RoundUpPow2(defaultChunkSize) == defaultChunkSize);
        while (!chunks_.empty())
            chunks_.popFirst();
        while (!unused_.empty())
            unused_.popFirst();
        defaultChunkSize_ = defaultChunkSize;
        markCount = 0;
        curSize_ = 0;
    }

    // Append unused chunks to the end of this LifoAlloc.
    void appendUnused(BumpChunkList&& otherUnused) {
#ifdef DEBUG
        for (detail::BumpChunk& bc: otherUnused)
            MOZ_ASSERT(bc.empty());
#endif
        unused_.appendAll(mozilla::Move(otherUnused));
    }

    // Append used chunks to the end of this LifoAlloc. We act as if all the
    // chunks in |this| are used, even if they're not, so memory may be wasted.
    void appendUsed(BumpChunkList&& otherChunks) {
        chunks_.appendAll(mozilla::Move(otherChunks));
    }

    // Track the amount of space allocated in used and unused chunks.
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
        if (!chunks_.empty() && (result = chunks_.last()->tryAlloc(n)))
            return result;

        if (!getOrCreateChunk(n))
            return nullptr;

        // Since we just created a large enough chunk, this can't fail.
        result = chunks_.last()->tryAlloc(n);
        MOZ_ASSERT(result);
        return result;
    }

  public:
    explicit LifoAlloc(size_t defaultChunkSize)
      : peakSize_(0)
#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
      , fallibleScope_(true)
#endif
    {
        reset(defaultChunkSize);
    }

    // Steal allocated chunks from |other|.
    void steal(LifoAlloc* other) {
        MOZ_ASSERT(!other->markCount);
        MOZ_ASSERT(chunks_.empty());

        // Copy everything from |other| to |this| except for |peakSize_|, which
        // requires some care.
        chunks_ = mozilla::Move(other->chunks_);
        unused_ = mozilla::Move(other->unused_);
        markCount = other->markCount;
        defaultChunkSize_ = other->defaultChunkSize_;
        curSize_ = other->curSize_;
        peakSize_ = Max(peakSize_, other->peakSize_);
#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
        fallibleScope_ = other->fallibleScope_;
#endif

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
#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
        // Only simulate OOMs when we are not using the LifoAlloc as an
        // infallible allocator.
        if (fallibleScope_)
            JS_OOM_POSSIBLY_FAIL();
#endif
        return allocImpl(n);
    }

    MOZ_ALWAYS_INLINE
    void* allocInfallible(size_t n) {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (void* result = allocImpl(n))
            return result;
        oomUnsafe.crash("LifoAlloc::allocInfallible");
        return nullptr;
    }

    // Ensures that enough space exists to satisfy N bytes worth of
    // allocation requests, not necessarily contiguous. Note that this does
    // not guarantee a successful single allocation of N bytes.
    MOZ_ALWAYS_INLINE
    MOZ_MUST_USE bool ensureUnusedApproximate(size_t n) {
        AutoFallibleScope fallibleAllocator(this);
        size_t total = 0;
        if (!chunks_.empty()) {
            total += chunks_.last()->unused();
            if (total >= n)
                return true;
        }

        for (detail::BumpChunk& bc : unused_) {
            total += bc.unused();
            if (total >= n)
                return true;
        }

        BumpChunk newChunk = newChunkWithCapacity(n);
        if (!newChunk)
            return false;
        size_t size = newChunk->computedSizeOfIncludingThis();
        unused_.pushFront(mozilla::Move(newChunk));
        incrementCurSize(size);
        return true;
    }

    MOZ_ALWAYS_INLINE
    void setAsInfallibleByDefault() {
#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
        fallibleScope_ = false;
#endif
    }

    class MOZ_NON_TEMPORARY_CLASS AutoFallibleScope {
#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
        LifoAlloc* lifoAlloc_;
        bool prevFallibleScope_;
        MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

      public:
        explicit AutoFallibleScope(LifoAlloc* lifoAlloc MOZ_GUARD_OBJECT_NOTIFIER_PARAM) {
            MOZ_GUARD_OBJECT_NOTIFIER_INIT;
            lifoAlloc_ = lifoAlloc;
            prevFallibleScope_ = lifoAlloc->fallibleScope_;
            lifoAlloc->fallibleScope_ = true;
        }

        ~AutoFallibleScope() {
            lifoAlloc_->fallibleScope_ = prevFallibleScope_;
        }
#else
      public:
        explicit AutoFallibleScope(LifoAlloc*) {}
#endif
    };

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

    using Mark = detail::BumpChunk::Mark;

    Mark mark() {
        markCount++;
        if (chunks_.empty())
            return Mark();
        return chunks_.last()->mark();
    }

    void release(Mark mark) {
        markCount--;

        // Move the blocks which are after the mark to the set of unused chunks.
        BumpChunkList released;
        if (!mark.markedChunk())
            released = mozilla::Move(chunks_);
        else
            released = mozilla::Move(chunks_.splitAfter(mark.markedChunk()));

        // Release the content of all the blocks which are after the marks.
        for (detail::BumpChunk& bc : released)
            bc.release();
        unused_.appendAll(mozilla::Move(released));

        // Release everything which follows the mark in the last chunk.
        if (!chunks_.empty())
            chunks_.last()->release(mark);
    }

    void releaseAll() {
        MOZ_ASSERT(!markCount);
        for (detail::BumpChunk& bc : chunks_)
            bc.release();
        unused_.appendAll(mozilla::Move(chunks_));
    }

    // Get the total "used" (occupied bytes) count for the arena chunks.
    size_t used() const {
        size_t accum = 0;
        for (const detail::BumpChunk& chunk : chunks_)
            accum += chunk.used();
        return accum;
    }

    // Return true if the LifoAlloc does not currently contain any allocations.
    bool isEmpty() const {
        return chunks_.empty() || !chunks_.last()->empty();
    }

    // Return the number of bytes remaining to allocate in the current chunk.
    // e.g. How many bytes we can allocate before needing a new block.
    size_t availableInCurrentChunk() const {
        if (!chunks_.empty())
            return 0;
        return chunks_.last()->unused();
    }

    // Get the total size of the arena chunks (including unused space).
    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        size_t n = 0;
        for (const detail::BumpChunk& chunk : chunks_)
            n += chunk.sizeOfIncludingThis(mallocSizeOf);
        for (const detail::BumpChunk& chunk : unused_)
            n += chunk.sizeOfIncludingThis(mallocSizeOf);
        return n;
    }

    // Get the total size of the arena chunks (including unused space).
    size_t computedSizeOfExcludingThis() const {
        size_t n = 0;
        for (const detail::BumpChunk& chunk : chunks_)
            n += chunk.computedSizeOfIncludingThis();
        for (const detail::BumpChunk& chunk : unused_)
            n += chunk.computedSizeOfIncludingThis();
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

#ifdef DEBUG
    bool contains(void* ptr) const {
        for (const detail::BumpChunk& chunk : chunks_) {
            if (chunk.contains(ptr))
                return true;
        }
        return false;
    }
#endif

    // Iterate over the data allocated in a LifoAlloc, and interpret it.
    class Enum
    {
        friend class LifoAlloc;
        friend class detail::BumpChunk;

        // Iterator over the list of bump chunks.
        BumpChunkList::Iterator chunkIt_;
        BumpChunkList::Iterator chunkEnd_;
        // Read head (must be within chunk_).
        uint8_t* head_;

        // If there is not enough room in the remaining block for |size|,
        // advance to the next block and update the position.
        uint8_t* seekBaseAndAdvanceBy(size_t size) {
            MOZ_ASSERT(!empty());

            uint8_t* aligned = detail::AlignPtr(head_);
            if (aligned + size > chunkIt_->end()) {
                ++chunkIt_;
                aligned = chunkIt_->begin();
                // The current code assumes that if we have a chunk, then we
                // have allocated something it in.
                MOZ_ASSERT(!chunkIt_->empty());
            }
            head_ = aligned + size;
            MOZ_ASSERT(head_ <= chunkIt_->end());
            return aligned;
        }

      public:
        explicit Enum(LifoAlloc& alloc)
          : chunkIt_(alloc.chunks_.begin()),
            chunkEnd_(alloc.chunks_.end()),
            head_(nullptr)
        {
            if (chunkIt_ != chunkEnd_)
                head_ = chunkIt_->begin();
        }

        // Return true if there are no more bytes to enumerate.
        bool empty() {
            return chunkIt_ == chunkEnd_ ||
                (chunkIt_->next() == chunkEnd_.get() && head_ >= chunkIt_->end());
        }

        // Move the read position forward by the size of one T.
        template <typename T>
        T* read(size_t size = sizeof(T)) {
            return reinterpret_cast<T*>(read(size));
        }

        // Return a pointer to the item at the current position. This returns a
        // pointer to the inline storage, not a copy, and moves the read-head by
        // the requested |size|.
        void* read(size_t size) {
            return seekBaseAndAdvanceBy(size);
        }
    };
};

class MOZ_NON_TEMPORARY_CLASS LifoAllocScope
{
    LifoAlloc*      lifoAlloc;
    LifoAlloc::Mark mark;
    LifoAlloc::AutoFallibleScope fallibleScope;
    bool            shouldRelease;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

  public:
    explicit LifoAllocScope(LifoAlloc* lifoAlloc
                            MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : lifoAlloc(lifoAlloc),
        mark(lifoAlloc->mark()),
        fallibleScope(lifoAlloc),
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
    MOZ_MUST_USE bool checkSimulatedOOM() const {
        return fb == Infallible || !js::oom::ShouldFailWithOOM();
    }
};

} // namespace js

#endif /* ds_LifoAlloc_h */
