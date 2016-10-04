/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Nursery_h
#define gc_Nursery_h

#include "jsalloc.h"
#include "jspubtd.h"

#include "ds/BitArray.h"
#include "gc/Heap.h"
#include "gc/Memory.h"
#include "js/Class.h"
#include "js/GCAPI.h"
#include "js/HashTable.h"
#include "js/HeapAPI.h"
#include "js/Value.h"
#include "js/Vector.h"
#include "vm/SharedMem.h"

namespace JS {
struct Zone;
} // namespace JS

namespace js {

class ObjectElements;
class NativeObject;
class Nursery;
class HeapSlot;
class ObjectGroup;

void SetGCZeal(JSRuntime*, uint8_t, uint32_t);

namespace gc {
struct Cell;
class MinorCollectionTracer;
class RelocationOverlay;
struct TenureCountCache;
} /* namespace gc */

namespace jit {
class MacroAssembler;
} // namespace jit

class TenuringTracer : public JSTracer
{
    friend class Nursery;
    Nursery& nursery_;

    // Amount of data moved to the tenured generation during collection.
    size_t tenuredSize;

    // This list is threaded through the Nursery using the space from already
    // moved things. The list is used to fix up the moved things and to find
    // things held live by intra-Nursery pointers.
    gc::RelocationOverlay* head;
    gc::RelocationOverlay** tail;

    TenuringTracer(JSRuntime* rt, Nursery* nursery);

  public:
    const Nursery& nursery() const { return nursery_; }

    // Returns true if the pointer was updated.
    template <typename T> void traverse(T** thingp);
    template <typename T> void traverse(T* thingp);

    void insertIntoFixupList(gc::RelocationOverlay* entry);

    // The store buffers need to be able to call these directly.
    void traceObject(JSObject* src);
    void traceObjectSlots(NativeObject* nobj, uint32_t start, uint32_t length);
    void traceSlots(JS::Value* vp, uint32_t nslots) { traceSlots(vp, vp + nslots); }

  private:
    Nursery& nursery() { return nursery_; }

    JSObject* moveToTenured(JSObject* src);
    size_t moveObjectToTenured(JSObject* dst, JSObject* src, gc::AllocKind dstKind);
    size_t moveElementsToTenured(NativeObject* dst, NativeObject* src, gc::AllocKind dstKind);
    size_t moveSlotsToTenured(NativeObject* dst, NativeObject* src, gc::AllocKind dstKind);

    void traceSlots(JS::Value* vp, JS::Value* end);
};

class Nursery
{
  public:
    static const size_t Alignment = gc::ChunkSize;
    static const size_t ChunkShift = gc::ChunkShift;

    explicit Nursery(JSRuntime* rt)
      : runtime_(rt),
        position_(0),
        currentStart_(0),
        currentEnd_(0),
        heapStart_(0),
        heapEnd_(0),
        currentChunk_(0),
        numActiveChunks_(0),
        numNurseryChunks_(0),
        profileThreshold_(0),
        enableProfiling_(false),
        freeMallocedBuffersTask(nullptr)
    {}
    ~Nursery();

    bool init(uint32_t maxNurseryBytes);

    bool exists() const { return numNurseryChunks_ != 0; }
    size_t numChunks() const { return numNurseryChunks_; }
    size_t nurserySize() const { return numNurseryChunks_ << ChunkShift; }

    void enable();
    void disable();
    bool isEnabled() const { return numActiveChunks_ != 0; }

    /* Return true if no allocations have been made since the last collection. */
    bool isEmpty() const;

    /*
     * Check whether an arbitrary pointer is within the nursery. This is
     * slower than IsInsideNursery(Cell*), but works on all types of pointers.
     */
    MOZ_ALWAYS_INLINE bool isInside(gc::Cell* cellp) const = delete;
    MOZ_ALWAYS_INLINE bool isInside(const void* p) const {
        return uintptr_t(p) >= heapStart_ && uintptr_t(p) < heapEnd_;
    }
    template<typename T>
    bool isInside(const SharedMem<T>& p) const {
        return isInside(p.unwrap(/*safe - used for value in comparison above*/));
    }

    /*
     * Allocate and return a pointer to a new GC object with its |slots|
     * pointer pre-filled. Returns nullptr if the Nursery is full.
     */
    JSObject* allocateObject(JSContext* cx, size_t size, size_t numDynamic, const js::Class* clasp);

    /* Allocate a buffer for a given zone, using the nursery if possible. */
    void* allocateBuffer(JS::Zone* zone, uint32_t nbytes);

    /*
     * Allocate a buffer for a given object, using the nursery if possible and
     * obj is in the nursery.
     */
    void* allocateBuffer(JSObject* obj, uint32_t nbytes);

    /* Resize an existing object buffer. */
    void* reallocateBuffer(JSObject* obj, void* oldBuffer,
                           uint32_t oldBytes, uint32_t newBytes);

    /* Free an object buffer. */
    void freeBuffer(void* buffer);

    typedef Vector<ObjectGroup*, 0, SystemAllocPolicy> ObjectGroupList;

    /*
     * Do a minor collection, optionally specifying a list to store groups which
     * should be pretenured afterwards.
     */
    void collect(JSRuntime* rt, JS::gcreason::Reason reason, ObjectGroupList* pretenureGroups);

    /*
     * Check if the thing at |*ref| in the Nursery has been forwarded. If so,
     * sets |*ref| to the new location of the object and returns true. Otherwise
     * returns false and leaves |*ref| unset.
     */
    MOZ_ALWAYS_INLINE bool getForwardedPointer(JSObject** ref) const;

    /* Forward a slots/elements pointer stored in an Ion frame. */
    void forwardBufferPointer(HeapSlot** pSlotsElems);

    void maybeSetForwardingPointer(JSTracer* trc, void* oldData, void* newData, bool direct) {
        if (trc->isTenuringTracer() && isInside(oldData))
            setForwardingPointer(oldData, newData, direct);
    }

    /* Mark a malloced buffer as no longer needing to be freed. */
    void removeMallocedBuffer(void* buffer) {
        mallocedBuffers.remove(buffer);
    }

    void waitBackgroundFreeEnd();

    bool addedUniqueIdToCell(gc::Cell* cell) {
        if (!IsInsideNursery(cell) || !isEnabled())
            return true;
        MOZ_ASSERT(cellsWithUid_.initialized());
        MOZ_ASSERT(!cellsWithUid_.has(cell));
        return cellsWithUid_.put(cell);
    }

    size_t sizeOfHeapCommitted() const {
        return numActiveChunks_ * gc::ChunkSize;
    }
    size_t sizeOfHeapDecommitted() const {
        return (numNurseryChunks_ - numActiveChunks_) * gc::ChunkSize;
    }
    size_t sizeOfMallocedBuffers(mozilla::MallocSizeOf mallocSizeOf) const {
        size_t total = 0;
        for (MallocedBuffersSet::Range r = mallocedBuffers.all(); !r.empty(); r.popFront())
            total += mallocSizeOf(r.front());
        total += mallocedBuffers.sizeOfExcludingThis(mallocSizeOf);
        return total;
    }

    MOZ_ALWAYS_INLINE uintptr_t start() const {
        return heapStart_;
    }

    MOZ_ALWAYS_INLINE uintptr_t heapEnd() const {
        return heapEnd_;
    }

#ifdef JS_GC_ZEAL
    void enterZealMode();
    void leaveZealMode();
#endif

  private:
    /*
     * The start and end pointers are stored under the runtime so that we can
     * inline the isInsideNursery check into embedder code. Use the start()
     * and heapEnd() functions to access these values.
     */
    JSRuntime* runtime_;

    /* Pointer to the first unallocated byte in the nursery. */
    uintptr_t position_;

    /* Pointer to the logical start of the Nursery. */
    uintptr_t currentStart_;

    /* Pointer to the last byte of space in the current chunk. */
    uintptr_t currentEnd_;

    /* Pointer to first and last address of the total nursery allocation. */
    uintptr_t heapStart_;
    uintptr_t heapEnd_;

    /* The index of the chunk that is currently being allocated from. */
    int currentChunk_;

    /* The index after the last chunk that we will allocate from. */
    int numActiveChunks_;

    /* Number of chunks allocated for the nursery. */
    int numNurseryChunks_;

    /* Report minor collections taking more than this many us, if enabled. */
    int64_t profileThreshold_;
    bool enableProfiling_;

    /*
     * The set of externally malloced buffers potentially kept live by objects
     * stored in the nursery. Any external buffers that do not belong to a
     * tenured thing at the end of a minor GC must be freed.
     */
    typedef HashSet<void*, PointerHasher<void*, 3>, SystemAllocPolicy> MallocedBuffersSet;
    MallocedBuffersSet mallocedBuffers;

    /* A task structure used to free the malloced bufers on a background thread. */
    struct FreeMallocedBuffersTask;
    FreeMallocedBuffersTask* freeMallocedBuffersTask;

    /*
     * During a collection most hoisted slot and element buffers indicate their
     * new location with a forwarding pointer at the base. This does not work
     * for buffers whose length is less than pointer width, or when different
     * buffers might overlap each other. For these, an entry in the following
     * table is used.
     */
    typedef HashMap<void*, void*, PointerHasher<void*, 1>, SystemAllocPolicy> ForwardedBufferMap;
    ForwardedBufferMap forwardedBuffers;

    /*
     * When we assign a unique id to cell in the nursery, that almost always
     * means that the cell will be in a hash table, and thus, held live,
     * automatically moving the uid from the nursery to its new home in
     * tenured. It is possible, if rare, for an object that acquired a uid to
     * be dead before the next collection, in which case we need to know to
     * remove it when we sweep.
     *
     * Note: we store the pointers as Cell* here, resulting in an ugly cast in
     *       sweep. This is because this structure is used to help implement
     *       stable object hashing and we have to break the cycle somehow.
     */
    using CellsWithUniqueIdSet = HashSet<gc::Cell*, PointerHasher<gc::Cell*, 3>, SystemAllocPolicy>;
    CellsWithUniqueIdSet cellsWithUid_;

    /* The maximum number of bytes allowed to reside in nursery buffers. */
    static const size_t MaxNurseryBufferSize = 1024;

    /* The amount of space in the mapped nursery available to allocations. */
    static const size_t NurseryChunkUsableSize = gc::ChunkSize - sizeof(gc::ChunkTrailer);

    struct NurseryChunkLayout {
        char data[NurseryChunkUsableSize];
        gc::ChunkTrailer trailer;
        uintptr_t start() { return uintptr_t(&data); }
        uintptr_t end() { return uintptr_t(&trailer); }
    };
    static_assert(sizeof(NurseryChunkLayout) == gc::ChunkSize,
                  "Nursery chunk size must match gc::Chunk size.");
    NurseryChunkLayout& chunk(int index) const {
        MOZ_ASSERT(index < numNurseryChunks_);
        MOZ_ASSERT(start());
        return reinterpret_cast<NurseryChunkLayout*>(start())[index];
    }

    MOZ_ALWAYS_INLINE void initChunk(int chunkno) {
        gc::StoreBuffer* sb = JS::shadow::Runtime::asShadowRuntime(runtime())->gcStoreBufferPtr();
        new (&chunk(chunkno).trailer) gc::ChunkTrailer(runtime(), sb);
    }

    MOZ_ALWAYS_INLINE void setCurrentChunk(int chunkno) {
        MOZ_ASSERT(chunkno < numNurseryChunks_);
        MOZ_ASSERT(chunkno < numActiveChunks_);
        currentChunk_ = chunkno;
        position_ = chunk(chunkno).start();
        currentEnd_ = chunk(chunkno).end();
        initChunk(chunkno);
    }

    void updateDecommittedRegion();

    MOZ_ALWAYS_INLINE uintptr_t allocationEnd() const {
        MOZ_ASSERT(numActiveChunks_ > 0);
        return chunk(numActiveChunks_ - 1).end();
    }

    MOZ_ALWAYS_INLINE uintptr_t currentEnd() const {
        MOZ_ASSERT(runtime_);
        MOZ_ASSERT(currentEnd_ == chunk(currentChunk_).end());
        return currentEnd_;
    }
    void* addressOfCurrentEnd() const {
        MOZ_ASSERT(runtime_);
        return (void*)&currentEnd_;
    }

    uintptr_t position() const { return position_; }
    void* addressOfPosition() const { return (void*)&position_; }

    JSRuntime* runtime() const { return runtime_; }

    /* Allocates a new GC thing from the tenured generation during minor GC. */
    gc::TenuredCell* allocateFromTenured(JS::Zone* zone, gc::AllocKind thingKind);

    /* Common internal allocator function. */
    void* allocate(size_t size);

    /*
     * Move the object at |src| in the Nursery to an already-allocated cell
     * |dst| in Tenured.
     */
    void collectToFixedPoint(TenuringTracer& trc, gc::TenureCountCache& tenureCounts);

    /* Handle relocation of slots/elements pointers stored in Ion frames. */
    void setForwardingPointer(void* oldData, void* newData, bool direct);

    void setSlotsForwardingPointer(HeapSlot* oldSlots, HeapSlot* newSlots, uint32_t nslots);
    void setElementsForwardingPointer(ObjectElements* oldHeader, ObjectElements* newHeader,
                                      uint32_t nelems);

    /* Free malloced pointers owned by freed things in the nursery. */
    void freeMallocedBuffers();

    /*
     * Frees all non-live nursery-allocated things at the end of a minor
     * collection.
     */
    void sweep();

    /* Change the allocable space provided by the nursery. */
    void growAllocableSpace();
    void shrinkAllocableSpace();

    friend class TenuringTracer;
    friend class gc::MinorCollectionTracer;
    friend class jit::MacroAssembler;
};

} /* namespace js */

#endif /* gc_Nursery_h */
