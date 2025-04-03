/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sw=2 et tw=80:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Nursery_h
#define gc_Nursery_h

#include "mozilla/EnumeratedArray.h"
#include "mozilla/TimeStamp.h"

#include "gc/Heap.h"
#include "gc/MallocedBlockCache.h"
#include "gc/Pretenuring.h"
#include "js/AllocPolicy.h"
#include "js/Class.h"
#include "js/GCAPI.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
#include "js/Vector.h"

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
  _(FreeTrailerBlocks, "frTrBs")              \
  _(ClearStoreBuffer, "clrSB")                \
  _(ClearNursery, "clear")                    \
  _(PurgeStringToAtomCache, "pStoA")          \
  _(Pretenure, "pretnr")

template <typename T>
class SharedMem;

namespace js {

struct StringStats;
class AutoLockGCBgAlloc;
class ObjectElements;
struct NurseryChunk;
class HeapSlot;
class JSONPrinter;
class MapObject;
class SetObject;
class JS_PUBLIC_API Sprinter;

namespace gc {
class AutoGCSession;
struct Cell;
class GCSchedulingTunables;
class TenuringTracer;
}  // namespace gc

class Nursery {
 public:
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

  // Allocate and return a pointer to a new GC thing. Returns nullptr if the
  // Nursery is full.
  void* allocateCell(gc::AllocSite* site, size_t size, JS::TraceKind kind);

  static size_t nurseryCellHeaderSize() {
    return sizeof(gc::NurseryCellHeader);
  }

  // Allocate a buffer for a given zone, using the nursery if possible.
  void* allocateBuffer(JS::Zone* zone, size_t nbytes);

  // Allocate a buffer for a given object, using the nursery if possible and
  // obj is in the nursery.
  void* allocateBuffer(JS::Zone* zone, JSObject* obj, size_t nbytes);

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

  size_t sizeOfMallocedBuffers(mozilla::MallocSizeOf mallocSizeOf) const;

  // Wasm "trailer" (C++-heap-allocated) blocks.
  //
  // All involved blocks are allocated/deallocated via this nursery's
  // `mallocedBlockCache_`.  Hence we must store both the block address and
  // its freelist ID, wrapped up in a PointerAndUint7.
  //
  // Trailer blocks registered here are added to `trailersAdded_`.  Those that
  // are later deregistered as a result of `obj_moved` calls that indicate
  // tenuring, should be added to `trailersRemoved_`.
  //
  // Unfortunately ::unregisterTrailer cannot be allowed to OOM.  To get
  // around this we rely on the observation that all deregistered blocks
  // should previously have been registered, so the deregistered set can never
  // be larger than the registered set.  Hence ::registerTrailer effectively
  // preallocates space in `trailersRemoved_` so as to ensure that, in the
  // worst case, all registered blocks can be handed to ::unregisterTrailer
  // without needing to resize `trailersRemoved_` in ::unregisterTrailer.
  //
  // The downside is that most of the space in `trailersRemoved_` is wasted in
  // the case where there are few blocks deregistered.  This is unfortunate
  // but it's hard to see how to avoid it.
  //
  // At the end of a minor collection, all blocks in the set `trailersAdded_ -
  // trailersRemoved_[0 .. trailersRemovedUsed_ - 1]` are handed back to the
  // `mallocedBlockCache_`.
  [[nodiscard]] bool registerTrailer(PointerAndUint7 blockAndListID,
                                     size_t nBytes) {
    MOZ_ASSERT(trailersAdded_.length() == trailersRemoved_.length());
    MOZ_ASSERT(nBytes > 0);
    if (MOZ_UNLIKELY(!trailersAdded_.append(blockAndListID))) {
      return false;
    }
    if (MOZ_UNLIKELY(!trailersRemoved_.append(nullptr))) {
      trailersAdded_.popBack();
      return false;
    }

    // This is a clone of the logic in ::registerMallocedBuffer.  It may be
    // that some other heuristic is better, once we know more about the
    // typical behaviour of wasm-GC applications.
    trailerBytes_ += nBytes;
    if (MOZ_UNLIKELY(trailerBytes_ > capacity() * 8)) {
      requestMinorGC(JS::GCReason::NURSERY_TRAILERS);
    }
    return true;
  }

  void unregisterTrailer(void* block) {
    MOZ_ASSERT(trailersRemovedUsed_ < trailersRemoved_.length());
    trailersRemoved_[trailersRemovedUsed_] = block;
    trailersRemovedUsed_++;
  }

  size_t sizeOfTrailerBlockSets(mozilla::MallocSizeOf mallocSizeOf) const;

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
  void printProfileHeader();

  // Print total profile times on shutdown.
  void printTotalProfileTimes();

  void* addressOfPosition() const { return (void**)&position_; }
  static constexpr int32_t offsetOfCurrentEndFromPosition() {
    return offsetof(Nursery, currentEnd_) - offsetof(Nursery, position_);
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

  void joinDecommitTask();

  mozilla::TimeStamp collectionStartTime() {
    return startTimes_[ProfileKey::Total];
  }

  bool canCreateAllocSite() { return pretenuringNursery.canCreateAllocSite(); }
  void noteAllocSiteCreated() { pretenuringNursery.noteAllocSiteCreated(); }
  bool reportPretenuring() const { return reportPretenuring_; }
  void maybeStopPretenuring(gc::GCRuntime* gc) {
    pretenuringNursery.maybeStopPretenuring(gc);
  }

  void setAllocFlagsForZone(JS::Zone* zone);

  // Round a size in bytes to the nearest valid nursery size.
  static size_t roundSize(size_t size);

  // The malloc'd block cache.
  gc::MallocedBlockCache& mallocedBlockCache() { return mallocedBlockCache_; }
  size_t sizeOfMallocedBlockCache(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocedBlockCache_.sizeOfExcludingThis(mallocSizeOf);
  }

 private:
  // Fields used during allocation fast path are grouped first:

  // Pointer to the first unallocated byte in the nursery.
  uintptr_t position_;

  // Pointer to the last byte of space in the current chunk.
  uintptr_t currentEnd_;

  // Other fields not necessarily used during allocation follow:

  gc::GCRuntime* const gc;

  // Vector of allocated chunks to allocate from.
  Vector<NurseryChunk*, 0, SystemAllocPolicy> chunks_;

  // The index of the chunk that is currently being allocated from.
  uint32_t currentChunk_;

  // These fields refer to the beginning of the nursery. They're normally 0
  // and chunk(0).start() respectively. Except when a generational GC zeal
  // mode is active, then they may be arbitrary (see Nursery::clear()).
  uint32_t currentStartChunk_;
  uintptr_t currentStartPosition_;

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

  // Whether to report information on pretenuring, and if so the allocation
  // threshold at which to report details of each allocation site.
  bool reportPretenuring_;
  size_t reportPretenuringThreshold_;

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
    size_t nurseryUsedChunkCount = 0;
    size_t tenuredBytes = 0;
    size_t tenuredCells = 0;
    mozilla::TimeStamp endTime;
  };
  PreviousGC previousGC;

  bool hasRecentGrowthData;
  double smoothedTargetSize;

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
  using BufferRelocationOverlay = void*;
  using BufferSet = HashSet<void*, PointerHasher<void*>, SystemAllocPolicy>;
  BufferSet mallocedBuffers;
  size_t mallocedBufferBytes = 0;

  // Wasm "trailer" (C++-heap-allocated) blocks.  See comments above on
  // ::registerTrailer and ::unregisterTrailer.
  Vector<PointerAndUint7, 0, SystemAllocPolicy> trailersAdded_;
  Vector<void*, 0, SystemAllocPolicy> trailersRemoved_;
  size_t trailersRemovedUsed_ = 0;
  size_t trailerBytes_ = 0;

  void freeTrailerBlocks();

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

  // Lists of map and set objects allocated in the nursery or with iterators
  // allocated there. Such objects need to be swept after minor GC.
  Vector<MapObject*, 0, SystemAllocPolicy> mapsWithNurseryMemory_;
  Vector<SetObject*, 0, SystemAllocPolicy> setsWithNurseryMemory_;

  UniquePtr<NurseryDecommitTask> decommitTask;

  // A cache of small C++-heap allocated blocks associated with this Nursery.
  // This provided so as to provide cheap allocation/deallocation of
  // out-of-line storage areas as used by WasmStructObject and
  // WasmArrayObject, although the mechanism is general and not specific to
  // these object types.  Regarding lifetimes, because the cache holds only
  // blocks that are not currently in use, it can be flushed at any point with
  // no correctness impact, only a performance impact.
  gc::MallocedBlockCache mallocedBlockCache_;

  NurseryChunk& chunk(unsigned index) const { return *chunks_[index]; }

  // Set the current chunk. This updates the currentChunk_, position_ and
  // currentEnd_ values as appropriate. It'll also poison the chunk, either a
  // portion of the chunk if it is already the current chunk, or the whole chunk
  // if fullPoison is true or it is not the current chunk.
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

  void getAllocFlagsForZone(JS::Zone* zone, bool* allocObjectsOut,
                            bool* allocStringsOut, bool* allocBigIntsOut);
  void updateAllZoneAllocFlags();
  void updateAllocFlagsForZone(JS::Zone* zone);
  void discardCodeAndSetJitFlagsForZone(JS::Zone* zone);

  // Common internal allocator function.
  void* allocate(size_t size);

  void* moveToNextChunkAndAllocate(size_t size);

  struct CollectionResult {
    size_t tenuredBytes;
    size_t tenuredCells;
  };
  CollectionResult doCollection(gc::AutoGCSession& session,
                                JS::GCOptions options, JS::GCReason reason);
  void traceRoots(gc::AutoGCSession& session, gc::TenuringTracer& mover);

  size_t doPretenuring(JSRuntime* rt, JS::GCReason reason,
                       bool validPromotionRate, double promotionRate);

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
  void sweep();

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
  static bool printProfileDurations(const ProfileDurations& times,
                                    Sprinter& sprinter);

  mozilla::TimeStamp collectionStartTime() const;
  mozilla::TimeStamp lastCollectionEndTime() const;

  friend class gc::GCRuntime;
  friend class gc::TenuringTracer;
  friend struct NurseryChunk;
};

}  // namespace js

#endif  // gc_Nursery_h
