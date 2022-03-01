/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sw=2 et tw=80:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Nursery_h
#define gc_Nursery_h

#include "mozilla/ArrayUtils.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/Maybe.h"
#include "mozilla/TimeStamp.h"

#include "gc/GCParallelTask.h"
#include "gc/Heap.h"
#include "js/AllocPolicy.h"
#include "js/Class.h"
#include "js/GCAPI.h"
#include "js/HeapAPI.h"
#include "js/TracingAPI.h"
#include "js/TypeDecls.h"
#include "js/Vector.h"
#include "util/Text.h"

#define FOR_EACH_NURSERY_PROFILE_TIME(_)      \
  /* Key                       Header text */ \
  _(Total, "total")                           \
  _(TraceValues, "mkVals")                    \
  _(TraceCells, "mkClls")                     \
  _(TraceSlots, "mkSlts")                     \
  _(TraceWholeCells, "mcWCll")                \
  _(TraceGenericEntries, "mkGnrc")            \
  _(CheckHashTables, "ckTbls")                \
  _(MarkRuntime, "mkRntm")                    \
  _(MarkDebugger, "mkDbgr")                   \
  _(SweepCaches, "swpCch")                    \
  _(CollectToObjFP, "colObj")                 \
  _(CollectToStrFP, "colStr")                 \
  _(ObjectsTenuredCallback, "tenCB")          \
  _(Sweep, "sweep")                           \
  _(UpdateJitActivations, "updtIn")           \
  _(FreeMallocedBuffers, "frSlts")            \
  _(ClearStoreBuffer, "clrSB")                \
  _(ClearNursery, "clear")                    \
  _(PurgeStringToAtomCache, "pStoA")          \
  _(Pretenure, "pretnr")

template <typename T>
class SharedMem;
class JSDependentString;

namespace js {

struct StringStats;
class AutoLockGCBgAlloc;
class ObjectElements;
class PlainObject;
class NativeObject;
class Nursery;
struct NurseryChunk;
class HeapSlot;
class JSONPrinter;
class MapObject;
class SetObject;

namespace gc {
class AutoMaybeStartBackgroundAllocation;
class AutoTraceSession;
struct Cell;
class GCSchedulingTunables;
class MinorCollectionTracer;
class RelocationOverlay;
class StringRelocationOverlay;
enum class AllocKind : uint8_t;
class TenuredCell;
}  // namespace gc

namespace jit {
class MacroAssembler;
}  // namespace jit

class NurseryDecommitTask : public GCParallelTask {
 public:
  explicit NurseryDecommitTask(gc::GCRuntime* gc);
  bool reserveSpaceForBytes(size_t nbytes);

  bool isEmpty(const AutoLockHelperThreadState& lock) const;

  void queueChunk(NurseryChunk* chunk, const AutoLockHelperThreadState& lock);
  void queueRange(size_t newCapacity, NurseryChunk& chunk,
                  const AutoLockHelperThreadState& lock);

 private:
  using NurseryChunkVector = Vector<NurseryChunk*, 0, SystemAllocPolicy>;

  void run(AutoLockHelperThreadState& lock) override;

  NurseryChunkVector& chunksToDecommit() { return chunksToDecommit_.ref(); }
  const NurseryChunkVector& chunksToDecommit() const {
    return chunksToDecommit_.ref();
  }

  MainThreadOrGCTaskData<NurseryChunkVector> chunksToDecommit_;

  MainThreadOrGCTaskData<NurseryChunk*> partialChunk;
  MainThreadOrGCTaskData<size_t> partialCapacity;
};

class TenuringTracer final : public GenericTracer {
  friend class Nursery;
  Nursery& nursery_;

  // Amount of data moved to the tenured generation during collection.
  size_t tenuredSize;
  // Number of cells moved to the tenured generation.
  size_t tenuredCells;

  // These lists are threaded through the Nursery using the space from
  // already moved things. The lists are used to fix up the moved things and
  // to find things held live by intra-Nursery pointers.
  gc::RelocationOverlay* objHead;
  gc::RelocationOverlay** objTail;
  gc::StringRelocationOverlay* stringHead;
  gc::StringRelocationOverlay** stringTail;

  TenuringTracer(JSRuntime* rt, Nursery* nursery);

  JSObject* onObjectEdge(JSObject* obj) override;
  JSString* onStringEdge(JSString* str) override;
  JS::Symbol* onSymbolEdge(JS::Symbol* sym) override;
  JS::BigInt* onBigIntEdge(JS::BigInt* bi) override;
  js::BaseScript* onScriptEdge(BaseScript* script) override;
  js::Shape* onShapeEdge(Shape* shape) override;
  js::RegExpShared* onRegExpSharedEdge(RegExpShared* shared) override;
  js::BaseShape* onBaseShapeEdge(BaseShape* base) override;
  js::GetterSetter* onGetterSetterEdge(GetterSetter* gs) override;
  js::PropMap* onPropMapEdge(PropMap* map) override;
  js::jit::JitCode* onJitCodeEdge(jit::JitCode* code) override;
  js::Scope* onScopeEdge(Scope* scope) override;

 public:
  Nursery& nursery() { return nursery_; }

  void traverse(JS::Value* thingp);

  // The store buffers need to be able to call these directly.
  void traceObject(JSObject* src);
  void traceObjectSlots(NativeObject* nobj, uint32_t start, uint32_t end);
  void traceSlots(JS::Value* vp, uint32_t nslots);
  void traceString(JSString* src);
  void traceBigInt(JS::BigInt* src);

 private:
  inline void insertIntoObjectFixupList(gc::RelocationOverlay* entry);
  inline void insertIntoStringFixupList(gc::StringRelocationOverlay* entry);

  template <typename T>
  inline T* allocTenured(JS::Zone* zone, gc::AllocKind kind);
  JSString* allocTenuredString(JSString* src, JS::Zone* zone,
                               gc::AllocKind dstKind);

  inline JSObject* movePlainObjectToTenured(PlainObject* src);
  JSObject* moveToTenuredSlow(JSObject* src);
  JSString* moveToTenured(JSString* src);
  JS::BigInt* moveToTenured(JS::BigInt* src);

  size_t moveElementsToTenured(NativeObject* dst, NativeObject* src,
                               gc::AllocKind dstKind);
  size_t moveSlotsToTenured(NativeObject* dst, NativeObject* src);
  size_t moveStringToTenured(JSString* dst, JSString* src,
                             gc::AllocKind dstKind);
  size_t moveBigIntToTenured(JS::BigInt* dst, JS::BigInt* src,
                             gc::AllocKind dstKind);

  void traceSlots(JS::Value* vp, JS::Value* end);
};

// Classes with JSCLASS_SKIP_NURSERY_FINALIZE or Wrapper classes with
// CROSS_COMPARTMENT flags will not have their finalizer called if they are
// nursery allocated and not promoted to the tenured heap. The finalizers for
// these classes must do nothing except free data which was allocated via
// Nursery::allocateBuffer.
inline bool CanNurseryAllocateFinalizedClass(const JSClass* const clasp) {
  MOZ_ASSERT(clasp->hasFinalize());
  return clasp->flags & JSCLASS_SKIP_NURSERY_FINALIZE;
}

class Nursery {
 public:
  static const size_t Alignment = gc::ChunkSize;
  static const size_t ChunkShift = gc::ChunkShift;

  using BufferRelocationOverlay = void*;
  using BufferSet = HashSet<void*, PointerHasher<void*>, SystemAllocPolicy>;

  explicit Nursery(gc::GCRuntime* gc);
  ~Nursery();

  [[nodiscard]] bool init(AutoLockGCBgAlloc& lock);

  // Number of allocated (ready to use) chunks.
  unsigned allocatedChunkCount() const { return chunks_.length(); }

  // Total number of chunks and the capacity of the nursery. Chunks will be
  // lazilly allocated and added to the chunks array up to this limit, after
  // that the nursery must be collected, this limit may be raised during
  // collection.
  unsigned maxChunkCount() const {
    MOZ_ASSERT(capacity());
    return HowMany(capacity(), gc::ChunkSize);
  }

  void enable();
  void disable();
  bool isEnabled() const { return capacity() != 0; }

  void enableStrings();
  void disableStrings();
  bool canAllocateStrings() const { return canAllocateStrings_; }

  void enableBigInts();
  void disableBigInts();
  bool canAllocateBigInts() const { return canAllocateBigInts_; }

  // Return true if no allocations have been made since the last collection.
  bool isEmpty() const;

  // Check whether an arbitrary pointer is within the nursery. This is
  // slower than IsInsideNursery(Cell*), but works on all types of pointers.
  MOZ_ALWAYS_INLINE bool isInside(gc::Cell* cellp) const = delete;
  MOZ_ALWAYS_INLINE bool isInside(const void* p) const {
    for (auto chunk : chunks_) {
      if (uintptr_t(p) - uintptr_t(chunk) < gc::ChunkSize) {
        return true;
      }
    }
    return false;
  }

  template <typename T>
  inline bool isInside(const SharedMem<T>& p) const;

  // Allocate and return a pointer to a new GC object with its |slots|
  // pointer pre-filled. Returns nullptr if the Nursery is full.
  JSObject* allocateObject(gc::AllocSite* site, size_t size,
                           size_t numDynamicSlots, const JSClass* clasp);

  // Allocate and return a pointer to a new GC thing. Returns nullptr if the
  // Nursery is full.
  gc::Cell* allocateCell(gc::AllocSite* site, size_t size, JS::TraceKind kind);

  gc::Cell* allocateBigInt(gc::AllocSite* site, size_t size) {
    return allocateCell(site, size, JS::TraceKind::BigInt);
  }
  gc::Cell* allocateString(gc::AllocSite* site, size_t size);

  static size_t nurseryCellHeaderSize() {
    return sizeof(gc::NurseryCellHeader);
  }

  // Allocate a buffer for a given zone, using the nursery if possible.
  void* allocateBuffer(JS::Zone* zone, size_t nbytes);

  // Allocate a buffer for a given object, using the nursery if possible and
  // obj is in the nursery.
  void* allocateBuffer(JSObject* obj, size_t nbytes);

  // Allocate a buffer for a given object, always using the nursery if obj is
  // in the nursery. The requested size must be less than or equal to
  // MaxNurseryBufferSize.
  void* allocateBufferSameLocation(JSObject* obj, size_t nbytes);

  // Allocate a zero-initialized buffer for a given zone, using the nursery if
  // possible. If the buffer isn't allocated in the nursery, the given arena is
  // used.
  void* allocateZeroedBuffer(JS::Zone* zone, size_t nbytes,
                             arena_id_t arena = js::MallocArena);

  // Allocate a zero-initialized buffer for a given object, using the nursery if
  // possible and obj is in the nursery. If the buffer isn't allocated in the
  // nursery, the given arena is used.
  void* allocateZeroedBuffer(JSObject* obj, size_t nbytes,
                             arena_id_t arena = js::MallocArena);

  // Resize an existing buffer.
  void* reallocateBuffer(JS::Zone* zone, gc::Cell* cell, void* oldBuffer,
                         size_t oldBytes, size_t newBytes);

  // Allocate a digits buffer for a given BigInt, using the nursery if possible
  // and |bi| is in the nursery.
  void* allocateBuffer(JS::BigInt* bi, size_t nbytes);

  // Free an object buffer.
  void freeBuffer(void* buffer, size_t nbytes);

  // The maximum number of bytes allowed to reside in nursery buffers.
  static const size_t MaxNurseryBufferSize = 1024;

  // Do a minor collection.
  void collect(JS::GCOptions options, JS::GCReason reason);

  // If the thing at |*ref| in the Nursery has been forwarded, set |*ref| to
  // the new location and return true. Otherwise return false and leave
  // |*ref| unset.
  [[nodiscard]] MOZ_ALWAYS_INLINE static bool getForwardedPointer(
      js::gc::Cell** ref);

  // Forward a slots/elements pointer stored in an Ion frame.
  void forwardBufferPointer(uintptr_t* pSlotsElems);

  inline void maybeSetForwardingPointer(JSTracer* trc, void* oldData,
                                        void* newData, bool direct);
  inline void setForwardingPointerWhileTenuring(void* oldData, void* newData,
                                                bool direct);

  // Register a malloced buffer that is held by a nursery object, which
  // should be freed at the end of a minor GC. Buffers are unregistered when
  // their owning objects are tenured.
  [[nodiscard]] bool registerMallocedBuffer(void* buffer, size_t nbytes);

  // Mark a malloced buffer as no longer needing to be freed.
  void removeMallocedBuffer(void* buffer, size_t nbytes) {
    MOZ_ASSERT(mallocedBuffers.has(buffer));
    MOZ_ASSERT(nbytes > 0);
    MOZ_ASSERT(mallocedBufferBytes >= nbytes);
    mallocedBuffers.remove(buffer);
    mallocedBufferBytes -= nbytes;
  }

  // Mark a malloced buffer as no longer needing to be freed during minor
  // GC. There's no need to account for the size here since all remaining
  // buffers will soon be freed.
  void removeMallocedBufferDuringMinorGC(void* buffer) {
    MOZ_ASSERT(JS::RuntimeHeapIsMinorCollecting());
    MOZ_ASSERT(mallocedBuffers.has(buffer));
    mallocedBuffers.remove(buffer);
  }

  [[nodiscard]] bool addedUniqueIdToCell(gc::Cell* cell) {
    MOZ_ASSERT(IsInsideNursery(cell));
    MOZ_ASSERT(isEnabled());
    return cellsWithUid_.append(cell);
  }

  size_t sizeOfMallocedBuffers(mozilla::MallocSizeOf mallocSizeOf) const {
    size_t total = 0;
    for (BufferSet::Range r = mallocedBuffers.all(); !r.empty(); r.popFront()) {
      total += mallocSizeOf(r.front());
    }
    total += mallocedBuffers.shallowSizeOfExcludingThis(mallocSizeOf);
    return total;
  }

  // The number of bytes from the start position to the end of the nursery.
  // pass maxChunkCount(), allocatedChunkCount() or chunkCountLimit()
  // to calculate the nursery size, current lazy-allocated size or nursery
  // limit respectively.
  size_t spaceToEnd(unsigned chunkCount) const;

  size_t capacity() const { return capacity_; }
  size_t committed() const { return spaceToEnd(allocatedChunkCount()); }

  // Used and free space both include chunk headers for that part of the
  // nursery.
  //
  // usedSpace() + freeSpace() == capacity()
  //
  MOZ_ALWAYS_INLINE size_t usedSpace() const {
    return capacity() - freeSpace();
  }
  MOZ_ALWAYS_INLINE size_t freeSpace() const {
    MOZ_ASSERT(isEnabled());
    MOZ_ASSERT(currentEnd_ - position_ <= NurseryChunkUsableSize);
    MOZ_ASSERT(currentChunk_ < maxChunkCount());
    return (currentEnd_ - position_) +
           (maxChunkCount() - currentChunk_ - 1) * gc::ChunkSize;
  }

#ifdef JS_GC_ZEAL
  void enterZealMode();
  void leaveZealMode();
#endif

  // Write profile time JSON on JSONPrinter.
  void renderProfileJSON(JSONPrinter& json) const;

  // Print header line for profile times.
  static void printProfileHeader();

  // Print total profile times on shutdown.
  void printTotalProfileTimes();

  void* addressOfPosition() const { return (void**)&position_; }
  const void* addressOfCurrentEnd() const { return (void**)&currentEnd_; }
  const void* addressOfCurrentStringEnd() const {
    return (void*)&currentStringEnd_;
  }
  const void* addressOfCurrentBigIntEnd() const {
    return (void*)&currentBigIntEnd_;
  }
  void* addressOfNurseryAllocatedSites() {
    return pretenuringNursery.addressOfAllocatedSites();
  }

  void requestMinorGC(JS::GCReason reason) const;

  bool minorGCRequested() const {
    return minorGCTriggerReason_ != JS::GCReason::NO_REASON;
  }
  JS::GCReason minorGCTriggerReason() const { return minorGCTriggerReason_; }
  void clearMinorGCRequest() {
    minorGCTriggerReason_ = JS::GCReason::NO_REASON;
  }

  bool shouldCollect() const;
  bool isNearlyFull() const;
  bool isUnderused() const;

  bool enableProfiling() const { return enableProfiling_; }

  bool addMapWithNurseryMemory(MapObject* obj) {
    MOZ_ASSERT_IF(!mapsWithNurseryMemory_.empty(),
                  mapsWithNurseryMemory_.back() != obj);
    return mapsWithNurseryMemory_.append(obj);
  }
  bool addSetWithNurseryMemory(SetObject* obj) {
    MOZ_ASSERT_IF(!setsWithNurseryMemory_.empty(),
                  setsWithNurseryMemory_.back() != obj);
    return setsWithNurseryMemory_.append(obj);
  }

  // The amount of space in the mapped nursery available to allocations.
  static const size_t NurseryChunkUsableSize =
      gc::ChunkSize - sizeof(gc::ChunkBase);

  void joinDecommitTask() { decommitTask.join(); }

  mozilla::TimeStamp collectionStartTime() {
    return startTimes_[ProfileKey::Total];
  }

  bool canCreateAllocSite() { return pretenuringNursery.canCreateAllocSite(); }
  void noteAllocSiteCreated() { pretenuringNursery.noteAllocSiteCreated(); }
  bool reportPretenuring() const { return reportPretenuring_; }
  void maybeStopPretenuring(gc::GCRuntime* gc) {
    pretenuringNursery.maybeStopPretenuring(gc);
  }

  // Round a size in bytes to the nearest valid nursery size.
  static size_t roundSize(size_t size);

 private:
  gc::GCRuntime* const gc;

  // Vector of allocated chunks to allocate from.
  Vector<NurseryChunk*, 0, SystemAllocPolicy> chunks_;

  // Pointer to the first unallocated byte in the nursery.
  uintptr_t position_;

  // These fields refer to the beginning of the nursery. They're normally 0
  // and chunk(0).start() respectively. Except when a generational GC zeal
  // mode is active, then they may be arbitrary (see Nursery::clear()).
  unsigned currentStartChunk_;
  uintptr_t currentStartPosition_;

  // Pointer to the last byte of space in the current chunk.
  uintptr_t currentEnd_;

  // Pointer to the last byte of space in the current chunk, or nullptr if we
  // are not allocating strings in the nursery.
  uintptr_t currentStringEnd_;

  // Pointer to the last byte of space in the current chunk, or nullptr if we
  // are not allocating BigInts in the nursery.
  uintptr_t currentBigIntEnd_;

  // The index of the chunk that is currently being allocated from.
  unsigned currentChunk_;

  // The current nursery capacity measured in bytes. It may grow up to this
  // value without a collection, allocating chunks on demand. This limit may be
  // changed by maybeResizeNursery() each collection. It includes chunk headers.
  size_t capacity_;

  gc::PretenuringNursery pretenuringNursery;

  mozilla::TimeDuration timeInChunkAlloc_;

  // Report minor collections taking at least this long, if enabled.
  bool enableProfiling_;
  bool profileWorkers_;
  mozilla::TimeDuration profileThreshold_;

  // Whether we will nursery-allocate strings.
  bool canAllocateStrings_;

  // Whether we will nursery-allocate BigInts.
  bool canAllocateBigInts_;

  // Report how many strings were deduplicated.
  bool reportDeduplications_;

  // Report information on allocation sites and pretenuring.
  bool reportPretenuring_;

  // Whether and why a collection of this nursery has been requested. This is
  // mutable as it is set by the store buffer, which otherwise cannot modify
  // anything in the nursery.
  mutable JS::GCReason minorGCTriggerReason_;

  // Profiling data.

  enum class ProfileKey {
#define DEFINE_TIME_KEY(name, text) name,
    FOR_EACH_NURSERY_PROFILE_TIME(DEFINE_TIME_KEY)
#undef DEFINE_TIME_KEY
        KeyCount
  };

  using ProfileTimes =
      mozilla::EnumeratedArray<ProfileKey, ProfileKey::KeyCount,
                               mozilla::TimeStamp>;
  using ProfileDurations =
      mozilla::EnumeratedArray<ProfileKey, ProfileKey::KeyCount,
                               mozilla::TimeDuration>;

  ProfileTimes startTimes_;
  ProfileDurations profileDurations_;
  ProfileDurations totalDurations_;

  // Data about the previous collection.
  struct PreviousGC {
    JS::GCReason reason = JS::GCReason::NO_REASON;
    size_t nurseryCapacity = 0;
    size_t nurseryCommitted = 0;
    size_t nurseryUsedBytes = 0;
    size_t tenuredBytes = 0;
    size_t tenuredCells = 0;
    mozilla::TimeStamp endTime;
  };
  PreviousGC previousGC;

  bool hasRecentGrowthData;
  double smoothedGrowthFactor;

  // Calculate the promotion rate of the most recent minor GC.
  // The valid_for_tenuring parameter is used to return whether this
  // promotion rate is accurate enough (the nursery was full enough) to be
  // used for tenuring and other decisions.
  //
  // Must only be called if the previousGC data is initialised.
  double calcPromotionRate(bool* validForTenuring) const;

  // The set of externally malloced buffers potentially kept live by objects
  // stored in the nursery. Any external buffers that do not belong to a
  // tenured thing at the end of a minor GC must be freed.
  BufferSet mallocedBuffers;
  size_t mallocedBufferBytes = 0;

  // During a collection most hoisted slot and element buffers indicate their
  // new location with a forwarding pointer at the base. This does not work
  // for buffers whose length is less than pointer width, or when different
  // buffers might overlap each other. For these, an entry in the following
  // table is used.
  typedef HashMap<void*, void*, PointerHasher<void*>, SystemAllocPolicy>
      ForwardedBufferMap;
  ForwardedBufferMap forwardedBuffers;

  // When we assign a unique id to cell in the nursery, that almost always
  // means that the cell will be in a hash table, and thus, held live,
  // automatically moving the uid from the nursery to its new home in
  // tenured. It is possible, if rare, for an object that acquired a uid to
  // be dead before the next collection, in which case we need to know to
  // remove it when we sweep.
  //
  // Note: we store the pointers as Cell* here, resulting in an ugly cast in
  //       sweep. This is because this structure is used to help implement
  //       stable object hashing and we have to break the cycle somehow.
  using CellsWithUniqueIdVector = Vector<gc::Cell*, 8, SystemAllocPolicy>;
  CellsWithUniqueIdVector cellsWithUid_;

  template <typename Key>
  struct DeduplicationStringHasher {
    using Lookup = Key;

    static inline HashNumber hash(const Lookup& lookup) {
      JS::AutoCheckCannotGC nogc;
      HashNumber strHash;

      // Include flags in the hash. A string relocation overlay stores either
      // the nursery root base chars or the dependent string nursery base, but
      // does not indicate which one. If strings with different string types
      // were deduplicated, for example, a dependent string gets deduplicated
      // into an extensible string, the base chain would be broken and the root
      // base would be unreachable.

      if (lookup->asLinear().hasLatin1Chars()) {
        strHash = mozilla::HashString(lookup->asLinear().latin1Chars(nogc),
                                      lookup->length());
      } else {
        MOZ_ASSERT(lookup->asLinear().hasTwoByteChars());
        strHash = mozilla::HashString(lookup->asLinear().twoByteChars(nogc),
                                      lookup->length());
      }

      return mozilla::HashGeneric(strHash, lookup->zone(), lookup->flags());
    }

    static MOZ_ALWAYS_INLINE bool match(const Key& key, const Lookup& lookup) {
      if (!key->sameLengthAndFlags(*lookup) ||
          key->asTenured().zone() != lookup->zone() ||
          key->asTenured().getAllocKind() != lookup->getAllocKind()) {
        return false;
      }

      JS::AutoCheckCannotGC nogc;

      if (key->asLinear().hasLatin1Chars()) {
        MOZ_ASSERT(lookup->asLinear().hasLatin1Chars());
        return mozilla::ArrayEqual(key->asLinear().latin1Chars(nogc),
                                   lookup->asLinear().latin1Chars(nogc),
                                   lookup->length());
      } else {
        MOZ_ASSERT(key->asLinear().hasTwoByteChars());
        MOZ_ASSERT(lookup->asLinear().hasTwoByteChars());
        return EqualChars(key->asLinear().twoByteChars(nogc),
                          lookup->asLinear().twoByteChars(nogc),
                          lookup->length());
      }
    }
  };

  using StringDeDupSet =
      HashSet<JSString*, DeduplicationStringHasher<JSString*>,
              SystemAllocPolicy>;

  // deDupSet is emplaced at the beginning of the nursery collection and reset
  // at the end of the nursery collection. It can also be reset during nursery
  // collection when out of memory to insert new entries.
  mozilla::Maybe<StringDeDupSet> stringDeDupSet;

  // Lists of map and set objects allocated in the nursery or with iterators
  // allocated there. Such objects need to be swept after minor GC.
  Vector<MapObject*, 0, SystemAllocPolicy> mapsWithNurseryMemory_;
  Vector<SetObject*, 0, SystemAllocPolicy> setsWithNurseryMemory_;

  NurseryDecommitTask decommitTask;

#ifdef JS_GC_ZEAL
  struct Canary;
  Canary* lastCanary_;
#endif

  NurseryChunk& chunk(unsigned index) const { return *chunks_[index]; }

  // Set the current chunk. This updates the currentChunk_, position_
  // currentEnd_ and currentStringEnd_ values as approprite. It'll also
  // poison the chunk, either a portion of the chunk if it is already the
  // current chunk, or the whole chunk if fullPoison is true or it is not
  // the current chunk.
  void setCurrentChunk(unsigned chunkno);

  bool initFirstChunk(AutoLockGCBgAlloc& lock);

  // extent is advisory, it will be ignored in sub-chunk and generational zeal
  // modes. It will be clamped to Min(NurseryChunkUsableSize, capacity_).
  void poisonAndInitCurrentChunk(size_t extent = gc::ChunkSize);

  void setCurrentEnd();
  void setStartPosition();

  // Allocate the next chunk, or the first chunk for initialization.
  // Callers will probably want to call setCurrentChunk(0) next.
  [[nodiscard]] bool allocateNextChunk(unsigned chunkno,
                                       AutoLockGCBgAlloc& lock);

  MOZ_ALWAYS_INLINE uintptr_t currentEnd() const;

  uintptr_t position() const { return position_; }

  MOZ_ALWAYS_INLINE bool isSubChunkMode() const;

  JSRuntime* runtime() const;
  gcstats::Statistics& stats() const;

  const js::gc::GCSchedulingTunables& tunables() const;

  // Common internal allocator function.
  void* allocate(size_t size);

  void* moveToNextChunkAndAllocate(size_t size);

#ifdef JS_GC_ZEAL
  void writeCanary(uintptr_t address);
#endif

  struct CollectionResult {
    size_t tenuredBytes;
    size_t tenuredCells;
  };
  CollectionResult doCollection(JS::GCReason reason);

  size_t doPretenuring(JSRuntime* rt, JS::GCReason reason,
                       bool validPromotionRate, double promotionRate);

  // Move all objects and everything they can reach to the tenured heap.
  void collectToObjectFixedPoint(TenuringTracer& mover);

  // Move all strings and all strings they can reach to the tenured heap, and
  // additionally do any fixups for when strings are pointing into memory that
  // was deduplicated.
  void collectToStringFixedPoint(TenuringTracer& mover);

  // The dependent string chars needs to be relocated if the base which it's
  // using chars from has been deduplicated.
  template <typename CharT>
  void relocateDependentStringChars(JSDependentString* tenuredDependentStr,
                                    JSLinearString* baseOrRelocOverlay,
                                    size_t* offset,
                                    bool* rootBaseNotYetForwarded,
                                    JSLinearString** rootBase);

  // Handle relocation of slots/elements pointers stored in Ion frames.
  inline void setForwardingPointer(void* oldData, void* newData, bool direct);

  inline void setDirectForwardingPointer(void* oldData, void* newData);
  void setIndirectForwardingPointer(void* oldData, void* newData);

  inline void setSlotsForwardingPointer(HeapSlot* oldSlots, HeapSlot* newSlots,
                                        uint32_t nslots);
  inline void setElementsForwardingPointer(ObjectElements* oldHeader,
                                           ObjectElements* newHeader,
                                           uint32_t capacity);

#ifdef DEBUG
  bool checkForwardingPointerLocation(void* ptr, bool expectedInside);
#endif

  // Updates pointers to nursery objects that have been tenured and discards
  // pointers to objects that have been freed.
  void sweep(JSTracer* trc);

  // Reset the current chunk and position after a minor collection. Also poison
  // the nursery on debug & nightly builds.
  void clear();

  void sweepMapAndSetObjects();

  // Change the allocable space provided by the nursery.
  void maybeResizeNursery(JS::GCOptions options, JS::GCReason reason);
  size_t targetSize(JS::GCOptions options, JS::GCReason reason);
  void clearRecentGrowthData();
  void growAllocableSpace(size_t newCapacity);
  void shrinkAllocableSpace(size_t newCapacity);
  void minimizeAllocableSpace();

  // Free the chunks starting at firstFreeChunk until the end of the chunks
  // vector. Shrinks the vector but does not update maxChunkCount().
  void freeChunksFrom(unsigned firstFreeChunk);

  void sendTelemetry(JS::GCReason reason, mozilla::TimeDuration totalTime,
                     bool wasEmpty, double promotionRate,
                     size_t sitesPretenured);

  void printCollectionProfile(JS::GCReason reason, double promotionRate);
  void printDeduplicationData(js::StringStats& prev, js::StringStats& curr);

  // Profile recording and printing.
  void maybeClearProfileDurations();
  void startProfile(ProfileKey key);
  void endProfile(ProfileKey key);
  static void printProfileDurations(const ProfileDurations& times);

  mozilla::TimeStamp collectionStartTime() const;
  mozilla::TimeStamp lastCollectionEndTime() const;

  friend class TenuringTracer;
  friend class gc::MinorCollectionTracer;
  friend class jit::MacroAssembler;
  friend struct NurseryChunk;
};

}  // namespace js

#endif  // gc_Nursery_h
