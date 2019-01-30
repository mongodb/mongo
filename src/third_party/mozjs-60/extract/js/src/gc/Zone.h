/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Zone_h
#define gc_Zone_h

#include "mozilla/Atomics.h"
#include "mozilla/HashFunctions.h"

#include "gc/FindSCCs.h"
#include "gc/GCRuntime.h"
#include "js/GCHashTable.h"
#include "vm/MallocProvider.h"
#include "vm/RegExpShared.h"
#include "vm/Runtime.h"

namespace js {

class Debugger;

namespace jit {
class JitZone;
} // namespace jit

namespace gc {

struct ZoneComponentFinder : public ComponentFinder<JS::Zone, ZoneComponentFinder>
{
    ZoneComponentFinder(uintptr_t sl, JS::Zone* maybeAtomsZone)
      : ComponentFinder<JS::Zone, ZoneComponentFinder>(sl), maybeAtomsZone(maybeAtomsZone)
    {}

    JS::Zone* maybeAtomsZone;
};

struct UniqueIdGCPolicy {
    static bool needsSweep(Cell** cell, uint64_t* value);
};

// Maps a Cell* to a unique, 64bit id.
using UniqueIdMap = GCHashMap<Cell*,
                              uint64_t,
                              PointerHasher<Cell*>,
                              SystemAllocPolicy,
                              UniqueIdGCPolicy>;

extern uint64_t NextCellUniqueId(JSRuntime* rt);

template <typename T>
class ZoneCellIter;

} // namespace gc

class MOZ_NON_TEMPORARY_CLASS ExternalStringCache
{
    static const size_t NumEntries = 4;
    mozilla::Array<JSString*, NumEntries> entries_;

    ExternalStringCache(const ExternalStringCache&) = delete;
    void operator=(const ExternalStringCache&) = delete;

  public:
    ExternalStringCache() { purge(); }
    void purge() { mozilla::PodArrayZero(entries_); }

    MOZ_ALWAYS_INLINE JSString* lookup(const char16_t* chars, size_t len) const;
    MOZ_ALWAYS_INLINE void put(JSString* s);
};

class MOZ_NON_TEMPORARY_CLASS FunctionToStringCache
{
    struct Entry {
        JSScript* script;
        JSString* string;

        void set(JSScript* scriptArg, JSString* stringArg) {
            script = scriptArg;
            string = stringArg;
        }
    };
    static const size_t NumEntries = 2;
    mozilla::Array<Entry, NumEntries> entries_;

    FunctionToStringCache(const FunctionToStringCache&) = delete;
    void operator=(const FunctionToStringCache&) = delete;

  public:
    FunctionToStringCache() { purge(); }
    void purge() { mozilla::PodArrayZero(entries_); }

    MOZ_ALWAYS_INLINE JSString* lookup(JSScript* script) const;
    MOZ_ALWAYS_INLINE void put(JSScript* script, JSString* string);
};

} // namespace js

namespace JS {

// A zone is a collection of compartments. Every compartment belongs to exactly
// one zone. In Firefox, there is roughly one zone per tab along with a system
// zone for everything else. Zones mainly serve as boundaries for garbage
// collection. Unlike compartments, they have no special security properties.
//
// Every GC thing belongs to exactly one zone. GC things from the same zone but
// different compartments can share an arena (4k page). GC things from different
// zones cannot be stored in the same arena. The garbage collector is capable of
// collecting one zone at a time; it cannot collect at the granularity of
// compartments.
//
// GC things are tied to zones and compartments as follows:
//
// - JSObjects belong to a compartment and cannot be shared between
//   compartments. If an object needs to point to a JSObject in a different
//   compartment, regardless of zone, it must go through a cross-compartment
//   wrapper. Each compartment keeps track of its outgoing wrappers in a table.
//   JSObjects find their compartment via their ObjectGroup.
//
// - JSStrings do not belong to any particular compartment, but they do belong
//   to a zone. Thus, two different compartments in the same zone can point to a
//   JSString. When a string needs to be wrapped, we copy it if it's in a
//   different zone and do nothing if it's in the same zone. Thus, transferring
//   strings within a zone is very efficient.
//
// - Shapes and base shapes belong to a zone and are shared between compartments
//   in that zone where possible. Accessor shapes store getter and setter
//   JSObjects which belong to a single compartment, so these shapes and all
//   their descendants can't be shared with other compartments.
//
// - Scripts are also compartment-local and cannot be shared. A script points to
//   its compartment.
//
// - ObjectGroup and JitCode objects belong to a compartment and cannot be
//   shared. There is no mechanism to obtain the compartment from a JitCode
//   object.
//
// A zone remains alive as long as any GC things in the zone are alive. A
// compartment remains alive as long as any JSObjects, scripts, shapes, or base
// shapes within it are alive.
//
// We always guarantee that a zone has at least one live compartment by refusing
// to delete the last compartment in a live zone.
struct Zone : public JS::shadow::Zone,
              public js::gc::GraphNodeBase<JS::Zone>,
              public js::MallocProvider<JS::Zone>
{
    explicit Zone(JSRuntime* rt, js::ZoneGroup* group);
    ~Zone();
    MOZ_MUST_USE bool init(bool isSystem);
    void destroy(js::FreeOp *fop);

  private:
    js::ZoneGroup* const group_;
  public:
    js::ZoneGroup* group() const {
        return group_;
    }

    // For JIT use.
    static size_t offsetOfGroup() {
        return offsetof(Zone, group_);
    }

    void findOutgoingEdges(js::gc::ZoneComponentFinder& finder);

    void discardJitCode(js::FreeOp* fop, bool discardBaselineCode = true);

    void addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                size_t* typePool,
                                size_t* regexpZone,
                                size_t* jitZone,
                                size_t* baselineStubsOptimized,
                                size_t* cachedCFG,
                                size_t* uniqueIdMap,
                                size_t* shapeTables,
                                size_t* atomsMarkBitmaps);

    // Iterate over all cells in the zone. See the definition of ZoneCellIter
    // in gc/GC-inl.h for the possible arguments and documentation.
    template <typename T, typename... Args>
    js::gc::ZoneCellIter<T> cellIter(Args&&... args) {
        return js::gc::ZoneCellIter<T>(const_cast<Zone*>(this), mozilla::Forward<Args>(args)...);
    }

    MOZ_MUST_USE void* onOutOfMemory(js::AllocFunction allocFunc, size_t nbytes,
                                     void* reallocPtr = nullptr) {
        if (!js::CurrentThreadCanAccessRuntime(runtime_))
            return nullptr;
        return runtimeFromActiveCooperatingThread()->onOutOfMemory(allocFunc, nbytes, reallocPtr);
    }
    void reportAllocationOverflow() { js::ReportAllocationOverflow(nullptr); }

    void beginSweepTypes(bool releaseTypes);

    bool hasMarkedCompartments();

    void scheduleGC() { MOZ_ASSERT(!CurrentThreadIsHeapBusy()); gcScheduled_ = true; }
    void unscheduleGC() { gcScheduled_ = false; }
    bool isGCScheduled() { return gcScheduled_; }

    void setPreservingCode(bool preserving) { gcPreserveCode_ = preserving; }
    bool isPreservingCode() const { return gcPreserveCode_; }

    // Whether this zone can currently be collected. This doesn't take account
    // of AutoKeepAtoms for the atoms zone.
    bool canCollect();

    void changeGCState(GCState prev, GCState next) {
        MOZ_ASSERT(CurrentThreadIsHeapBusy());
        MOZ_ASSERT(gcState() == prev);
        MOZ_ASSERT_IF(next != NoGC, canCollect());
        gcState_ = next;
    }

    bool isCollecting() const {
        MOZ_ASSERT(js::CurrentThreadCanAccessRuntime(runtimeFromActiveCooperatingThread()));
        return isCollectingFromAnyThread();
    }

    bool isCollectingFromAnyThread() const {
        if (CurrentThreadIsHeapCollecting())
            return gcState_ != NoGC;
        else
            return needsIncrementalBarrier();
    }

    // If this returns true, all object tracing must be done with a GC marking
    // tracer.
    bool requireGCTracer() const {
        JSRuntime* rt = runtimeFromAnyThread();
        return CurrentThreadIsHeapMajorCollecting() && !rt->gc.isHeapCompacting() && gcState_ != NoGC;
    }

    bool shouldMarkInZone() const {
        return needsIncrementalBarrier() || isGCMarking();
    }

    // Get a number that is incremented whenever this zone is collected, and
    // possibly at other times too.
    uint64_t gcNumber();

    bool compileBarriers() const { return compileBarriers(needsIncrementalBarrier()); }
    bool compileBarriers(bool needsIncrementalBarrier) const {
        return needsIncrementalBarrier ||
               runtimeFromActiveCooperatingThread()->hasZealMode(js::gc::ZealMode::VerifierPre);
    }

    void setNeedsIncrementalBarrier(bool needs);
    const uint32_t* addressOfNeedsIncrementalBarrier() const { return &needsIncrementalBarrier_; }

    js::jit::JitZone* getJitZone(JSContext* cx) { return jitZone_ ? jitZone_ : createJitZone(cx); }
    js::jit::JitZone* jitZone() { return jitZone_; }

    bool isAtomsZone() const { return runtimeFromAnyThread()->isAtomsZone(this); }
    bool isSelfHostingZone() const { return runtimeFromAnyThread()->isSelfHostingZone(this); }

    void prepareForCompacting();

#ifdef DEBUG
    // For testing purposes, return the index of the sweep group which this zone
    // was swept in in the last GC.
    unsigned lastSweepGroupIndex() { return gcLastSweepGroupIndex; }
#endif

    void sweepBreakpoints(js::FreeOp* fop);
    void sweepUniqueIds();
    void sweepWeakMaps();
    void sweepCompartments(js::FreeOp* fop, bool keepAtleastOne, bool lastGC);

    using DebuggerVector = js::Vector<js::Debugger*, 0, js::SystemAllocPolicy>;

  private:
    js::ZoneGroupData<DebuggerVector*> debuggers;

    js::jit::JitZone* createJitZone(JSContext* cx);

    bool isQueuedForBackgroundSweep() {
        return isOnList();
    }

    // Side map for storing a unique ids for cells, independent of address.
    js::ZoneGroupOrGCTaskData<js::gc::UniqueIdMap> uniqueIds_;

    js::gc::UniqueIdMap& uniqueIds() { return uniqueIds_.ref(); }

  public:
    bool hasDebuggers() const { return debuggers && debuggers->length(); }
    DebuggerVector* getDebuggers() const { return debuggers; }
    DebuggerVector* getOrCreateDebuggers(JSContext* cx);

    void notifyObservingDebuggers();

    void clearTables();

    /*
     * When true, skip calling the metadata callback. We use this:
     * - to avoid invoking the callback recursively;
     * - to avoid observing lazy prototype setup (which confuses callbacks that
     *   want to use the types being set up!);
     * - to avoid attaching allocation stacks to allocation stack nodes, which
     *   is silly
     * And so on.
     */
    js::ZoneGroupData<bool> suppressAllocationMetadataBuilder;

    js::gc::ArenaLists arenas;

    js::TypeZone types;

  private:
    /* Live weakmaps in this zone. */
    js::ZoneGroupOrGCTaskData<mozilla::LinkedList<js::WeakMapBase>> gcWeakMapList_;
  public:
    mozilla::LinkedList<js::WeakMapBase>& gcWeakMapList() { return gcWeakMapList_.ref(); }

    typedef js::Vector<JSCompartment*, 1, js::SystemAllocPolicy> CompartmentVector;

  private:
    // The set of compartments in this zone.
    js::ActiveThreadOrGCTaskData<CompartmentVector> compartments_;
  public:
    CompartmentVector& compartments() { return compartments_.ref(); }

    // This zone's gray roots.
    typedef js::Vector<js::gc::Cell*, 0, js::SystemAllocPolicy> GrayRootVector;
  private:
    js::ZoneGroupOrGCTaskData<GrayRootVector> gcGrayRoots_;
  public:
    GrayRootVector& gcGrayRoots() { return gcGrayRoots_.ref(); }

    // This zone's weak edges found via graph traversal during marking,
    // preserved for re-scanning during sweeping.
    using WeakEdges = js::Vector<js::gc::TenuredCell**, 0, js::SystemAllocPolicy>;
  private:
    js::ZoneGroupOrGCTaskData<WeakEdges> gcWeakRefs_;
  public:
    WeakEdges& gcWeakRefs() { return gcWeakRefs_.ref(); }

  private:
    // List of non-ephemeron weak containers to sweep during beginSweepingSweepGroup.
    js::ZoneGroupOrGCTaskData<mozilla::LinkedList<detail::WeakCacheBase>> weakCaches_;
  public:
    mozilla::LinkedList<detail::WeakCacheBase>& weakCaches() { return weakCaches_.ref(); }
    void registerWeakCache(detail::WeakCacheBase* cachep) {
        weakCaches().insertBack(cachep);
    }

  private:
    /*
     * Mapping from not yet marked keys to a vector of all values that the key
     * maps to in any live weak map.
     */
    js::ZoneGroupOrGCTaskData<js::gc::WeakKeyTable> gcWeakKeys_;
  public:
    js::gc::WeakKeyTable& gcWeakKeys() { return gcWeakKeys_.ref(); }

  private:
    // A set of edges from this zone to other zones.
    //
    // This is used during GC while calculating sweep groups to record edges
    // that can't be determined by examining this zone by itself.
    js::ActiveThreadData<ZoneSet> gcSweepGroupEdges_;

  public:
    ZoneSet& gcSweepGroupEdges() { return gcSweepGroupEdges_.ref(); }

    // Keep track of all TypeDescr and related objects in this compartment.
    // This is used by the GC to trace them all first when compacting, since the
    // TypedObject trace hook may access these objects.
    //
    // There are no barriers here - the set contains only tenured objects so no
    // post-barrier is required, and these are weak references so no pre-barrier
    // is required.
    using TypeDescrObjectSet = js::GCHashSet<JSObject*,
                                             js::MovableCellHasher<JSObject*>,
                                             js::SystemAllocPolicy>;
  private:
    js::ZoneGroupData<JS::WeakCache<TypeDescrObjectSet>> typeDescrObjects_;

    // Malloc counter to measure memory pressure for GC scheduling. This
    // counter should be used only when it's not possible to know the size of
    // a free.
    js::gc::MemoryCounter gcMallocCounter;

    // Counter of JIT code executable memory for GC scheduling. Also imprecise,
    // since wasm can generate code that outlives a zone.
    js::gc::MemoryCounter jitCodeCounter;

    void updateMemoryCounter(js::gc::MemoryCounter& counter, size_t nbytes) {
        JSRuntime* rt = runtimeFromAnyThread();

        counter.update(nbytes);
        auto trigger = counter.shouldTriggerGC(rt->gc.tunables);
        if (MOZ_LIKELY(trigger == js::gc::NoTrigger) || trigger <= counter.triggered())
            return;

        if (!js::CurrentThreadCanAccessRuntime(rt))
            return;

        bool wouldInterruptGC = rt->gc.isIncrementalGCInProgress() && !isCollecting();
        if (wouldInterruptGC && !counter.shouldResetIncrementalGC(rt->gc.tunables))
            return;

        if (!rt->gc.triggerZoneGC(this, JS::gcreason::TOO_MUCH_MALLOC,
                                  counter.bytes(), counter.maxBytes()))
        {
            return;
        }

        counter.recordTrigger(trigger);
    }

  public:
    js::RegExpZone regExps;

    JS::WeakCache<TypeDescrObjectSet>& typeDescrObjects() { return typeDescrObjects_.ref(); }

    bool addTypeDescrObject(JSContext* cx, HandleObject obj);

    void setGCMaxMallocBytes(size_t value, const js::AutoLockGC& lock) {
        gcMallocCounter.setMax(value, lock);
    }
    void updateMallocCounter(size_t nbytes) {
        updateMemoryCounter(gcMallocCounter, nbytes);
    }
    void adoptMallocBytes(Zone* other) {
        gcMallocCounter.adopt(other->gcMallocCounter);
    }
    size_t GCMaxMallocBytes() const { return gcMallocCounter.maxBytes(); }
    size_t GCMallocBytes() const { return gcMallocCounter.bytes(); }

    void updateJitCodeMallocBytes(size_t nbytes) {
        updateMemoryCounter(jitCodeCounter, nbytes);
    }

    void updateAllGCMallocCountersOnGCStart() {
        gcMallocCounter.updateOnGCStart();
        jitCodeCounter.updateOnGCStart();
    }
    void updateAllGCMallocCountersOnGCEnd(const js::AutoLockGC& lock) {
        auto& gc = runtimeFromAnyThread()->gc;
        gcMallocCounter.updateOnGCEnd(gc.tunables, lock);
        jitCodeCounter.updateOnGCEnd(gc.tunables, lock);
    }
    js::gc::TriggerKind shouldTriggerGCForTooMuchMalloc() {
        auto& gc = runtimeFromAnyThread()->gc;
        return std::max(gcMallocCounter.shouldTriggerGC(gc.tunables),
                        jitCodeCounter.shouldTriggerGC(gc.tunables));
    }

  private:
    // Bitmap of atoms marked by this zone.
    js::ZoneGroupOrGCTaskData<js::SparseBitmap> markedAtoms_;

    // Set of atoms recently used by this Zone. Purged on GC.
    js::ZoneGroupOrGCTaskData<js::AtomSet> atomCache_;

    // Cache storing allocated external strings. Purged on GC.
    js::ZoneGroupOrGCTaskData<js::ExternalStringCache> externalStringCache_;

    // Cache for Function.prototype.toString. Purged on GC.
    js::ZoneGroupOrGCTaskData<js::FunctionToStringCache> functionToStringCache_;

  public:
    js::SparseBitmap& markedAtoms() { return markedAtoms_.ref(); }

    js::AtomSet& atomCache() { return atomCache_.ref(); }

    js::ExternalStringCache& externalStringCache() { return externalStringCache_.ref(); };

    js::FunctionToStringCache& functionToStringCache() { return functionToStringCache_.ref(); }

    // Track heap usage under this Zone.
    js::gc::HeapUsage usage;

    // Thresholds used to trigger GC.
    js::gc::ZoneHeapThreshold threshold;

    // Amount of data to allocate before triggering a new incremental slice for
    // the current GC.
    js::UnprotectedData<size_t> gcDelayBytes;

    js::ZoneGroupData<uint32_t> tenuredStrings;
    js::ZoneGroupData<bool> allocNurseryStrings;

  private:
    // Shared Shape property tree.
    js::ZoneGroupData<js::PropertyTree> propertyTree_;
  public:
    js::PropertyTree& propertyTree() { return propertyTree_.ref(); }

  private:
    // Set of all unowned base shapes in the Zone.
    js::ZoneGroupData<js::BaseShapeSet> baseShapes_;
  public:
    js::BaseShapeSet& baseShapes() { return baseShapes_.ref(); }

  private:
    // Set of initial shapes in the Zone. For certain prototypes -- namely,
    // those of various builtin classes -- there are two entries: one for a
    // lookup via TaggedProto, and one for a lookup via JSProtoKey. See
    // InitialShapeProto.
    js::ZoneGroupData<js::InitialShapeSet> initialShapes_;
  public:
    js::InitialShapeSet& initialShapes() { return initialShapes_.ref(); }

  private:
    // List of shapes that may contain nursery pointers.
    using NurseryShapeVector = js::Vector<js::AccessorShape*, 0, js::SystemAllocPolicy>;
    js::ZoneGroupData<NurseryShapeVector> nurseryShapes_;
  public:
    NurseryShapeVector& nurseryShapes() { return nurseryShapes_.ref(); }

#ifdef JSGC_HASH_TABLE_CHECKS
    void checkInitialShapesTableAfterMovingGC();
    void checkBaseShapeTableAfterMovingGC();
#endif
    void fixupInitialShapeTable();
    void fixupAfterMovingGC();

    // Per-zone data for use by an embedder.
    js::ZoneGroupData<void*> data;

    js::ZoneGroupData<bool> isSystem;

    bool usedByHelperThread() {
        return !isAtomsZone() && group()->usedByHelperThread();
    }

#ifdef DEBUG
    js::ZoneGroupData<unsigned> gcLastSweepGroupIndex;
#endif

    static js::HashNumber UniqueIdToHash(uint64_t uid) {
        return mozilla::HashGeneric(uid);
    }

    // Creates a HashNumber based on getUniqueId. Returns false on OOM.
    MOZ_MUST_USE bool getHashCode(js::gc::Cell* cell, js::HashNumber* hashp) {
        uint64_t uid;
        if (!getOrCreateUniqueId(cell, &uid))
            return false;
        *hashp = UniqueIdToHash(uid);
        return true;
    }

    // Gets an existing UID in |uidp| if one exists.
    MOZ_MUST_USE bool maybeGetUniqueId(js::gc::Cell* cell, uint64_t* uidp) {
        MOZ_ASSERT(uidp);
        MOZ_ASSERT(js::CurrentThreadCanAccessZone(this));

        // Get an existing uid, if one has been set.
        auto p = uniqueIds().lookup(cell);
        if (p)
            *uidp = p->value();

        return p.found();
    }

    // Puts an existing UID in |uidp|, or creates a new UID for this Cell and
    // puts that into |uidp|. Returns false on OOM.
    MOZ_MUST_USE bool getOrCreateUniqueId(js::gc::Cell* cell, uint64_t* uidp) {
        MOZ_ASSERT(uidp);
        MOZ_ASSERT(js::CurrentThreadCanAccessZone(this) || js::CurrentThreadIsPerformingGC());

        // Get an existing uid, if one has been set.
        auto p = uniqueIds().lookupForAdd(cell);
        if (p) {
            *uidp = p->value();
            return true;
        }

        MOZ_ASSERT(js::CurrentThreadCanAccessZone(this));

        // Set a new uid on the cell.
        *uidp = js::gc::NextCellUniqueId(runtimeFromAnyThread());
        if (!uniqueIds().add(p, cell, *uidp))
            return false;

        // If the cell was in the nursery, hopefully unlikely, then we need to
        // tell the nursery about it so that it can sweep the uid if the thing
        // does not get tenured.
        if (IsInsideNursery(cell) && !group()->nursery().addedUniqueIdToCell(cell)) {
            uniqueIds().remove(cell);
            return false;
        }

        return true;
    }

    js::HashNumber getHashCodeInfallible(js::gc::Cell* cell) {
        return UniqueIdToHash(getUniqueIdInfallible(cell));
    }

    uint64_t getUniqueIdInfallible(js::gc::Cell* cell) {
        uint64_t uid;
        js::AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!getOrCreateUniqueId(cell, &uid))
            oomUnsafe.crash("failed to allocate uid");
        return uid;
    }

    // Return true if this cell has a UID associated with it.
    MOZ_MUST_USE bool hasUniqueId(js::gc::Cell* cell) {
        MOZ_ASSERT(js::CurrentThreadCanAccessZone(this) || js::CurrentThreadIsPerformingGC());
        return uniqueIds().has(cell);
    }

    // Transfer an id from another cell. This must only be called on behalf of a
    // moving GC. This method is infallible.
    void transferUniqueId(js::gc::Cell* tgt, js::gc::Cell* src) {
        MOZ_ASSERT(src != tgt);
        MOZ_ASSERT(!IsInsideNursery(tgt));
        MOZ_ASSERT(js::CurrentThreadCanAccessRuntime(runtimeFromActiveCooperatingThread()));
        MOZ_ASSERT(js::CurrentThreadCanAccessZone(this));
        MOZ_ASSERT(!uniqueIds().has(tgt));
        uniqueIds().rekeyIfMoved(src, tgt);
    }

    // Remove any unique id associated with this Cell.
    void removeUniqueId(js::gc::Cell* cell) {
        MOZ_ASSERT(js::CurrentThreadCanAccessZone(this));
        uniqueIds().remove(cell);
    }

    // When finished parsing off-thread, transfer any UIDs we created in the
    // off-thread zone into the target zone.
    void adoptUniqueIds(JS::Zone* source) {
        js::AutoEnterOOMUnsafeRegion oomUnsafe;
        for (js::gc::UniqueIdMap::Enum e(source->uniqueIds()); !e.empty(); e.popFront()) {
            MOZ_ASSERT(!uniqueIds().has(e.front().key()));
            if (!uniqueIds().put(e.front().key(), e.front().value()))
                oomUnsafe.crash("failed to transfer unique ids from off-thread");
        }
        source->uniqueIds().clear();
    }

#ifdef JSGC_HASH_TABLE_CHECKS
    // Assert that the UniqueId table has been redirected successfully.
    void checkUniqueIdTableAfterMovingGC();
#endif

    bool keepShapeTables() const {
        return keepShapeTables_;
    }
    void setKeepShapeTables(bool b) {
        keepShapeTables_ = b;
    }

    // Delete an empty compartment after its contents have been merged.
    void deleteEmptyCompartment(JSCompartment* comp);

    /*
     * This variation of calloc will call the large-allocation-failure callback
     * on OOM and retry the allocation.
     */
    template <typename T>
    T* pod_callocCanGC(size_t numElems) {
        T* p = pod_calloc<T>(numElems);
        if (MOZ_LIKELY(!!p))
            return p;
        size_t bytes;
        if (MOZ_UNLIKELY(!js::CalculateAllocSize<T>(numElems, &bytes))) {
            reportAllocationOverflow();
            return nullptr;
        }
        JSRuntime* rt = runtimeFromActiveCooperatingThread();
        p = static_cast<T*>(rt->onOutOfMemoryCanGC(js::AllocFunction::Calloc, bytes));
        if (!p)
            return nullptr;
        updateMallocCounter(bytes);
        return p;
    }

  private:
    js::ZoneGroupData<js::jit::JitZone*> jitZone_;

    js::ActiveThreadData<bool> gcScheduled_;
    js::ActiveThreadData<bool> gcScheduledSaved_;
    js::ZoneGroupData<bool> gcPreserveCode_;
    js::ZoneGroupData<bool> keepShapeTables_;

    // Allow zones to be linked into a list
    friend class js::gc::ZoneList;
    static Zone * const NotOnList;
    js::ActiveThreadOrGCTaskData<Zone*> listNext_;
    bool isOnList() const;
    Zone* nextZone() const;

    friend bool js::CurrentThreadCanAccessZone(Zone* zone);
    friend class js::gc::GCRuntime;
};

} // namespace JS

namespace js {

template <typename T>
inline T*
ZoneAllocPolicy::maybe_pod_malloc(size_t numElems)
{
    return zone->maybe_pod_malloc<T>(numElems);
}

template <typename T>
inline T*
ZoneAllocPolicy::maybe_pod_calloc(size_t numElems)
{
    return zone->maybe_pod_calloc<T>(numElems);
}

template <typename T>
inline T*
ZoneAllocPolicy::maybe_pod_realloc(T* p, size_t oldSize, size_t newSize)
{
    return zone->maybe_pod_realloc<T>(p, oldSize, newSize);
}

template <typename T>
inline T*
ZoneAllocPolicy::pod_malloc(size_t numElems)
{
    return zone->pod_malloc<T>(numElems);
}

template <typename T>
inline T*
ZoneAllocPolicy::pod_calloc(size_t numElems)
{
    return zone->pod_calloc<T>(numElems);
}

template <typename T>
inline T*
ZoneAllocPolicy::pod_realloc(T* p, size_t oldSize, size_t newSize)
{
    return zone->pod_realloc<T>(p, oldSize, newSize);
}

} // namespace js

#endif // gc_Zone_h
