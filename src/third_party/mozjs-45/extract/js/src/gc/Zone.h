/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Zone_h
#define gc_Zone_h

#include "mozilla/Atomics.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/MemoryReporting.h"

#include "jscntxt.h"

#include "ds/SplayTree.h"
#include "gc/FindSCCs.h"
#include "gc/GCRuntime.h"
#include "js/GCHashTable.h"
#include "js/TracingAPI.h"
#include "vm/MallocProvider.h"
#include "vm/TypeInference.h"

namespace js {

namespace jit {
class JitZone;
} // namespace jit

namespace gc {

// This class encapsulates the data that determines when we need to do a zone GC.
class ZoneHeapThreshold
{
    // The "growth factor" for computing our next thresholds after a GC.
    double gcHeapGrowthFactor_;

    // GC trigger threshold for allocations on the GC heap.
    size_t gcTriggerBytes_;

  public:
    ZoneHeapThreshold()
      : gcHeapGrowthFactor_(3.0),
        gcTriggerBytes_(0)
    {}

    double gcHeapGrowthFactor() const { return gcHeapGrowthFactor_; }
    size_t gcTriggerBytes() const { return gcTriggerBytes_; }
    double allocTrigger(bool highFrequencyGC) const;

    void updateAfterGC(size_t lastBytes, JSGCInvocationKind gckind,
                       const GCSchedulingTunables& tunables, const GCSchedulingState& state,
                       const AutoLockGC& lock);
    void updateForRemovedArena(const GCSchedulingTunables& tunables);

  private:
    static double computeZoneHeapGrowthFactorForHeapSize(size_t lastBytes,
                                                         const GCSchedulingTunables& tunables,
                                                         const GCSchedulingState& state);
    static size_t computeZoneTriggerBytes(double growthFactor, size_t lastBytes,
                                          JSGCInvocationKind gckind,
                                          const GCSchedulingTunables& tunables,
                                          const AutoLockGC& lock);
};

struct UniqueIdGCPolicy {
    static bool needsSweep(Cell** cell, uint64_t* value);
};

// Maps a Cell* to a unique, 64bit id.
using UniqueIdMap = GCHashMap<Cell*,
                              uint64_t,
                              PointerHasher<Cell*, 3>,
                              SystemAllocPolicy,
                              UniqueIdGCPolicy>;

extern uint64_t NextCellUniqueId(JSRuntime* rt);

} // namespace gc
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
//
// - JSStrings do not belong to any particular compartment, but they do belong
//   to a zone. Thus, two different compartments in the same zone can point to a
//   JSString. When a string needs to be wrapped, we copy it if it's in a
//   different zone and do nothing if it's in the same zone. Thus, transferring
//   strings within a zone is very efficient.
//
// - Shapes and base shapes belong to a compartment and cannot be shared between
//   compartments. A base shape holds a pointer to its compartment. Shapes find
//   their compartment via their base shape. JSObjects find their compartment
//   via their shape.
//
// - Scripts are also compartment-local and cannot be shared. A script points to
//   its compartment.
//
// - Type objects and JitCode objects belong to a compartment and cannot be
//   shared. However, there is no mechanism to obtain their compartments.
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
    explicit Zone(JSRuntime* rt);
    ~Zone();
    bool init(bool isSystem);

    void findOutgoingEdges(js::gc::ComponentFinder<JS::Zone>& finder);

    void discardJitCode(js::FreeOp* fop);

    void addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                size_t* typePool,
                                size_t* baselineStubsOptimized,
                                size_t* uniqueIdMap);

    void resetGCMallocBytes();
    void setGCMaxMallocBytes(size_t value);
    void updateMallocCounter(size_t nbytes) {
        // Note: this code may be run from worker threads. We tolerate any
        // thread races when updating gcMallocBytes.
        gcMallocBytes -= ptrdiff_t(nbytes);
        if (MOZ_UNLIKELY(isTooMuchMalloc()))
            onTooMuchMalloc();
    }

    bool isTooMuchMalloc() const { return gcMallocBytes <= 0; }
    void onTooMuchMalloc();

    void* onOutOfMemory(js::AllocFunction allocFunc, size_t nbytes, void* reallocPtr = nullptr) {
        if (!js::CurrentThreadCanAccessRuntime(runtime_))
            return nullptr;
        return runtimeFromMainThread()->onOutOfMemory(allocFunc, nbytes, reallocPtr);
    }
    void reportAllocationOverflow() { js::ReportAllocationOverflow(nullptr); }

    void beginSweepTypes(js::FreeOp* fop, bool releaseTypes);

    bool hasMarkedCompartments();

    void scheduleGC() { MOZ_ASSERT(!runtimeFromMainThread()->isHeapBusy()); gcScheduled_ = true; }
    void unscheduleGC() { gcScheduled_ = false; }
    bool isGCScheduled() { return gcScheduled_ && canCollect(); }

    void setPreservingCode(bool preserving) { gcPreserveCode_ = preserving; }
    bool isPreservingCode() const { return gcPreserveCode_; }

    bool canCollect();

    void notifyObservingDebuggers();

    enum GCState {
        NoGC,
        Mark,
        MarkGray,
        Sweep,
        Finished,
        Compact
    };
    void setGCState(GCState state) {
        MOZ_ASSERT(runtimeFromMainThread()->isHeapBusy());
        MOZ_ASSERT_IF(state != NoGC, canCollect());
        gcState_ = state;
        if (state == Finished)
            notifyObservingDebuggers();
    }

    bool isCollecting() const {
        if (runtimeFromMainThread()->isHeapCollecting())
            return gcState_ != NoGC;
        else
            return needsIncrementalBarrier();
    }

    bool isCollectingFromAnyThread() const {
        if (runtimeFromAnyThread()->isHeapCollecting())
            return gcState_ != NoGC;
        else
            return needsIncrementalBarrier();
    }

    // If this returns true, all object tracing must be done with a GC marking
    // tracer.
    bool requireGCTracer() const {
        JSRuntime* rt = runtimeFromAnyThread();
        return rt->isHeapMajorCollecting() && !rt->gc.isHeapCompacting() && gcState_ != NoGC;
    }

    bool isGCMarking() {
        if (runtimeFromMainThread()->isHeapCollecting())
            return gcState_ == Mark || gcState_ == MarkGray;
        else
            return needsIncrementalBarrier();
    }

    bool wasGCStarted() const { return gcState_ != NoGC; }
    bool isGCMarkingBlack() { return gcState_ == Mark; }
    bool isGCMarkingGray() { return gcState_ == MarkGray; }
    bool isGCSweeping() { return gcState_ == Sweep; }
    bool isGCFinished() { return gcState_ == Finished; }
    bool isGCCompacting() { return gcState_ == Compact; }
    bool isGCSweepingOrCompacting() { return gcState_ == Sweep || gcState_ == Compact; }

    // Get a number that is incremented whenever this zone is collected, and
    // possibly at other times too.
    uint64_t gcNumber();

    bool compileBarriers() const { return compileBarriers(needsIncrementalBarrier()); }
    bool compileBarriers(bool needsIncrementalBarrier) const {
        return needsIncrementalBarrier ||
               runtimeFromMainThread()->gcZeal() == js::gc::ZealVerifierPreValue;
    }

    enum ShouldUpdateJit { DontUpdateJit, UpdateJit };
    void setNeedsIncrementalBarrier(bool needs, ShouldUpdateJit updateJit);
    const bool* addressOfNeedsIncrementalBarrier() const { return &needsIncrementalBarrier_; }

    js::jit::JitZone* getJitZone(JSContext* cx) { return jitZone_ ? jitZone_ : createJitZone(cx); }
    js::jit::JitZone* jitZone() { return jitZone_; }

    bool isAtomsZone() const { return runtimeFromAnyThread()->isAtomsZone(this); }
    bool isSelfHostingZone() const { return runtimeFromAnyThread()->isSelfHostingZone(this); }

    void prepareForCompacting();

#ifdef DEBUG
    // For testing purposes, return the index of the zone group which this zone
    // was swept in in the last GC.
    unsigned lastZoneGroupIndex() { return gcLastZoneGroupIndex; }
#endif

    using DebuggerVector = js::Vector<js::Debugger*, 0, js::SystemAllocPolicy>;

  private:
    DebuggerVector* debuggers;

    using LogTenurePromotionQueue = js::Vector<JSObject*, 0, js::SystemAllocPolicy>;
    LogTenurePromotionQueue awaitingTenureLogging;

    void sweepBreakpoints(js::FreeOp* fop);
    void sweepUniqueIds(js::FreeOp* fop);
    void sweepWeakMaps();
    void sweepCompartments(js::FreeOp* fop, bool keepAtleastOne, bool lastGC);

    js::jit::JitZone* createJitZone(JSContext* cx);

    bool isQueuedForBackgroundSweep() {
        return isOnList();
    }

    // Side map for storing a unique ids for cells, independent of address.
    js::gc::UniqueIdMap uniqueIds_;

  public:
    bool hasDebuggers() const { return debuggers && debuggers->length(); }
    DebuggerVector* getDebuggers() const { return debuggers; }
    DebuggerVector* getOrCreateDebuggers(JSContext* cx);

    void enqueueForPromotionToTenuredLogging(JSObject& obj) {
        MOZ_ASSERT(hasDebuggers());
        MOZ_ASSERT(!IsInsideNursery(&obj));
        js::AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!awaitingTenureLogging.append(&obj))
            oomUnsafe.crash("Zone::enqueueForPromotionToTenuredLogging");
    }
    void logPromotionsToTenured();

    js::gc::ArenaLists arenas;

    js::TypeZone types;

    /* Live weakmaps in this zone. */
    mozilla::LinkedList<js::WeakMapBase> gcWeakMapList;

    // The set of compartments in this zone.
    typedef js::Vector<JSCompartment*, 1, js::SystemAllocPolicy> CompartmentVector;
    CompartmentVector compartments;

    // This zone's gray roots.
    typedef js::Vector<js::gc::Cell*, 0, js::SystemAllocPolicy> GrayRootVector;
    GrayRootVector gcGrayRoots;

    // This zone's weak edges found via graph traversal during marking,
    // preserved for re-scanning during sweeping.
    using WeakEdges = js::Vector<js::gc::TenuredCell**, 0, js::SystemAllocPolicy>;
    WeakEdges gcWeakRefs;

    /*
     * Mapping from not yet marked keys to a vector of all values that the key
     * maps to in any live weak map.
     */
    js::gc::WeakKeyTable gcWeakKeys;

    // A set of edges from this zone to other zones.
    //
    // This is used during GC while calculating zone groups to record edges that
    // can't be determined by examining this zone by itself.
    ZoneSet gcZoneGroupEdges;

    // Malloc counter to measure memory pressure for GC scheduling. It runs from
    // gcMaxMallocBytes down to zero. This counter should be used only when it's
    // not possible to know the size of a free.
    mozilla::Atomic<ptrdiff_t, mozilla::ReleaseAcquire> gcMallocBytes;

    // GC trigger threshold for allocations on the C heap.
    size_t gcMaxMallocBytes;

    // Whether a GC has been triggered as a result of gcMallocBytes falling
    // below zero.
    //
    // This should be a bool, but Atomic only supports 32-bit and pointer-sized
    // types.
    mozilla::Atomic<uint32_t, mozilla::ReleaseAcquire> gcMallocGCTriggered;

    // Track heap usage under this Zone.
    js::gc::HeapUsage usage;

    // Thresholds used to trigger GC.
    js::gc::ZoneHeapThreshold threshold;

    // Amount of data to allocate before triggering a new incremental slice for
    // the current GC.
    size_t gcDelayBytes;

    // Per-zone data for use by an embedder.
    void* data;

    bool isSystem;

    bool usedByExclusiveThread;

    // True when there are active frames.
    bool active;

    mozilla::DebugOnly<unsigned> gcLastZoneGroupIndex;

    // Creates a HashNumber based on getUniqueId. Returns false on OOM.
    bool getHashCode(js::gc::Cell* cell, js::HashNumber* hashp) {
        uint64_t uid;
        if (!getUniqueId(cell, &uid))
            return false;
        *hashp = js::HashNumber(uid >> 32) ^ js::HashNumber(uid & 0xFFFFFFFF);
        return true;
    }

    // Puts an existing UID in |uidp|, or creates a new UID for this Cell and
    // puts that into |uidp|. Returns false on OOM.
    bool getUniqueId(js::gc::Cell* cell, uint64_t* uidp) {
        MOZ_ASSERT(uidp);
        MOZ_ASSERT(js::CurrentThreadCanAccessZone(this));

        // Get an existing uid, if one has been set.
        auto p = uniqueIds_.lookupForAdd(cell);
        if (p) {
            *uidp = p->value();
            return true;
        }

        // Set a new uid on the cell.
        *uidp = js::gc::NextCellUniqueId(runtimeFromAnyThread());
        if (!uniqueIds_.add(p, cell, *uidp))
            return false;

        // If the cell was in the nursery, hopefully unlikely, then we need to
        // tell the nursery about it so that it can sweep the uid if the thing
        // does not get tenured.
        js::AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!runtimeFromAnyThread()->gc.nursery.addedUniqueIdToCell(cell))
            oomUnsafe.crash("failed to allocate tracking data for a nursery uid");
        return true;
    }

    // Return true if this cell has a UID associated with it.
    bool hasUniqueId(js::gc::Cell* cell) {
        MOZ_ASSERT(js::CurrentThreadCanAccessZone(this));
        return uniqueIds_.has(cell);
    }

    // Transfer an id from another cell. This must only be called on behalf of a
    // moving GC. This method is infallible.
    void transferUniqueId(js::gc::Cell* tgt, js::gc::Cell* src) {
        MOZ_ASSERT(src != tgt);
        MOZ_ASSERT(!IsInsideNursery(tgt));
        MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtimeFromMainThread()));
        MOZ_ASSERT(js::CurrentThreadCanAccessZone(this));
        uniqueIds_.rekeyIfMoved(src, tgt);
    }

    // Remove any unique id associated with this Cell.
    void removeUniqueId(js::gc::Cell* cell) {
        MOZ_ASSERT(js::CurrentThreadCanAccessZone(this));
        uniqueIds_.remove(cell);
    }

    // Off-thread parsing should not result in any UIDs being created.
    void assertNoUniqueIdsInZone() const {
        MOZ_ASSERT(uniqueIds_.count() == 0);
    }

#ifdef JSGC_HASH_TABLE_CHECKS
    // Assert that the UniqueId table has been redirected successfully.
    void checkUniqueIdTableAfterMovingGC();
#endif

  private:
    js::jit::JitZone* jitZone_;

    GCState gcState_;
    bool gcScheduled_;
    bool gcPreserveCode_;
    bool jitUsingBarriers_;

    // Allow zones to be linked into a list
    friend class js::gc::ZoneList;
    static Zone * const NotOnList;
    Zone* listNext_;
    bool isOnList() const;
    Zone* nextZone() const;

    friend bool js::CurrentThreadCanAccessZone(Zone* zone);
    friend class js::gc::GCRuntime;
};

} // namespace JS

namespace js {

// Using the atoms zone without holding the exclusive access lock is dangerous
// because worker threads may be using it simultaneously. Therefore, it's
// better to skip the atoms zone when iterating over zones. If you need to
// iterate over the atoms zone, consider taking the exclusive access lock first.
enum ZoneSelector {
    WithAtoms,
    SkipAtoms
};

class ZonesIter
{
    gc::AutoEnterIteration iterMarker;
    JS::Zone** it;
    JS::Zone** end;

  public:
    ZonesIter(JSRuntime* rt, ZoneSelector selector) : iterMarker(&rt->gc) {
        it = rt->gc.zones.begin();
        end = rt->gc.zones.end();

        if (selector == SkipAtoms) {
            MOZ_ASSERT(atAtomsZone(rt));
            it++;
        }
    }

    bool atAtomsZone(JSRuntime* rt);

    bool done() const { return it == end; }

    void next() {
        MOZ_ASSERT(!done());
        do {
            it++;
        } while (!done() && (*it)->usedByExclusiveThread);
    }

    JS::Zone* get() const {
        MOZ_ASSERT(!done());
        return *it;
    }

    operator JS::Zone*() const { return get(); }
    JS::Zone* operator->() const { return get(); }
};

struct CompartmentsInZoneIter
{
    explicit CompartmentsInZoneIter(JS::Zone* zone) : zone(zone) {
        it = zone->compartments.begin();
    }

    bool done() const {
        MOZ_ASSERT(it);
        return it < zone->compartments.begin() ||
               it >= zone->compartments.end();
    }
    void next() {
        MOZ_ASSERT(!done());
        it++;
    }

    JSCompartment* get() const {
        MOZ_ASSERT(it);
        return *it;
    }

    operator JSCompartment*() const { return get(); }
    JSCompartment* operator->() const { return get(); }

  private:
    JS::Zone* zone;
    JSCompartment** it;

    CompartmentsInZoneIter()
      : zone(nullptr), it(nullptr)
    {}

    // This is for the benefit of CompartmentsIterT::comp.
    friend class mozilla::Maybe<CompartmentsInZoneIter>;
};

// This iterator iterates over all the compartments in a given set of zones. The
// set of zones is determined by iterating ZoneIterT.
template<class ZonesIterT>
class CompartmentsIterT
{
    gc::AutoEnterIteration iterMarker;
    ZonesIterT zone;
    mozilla::Maybe<CompartmentsInZoneIter> comp;

  public:
    explicit CompartmentsIterT(JSRuntime* rt)
      : iterMarker(&rt->gc), zone(rt)
    {
        if (zone.done())
            comp.emplace();
        else
            comp.emplace(zone);
    }

    CompartmentsIterT(JSRuntime* rt, ZoneSelector selector)
      : iterMarker(&rt->gc), zone(rt, selector)
    {
        if (zone.done())
            comp.emplace();
        else
            comp.emplace(zone);
    }

    bool done() const { return zone.done(); }

    void next() {
        MOZ_ASSERT(!done());
        MOZ_ASSERT(!comp.ref().done());
        comp->next();
        if (comp->done()) {
            comp.reset();
            zone.next();
            if (!zone.done())
                comp.emplace(zone);
        }
    }

    JSCompartment* get() const {
        MOZ_ASSERT(!done());
        return *comp;
    }

    operator JSCompartment*() const { return get(); }
    JSCompartment* operator->() const { return get(); }
};

typedef CompartmentsIterT<ZonesIter> CompartmentsIter;

/*
 * Allocation policy that uses Zone::pod_malloc and friends, so that memory
 * pressure is accounted for on the zone. This is suitable for memory associated
 * with GC things allocated in the zone.
 *
 * Since it doesn't hold a JSContext (those may not live long enough), it can't
 * report out-of-memory conditions itself; the caller must check for OOM and
 * take the appropriate action.
 *
 * FIXME bug 647103 - replace these *AllocPolicy names.
 */
class ZoneAllocPolicy
{
    Zone* const zone;

  public:
    MOZ_IMPLICIT ZoneAllocPolicy(Zone* zone) : zone(zone) {}

    template <typename T>
    T* maybe_pod_malloc(size_t numElems) {
        return zone->maybe_pod_malloc<T>(numElems);
    }

    template <typename T>
    T* maybe_pod_calloc(size_t numElems) {
        return zone->maybe_pod_calloc<T>(numElems);
    }

    template <typename T>
    T* maybe_pod_realloc(T* p, size_t oldSize, size_t newSize) {
        return zone->maybe_pod_realloc<T>(p, oldSize, newSize);
    }

    template <typename T>
    T* pod_malloc(size_t numElems) {
        return zone->pod_malloc<T>(numElems);
    }

    template <typename T>
    T* pod_calloc(size_t numElems) {
        return zone->pod_calloc<T>(numElems);
    }

    template <typename T>
    T* pod_realloc(T* p, size_t oldSize, size_t newSize) {
        return zone->pod_realloc<T>(p, oldSize, newSize);
    }

    void free_(void* p) { js_free(p); }
    void reportAllocOverflow() const {}

    bool checkSimulatedOOM() const {
        return !js::oom::ShouldFailWithOOM();
    }
};

} // namespace js

#endif // gc_Zone_h
