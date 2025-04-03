/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Zone_h
#define gc_Zone_h

#include "mozilla/Array.h"
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/LinkedList.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/PodOperations.h"
#include "mozilla/TimeStamp.h"

#include "jstypes.h"

#include "ds/Bitmap.h"
#include "gc/ArenaList.h"
#include "gc/Barrier.h"
#include "gc/FindSCCs.h"
#include "gc/GCMarker.h"
#include "gc/NurseryAwareHashMap.h"
#include "gc/Pretenuring.h"
#include "gc/Statistics.h"
#include "gc/ZoneAllocator.h"
#include "js/GCHashTable.h"
#include "js/Vector.h"
#include "vm/AtomsTable.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/ShapeZone.h"

namespace js {

class DebugScriptMap;
class RegExpZone;
class WeakRefObject;

namespace jit {
class JitZone;
}  // namespace jit

namespace gc {

class FinalizationObservers;
class ZoneList;

using ZoneComponentFinder = ComponentFinder<JS::Zone>;

struct UniqueIdGCPolicy {
  static bool traceWeak(JSTracer* trc, Cell** keyp, uint64_t* valuep);
};

// Maps a Cell* to a unique, 64bit id.
using UniqueIdMap = GCHashMap<Cell*, uint64_t, PointerHasher<Cell*>,
                              SystemAllocPolicy, UniqueIdGCPolicy>;

template <typename T>
class ZoneAllCellIter;

template <typename T>
class ZoneCellIter;

}  // namespace gc

// If two different nursery strings are wrapped into the same zone, and have
// the same contents, then deduplication may make them duplicates.
// `DuplicatesPossible` will allow this and map both wrappers to the same (now
// tenured) source string.
using StringWrapperMap =
    NurseryAwareHashMap<JSString*, JSString*, ZoneAllocPolicy,
                        DuplicatesPossible>;

class MOZ_NON_TEMPORARY_CLASS ExternalStringCache {
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

class MOZ_NON_TEMPORARY_CLASS FunctionToStringCache {
  struct Entry {
    BaseScript* script;
    JSString* string;

    void set(BaseScript* scriptArg, JSString* stringArg) {
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

  MOZ_ALWAYS_INLINE JSString* lookup(BaseScript* script) const;
  MOZ_ALWAYS_INLINE void put(BaseScript* script, JSString* string);
};

}  // namespace js

namespace JS {

// [SMDOC] GC Zones
//
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
class Zone : public js::ZoneAllocator, public js::gc::GraphNodeBase<JS::Zone> {
 public:
  js::gc::ArenaLists arenas;

  // Per-zone data for use by an embedder.
  js::MainThreadData<void*> data;

  js::MainThreadData<uint32_t> tenuredBigInts;

  // Number of marked/finalized JSStrings/JSFatInlineStrings during major GC.
  js::MainThreadOrGCTaskData<size_t> markedStrings;
  js::MainThreadOrGCTaskData<size_t> finalizedStrings;

  // When true, skip calling the metadata callback. We use this:
  // - to avoid invoking the callback recursively;
  // - to avoid observing lazy prototype setup (which confuses callbacks that
  //   want to use the types being set up!);
  // - to avoid attaching allocation stacks to allocation stack nodes, which
  //   is silly
  // And so on.
  js::MainThreadData<bool> suppressAllocationMetadataBuilder;

  // Flags permanently set when nursery allocation is disabled for this zone.
  js::MainThreadData<bool> nurseryStringsDisabled;
  js::MainThreadData<bool> nurseryBigIntsDisabled;

 private:
  // Flags dynamically updated based on more than one condition, including the
  // flags above.
  js::MainThreadOrIonCompileData<bool> allocNurseryObjects_;
  js::MainThreadOrIonCompileData<bool> allocNurseryStrings_;
  js::MainThreadOrIonCompileData<bool> allocNurseryBigInts_;

  // Minimum Heap value which results in tenured allocation.
  js::MainThreadData<js::gc::Heap> minObjectHeapToTenure_;
  js::MainThreadData<js::gc::Heap> minStringHeapToTenure_;
  js::MainThreadData<js::gc::Heap> minBigintHeapToTenure_;

 public:
  // Script side-tables. These used to be held by Realm, but are now placed
  // here in order to allow JSScript to access them during finalize (see bug
  // 1568245; this change in 1575350). The tables are initialized lazily by
  // JSScript.
  js::UniquePtr<js::ScriptCountsMap> scriptCountsMap;
  js::UniquePtr<js::ScriptLCovMap> scriptLCovMap;
  js::MainThreadData<js::DebugScriptMap*> debugScriptMap;
#ifdef MOZ_VTUNE
  js::UniquePtr<js::ScriptVTuneIdMap> scriptVTuneIdMap;
#endif
#ifdef JS_CACHEIR_SPEW
  js::UniquePtr<js::ScriptFinalWarmUpCountMap> scriptFinalWarmUpCountMap;
#endif

  js::MainThreadData<js::StringStats> previousGCStringStats;
  js::MainThreadData<js::StringStats> stringStats;

#ifdef DEBUG
  js::MainThreadData<unsigned> gcSweepGroupIndex;
#endif

  js::gc::PretenuringZone pretenuring;

 private:
  // Side map for storing unique ids for cells, independent of address.
  js::MainThreadOrGCTaskData<js::gc::UniqueIdMap> uniqueIds_;

  // Number of allocations since the most recent minor GC for this thread.
  uint32_t tenuredAllocsSinceMinorGC_ = 0;

  // Live weakmaps in this zone.
  js::MainThreadOrGCTaskData<mozilla::LinkedList<js::WeakMapBase>>
      gcWeakMapList_;

  // The set of compartments in this zone.
  using CompartmentVector =
      js::Vector<JS::Compartment*, 1, js::SystemAllocPolicy>;
  js::MainThreadOrGCTaskData<CompartmentVector> compartments_;

  // All cross-zone string wrappers in the zone.
  js::MainThreadOrGCTaskData<js::StringWrapperMap> crossZoneStringWrappers_;

  // List of non-ephemeron weak containers to sweep during
  // beginSweepingSweepGroup.
  js::MainThreadOrGCTaskData<mozilla::LinkedList<detail::WeakCacheBase>>
      weakCaches_;

  // Mapping from not yet marked keys to a vector of all values that the key
  // maps to in any live weak map. Separate tables for nursery and tenured
  // keys.
  js::MainThreadOrGCTaskData<js::gc::EphemeronEdgeTable> gcEphemeronEdges_;
  js::MainThreadOrGCTaskData<js::gc::EphemeronEdgeTable>
      gcNurseryEphemeronEdges_;

  js::MainThreadData<js::UniquePtr<js::RegExpZone>> regExps_;

  // Bitmap of atoms marked by this zone.
  js::MainThreadOrGCTaskData<js::SparseBitmap> markedAtoms_;

  // Set of atoms recently used by this Zone. Purged on GC.
  js::MainThreadOrGCTaskData<js::AtomSet> atomCache_;

  // Cache storing allocated external strings. Purged on GC.
  js::MainThreadOrGCTaskData<js::ExternalStringCache> externalStringCache_;

  // Cache for Function.prototype.toString. Purged on GC.
  js::MainThreadOrGCTaskData<js::FunctionToStringCache> functionToStringCache_;

  // Cache for Function.prototype.bind mapping an atom `name` to atom
  // `"bound " + name`. Purged on GC.
  using BoundPrefixCache =
      js::HashMap<JSAtom*, JSAtom*, js::PointerHasher<JSAtom*>,
                  js::SystemAllocPolicy>;
  js::MainThreadData<BoundPrefixCache> boundPrefixCache_;

  // Information about Shapes and BaseShapes.
  js::MainThreadData<js::ShapeZone> shapeZone_;

  // Information about finalization registries, created on demand.
  js::MainThreadOrGCTaskData<js::UniquePtr<js::gc::FinalizationObservers>>
      finalizationObservers_;

  js::MainThreadOrGCTaskData<js::jit::JitZone*> jitZone_;

  // Last time at which JIT code was discarded for this zone. This is only set
  // when JitScripts and Baseline code are discarded as well.
  js::MainThreadData<mozilla::TimeStamp> lastDiscardedCodeTime_;

  js::MainThreadData<bool> gcScheduled_;
  js::MainThreadData<bool> gcScheduledSaved_;
  js::MainThreadData<bool> gcPreserveCode_;
  js::MainThreadData<bool> keepPropMapTables_;
  js::MainThreadData<bool> wasCollected_;

  // Allow zones to be linked into a list
  js::MainThreadOrGCTaskData<Zone*> listNext_;
  static Zone* const NotOnList;
  friend class js::gc::ZoneList;

  using KeptAliveSet =
      JS::GCHashSet<js::HeapPtr<JSObject*>,
                    js::StableCellHasher<js::HeapPtr<JSObject*>>,
                    js::ZoneAllocPolicy>;
  friend class js::WeakRefObject;
  js::MainThreadOrGCTaskData<KeptAliveSet> keptObjects;

 public:
  static JS::Zone* from(ZoneAllocator* zoneAlloc) {
    return static_cast<Zone*>(zoneAlloc);
  }

  explicit Zone(JSRuntime* rt, Kind kind = NormalZone);
  ~Zone();

  [[nodiscard]] bool init();

  void destroy(JS::GCContext* gcx);

  [[nodiscard]] bool findSweepGroupEdges(Zone* atomsZone);

  struct DiscardOptions {
    DiscardOptions() {}
    bool discardBaselineCode = true;
    bool discardJitScripts = false;
    bool resetNurseryAllocSites = false;
    bool resetPretenuredAllocSites = false;
  };

  void discardJitCode(JS::GCContext* gcx,
                      const DiscardOptions& options = DiscardOptions());

  // Discard JIT code regardless of isPreservingCode().
  void forceDiscardJitCode(JS::GCContext* gcx,
                           const DiscardOptions& options = DiscardOptions());

  void resetAllocSitesAndInvalidate(bool resetNurserySites,
                                    bool resetPretenuredSites);

  void addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf,
                              JS::CodeSizes* code, size_t* regexpZone,
                              size_t* jitZone, size_t* baselineStubsOptimized,
                              size_t* uniqueIdMap, size_t* initialPropMapTable,
                              size_t* shapeTables, size_t* atomsMarkBitmaps,
                              size_t* compartmentObjects,
                              size_t* crossCompartmentWrappersTables,
                              size_t* compartmentsPrivateData,
                              size_t* scriptCountsMapArg);

  // Iterate over all cells in the zone. See the definition of ZoneCellIter
  // in gc/GC-inl.h for the possible arguments and documentation.
  template <typename T, typename... Args>
  js::gc::ZoneCellIter<T> cellIter(Args&&... args) {
    return js::gc::ZoneCellIter<T>(const_cast<Zone*>(this),
                                   std::forward<Args>(args)...);
  }

  // As above, but can return about-to-be-finalised things.
  template <typename T, typename... Args>
  js::gc::ZoneAllCellIter<T> cellIterUnsafe(Args&&... args) {
    return js::gc::ZoneAllCellIter<T>(const_cast<Zone*>(this),
                                      std::forward<Args>(args)...);
  }

  bool hasMarkedRealms();

  void scheduleGC() {
    MOZ_ASSERT(!RuntimeHeapIsBusy());
    gcScheduled_ = true;
  }
  void unscheduleGC() { gcScheduled_ = false; }
  bool isGCScheduled() { return gcScheduled_; }

  void setPreservingCode(bool preserving) { gcPreserveCode_ = preserving; }
  bool isPreservingCode() const { return gcPreserveCode_; }

  mozilla::TimeStamp lastDiscardedCodeTime() const {
    return lastDiscardedCodeTime_;
  }

  void changeGCState(GCState prev, GCState next);

  bool isCollecting() const {
    MOZ_ASSERT(js::CurrentThreadCanAccessRuntime(runtimeFromMainThread()));
    return isCollectingFromAnyThread();
  }

  inline bool isCollectingFromAnyThread() const {
    return needsIncrementalBarrier() || wasGCStarted();
  }

  GCState initialMarkingState() const;

  bool shouldMarkInZone(js::gc::MarkColor color) const {
    // Check whether the zone is in one or both of the MarkBlackOnly and
    // MarkBlackAndGray states, depending on the mark color. Also check for
    // VerifyPreBarriers when the mark color is black (we don't do any gray
    // marking when verifying pre-barriers).
    if (color == js::gc::MarkColor::Black) {
      return isGCMarkingOrVerifyingPreBarriers();
    }

    return isGCMarkingBlackAndGray();
  }

  // Was this zone collected in the last GC.
  bool wasCollected() const { return wasCollected_; }
  void setWasCollected(bool v) { wasCollected_ = v; }

  void setNeedsIncrementalBarrier(bool needs);
  const BarrierState* addressOfNeedsIncrementalBarrier() const {
    return &needsIncrementalBarrier_;
  }

  static constexpr size_t offsetOfNeedsIncrementalBarrier() {
    return offsetof(Zone, needsIncrementalBarrier_);
  }

  js::jit::JitZone* getJitZone(JSContext* cx) {
    return jitZone_ ? jitZone_ : createJitZone(cx);
  }
  js::jit::JitZone* jitZone() { return jitZone_; }

  void prepareForCompacting();

  void traceRootsInMajorGC(JSTracer* trc);

  void sweepAfterMinorGC(JSTracer* trc);
  void sweepUniqueIds();
  void sweepCompartments(JS::GCContext* gcx, bool keepAtleastOne, bool lastGC);

  // Remove dead weak maps from gcWeakMapList_ and remove entries from the
  // remaining weak maps whose keys are dead.
  void sweepWeakMaps(JSTracer* trc);

  // Trace all weak maps in this zone. Used to update edges after a moving GC.
  void traceWeakMaps(JSTracer* trc);

  js::gc::UniqueIdMap& uniqueIds() { return uniqueIds_.ref(); }

  void notifyObservingDebuggers();

  void noteTenuredAlloc() { tenuredAllocsSinceMinorGC_++; }

  uint32_t* addressOfTenuredAllocCount() { return &tenuredAllocsSinceMinorGC_; }

  uint32_t getAndResetTenuredAllocsSinceMinorGC() {
    uint32_t res = tenuredAllocsSinceMinorGC_;
    tenuredAllocsSinceMinorGC_ = 0;
    return res;
  }

  mozilla::LinkedList<js::WeakMapBase>& gcWeakMapList() {
    return gcWeakMapList_.ref();
  }

  CompartmentVector& compartments() { return compartments_.ref(); }

  js::StringWrapperMap& crossZoneStringWrappers() {
    return crossZoneStringWrappers_.ref();
  }
  const js::StringWrapperMap& crossZoneStringWrappers() const {
    return crossZoneStringWrappers_.ref();
  }

  void dropStringWrappersOnGC();

  void traceWeakCCWEdges(JSTracer* trc);
  static void fixupAllCrossCompartmentWrappersAfterMovingGC(JSTracer* trc);

  void fixupAfterMovingGC();
  void fixupScriptMapsAfterMovingGC(JSTracer* trc);

  void setNurseryAllocFlags(bool allocObjects, bool allocStrings,
                            bool allocBigInts);

  bool allocKindInNursery(JS::TraceKind kind) const {
    switch (kind) {
      case JS::TraceKind::Object:
        return allocNurseryObjects_;
      case JS::TraceKind::String:
        return allocNurseryStrings_;
      case JS::TraceKind::BigInt:
        return allocNurseryBigInts_;
      default:
        MOZ_CRASH("Unsupported kind for nursery allocation");
    }
  }
  bool allocNurseryObjects() const { return allocNurseryObjects_; }
  bool allocNurseryStrings() const { return allocNurseryStrings_; }
  bool allocNurseryBigInts() const { return allocNurseryBigInts_; }

  js::gc::Heap minHeapToTenure(JS::TraceKind kind) const {
    switch (kind) {
      case JS::TraceKind::Object:
        return minObjectHeapToTenure_;
      case JS::TraceKind::String:
        return minStringHeapToTenure_;
      case JS::TraceKind::BigInt:
        return minBigintHeapToTenure_;
      default:
        MOZ_CRASH("Unsupported kind for nursery allocation");
    }
  }

  mozilla::LinkedList<detail::WeakCacheBase>& weakCaches() {
    return weakCaches_.ref();
  }
  void registerWeakCache(detail::WeakCacheBase* cachep) {
    weakCaches().insertBack(cachep);
  }

  void beforeClearDelegate(JSObject* wrapper, JSObject* delegate) {
    if (needsIncrementalBarrier()) {
      beforeClearDelegateInternal(wrapper, delegate);
    }
  }

  void afterAddDelegate(JSObject* wrapper) {
    if (needsIncrementalBarrier()) {
      afterAddDelegateInternal(wrapper);
    }
  }

  void beforeClearDelegateInternal(JSObject* wrapper, JSObject* delegate);
  void afterAddDelegateInternal(JSObject* wrapper);
  js::gc::EphemeronEdgeTable& gcEphemeronEdges() {
    return gcEphemeronEdges_.ref();
  }
  js::gc::EphemeronEdgeTable& gcNurseryEphemeronEdges() {
    return gcNurseryEphemeronEdges_.ref();
  }

  js::gc::EphemeronEdgeTable& gcEphemeronEdges(const js::gc::Cell* cell) {
    return cell->isTenured() ? gcEphemeronEdges() : gcNurseryEphemeronEdges();
  }

  // Perform all pending weakmap entry marking for this zone after
  // transitioning to weak marking mode.
  js::gc::IncrementalProgress enterWeakMarkingMode(js::GCMarker* marker,
                                                   js::SliceBudget& budget);

  // A set of edges from this zone to other zones used during GC to calculate
  // sweep groups.
  NodeSet& gcSweepGroupEdges() {
    return gcGraphEdges;  // Defined in GraphNodeBase base class.
  }
  bool hasSweepGroupEdgeTo(Zone* otherZone) const {
    return gcGraphEdges.has(otherZone);
  }
  [[nodiscard]] bool addSweepGroupEdgeTo(Zone* otherZone) {
    MOZ_ASSERT(otherZone->isGCMarking());
    return gcSweepGroupEdges().put(otherZone);
  }
  void clearSweepGroupEdges() { gcSweepGroupEdges().clear(); }

  js::RegExpZone& regExps() { return *regExps_.ref(); }

  js::SparseBitmap& markedAtoms() { return markedAtoms_.ref(); }

  js::AtomSet& atomCache() { return atomCache_.ref(); }

  void purgeAtomCache();

  js::ExternalStringCache& externalStringCache() {
    return externalStringCache_.ref();
  };

  js::FunctionToStringCache& functionToStringCache() {
    return functionToStringCache_.ref();
  }

  BoundPrefixCache& boundPrefixCache() { return boundPrefixCache_.ref(); }

  js::ShapeZone& shapeZone() { return shapeZone_.ref(); }

  bool keepPropMapTables() const { return keepPropMapTables_; }
  void setKeepPropMapTables(bool b) { keepPropMapTables_ = b; }

  void clearRootsForShutdownGC();
  void finishRoots();

  void traceScriptTableRoots(JSTracer* trc);

  void clearScriptCounts(Realm* realm);
  void clearScriptLCov(Realm* realm);

  // Add the target of JS WeakRef to a kept-alive set maintained by GC.
  // See: https://tc39.es/proposal-weakrefs/#sec-keepduringjob
  bool keepDuringJob(HandleObject target);

  void traceKeptObjects(JSTracer* trc);

  // Clear the kept-alive set.
  // See: https://tc39.es/proposal-weakrefs/#sec-clear-kept-objects
  void clearKeptObjects();

  js::gc::AllocSite* unknownAllocSite(JS::TraceKind kind) {
    return &pretenuring.unknownAllocSite(kind);
  }
  js::gc::AllocSite* optimizedAllocSite() {
    return &pretenuring.optimizedAllocSite;
  }
  uint32_t nurseryAllocCount(JS::TraceKind kind) const {
    return pretenuring.nurseryAllocCount(kind);
  }

#ifdef JSGC_HASH_TABLE_CHECKS
  void checkAllCrossCompartmentWrappersAfterMovingGC();
  void checkStringWrappersAfterMovingGC();

  // Assert that the UniqueId table has been redirected successfully.
  void checkUniqueIdTableAfterMovingGC();

  void checkScriptMapsAfterMovingGC();
#endif

#ifdef DEBUG
  // For testing purposes, return the index of the sweep group which this zone
  // was swept in in the last GC.
  unsigned lastSweepGroupIndex() { return gcSweepGroupIndex; }
#endif

 private:
  js::jit::JitZone* createJitZone(JSContext* cx);

  bool isQueuedForBackgroundSweep() { return isOnList(); }

  void sweepEphemeronTablesAfterMinorGC();

  js::gc::FinalizationObservers* finalizationObservers() {
    return finalizationObservers_.ref().get();
  }
  bool ensureFinalizationObservers();

  bool isOnList() const;
  Zone* nextZone() const;

  friend bool js::CurrentThreadCanAccessZone(Zone* zone);
  friend class js::gc::GCRuntime;
};

}  // namespace JS

namespace js {
namespace gc {
const char* StateName(JS::Zone::GCState state);
}  // namespace gc
}  // namespace js

#endif  // gc_Zone_h
