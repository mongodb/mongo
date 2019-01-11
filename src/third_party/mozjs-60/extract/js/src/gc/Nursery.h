/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Nursery_h
#define gc_Nursery_h

#include "mozilla/EnumeratedArray.h"
#include "mozilla/TimeStamp.h"

#include "js/Class.h"
#include "js/HeapAPI.h"
#include "js/TracingAPI.h"
#include "js/TypeDecls.h"
#include "js/Vector.h"

#define FOR_EACH_NURSERY_PROFILE_TIME(_)                                      \
   /* Key                       Header text */                                \
    _(Total,                    "total")                                      \
    _(CancelIonCompilations,    "canIon")                                     \
    _(TraceValues,              "mkVals")                                     \
    _(TraceCells,               "mkClls")                                     \
    _(TraceSlots,               "mkSlts")                                     \
    _(TraceWholeCells,          "mcWCll")                                     \
    _(TraceGenericEntries,      "mkGnrc")                                     \
    _(CheckHashTables,          "ckTbls")                                     \
    _(MarkRuntime,              "mkRntm")                                     \
    _(MarkDebugger,             "mkDbgr")                                     \
    _(SweepCaches,              "swpCch")                                     \
    _(CollectToFP,              "collct")                                     \
    _(ObjectsTenuredCallback,   "tenCB")                                      \
    _(Sweep,                    "sweep")                                      \
    _(UpdateJitActivations,     "updtIn")                                     \
    _(FreeMallocedBuffers,      "frSlts")                                     \
    _(ClearStoreBuffer,         "clrSB")                                      \
    _(ClearNursery,             "clear")                                      \
    _(Pretenure,                "pretnr")

template<typename T> class SharedMem;

namespace js {

class AutoLockGCBgAlloc;
class ObjectElements;
class PlainObject;
class NativeObject;
class Nursery;
struct NurseryChunk;
class HeapSlot;
class ZoneGroup;
class JSONPrinter;

void SetGCZeal(JSRuntime*, uint8_t, uint32_t);

namespace gc {
class AutoMaybeStartBackgroundAllocation;
struct Cell;
class MinorCollectionTracer;
class RelocationOverlay;
struct TenureCountCache;
enum class AllocKind : uint8_t;
class TenuredCell;
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

    // These lists are threaded through the Nursery using the space from
    // already moved things. The lists are used to fix up the moved things and
    // to find things held live by intra-Nursery pointers.
    gc::RelocationOverlay* objHead;
    gc::RelocationOverlay** objTail;
    gc::RelocationOverlay* stringHead;
    gc::RelocationOverlay** stringTail;

    TenuringTracer(JSRuntime* rt, Nursery* nursery);

  public:
    Nursery& nursery() { return nursery_; }

    template <typename T> void traverse(T** thingp);
    template <typename T> void traverse(T* thingp);

    // The store buffers need to be able to call these directly.
    void traceObject(JSObject* src);
    void traceObjectSlots(NativeObject* nobj, uint32_t start, uint32_t length);
    void traceSlots(JS::Value* vp, uint32_t nslots);
    void traceString(JSString* src);

  private:
    inline void insertIntoObjectFixupList(gc::RelocationOverlay* entry);
    inline void insertIntoStringFixupList(gc::RelocationOverlay* entry);
    template <typename T>
    inline T* allocTenured(JS::Zone* zone, gc::AllocKind kind);

    inline JSObject* movePlainObjectToTenured(PlainObject* src);
    JSObject* moveToTenuredSlow(JSObject* src);
    JSString* moveToTenured(JSString* src);

    size_t moveElementsToTenured(NativeObject* dst, NativeObject* src, gc::AllocKind dstKind);
    size_t moveSlotsToTenured(NativeObject* dst, NativeObject* src);
    size_t moveStringToTenured(JSString* dst, JSString* src, gc::AllocKind dstKind);

    void traceSlots(JS::Value* vp, JS::Value* end);
};

/*
 * Classes with JSCLASS_SKIP_NURSERY_FINALIZE or Wrapper classes with
 * CROSS_COMPARTMENT flags will not have their finalizer called if they are
 * nursery allocated and not promoted to the tenured heap. The finalizers for
 * these classes must do nothing except free data which was allocated via
 * Nursery::allocateBuffer.
 */
inline bool
CanNurseryAllocateFinalizedClass(const js::Class* const clasp)
{
    MOZ_ASSERT(clasp->hasFinalize());
    return clasp->flags & JSCLASS_SKIP_NURSERY_FINALIZE;
}

class Nursery
{
  public:
    static const size_t Alignment = gc::ChunkSize;
    static const size_t ChunkShift = gc::ChunkShift;

    struct alignas(gc::CellAlignBytes) CellAlignedByte {
        char byte;
    };

    struct StringLayout {
        JS::Zone* zone;
        CellAlignedByte cell;
    };

    explicit Nursery(JSRuntime* rt);
    ~Nursery();

    MOZ_MUST_USE bool init(uint32_t maxNurseryBytes, AutoLockGCBgAlloc& lock);

    unsigned chunkCountLimit() const { return chunkCountLimit_; }

    // Number of allocated (ready to use) chunks.
    unsigned allocatedChunkCount() const { return chunks_.length(); }

    // Total number of chunks and the capacity of the nursery. Chunks will be
    // lazilly allocated and added to the chunks array up to this limit, after
    // that the nursery must be collected, this limit may be raised during
    // collection.
    unsigned maxChunkCount() const { return maxChunkCount_; }

    bool exists() const { return chunkCountLimit() != 0; }

    void enable();
    void disable();
    bool isEnabled() const { return maxChunkCount() != 0; }

    void enableStrings();
    void disableStrings();
    bool canAllocateStrings() const { return canAllocateStrings_; }

    /* Return true if no allocations have been made since the last collection. */
    bool isEmpty() const;

    /*
     * Check whether an arbitrary pointer is within the nursery. This is
     * slower than IsInsideNursery(Cell*), but works on all types of pointers.
     */
    MOZ_ALWAYS_INLINE bool isInside(gc::Cell* cellp) const = delete;
    MOZ_ALWAYS_INLINE bool isInside(const void* p) const {
        for (auto chunk : chunks_) {
            if (uintptr_t(p) - uintptr_t(chunk) < gc::ChunkSize)
                return true;
        }
        return false;
    }

    template<typename T>
    inline bool isInside(const SharedMem<T>& p) const;

    /*
     * Allocate and return a pointer to a new GC object with its |slots|
     * pointer pre-filled. Returns nullptr if the Nursery is full.
     */
    JSObject* allocateObject(JSContext* cx, size_t size, size_t numDynamic, const js::Class* clasp);

    /*
     * Allocate and return a pointer to a new string. Returns nullptr if the
     * Nursery is full.
     */
    gc::Cell* allocateString(JS::Zone* zone, size_t size, gc::AllocKind kind);

    /*
     * String zones are stored just before the string in nursery memory.
     */
    static JS::Zone* getStringZone(const JSString* str) {
#ifdef DEBUG
        auto cell = reinterpret_cast<const js::gc::Cell*>(str); // JSString type is incomplete here
        MOZ_ASSERT(js::gc::IsInsideNursery(cell), "getStringZone must be passed a nursery string");
#endif

        auto layout = reinterpret_cast<const uint8_t*>(str) - offsetof(StringLayout, cell);
        return reinterpret_cast<const StringLayout*>(layout)->zone;
    }

    static size_t stringHeaderSize() {
        return offsetof(StringLayout, cell);
    }

    /* Allocate a buffer for a given zone, using the nursery if possible. */
    void* allocateBuffer(JS::Zone* zone, size_t nbytes);

    /*
     * Allocate a buffer for a given object, using the nursery if possible and
     * obj is in the nursery.
     */
    void* allocateBuffer(JSObject* obj, size_t nbytes);

    /*
     * Allocate a buffer for a given object, always using the nursery if obj is
     * in the nursery. The requested size must be less than or equal to
     * MaxNurseryBufferSize.
     */
    void* allocateBufferSameLocation(JSObject* obj, size_t nbytes);

    /* Resize an existing object buffer. */
    void* reallocateBuffer(JSObject* obj, void* oldBuffer,
                           size_t oldBytes, size_t newBytes);

    /* Free an object buffer. */
    void freeBuffer(void* buffer);

    /* The maximum number of bytes allowed to reside in nursery buffers. */
    static const size_t MaxNurseryBufferSize = 1024;

    /* Do a minor collection. */
    void collect(JS::gcreason::Reason reason);

    /*
     * If the thing at |*ref| in the Nursery has been forwarded, set |*ref| to
     * the new location and return true. Otherwise return false and leave
     * |*ref| unset.
     */
    MOZ_ALWAYS_INLINE MOZ_MUST_USE static bool getForwardedPointer(js::gc::Cell** ref);

    /* Forward a slots/elements pointer stored in an Ion frame. */
    void forwardBufferPointer(HeapSlot** pSlotsElems);

    inline void maybeSetForwardingPointer(JSTracer* trc, void* oldData, void* newData, bool direct);
    inline void setForwardingPointerWhileTenuring(void* oldData, void* newData, bool direct);

    /*
     * Register a malloced buffer that is held by a nursery object, which
     * should be freed at the end of a minor GC. Buffers are unregistered when
     * their owning objects are tenured.
     */
    bool registerMallocedBuffer(void* buffer);

    /* Mark a malloced buffer as no longer needing to be freed. */
    void removeMallocedBuffer(void* buffer) {
        mallocedBuffers.remove(buffer);
    }

    void waitBackgroundFreeEnd();

    MOZ_MUST_USE bool addedUniqueIdToCell(gc::Cell* cell) {
        MOZ_ASSERT(IsInsideNursery(cell));
        MOZ_ASSERT(isEnabled());
        return cellsWithUid_.append(cell);
    }

    MOZ_MUST_USE bool queueDictionaryModeObjectToSweep(NativeObject* obj);

    size_t sizeOfHeapCommitted() const {
        return allocatedChunkCount() * gc::ChunkSize;
    }
    size_t sizeOfMallocedBuffers(mozilla::MallocSizeOf mallocSizeOf) const {
        if (!mallocedBuffers.initialized())
            return 0;
        size_t total = 0;
        for (MallocedBuffersSet::Range r = mallocedBuffers.all(); !r.empty(); r.popFront())
            total += mallocSizeOf(r.front());
        total += mallocedBuffers.sizeOfExcludingThis(mallocSizeOf);
        return total;
    }

    // The number of bytes from the start position to the end of the nursery.
    // pass maxChunkCount(), allocatedChunkCount() or chunkCountLimit()
    // to calculate the nursery size, current lazy-allocated size or nursery
    // limit respectively.
    size_t spaceToEnd(unsigned chunkCount) const;

    // Free space remaining, not counting chunk trailers.
    MOZ_ALWAYS_INLINE size_t freeSpace() const {
        MOZ_ASSERT(currentEnd_ - position_ <= NurseryChunkUsableSize);
        return (currentEnd_ - position_) +
               (maxChunkCount() - currentChunk_ - 1) * NurseryChunkUsableSize;
    }

#ifdef JS_GC_ZEAL
    void enterZealMode();
    void leaveZealMode();
#endif

    /* Write profile time JSON on JSONPrinter. */
    void renderProfileJSON(JSONPrinter& json) const;

    /* Print header line for profile times. */
    static void printProfileHeader();

    /* Print total profile times on shutdown. */
    void printTotalProfileTimes();

    void* addressOfCurrentEnd() const { return (void*)&currentEnd_; }
    void* addressOfPosition() const { return (void*)&position_; }
    void* addressOfCurrentStringEnd() const { return (void*)&currentStringEnd_; }

    void requestMinorGC(JS::gcreason::Reason reason) const;

    bool minorGCRequested() const { return minorGCTriggerReason_ != JS::gcreason::NO_REASON; }
    JS::gcreason::Reason minorGCTriggerReason() const { return minorGCTriggerReason_; }
    void clearMinorGCRequest() { minorGCTriggerReason_ = JS::gcreason::NO_REASON; }

    bool needIdleTimeCollection() const {
        return minorGCRequested() ||
               (freeSpace() < kIdleTimeCollectionThreshold);
    }

    bool enableProfiling() const { return enableProfiling_; }

  private:
    /* The amount of space in the mapped nursery available to allocations. */
    static const size_t NurseryChunkUsableSize = gc::ChunkSize - gc::ChunkTrailerSize;

    /* Attemp to run a minor GC in the idle time if the free space falls below this threshold. */
    static constexpr size_t kIdleTimeCollectionThreshold = NurseryChunkUsableSize / 4;

    JSRuntime* runtime_;

    /* Vector of allocated chunks to allocate from. */
    Vector<NurseryChunk*, 0, SystemAllocPolicy> chunks_;

    /* Pointer to the first unallocated byte in the nursery. */
    uintptr_t position_;

    /* Pointer to the logical start of the Nursery. */
    unsigned currentStartChunk_;
    uintptr_t currentStartPosition_;

    /* Pointer to the last byte of space in the current chunk. */
    uintptr_t currentEnd_;

    /*
     * Pointer to the last byte of space in the current chunk, or nullptr if we
     * are not allocating strings in the nursery.
     */
    uintptr_t currentStringEnd_;

    /* The index of the chunk that is currently being allocated from. */
    unsigned currentChunk_;

    /*
     * The nursery may grow the chunks_ vector up to this size without a
     * collection.  This allows the nursery to grow lazilly.  This limit may
     * change during maybeResizeNursery() each collection.
     */
    unsigned maxChunkCount_;

    /*
     * This limit is fixed by configuration.  It represents the maximum size
     * the nursery is permitted to tune itself to in maybeResizeNursery();
     */
    unsigned chunkCountLimit_;

    mozilla::TimeDuration timeInChunkAlloc_;

    /* Promotion rate for the previous minor collection. */
    float previousPromotionRate_;

    /* Report minor collections taking at least this long, if enabled. */
    mozilla::TimeDuration profileThreshold_;
    bool enableProfiling_;

    /* Whether we will nursery-allocate strings. */
    bool canAllocateStrings_;

    /* Report ObjectGroups with at least this many instances tenured. */
    int64_t reportTenurings_;

    /*
     * Whether and why a collection of this nursery has been requested. This is
     * mutable as it is set by the store buffer, which otherwise cannot modify
     * anything in the nursery.
     */
    mutable JS::gcreason::Reason minorGCTriggerReason_;

    /* Profiling data. */

    enum class ProfileKey
    {
#define DEFINE_TIME_KEY(name, text)                                           \
        name,
        FOR_EACH_NURSERY_PROFILE_TIME(DEFINE_TIME_KEY)
#undef DEFINE_TIME_KEY
        KeyCount
    };

    using ProfileTimes =
        mozilla::EnumeratedArray<ProfileKey, ProfileKey::KeyCount, mozilla::TimeStamp>;
    using ProfileDurations =
        mozilla::EnumeratedArray<ProfileKey, ProfileKey::KeyCount, mozilla::TimeDuration>;

    ProfileTimes startTimes_;
    ProfileDurations profileDurations_;
    ProfileDurations totalDurations_;
    uint64_t minorGcCount_;

    /*
     * This data is initialised only if the nursery is enabled and after at
     * least one call to Nursery::collect()
     */
    struct {
        JS::gcreason::Reason reason;
        size_t nurseryCapacity;
        size_t nurseryLazyCapacity;
        size_t nurseryUsedBytes;
        size_t tenuredBytes;
    } previousGC;

    /*
     * Calculate the promotion rate of the most recent minor GC.
     * The valid_for_tenuring parameter is used to return whether this
     * promotion rate is accurate enough (the nursery was full enough) to be
     * used for tenuring and other decisions.
     *
     * Must only be called if the previousGC data is initialised.
     */
    float
    calcPromotionRate(bool *validForTenuring) const;

    /*
     * The set of externally malloced buffers potentially kept live by objects
     * stored in the nursery. Any external buffers that do not belong to a
     * tenured thing at the end of a minor GC must be freed.
     */
    typedef HashSet<void*, PointerHasher<void*>, SystemAllocPolicy> MallocedBuffersSet;
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
    typedef HashMap<void*, void*, PointerHasher<void*>, SystemAllocPolicy> ForwardedBufferMap;
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
    using CellsWithUniqueIdVector = Vector<gc::Cell*, 8, SystemAllocPolicy>;
    CellsWithUniqueIdVector cellsWithUid_;

    using NativeObjectVector = Vector<NativeObject*, 0, SystemAllocPolicy>;
    NativeObjectVector dictionaryModeObjects_;

#ifdef JS_GC_ZEAL
    struct Canary;
    Canary* lastCanary_;
#endif

    NurseryChunk& chunk(unsigned index) const {
        return *chunks_[index];
    }

    void setCurrentChunk(unsigned chunkno);
    void setStartPosition();

    /*
     * Allocate the next chunk, or the first chunk for initialization.
     * Callers will probably want to call setCurrentChunk(0) next.
     */
    MOZ_MUST_USE bool allocateNextChunk(unsigned chunkno,
        AutoLockGCBgAlloc& lock);

    MOZ_ALWAYS_INLINE uintptr_t currentEnd() const;

    uintptr_t position() const { return position_; }

    JSRuntime* runtime() const { return runtime_; }

    /* Common internal allocator function. */
    void* allocate(size_t size);

    void doCollection(JS::gcreason::Reason reason,
                        gc::TenureCountCache& tenureCounts);

    /*
     * Move the object at |src| in the Nursery to an already-allocated cell
     * |dst| in Tenured.
     */
    void collectToFixedPoint(TenuringTracer& trc, gc::TenureCountCache& tenureCounts);

    /* Handle relocation of slots/elements pointers stored in Ion frames. */
    inline void setForwardingPointer(void* oldData, void* newData, bool direct);

    inline void setDirectForwardingPointer(void* oldData, void* newData);
    void setIndirectForwardingPointer(void* oldData, void* newData);

    inline void setSlotsForwardingPointer(HeapSlot* oldSlots, HeapSlot* newSlots, uint32_t nslots);
    inline void setElementsForwardingPointer(ObjectElements* oldHeader, ObjectElements* newHeader,
                                             uint32_t capacity);

    /* Free malloced pointers owned by freed things in the nursery. */
    void freeMallocedBuffers();

    /*
     * Updates pointers to nursery objects that have been tenured and discards
     * pointers to objects that have been freed.
     */
    void sweep(JSTracer* trc);

    /*
     * Frees all non-live nursery-allocated things at the end of a minor
     * collection.
     */
    void clear();

    void sweepDictionaryModeObjects();

    /* Change the allocable space provided by the nursery. */
    void maybeResizeNursery(JS::gcreason::Reason reason);
    void growAllocableSpace();
    void shrinkAllocableSpace(unsigned newCount);
    void minimizeAllocableSpace();

    // Free the chunks starting at firstFreeChunk until the end of the chunks
    // vector. Shrinks the vector but does not update maxChunkCount().
    void freeChunksFrom(unsigned firstFreeChunk);

    /* Profile recording and printing. */
    void maybeClearProfileDurations();
    void startProfile(ProfileKey key);
    void endProfile(ProfileKey key);
    static void printProfileDurations(const ProfileDurations& times);

    friend class TenuringTracer;
    friend class gc::MinorCollectionTracer;
    friend class jit::MacroAssembler;
    friend struct NurseryChunk;
};

} /* namespace js */

#endif /* gc_Nursery_h */
