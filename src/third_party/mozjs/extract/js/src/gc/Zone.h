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

#include <array>

#include "jstypes.h"

#include "ds/Bitmap.h"
#include "gc/ArenaList.h"
#include "gc/Barrier.h"
#include "gc/FindSCCs.h"
#include "gc/GCMarker.h"
#include "gc/NurseryAwareHashMap.h"
#include "gc/Policy.h"
#include "gc/Pretenuring.h"
#include "gc/Statistics.h"
#include "gc/ZoneAllocator.h"
#include "js/GCHashTable.h"
#include "js/Vector.h"
#include "vm/AtomsTable.h"
#include "vm/InvalidatingFuse.h"
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

#ifdef JS_GC_ZEAL

class MissingAllocSites {
 public:
  using SiteMap = JS::GCHashMap<uint32_t, UniquePtr<AllocSite>,
                                DefaultHasher<uint32_t>, SystemAllocPolicy>;

  using ScriptMap = JS::GCHashMap<WeakHeapPtr<JSScript*>, SiteMap,
                                  StableCellHasher<WeakHeapPtr<JSScript*>>,
                                  SystemAllocPolicy>;
  JS::WeakCache<ScriptMap> scriptMap;

  explicit MissingAllocSites(JS::Zone* zone) : scriptMap(zone) {}
};

#endif  // JS_GC_ZEAL

}  // namespace gc

// If two different nursery strings are wrapped into the same zone, and have
// the same contents, then deduplication may make them duplicates.
// `DuplicatesPossible` will allow this and map both wrappers to the same (now
// tenured) source string.
using StringWrapperMap =
    NurseryAwareHashMap<JSString*, JSString*, ZoneAllocPolicy,
                        DuplicatesPossible>;

// Cache for NewMaybeExternalString. It has cache entries for both the
// Latin1 JSInlineString path and JSExternalString.
class MOZ_NON_TEMPORARY_CLASS ExternalStringCache {
  static const size_t NumEntries = 4;
  mozilla::Array<JSExternalString*, NumEntries> externalEntries_;
  mozilla::Array<JSInlineString*, NumEntries> inlineEntries_;

 public:
  ExternalStringCache() { purge(); }

  ExternalStringCache(const ExternalStringCache&) = delete;
  void operator=(const ExternalStringCache&) = delete;

  void purge() {
    externalEntries_ = {};
    inlineEntries_ = {};
  }

  MOZ_ALWAYS_INLINE JSExternalString* lookupExternal(
      const JS::Latin1Char* chars, size_t len) const;
  MOZ_ALWAYS_INLINE JSExternalString* lookupExternal(const char16_t* chars,
                                                     size_t len) const;
  MOZ_ALWAYS_INLINE void putExternal(JSExternalString* s);

  MOZ_ALWAYS_INLINE JSInlineString* lookupInline(const JS::Latin1Char* chars,
                                                 size_t len) const;
  MOZ_ALWAYS_INLINE JSInlineString* lookupInline(const char16_t* chars,
                                                 size_t len) const;
  MOZ_ALWAYS_INLINE void putInline(JSInlineString* s);

 private:
  template <typename CharT>
  MOZ_ALWAYS_INLINE JSExternalString* lookupExternalImpl(const CharT* chars,
                                                         size_t len) const;
  template <typename CharT>
  MOZ_ALWAYS_INLINE JSInlineString* lookupInlineImpl(const CharT* chars,
                                                     size_t len) const;
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

 public:
  FunctionToStringCache() { purge(); }

  FunctionToStringCache(const FunctionToStringCache&) = delete;
  void operator=(const FunctionToStringCache&) = delete;

  void purge() { mozilla::PodArrayZero(entries_); }

  MOZ_ALWAYS_INLINE JSString* lookup(BaseScript* script) const;
  MOZ_ALWAYS_INLINE void put(BaseScript* script, JSString* string);
};

// HashAndLength is a simple class encapsulating the combination of a HashNumber
// and a (string) length into a single 64-bit value. Having them bundled
// together like this enables us to compare pairs of hashes and lengths with a
// single 64-bit comparison.
class HashAndLength {
 public:
  MOZ_ALWAYS_INLINE explicit HashAndLength(uint64_t initialValue = unsetValue())
      : mHashAndLength(initialValue) {}
  MOZ_ALWAYS_INLINE HashAndLength(HashNumber hash, uint32_t length)
      : mHashAndLength(uint64FromHashAndLength(hash, length)) {}

  void MOZ_ALWAYS_INLINE set(HashNumber hash, uint32_t length) {
    mHashAndLength = uint64FromHashAndLength(hash, length);
  }

  constexpr MOZ_ALWAYS_INLINE HashNumber hash() const {
    return hashFromUint64(mHashAndLength);
  }
  constexpr MOZ_ALWAYS_INLINE uint32_t length() const {
    return lengthFromUint64(mHashAndLength);
  }

  constexpr MOZ_ALWAYS_INLINE bool isEqual(HashNumber hash,
                                           uint32_t length) const {
    return mHashAndLength == uint64FromHashAndLength(hash, length);
  }

  // This function is used at compile-time to verify and that we pack and unpack
  // hash and length values consistently.
  static constexpr bool staticChecks() {
    std::array<HashNumber, 5> hashes{0x00000000, 0xffffffff, 0xf0f0f0f0,
                                     0x0f0f0f0f, 0x73737373};
    std::array<uint32_t, 6> lengths{0, 1, 2, 3, 11, 56};

    for (const HashNumber hash : hashes) {
      for (const uint32_t length : lengths) {
        const uint64_t lengthAndHash = uint64FromHashAndLength(hash, length);
        if (hashFromUint64(lengthAndHash) != hash) {
          return false;
        }
        if (lengthFromUint64(lengthAndHash) != length) {
          return false;
        }
      }
    }

    return true;
  }

  static constexpr MOZ_ALWAYS_INLINE uint64_t unsetValue() {
    // This needs to be a combination of hash and length that would never occur
    // together. There is only one string of length zero, and its hash is zero,
    // so the hash here can be anything except zero.
    return uint64FromHashAndLength(0xffffffff, 0);
  }

 private:
  uint64_t mHashAndLength;

  static constexpr MOZ_ALWAYS_INLINE uint64_t
  uint64FromHashAndLength(HashNumber hash, uint32_t length) {
    return (static_cast<uint64_t>(length) << 32) | hash;
  }

  static constexpr MOZ_ALWAYS_INLINE uint32_t
  lengthFromUint64(uint64_t hashAndLength) {
    return static_cast<uint32_t>(hashAndLength >> 32);
  }

  static constexpr MOZ_ALWAYS_INLINE HashNumber
  hashFromUint64(uint64_t hashAndLength) {
    return hashAndLength & 0xffffffff;
  }
};

static_assert(HashAndLength::staticChecks());

// AtomCacheHashTable is a medium-capacity, low-overhead cache for matching
// strings to previously-added JSAtoms.
// This cache is very similar to a typical CPU memory cache. We use the low bits
// of the hash as an index into a table of sets of entries. Cache eviction
// follows a "least recently added" policy.
// All of the operations here are designed to be low-cost and efficient for
// modern CPU architectures. Failed lookups should incur at most one CPU memory
// cache miss and successful lookups should incur at most three (depending on
// whether or not the underlying chararacter buffers are already in the cache).
class AtomCacheHashTable {
 public:
  static MOZ_ALWAYS_INLINE constexpr uint32_t computeIndexFromHash(
      const HashNumber hash) {
    // Simply use the low bits of the hash value as the cache index.
    return hash & (sSize - 1);
  }

  MOZ_ALWAYS_INLINE JSAtom* lookupForAdd(
      const AtomHasher::Lookup& lookup) const {
    MOZ_ASSERT(lookup.atom == nullptr, "Lookup by atom is not supported");

    const uint32_t index = computeIndexFromHash(lookup.hash);

    const EntrySet& entrySet = mEntrySets[index];
    for (const Entry& entry : entrySet.mEntries) {
      JSAtom* const atom = entry.mAtom;

      if (!entry.mHashAndLength.isEqual(lookup.hash, lookup.length)) {
        continue;
      }

      // This is annotated with MOZ_UNLIKELY because it virtually never happens
      // that, after matching the hash and the length, the string isn't a match.
      if (MOZ_UNLIKELY(!lookup.StringsMatch(*atom))) {
        continue;
      }

      return atom;
    }

    return nullptr;
  }

  MOZ_ALWAYS_INLINE void add(const HashNumber hash, JSAtom* atom) {
    const uint32_t index = computeIndexFromHash(hash);

    mEntrySets[index].add(hash, atom->length(), atom);
  }

 private:
  struct Entry {
    MOZ_ALWAYS_INLINE Entry()
        : mHashAndLength(HashAndLength::unsetValue()), mAtom(nullptr) {}

    MOZ_ALWAYS_INLINE void set(const HashNumber hash, const uint32_t length,
                               JSAtom* const atom) {
      mHashAndLength.set(hash, length);
      mAtom = atom;
    }

    // Hash and length are also available, from JSAtom and JSString
    // respectively, but are cached here to avoid likely cache misses in the
    // frequent case of a missed lookup.
    HashAndLength mHashAndLength;
    // No read barrier is required here because the table is cleared at the
    // start of GC.
    JSAtom* mAtom;
  };

  static_assert(sizeof(Entry) <= 16);

  // EntrySet represents a bundling of all of the Entry's that are mapped to the
  // same index.
  // NOTE/TODO: Since we have a tendency to use the entirety of this structure
  // together, it would be really nice to mark this class with alignas(64) to
  // ensure that the entire thing ends up on a single (hardware) cache line but
  // we can't do that because AtomCacheHashTable is allocated with js::UniquePtr
  // which doesn't support alignments greater than 8. In practice, on my Windows
  // machine at least, I am seeing that these objects *are* 64-byte aligned, but
  // it would be nice to guarantee that this will be the case.
  struct EntrySet {
    MOZ_ALWAYS_INLINE void add(const HashNumber hash, const uint32_t length,
                               JSAtom* const atom) {
      MOZ_ASSERT(mEntries[0].mAtom != atom);
      MOZ_ASSERT(mEntries[1].mAtom != atom);
      MOZ_ASSERT(mEntries[2].mAtom != atom);
      MOZ_ASSERT(mEntries[3].mAtom != atom);
      mEntries[3] = mEntries[2];
      mEntries[2] = mEntries[1];
      mEntries[1] = mEntries[0];
      mEntries[0].set(hash, length, atom);
    }

    std::array<Entry, 4> mEntries;
  };

  static_assert(sizeof(EntrySet) <= 64,
                "EntrySet will not fit in a cache line");

  // This value was picked empirically based on performance testing using SP2
  // and SP3. 2k was better than 1k but 4k was not much better than 2k.
  static constexpr uint32_t sSize = 2 * 1024;
  static_assert(mozilla::IsPowerOfTwo(sSize));
  std::array<EntrySet, sSize> mEntrySets;
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
  js::MainThreadOrGCTaskData<js::UniquePtr<js::AtomCacheHashTable>> atomCache_;

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

  js::MainThreadOrGCTaskOrIonCompileData<js::jit::JitZone*> jitZone_;

  // Number of realms in this zone that have a non-null object allocation
  // metadata builder.
  js::MainThreadOrIonCompileData<size_t> numRealmsWithAllocMetadataBuilder_{0};

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

  // To support weak pointers in some special cases we keep a list of objects
  // that need to be traced weakly on GC. This is currently only used for the
  // JIT's ShapeListObject. It's assumed that there will not be many of these
  // objects.
  using ObjectVector = js::GCVector<JSObject*, 0, js::SystemAllocPolicy>;
  js::MainThreadOrGCTaskData<ObjectVector> objectsWithWeakPointers;

 public:
#ifdef JS_GC_ZEAL
  // Must come after weakCaches_ above.
  js::UniquePtr<js::gc::MissingAllocSites> missingSites;
#endif  // JS_GC_ZEAL

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
    bool discardJitScripts = false;
    bool resetNurseryAllocSites = false;
    bool resetPretenuredAllocSites = false;
    JSTracer* traceWeakJitScripts = nullptr;
  };

  void discardJitCode(JS::GCContext* gcx,
                      const DiscardOptions& options = DiscardOptions());

  // Discard JIT code regardless of isPreservingCode().
  void forceDiscardJitCode(JS::GCContext* gcx,
                           const DiscardOptions& options = DiscardOptions());

  void resetAllocSitesAndInvalidate(bool resetNurserySites,
                                    bool resetPretenuredSites);

  void traceWeakJitScripts(JSTracer* trc);

  bool registerObjectWithWeakPointers(JSObject* obj);
  void sweepObjectsWithWeakPointers(JSTracer* trc);

  void addSizeOfIncludingThis(
      mozilla::MallocSizeOf mallocSizeOf, size_t* zoneObject,
      JS::CodeSizes* code, size_t* regexpZone, size_t* jitZone,
      size_t* cacheIRStubs, size_t* uniqueIdMap, size_t* initialPropMapTable,
      size_t* shapeTables, size_t* atomsMarkBitmaps, size_t* compartmentObjects,
      size_t* crossCompartmentWrappersTables, size_t* compartmentsPrivateData,
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
  static constexpr size_t offsetOfJitZone() { return offsetof(Zone, jitZone_); }

  js::jit::JitZone* getJitZone(JSContext* cx) {
    return jitZone_ ? jitZone_ : createJitZone(cx);
  }
  js::jit::JitZone* jitZone() { return jitZone_; }

  bool ensureJitZoneExists(JSContext* cx) { return !!getJitZone(cx); }

  void incNumRealmsWithAllocMetadataBuilder() {
    numRealmsWithAllocMetadataBuilder_++;
  }
  void decNumRealmsWithAllocMetadataBuilder() {
    MOZ_ASSERT(numRealmsWithAllocMetadataBuilder_ > 0);
    numRealmsWithAllocMetadataBuilder_--;
  }
  bool hasRealmWithAllocMetadataBuilder() const {
    return numRealmsWithAllocMetadataBuilder_ > 0;
  }

  void prepareForCompacting();

  void traceRootsInMajorGC(JSTracer* trc);

  void sweepAfterMinorGC(JSTracer* trc);
  void sweepUniqueIds();
  void sweepCompartments(JS::GCContext* gcx, bool keepAtleastOne,
                         bool destroyingRuntime);

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

  // Note that this covers both allocating JSStrings themselves in the nursery,
  // as well as (possibly) the character data.
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

  void beforeClearDelegateInternal(JSObject* wrapper, JSObject* delegate);
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

  // The atom cache is "allocate-on-demand". This function can return nullptr if
  // the allocation failed.
  js::AtomCacheHashTable* atomCache() {
    if (atomCache_.ref()) {
      return atomCache_.ref().get();
    }

    atomCache_ = js::MakeUnique<js::AtomCacheHashTable>();
    return atomCache_.ref().get();
  }

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
  // https://tc39.es/ecma262/#sec-addtokeptobjects
  bool addToKeptObjects(HandleObject target);

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

  // Support for invalidating fuses
  js::DependentScriptGroup fuseDependencies;

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

namespace js::gc {
const char* StateName(JS::Zone::GCState state);
}  // namespace js::gc

#endif  // gc_Zone_h
