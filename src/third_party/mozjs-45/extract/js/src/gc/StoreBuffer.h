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
    virtual void trace(JSTracer* trc) = 0;
    bool maybeInRememberedSet(const Nursery&) const { return true; }
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
         * A one element cache in front of the canonical set to speed up
         * temporary instances of RelocatablePtr.
         */
        T last_;

        /* Maximum number of entries before we request a minor GC. */
        const static size_t MaxEntries = 48 * 1024 / sizeof(T);

        explicit MonoTypeBuffer() : last_(T()) {}
        ~MonoTypeBuffer() { stores_.finish(); }

        bool init() {
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
                owner->setAboutToOverflow();
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
                owner->setAboutToOverflow();
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

        void trace(TenuringTracer& mover) const;

        CellPtrEdge tagged() const { return CellPtrEdge((Cell**)(uintptr_t(edge) | 1)); }
        CellPtrEdge untagged() const { return CellPtrEdge((Cell**)(uintptr_t(edge) & ~1)); }
        bool isTagged() const { return bool(uintptr_t(edge) & 1); }

        explicit operator bool() const { return edge != nullptr; }

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

        void trace(TenuringTracer& mover) const;

        ValueEdge tagged() const { return ValueEdge((JS::Value*)(uintptr_t(edge) | 1)); }
        ValueEdge untagged() const { return ValueEdge((JS::Value*)(uintptr_t(edge) & ~1)); }
        bool isTagged() const { return bool(uintptr_t(edge) & 1); }

        explicit operator bool() const { return edge != nullptr; }

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

        void trace(TenuringTracer& mover) const;

        explicit operator bool() const { return objectAndKind_ != 0; }

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

        void trace(TenuringTracer& mover) const;

        explicit operator bool() const { return edge != nullptr; }

        typedef PointerEdgeHasher<WholeCellEdges> Hasher;
    };

    template <typename Key>
    struct CallbackRef : public BufferableRef
    {
        typedef void (*TraceCallback)(JSTracer* trc, Key* key, void* data);

        CallbackRef(TraceCallback cb, Key* k, void* d) : callback(cb), key(k), data(d) {}

        virtual void trace(JSTracer* trc) {
            callback(trc, key, data);
        }

      private:
        TraceCallback callback;
        Key* key;
        void* data;
    };

    template <typename Buffer, typename Edge>
    void unput(Buffer& buffer, const Edge& edge) {
        MOZ_ASSERT(!JS::shadow::Runtime::asShadowRuntime(runtime_)->isHeapBusy());
        MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime_));
        if (!isEnabled())
            return;
        mozilla::ReentrancyGuard g(*this);
        buffer.unput(this, edge);
    }

    template <typename Buffer, typename Edge>
    void put(Buffer& buffer, const Edge& edge) {
        MOZ_ASSERT(!JS::shadow::Runtime::asShadowRuntime(runtime_)->isHeapBusy());
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
    MonoTypeBuffer<WholeCellEdges> bufferWholeCell;
    GenericBuffer bufferGeneric;
    bool cancelIonCompilations_;

    JSRuntime* runtime_;
    const Nursery& nursery_;

    bool aboutToOverflow_;
    bool enabled_;
    mozilla::DebugOnly<bool> mEntered; /* For ReentrancyGuard. */

  public:
    explicit StoreBuffer(JSRuntime* rt, const Nursery& nursery)
      : bufferVal(), bufferCell(), bufferSlot(), bufferWholeCell(), bufferGeneric(),
        cancelIonCompilations_(false), runtime_(rt), nursery_(nursery), aboutToOverflow_(false),
        enabled_(false), mEntered(false)
    {
    }

    bool enable();
    void disable();
    bool isEnabled() const { return enabled_; }

    bool clear();

    /* Get the overflowed status. */
    bool isAboutToOverflow() const { return aboutToOverflow_; }

    bool cancelIonCompilations() const { return cancelIonCompilations_; }

    /* Insert a single edge into the buffer/remembered set. */
    void putValue(JS::Value* vp) { put(bufferVal, ValueEdge(vp)); }
    void unputValue(JS::Value* vp) { unput(bufferVal, ValueEdge(vp)); }
    void putCell(Cell** cellp) { put(bufferCell, CellPtrEdge(cellp)); }
    void unputCell(Cell** cellp) { unput(bufferCell, CellPtrEdge(cellp)); }
    void putSlot(NativeObject* obj, int kind, int32_t start, int32_t count) {
        put(bufferSlot, SlotsEdge(obj, kind, start, count));
    }
    void putWholeCell(Cell* cell) {
        MOZ_ASSERT(cell->isTenured());
        put(bufferWholeCell, WholeCellEdges(cell));
    }

    /* Insert an entry into the generic buffer. */
    template <typename T>
    void putGeneric(const T& t) { put(bufferGeneric, t);}

    /* Insert or update a callback entry. */
    template <typename Key>
    void putCallback(void (*callback)(JSTracer* trc, Key* key, void* data), Key* key, void* data) {
        put(bufferGeneric, CallbackRef<Key>(callback, key, data));
    }

    void setShouldCancelIonCompilations() {
        cancelIonCompilations_ = true;
    }

    /* Methods to trace the source of all edges in the store buffer. */
    void traceValues(TenuringTracer& mover)            { bufferVal.trace(this, mover); }
    void traceCells(TenuringTracer& mover)             { bufferCell.trace(this, mover); }
    void traceSlots(TenuringTracer& mover)             { bufferSlot.trace(this, mover); }
    void traceWholeCells(TenuringTracer& mover)        { bufferWholeCell.trace(this, mover); }
    void traceGenericEntries(JSTracer *trc)            { bufferGeneric.trace(this, trc); }

    /* For use by our owned buffers and for testing. */
    void setAboutToOverflow();

    bool hasPostBarrierCallbacks() {
        return !bufferGeneric.isEmpty();
    }

    void addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf, JS::GCSizes* sizes);
};

} /* namespace gc */
} /* namespace js */

#endif /* gc_StoreBuffer_h */
