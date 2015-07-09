/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_StoreBuffer_h
#define gc_StoreBuffer_h

#include "mozilla/Attributes.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/ReentrancyGuard.h"

#include "jsalloc.h"

#include "ds/LifoAlloc.h"
#include "gc/Nursery.h"
#include "gc/Tracer.h"
#include "js/MemoryMetrics.h"

namespace js {
namespace gc {

/*
 * BufferableRef represents an abstract reference for use in the generational
 * GC's remembered set. Entries in the store buffer that cannot be represented
 * with the simple pointer-to-a-pointer scheme must derive from this class and
 * use the generic store buffer interface.
 */
class BufferableRef
{
  public:
    virtual void mark(JSTracer* trc) = 0;
    bool maybeInRememberedSet(const Nursery&) const { return true; }
};

/*
 * HashKeyRef represents a reference to a HashMap key. This should normally
 * be used through the HashTableWriteBarrierPost function.
 */
template <typename Map, typename Key>
class HashKeyRef : public BufferableRef
{
    Map* map;
    Key key;

  public:
    HashKeyRef(Map* m, const Key& k) : map(m), key(k) {}

    void mark(JSTracer* trc) {
        Key prior = key;
        typename Map::Ptr p = map->lookup(key);
        if (!p)
            return;
        trc->setTracingLocation(&*p);
        Mark(trc, &key, "HashKeyRef");
        map->rekeyIfMoved(prior, key);
    }
};

typedef HashSet<void*, PointerHasher<void*, 3>, SystemAllocPolicy> EdgeSet;

/* The size of a single block of store buffer storage space. */
static const size_t LifoAllocBlockSize = 1 << 16; /* 64KiB */

/*
 * The StoreBuffer observes all writes that occur in the system and performs
 * efficient filtering of them to derive a remembered set for nursery GC.
 */
class StoreBuffer
{
    friend class mozilla::ReentrancyGuard;

    /* The size at which a block is about to overflow. */
    static const size_t LowAvailableThreshold = (size_t)(LifoAllocBlockSize * 1.0 / 16.0);

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
         * A small, fixed-size buffer in front of the canonical set to simplify
         * insertion via jit code.
         */
        const static size_t NumBufferEntries = 4096 / sizeof(T);
        T buffer_[NumBufferEntries];
        T* insert_;

        /* Maximum number of entries before we request a minor GC. */
        const static size_t MaxEntries = 48 * 1024 / sizeof(T);

        explicit MonoTypeBuffer() { clearBuffer(); }
        ~MonoTypeBuffer() { stores_.finish(); }

        bool init() {
            if (!stores_.initialized() && !stores_.init())
                return false;
            clear();
            return true;
        }

        void clearBuffer() {
            JS_POISON(buffer_, JS_EMPTY_STOREBUFFER_PATTERN, NumBufferEntries * sizeof(T));
            insert_ = buffer_;
        }

        void clear() {
            clearBuffer();
            if (stores_.initialized())
                stores_.clear();
        }

        /* Add one item to the buffer. */
        void put(StoreBuffer* owner, const T& t) {
            MOZ_ASSERT(stores_.initialized());
            *insert_++ = t;
            if (MOZ_UNLIKELY(insert_ == buffer_ + NumBufferEntries))
                sinkStores(owner);
        }

        /* Move any buffered stores to the canonical store set. */
        void sinkStores(StoreBuffer* owner) {
            MOZ_ASSERT(stores_.initialized());

            for (T* p = buffer_; p < insert_; ++p) {
                if (!stores_.put(*p))
                    CrashAtUnhandlableOOM("Failed to allocate for MonoTypeBuffer::sinkStores.");
            }
            clearBuffer();

            if (MOZ_UNLIKELY(stores_.count() > MaxEntries))
                owner->setAboutToOverflow();
        }

        /* Remove an item from the store buffer. */
        void unput(StoreBuffer* owner, const T& v) {
            sinkStores(owner);
            stores_.remove(v);
        }

        /* Mark the source of all edges in the store buffer. */
        void mark(StoreBuffer* owner, JSTracer* trc);

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

        bool init() {
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
            return !storage_->isEmpty() && storage_->availableInCurrentChunk() < LowAvailableThreshold;
        }

        /* Mark all generic edges. */
        void mark(StoreBuffer* owner, JSTracer* trc);

        template <typename T>
        void put(StoreBuffer* owner, const T& t) {
            MOZ_ASSERT(storage_);

            /* Ensure T is derived from BufferableRef. */
            (void)static_cast<const BufferableRef*>(&t);

            unsigned size = sizeof(T);
            unsigned* sizep = storage_->pod_malloc<unsigned>();
            if (!sizep)
                CrashAtUnhandlableOOM("Failed to allocate for GenericBuffer::put.");
            *sizep = size;

            T* tp = storage_->new_<T>(t);
            if (!tp)
                CrashAtUnhandlableOOM("Failed to allocate for GenericBuffer::put.");

            if (isAboutToOverflow())
                owner->setAboutToOverflow();
        }

        size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) {
            return storage_ ? storage_->sizeOfIncludingThis(mallocSizeOf) : 0;
        }

      private:
        GenericBuffer& operator=(const GenericBuffer& other) = delete;
    };

    template <typename Edge>
    struct PointerEdgeHasher
    {
        typedef Edge Lookup;
        static HashNumber hash(const Lookup& l) { return uintptr_t(l.edge) >> 3; }
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

        void mark(JSTracer* trc) const;

        CellPtrEdge tagged() const { return CellPtrEdge((Cell**)(uintptr_t(edge) | 1)); }
        CellPtrEdge untagged() const { return CellPtrEdge((Cell**)(uintptr_t(edge) & ~1)); }
        bool isTagged() const { return bool(uintptr_t(edge) & 1); }

        typedef PointerEdgeHasher<CellPtrEdge> Hasher;
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

        void mark(JSTracer* trc) const;

        ValueEdge tagged() const { return ValueEdge((JS::Value*)(uintptr_t(edge) | 1)); }
        ValueEdge untagged() const { return ValueEdge((JS::Value*)(uintptr_t(edge) & ~1)); }
        bool isTagged() const { return bool(uintptr_t(edge) & 1); }

        typedef PointerEdgeHasher<ValueEdge> Hasher;
    };

    struct SlotsEdge
    {
        // These definitions must match those in HeapSlot::Kind.
        const static int SlotKind = 0;
        const static int ElementKind = 1;

        uintptr_t objectAndKind_; // NativeObject* | Kind
        int32_t start_;
        int32_t count_;

        SlotsEdge() : objectAndKind_(0), start_(0), count_(0) {}
        SlotsEdge(NativeObject* object, int kind, int32_t start, int32_t count)
          : objectAndKind_(uintptr_t(object) | kind), start_(start), count_(count)
        {
            MOZ_ASSERT((uintptr_t(object) & 1) == 0);
            MOZ_ASSERT(kind <= 1);
            MOZ_ASSERT(start >= 0);
            MOZ_ASSERT(count > 0);
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

        bool maybeInRememberedSet(const Nursery& n) const {
            return !IsInsideNursery(reinterpret_cast<Cell*>(object()));
        }

        void mark(JSTracer* trc) const;

        typedef struct {
            typedef SlotsEdge Lookup;
            static HashNumber hash(const Lookup& l) { return l.objectAndKind_ ^ l.start_ ^ l.count_; }
            static bool match(const SlotsEdge& k, const Lookup& l) { return k == l; }
        } Hasher;
    };

    struct WholeCellEdges
    {
        Cell* edge;

        WholeCellEdges() : edge(nullptr) {}
        explicit WholeCellEdges(Cell* cell) : edge(cell) {
            MOZ_ASSERT(edge->isTenured());
        }

        bool operator==(const WholeCellEdges& other) const { return edge == other.edge; }
        bool operator!=(const WholeCellEdges& other) const { return edge != other.edge; }

        bool maybeInRememberedSet(const Nursery&) const { return true; }

        static bool supportsDeduplication() { return true; }
        void* deduplicationKey() const { return (void*)edge; }

        void mark(JSTracer* trc) const;

        typedef PointerEdgeHasher<WholeCellEdges> Hasher;
    };

    template <typename Key>
    struct CallbackRef : public BufferableRef
    {
        typedef void (*MarkCallback)(JSTracer* trc, Key* key, void* data);

        CallbackRef(MarkCallback cb, Key* k, void* d) : callback(cb), key(k), data(d) {}

        virtual void mark(JSTracer* trc) {
            callback(trc, key, data);
        }

      private:
        MarkCallback callback;
        Key* key;
        void* data;
    };

    bool isOkayToUseBuffer() const {
        /*
         * Disabled store buffers may not have a valid state; e.g. when stored
         * inline in the ChunkTrailer.
         */
        if (!isEnabled())
            return false;

        /*
         * The concurrent parsing thread cannot validly insert into the buffer,
         * but it should not activate the re-entrancy guard either.
         */
        if (!CurrentThreadCanAccessRuntime(runtime_))
            return false;

        return true;
    }

    template <typename Buffer, typename Edge>
    void putFromAnyThread(Buffer& buffer, const Edge& edge) {
        if (!isOkayToUseBuffer())
            return;
        mozilla::ReentrancyGuard g(*this);
        if (edge.maybeInRememberedSet(nursery_))
            buffer.put(this, edge);
    }

    template <typename Buffer, typename Edge>
    void unputFromAnyThread(Buffer& buffer, const Edge& edge) {
        if (!isOkayToUseBuffer())
            return;
        mozilla::ReentrancyGuard g(*this);
        buffer.unput(this, edge);
    }

    template <typename Buffer, typename Edge>
    void putFromMainThread(Buffer& buffer, const Edge& edge) {
        if (!isEnabled())
            return;
        MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime_));
        mozilla::ReentrancyGuard g(*this);
        if (edge.maybeInRememberedSet(nursery_))
            buffer.put(this, edge);
    }

    MonoTypeBuffer<ValueEdge> bufferVal;
    MonoTypeBuffer<CellPtrEdge> bufferCell;
    MonoTypeBuffer<SlotsEdge> bufferSlot;
    MonoTypeBuffer<WholeCellEdges> bufferWholeCell;
    MonoTypeBuffer<ValueEdge> bufferRelocVal;
    MonoTypeBuffer<CellPtrEdge> bufferRelocCell;
    GenericBuffer bufferGeneric;

    JSRuntime* runtime_;
    const Nursery& nursery_;

    bool aboutToOverflow_;
    bool enabled_;
    mozilla::DebugOnly<bool> mEntered; /* For ReentrancyGuard. */

  public:
    explicit StoreBuffer(JSRuntime* rt, const Nursery& nursery)
      : bufferVal(), bufferCell(), bufferSlot(), bufferWholeCell(),
        bufferRelocVal(), bufferRelocCell(), bufferGeneric(),
        runtime_(rt), nursery_(nursery), aboutToOverflow_(false), enabled_(false),
        mEntered(false)
    {
    }

    bool enable();
    void disable();
    bool isEnabled() const { return enabled_; }

    bool clear();

    /* Get the overflowed status. */
    bool isAboutToOverflow() const { return aboutToOverflow_; }

    /* Insert a single edge into the buffer/remembered set. */
    void putValueFromAnyThread(JS::Value* valuep) { putFromAnyThread(bufferVal, ValueEdge(valuep)); }
    void putCellFromAnyThread(Cell** cellp) { putFromAnyThread(bufferCell, CellPtrEdge(cellp)); }
    void putSlotFromAnyThread(NativeObject* obj, int kind, int32_t start, int32_t count) {
        putFromAnyThread(bufferSlot, SlotsEdge(obj, kind, start, count));
    }
    void putWholeCellFromMainThread(Cell* cell) {
        MOZ_ASSERT(cell->isTenured());
        putFromMainThread(bufferWholeCell, WholeCellEdges(cell));
    }

    /* Insert or update a single edge in the Relocatable buffer. */
    void putRelocatableValueFromAnyThread(JS::Value* valuep) {
        putFromAnyThread(bufferRelocVal, ValueEdge(valuep));
    }
    void removeRelocatableValueFromAnyThread(JS::Value* valuep) {
        unputFromAnyThread(bufferRelocVal, ValueEdge(valuep));
    }
    void putRelocatableCellFromAnyThread(Cell** cellp) {
        putFromAnyThread(bufferRelocCell, CellPtrEdge(cellp));
    }
    void removeRelocatableCellFromAnyThread(Cell** cellp) {
        unputFromAnyThread(bufferRelocCell, CellPtrEdge(cellp));
    }

    /* Insert an entry into the generic buffer. */
    template <typename T>
    void putGeneric(const T& t) { putFromAnyThread(bufferGeneric, t);}

    /* Insert or update a callback entry. */
    template <typename Key>
    void putCallback(void (*callback)(JSTracer* trc, Key* key, void* data), Key* key, void* data) {
        putFromAnyThread(bufferGeneric, CallbackRef<Key>(callback, key, data));
    }

    /* Methods to mark the source of all edges in the store buffer. */
    void markAll(JSTracer* trc);
    void markValues(JSTracer* trc)            { bufferVal.mark(this, trc); }
    void markCells(JSTracer* trc)             { bufferCell.mark(this, trc); }
    void markSlots(JSTracer* trc)             { bufferSlot.mark(this, trc); }
    void markWholeCells(JSTracer* trc)        { bufferWholeCell.mark(this, trc); }
    void markRelocatableValues(JSTracer* trc) { bufferRelocVal.mark(this, trc); }
    void markRelocatableCells(JSTracer* trc)  { bufferRelocCell.mark(this, trc); }
    void markGenericEntries(JSTracer* trc)    { bufferGeneric.mark(this, trc); }

    /* For use by our owned buffers and for testing. */
    void setAboutToOverflow();

    /* For jit access to the raw buffer. */
    void oolSinkStoresForWholeCellBuffer() { bufferWholeCell.sinkStores(this); }
    void* addressOfWholeCellBufferPointer() const { return (void*)&bufferWholeCell.insert_; }
    void* addressOfWholeCellBufferEnd() const {
        return (void*)(bufferWholeCell.buffer_ + bufferWholeCell.NumBufferEntries);
    }

    void addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf, JS::GCSizes* sizes);
};

} /* namespace gc */
} /* namespace js */

#endif /* gc_StoreBuffer_h */
