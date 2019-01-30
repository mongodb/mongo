/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_StoreBuffer_h
#define gc_StoreBuffer_h

#include "mozilla/Attributes.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/ReentrancyGuard.h"

#include <algorithm>

#include "ds/BitArray.h"
#include "ds/LifoAlloc.h"
#include "gc/Nursery.h"
#include "js/AllocPolicy.h"
#include "js/MemoryMetrics.h"

namespace js {
namespace gc {

class Arena;
class ArenaCellSet;

/*
 * BufferableRef represents an abstract reference for use in the generational
 * GC's remembered set. Entries in the store buffer that cannot be represented
 * with the simple pointer-to-a-pointer scheme must derive from this class and
 * use the generic store buffer interface.
 *
 * A single BufferableRef entry in the generic buffer can represent many entries
 * in the remembered set.  For example js::OrderedHashTableRef represents all
 * the incoming edges corresponding to keys in an ordered hash table.
 */
class BufferableRef
{
  public:
    virtual void trace(JSTracer* trc) = 0;
    bool maybeInRememberedSet(const Nursery&) const { return true; }
};

typedef HashSet<void*, PointerHasher<void*>, SystemAllocPolicy> EdgeSet;

/* The size of a single block of store buffer storage space. */
static const size_t LifoAllocBlockSize = 1 << 13; /* 8KiB */

/*
 * The StoreBuffer observes all writes that occur in the system and performs
 * efficient filtering of them to derive a remembered set for nursery GC.
 */
class StoreBuffer
{
    friend class mozilla::ReentrancyGuard;

    /* The size at which a block is about to overflow. */
    static const size_t LowAvailableThreshold = size_t(LifoAllocBlockSize / 2.0);

    /*
     * This buffer holds only a single type of edge. Using this buffer is more
     * efficient than the generic buffer when many writes will be to the same
     * type of edge: e.g. Value or Cell*.
     */
    template<typename T>
    struct MonoTypeBuffer
    {
        /* The canonical set of stores. */
        typedef HashSet<T, typename T::Hasher, SystemAllocPolicy> StoreSet;
        StoreSet stores_;

        /*
         * A one element cache in front of the canonical set to speed up
         * temporary instances of HeapPtr.
         */
        T last_;

        /* Maximum number of entries before we request a minor GC. */
        const static size_t MaxEntries = 48 * 1024 / sizeof(T);

        explicit MonoTypeBuffer() : last_(T()) {}
        ~MonoTypeBuffer() { stores_.finish(); }

        MOZ_MUST_USE bool init() {
            if (!stores_.initialized() && !stores_.init())
                return false;
            clear();
            return true;
        }

        void clear() {
            last_ = T();
            if (stores_.initialized())
                stores_.clear();
        }

        /* Add one item to the buffer. */
        void put(StoreBuffer* owner, const T& t) {
            MOZ_ASSERT(stores_.initialized());
            sinkStore(owner);
            last_ = t;
        }

        /* Remove an item from the store buffer. */
        void unput(StoreBuffer* owner, const T& v) {
            // Fast, hashless remove of last put.
            if (last_ == v) {
                last_ = T();
                return;
            }
            stores_.remove(v);
        }

        /* Move any buffered stores to the canonical store set. */
        void sinkStore(StoreBuffer* owner) {
            MOZ_ASSERT(stores_.initialized());
            if (last_) {
                AutoEnterOOMUnsafeRegion oomUnsafe;
                if (!stores_.put(last_))
                    oomUnsafe.crash("Failed to allocate for MonoTypeBuffer::put.");
            }
            last_ = T();

            if (MOZ_UNLIKELY(stores_.count() > MaxEntries))
                owner->setAboutToOverflow(T::FullBufferReason);
        }

        bool has(StoreBuffer* owner, const T& v) {
            sinkStore(owner);
            return stores_.has(v);
        }

        /* Trace the source of all edges in the store buffer. */
        void trace(StoreBuffer* owner, TenuringTracer& mover);

        size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) {
            return stores_.sizeOfExcludingThis(mallocSizeOf);
        }

      private:
        MonoTypeBuffer& operator=(const MonoTypeBuffer& other) = delete;
    };

    struct GenericBuffer
    {
        LifoAlloc* storage_;

        explicit GenericBuffer() : storage_(nullptr) {}
        ~GenericBuffer() { js_delete(storage_); }

        MOZ_MUST_USE bool init() {
            if (!storage_)
                storage_ = js_new<LifoAlloc>(LifoAllocBlockSize);
            clear();
            return bool(storage_);
        }

        void clear() {
            if (!storage_)
                return;

            storage_->used() ? storage_->releaseAll() : storage_->freeAll();
        }

        bool isAboutToOverflow() const {
            return !storage_->isEmpty() &&
                   storage_->availableInCurrentChunk() < LowAvailableThreshold;
        }

        /* Trace all generic edges. */
        void trace(StoreBuffer* owner, JSTracer* trc);

        template <typename T>
        void put(StoreBuffer* owner, const T& t) {
            MOZ_ASSERT(storage_);

            /* Ensure T is derived from BufferableRef. */
            (void)static_cast<const BufferableRef*>(&t);

            AutoEnterOOMUnsafeRegion oomUnsafe;
            unsigned size = sizeof(T);
            unsigned* sizep = storage_->pod_malloc<unsigned>();
            if (!sizep)
                oomUnsafe.crash("Failed to allocate for GenericBuffer::put.");
            *sizep = size;

            T* tp = storage_->new_<T>(t);
            if (!tp)
                oomUnsafe.crash("Failed to allocate for GenericBuffer::put.");

            if (isAboutToOverflow())
                owner->setAboutToOverflow(JS::gcreason::FULL_GENERIC_BUFFER);
        }

        size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) {
            return storage_ ? storage_->sizeOfIncludingThis(mallocSizeOf) : 0;
        }

        bool isEmpty() {
            return !storage_ || storage_->isEmpty();
        }

      private:
        GenericBuffer& operator=(const GenericBuffer& other) = delete;
    };

    template <typename Edge>
    struct PointerEdgeHasher
    {
        typedef Edge Lookup;
        static HashNumber hash(const Lookup& l) { return mozilla::HashGeneric(l.edge); }
        static bool match(const Edge& k, const Lookup& l) { return k == l; }
    };

    struct CellPtrEdge
    {
        Cell** edge;

        CellPtrEdge() : edge(nullptr) {}
        explicit CellPtrEdge(Cell** v) : edge(v) {}
        bool operator==(const CellPtrEdge& other) const { return edge == other.edge; }
        bool operator!=(const CellPtrEdge& other) const { return edge != other.edge; }

        bool maybeInRememberedSet(const Nursery& nursery) const {
            MOZ_ASSERT(IsInsideNursery(*edge));
            return !nursery.isInside(edge);
        }

        void trace(TenuringTracer& mover) const;

        CellPtrEdge tagged() const { return CellPtrEdge((Cell**)(uintptr_t(edge) | 1)); }
        CellPtrEdge untagged() const { return CellPtrEdge((Cell**)(uintptr_t(edge) & ~1)); }
        bool isTagged() const { return bool(uintptr_t(edge) & 1); }

        explicit operator bool() const { return edge != nullptr; }

        typedef PointerEdgeHasher<CellPtrEdge> Hasher;

        static const auto FullBufferReason = JS::gcreason::FULL_CELL_PTR_BUFFER;
    };

    struct ValueEdge
    {
        JS::Value* edge;

        ValueEdge() : edge(nullptr) {}
        explicit ValueEdge(JS::Value* v) : edge(v) {}
        bool operator==(const ValueEdge& other) const { return edge == other.edge; }
        bool operator!=(const ValueEdge& other) const { return edge != other.edge; }

        Cell* deref() const { return edge->isGCThing() ? static_cast<Cell*>(edge->toGCThing()) : nullptr; }

        bool maybeInRememberedSet(const Nursery& nursery) const {
            MOZ_ASSERT(IsInsideNursery(deref()));
            return !nursery.isInside(edge);
        }

        void trace(TenuringTracer& mover) const;

        ValueEdge tagged() const { return ValueEdge((JS::Value*)(uintptr_t(edge) | 1)); }
        ValueEdge untagged() const { return ValueEdge((JS::Value*)(uintptr_t(edge) & ~1)); }
        bool isTagged() const { return bool(uintptr_t(edge) & 1); }

        explicit operator bool() const { return edge != nullptr; }

        typedef PointerEdgeHasher<ValueEdge> Hasher;

        static const auto FullBufferReason = JS::gcreason::FULL_VALUE_BUFFER;
    };

    struct SlotsEdge
    {
        // These definitions must match those in HeapSlot::Kind.
        const static int SlotKind = 0;
        const static int ElementKind = 1;

        uintptr_t objectAndKind_; // NativeObject* | Kind
        uint32_t start_;
        uint32_t count_;

        SlotsEdge() : objectAndKind_(0), start_(0), count_(0) {}
        SlotsEdge(NativeObject* object, int kind, uint32_t start, uint32_t count)
          : objectAndKind_(uintptr_t(object) | kind), start_(start), count_(count)
        {
            MOZ_ASSERT((uintptr_t(object) & 1) == 0);
            MOZ_ASSERT(kind <= 1);
            MOZ_ASSERT(count > 0);
            MOZ_ASSERT(start + count > start);
        }

        NativeObject* object() const { return reinterpret_cast<NativeObject*>(objectAndKind_ & ~1); }
        int kind() const { return (int)(objectAndKind_ & 1); }

        bool operator==(const SlotsEdge& other) const {
            return objectAndKind_ == other.objectAndKind_ &&
                   start_ == other.start_ &&
                   count_ == other.count_;
        }

        bool operator!=(const SlotsEdge& other) const {
            return !(*this == other);
        }

        // True if this SlotsEdge range overlaps with the other SlotsEdge range,
        // false if they do not overlap.
        bool overlaps(const SlotsEdge& other) const {
            if (objectAndKind_ != other.objectAndKind_)
                return false;

            // Widen our range by one on each side so that we consider
            // adjacent-but-not-actually-overlapping ranges as overlapping. This
            // is particularly useful for coalescing a series of increasing or
            // decreasing single index writes 0, 1, 2, ..., N into a SlotsEdge
            // range of elements [0, N].
            uint32_t end = start_ + count_ + 1;
            uint32_t start = start_ > 0 ? start_ - 1 : 0;
            MOZ_ASSERT(start < end);

            uint32_t otherEnd = other.start_ + other.count_;
            MOZ_ASSERT(other.start_ <= otherEnd);
            return (start <= other.start_ && other.start_ <= end) ||
                   (start <= otherEnd && otherEnd <= end);
        }

        // Destructively make this SlotsEdge range the union of the other
        // SlotsEdge range and this one. A precondition is that the ranges must
        // overlap.
        void merge(const SlotsEdge& other) {
            MOZ_ASSERT(overlaps(other));
            uint32_t end = Max(start_ + count_, other.start_ + other.count_);
            start_ = Min(start_, other.start_);
            count_ = end - start_;
        }

        bool maybeInRememberedSet(const Nursery& n) const {
            return !IsInsideNursery(reinterpret_cast<Cell*>(object()));
        }

        void trace(TenuringTracer& mover) const;

        explicit operator bool() const { return objectAndKind_ != 0; }

        typedef struct {
            typedef SlotsEdge Lookup;
            static HashNumber hash(const Lookup& l) {
                return mozilla::HashGeneric(l.objectAndKind_, l.start_, l.count_);
            }
            static bool match(const SlotsEdge& k, const Lookup& l) { return k == l; }
        } Hasher;

        static const auto FullBufferReason = JS::gcreason::FULL_SLOT_BUFFER;
    };

    template <typename Buffer, typename Edge>
    void unput(Buffer& buffer, const Edge& edge) {
        MOZ_ASSERT(!JS::CurrentThreadIsHeapBusy());
        MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime_));
        if (!isEnabled())
            return;
        mozilla::ReentrancyGuard g(*this);
        buffer.unput(this, edge);
    }

    template <typename Buffer, typename Edge>
    void put(Buffer& buffer, const Edge& edge) {
        MOZ_ASSERT(!JS::CurrentThreadIsHeapBusy());
        MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime_));
        if (!isEnabled())
            return;
        mozilla::ReentrancyGuard g(*this);
        if (edge.maybeInRememberedSet(nursery_))
            buffer.put(this, edge);
    }

    MonoTypeBuffer<ValueEdge> bufferVal;
    MonoTypeBuffer<CellPtrEdge> bufferCell;
    MonoTypeBuffer<SlotsEdge> bufferSlot;
    ArenaCellSet* bufferWholeCell;
    GenericBuffer bufferGeneric;
    bool cancelIonCompilations_;

    JSRuntime* runtime_;
    const Nursery& nursery_;

    bool aboutToOverflow_;
    bool enabled_;
#ifdef DEBUG
    bool mEntered; /* For ReentrancyGuard. */
#endif

  public:
    explicit StoreBuffer(JSRuntime* rt, const Nursery& nursery)
      : bufferVal(), bufferCell(), bufferSlot(), bufferWholeCell(nullptr), bufferGeneric(),
        cancelIonCompilations_(false), runtime_(rt), nursery_(nursery), aboutToOverflow_(false),
        enabled_(false)
#ifdef DEBUG
        , mEntered(false)
#endif
    {
    }

    MOZ_MUST_USE bool enable();
    void disable();
    bool isEnabled() const { return enabled_; }

    void clear();

    const Nursery& nursery() const { return nursery_; }

    /* Get the overflowed status. */
    bool isAboutToOverflow() const { return aboutToOverflow_; }

    bool cancelIonCompilations() const { return cancelIonCompilations_; }

    /* Insert a single edge into the buffer/remembered set. */
    void putValue(JS::Value* vp) { put(bufferVal, ValueEdge(vp)); }
    void unputValue(JS::Value* vp) { unput(bufferVal, ValueEdge(vp)); }
    void putCell(Cell** cellp) { put(bufferCell, CellPtrEdge(cellp)); }
    void unputCell(Cell** cellp) { unput(bufferCell, CellPtrEdge(cellp)); }
    void putSlot(NativeObject* obj, int kind, uint32_t start, uint32_t count) {
        SlotsEdge edge(obj, kind, start, count);
        if (bufferSlot.last_.overlaps(edge))
            bufferSlot.last_.merge(edge);
        else
            put(bufferSlot, edge);
    }
    inline void putWholeCell(Cell* cell);

    /* Insert an entry into the generic buffer. */
    template <typename T>
    void putGeneric(const T& t) { put(bufferGeneric, t);}

    void setShouldCancelIonCompilations() {
        cancelIonCompilations_ = true;
    }

    /* Methods to trace the source of all edges in the store buffer. */
    void traceValues(TenuringTracer& mover)            { bufferVal.trace(this, mover); }
    void traceCells(TenuringTracer& mover)             { bufferCell.trace(this, mover); }
    void traceSlots(TenuringTracer& mover)             { bufferSlot.trace(this, mover); }
    void traceGenericEntries(JSTracer *trc)            { bufferGeneric.trace(this, trc); }

    void traceWholeCells(TenuringTracer& mover);
    void traceWholeCell(TenuringTracer& mover, JS::TraceKind kind, Cell* cell);

    /* For use by our owned buffers and for testing. */
    void setAboutToOverflow(JS::gcreason::Reason);

    void addToWholeCellBuffer(ArenaCellSet* set);

    void addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf, JS::GCSizes* sizes);
};

// A set of cells in an arena used to implement the whole cell store buffer.
class ArenaCellSet
{
    friend class StoreBuffer;

    // The arena this relates to.
    Arena* arena;

    // Pointer to next set forming a linked list.
    ArenaCellSet* next;

    // Bit vector for each possible cell start position.
    BitArray<MaxArenaCellIndex> bits;

  public:
    explicit ArenaCellSet(Arena* arena);

    bool hasCell(const TenuredCell* cell) const {
        return hasCell(getCellIndex(cell));
    }

    void putCell(const TenuredCell* cell) {
        putCell(getCellIndex(cell));
    }

    bool isEmpty() const {
        return this == &Empty;
    }

    bool hasCell(size_t cellIndex) const;

    void putCell(size_t cellIndex);

    void check() const;

    // Sentinel object used for all empty sets.
    static ArenaCellSet Empty;

    static size_t getCellIndex(const TenuredCell* cell);
    static void getWordIndexAndMask(size_t cellIndex, size_t* wordp, uint32_t* maskp);

    // Attempt to trigger a minor GC if free space in the nursery (where these
    // objects are allocated) falls below this threshold.
    static const size_t NurseryFreeThresholdBytes = 64 * 1024;

    static size_t offsetOfArena() {
        return offsetof(ArenaCellSet, arena);
    }
    static size_t offsetOfBits() {
        return offsetof(ArenaCellSet, bits);
    }
};

ArenaCellSet* AllocateWholeCellSet(Arena* arena);

} /* namespace gc */
} /* namespace js */

#endif /* gc_StoreBuffer_h */
