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

#include <tuple>

#include "ds/LifoAlloc.h"
#include "ds/SlimLinkedList.h"
#include "gc/Allocator.h"
#include "gc/GCEnum.h"
#include "gc/GCProbes.h"
#include "gc/Heap.h"
#include "gc/MallocedBlockCache.h"
#include "gc/Pretenuring.h"
#include "js/AllocPolicy.h"
#include "js/Class.h"
#include "js/GCAPI.h"
#include "js/GCVector.h"
#include "js/HeapAPI.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "js/Vector.h"

#define FOR_EACH_NURSERY_PROFILE_TIME(_)      \
  /* Key                       Header text */ \
  _(Total, "total")                           \
  _(TraceValues, "mkVals")                    \
  _(TraceCells, "mkClls")                     \
  _(TraceSlots, "mkSlts")                     \
  _(TraceWasmAnyRefs, "mkWars")               \
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
  _(ClearNursery, "clear")                    \
  _(PurgeStringToAtomCache, "pStoA")          \
  _(Pretenure, "pretnr")

template <typename T>
class SharedMem;

namespace mozilla {
class StringBuffer;
};

namespace js {

struct StringStats;
class AutoLockGCBgAlloc;
class ObjectElements;
struct NurseryChunk;
class HeapSlot;
class JSONPrinter;
class MapObject;
class NurseryDecommitTask;
class NurserySweepTask;
class SetObject;
class JS_PUBLIC_API Sprinter;

namespace gc {

class AutoGCSession;
struct Cell;
class GCSchedulingTunables;
struct LargeBuffer;
class StoreBuffer;
class TenuringTracer;

}  // namespace gc

class Nursery {
 public:
  explicit Nursery(gc::GCRuntime* gc);
  ~Nursery();

  [[nodiscard]] bool init(AutoLockGCBgAlloc& lock);

  void enable();
  void disable();
  bool isEnabled() const { return capacity() != 0; }

  void enableStrings();
  void disableStrings();
  bool canAllocateStrings() const { return canAllocateStrings_; }

  void enableBigInts();
  void disableBigInts();
  bool canAllocateBigInts() const { return canAllocateBigInts_; }

  void setSemispaceEnabled(bool enabled);
  bool semispaceEnabled() const { return semispaceEnabled_; }

  // Return true if no allocations have been made since the last collection.
  bool isEmpty() const;

  // Check whether an arbitrary pointer is within the nursery. This is
  // slower than IsInsideNursery(Cell*), but works on all types of pointers.
  bool isInside(gc::Cell* cellp) const = delete;
  inline bool isInside(const void* p) const;

  template <typename T>
  inline bool isInside(const SharedMem<T>& p) const;

  // Allocate and return a pointer to a new GC thing. Returns nullptr if the
  // Nursery is full.
  void* allocateCell(gc::AllocSite* site, size_t size, JS::TraceKind kind);

  // Allocate and return a pointer to a new GC thing. Returns nullptr if the
  // handleAllocationFailure() needs to be called before retrying.
  inline void* tryAllocateCell(gc::AllocSite* site, size_t size,
                               JS::TraceKind kind);

  // Attempt to handle the failure of tryAllocate. Returns a GCReason if minor
  // GC is required, or NO_REASON if the failure was handled and allocation will
  // now succeed.
  [[nodiscard]] JS::GCReason handleAllocationFailure();

  static size_t nurseryCellHeaderSize() {
    return sizeof(gc::NurseryCellHeader);
  }

  // Allocate a buffer for a given zone, using the nursery if possible. Returns
  // <buffer, isMalloced> so the caller can register the buffer if
  // needed. Returns false in |isMalloced| if the allocation fails.
  //
  // Use the following API if the owning Cell is already known.
  std::tuple<void*, bool> allocNurseryOrMallocBuffer(JS::Zone* zone,
                                                     size_t nbytes,
                                                     arena_id_t arenaId);
  std::tuple<void*, bool> allocateBuffer(JS::Zone* zone, size_t nbytes);

  // Like allocNurseryOrMallocBuffer, but returns nullptr if the buffer can't
  // be allocated in the nursery.
  void* tryAllocateNurseryBuffer(JS::Zone* zone, size_t nbytes,
                                 arena_id_t arenaId);

  // Allocate a buffer for a given Cell, using the nursery if possible and
  // owner is in the nursery.
  void* allocNurseryOrMallocBuffer(JS::Zone* zone, gc::Cell* owner,
                                   size_t nbytes, arena_id_t arenaId);
  void* allocateBuffer(JS::Zone* zone, gc::Cell* owner, size_t nbytes);

  // Allocate a zero-initialized buffer for a given zone, using the nursery if
  // possible. If the buffer isn't allocated in the nursery, the given arena is
  // used. Returns <buffer, isMalloced>. Returns false in |isMalloced| if the
  // allocation fails.
  std::tuple<void*, bool> allocateZeroedBuffer(JS::Zone* zone, size_t nbytes,
                                               arena_id_t arena);

  // Allocate a zero-initialized buffer for a given Cell, using the nursery if
  // possible and |owner| is in the nursery. If the buffer isn't allocated in
  // the nursery, the given arena is used.
  void* allocateZeroedBuffer(gc::Cell* owner, size_t nbytes, arena_id_t arena);

  // Resize an existing buffer.
  void* reallocNurseryOrMallocBuffer(JS::Zone* zone, gc::Cell* cell,
                                     void* oldBuffer, size_t oldBytes,
                                     size_t newBytes, arena_id_t arena);

  // Resize an existing buffer.
  void* reallocateBuffer(JS::Zone* zone, gc::Cell* cell, void* oldBuffer,
                         size_t oldBytes, size_t newBytes);

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

  // Handle an external buffer when a cell is promoted. Updates the pointer to
  // the (possibly moved) buffer and returns whether it was moved.
  // bytesUsed can be less than bytesCapacity if not all bytes need to be copied
  // when the buffer is moved.
  enum WasBufferMoved : bool { BufferNotMoved = false, BufferMoved = true };
  WasBufferMoved maybeMoveRawBufferOnPromotion(void** bufferp, gc::Cell* owner,
                                               size_t bytesUsed,
                                               size_t bytesCapacity,
                                               MemoryUse use, arena_id_t arena);
  template <typename T>
  WasBufferMoved maybeMoveBufferOnPromotion(T** bufferp, gc::Cell* owner,
                                            size_t bytesUsed,
                                            size_t bytesCapacity, MemoryUse use,
                                            arena_id_t arena) {
    return maybeMoveRawBufferOnPromotion(reinterpret_cast<void**>(bufferp),
                                         owner, bytesUsed, bytesCapacity, use,
                                         arena);
  }
  template <typename T>
  WasBufferMoved maybeMoveNurseryOrMallocBufferOnPromotion(T** bufferp,
                                                           gc::Cell* owner,
                                                           size_t nbytes,
                                                           MemoryUse use) {
    return maybeMoveBufferOnPromotion(bufferp, owner, nbytes, nbytes, use,
                                      MallocArena);
  }

  WasBufferMoved maybeMoveRawBufferOnPromotion(void** bufferp, gc::Cell* owner,
                                               size_t nbytes);
  template <typename T>
  WasBufferMoved maybeMoveBufferOnPromotion(T** bufferp, gc::Cell* owner,
                                            size_t nbytes) {
    return maybeMoveRawBufferOnPromotion(reinterpret_cast<void**>(bufferp),
                                         owner, nbytes);
  }

  // Register a malloced buffer that is held by a nursery object, which
  // should be freed at the end of a minor GC. Buffers are unregistered when
  // their owning objects are tenured.
  [[nodiscard]] bool registerMallocedBuffer(void* buffer, size_t nbytes);
  void registerBuffer(void* buffer, size_t nbytes);

  // Mark a malloced buffer as no longer needing to be freed.
  inline void removeMallocedBuffer(void* buffer, size_t nbytes);

  // Mark a malloced buffer as no longer needing to be freed during minor
  // GC. There's no need to account for the size here since all remaining
  // buffers will soon be freed.
  inline void removeMallocedBufferDuringMinorGC(void* buffer);

  [[nodiscard]] bool addedUniqueIdToCell(gc::Cell* cell) {
    MOZ_ASSERT(IsInsideNursery(cell));
    MOZ_ASSERT(isEnabled());
    return cellsWithUid_.append(cell);
  }

  [[nodiscard]] inline bool addStringBuffer(JSLinearString* s);

  [[nodiscard]] inline bool addExtensibleStringBuffer(
      JSLinearString* s, mozilla::StringBuffer* buffer,
      bool updateMallocBytes = true);
  inline void removeExtensibleStringBuffer(JSLinearString* s,
                                           bool updateMallocBytes = true);

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
  [[nodiscard]] inline bool registerTrailer(PointerAndUint7 blockAndListID,
                                            size_t nBytes);
  inline void unregisterTrailer(void* block);
  size_t sizeOfTrailerBlockSets(mozilla::MallocSizeOf mallocSizeOf) const;

  size_t totalCapacity() const;
  size_t totalCommitted() const;

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

  void* addressOfPosition() const { return (void**)&toSpace.position_; }
  static constexpr int32_t offsetOfCurrentEndFromPosition() {
    return offsetof(Nursery, toSpace.currentEnd_) -
           offsetof(Nursery, toSpace.position_);
  }

  void* addressOfNurseryAllocatedSites() {
    return pretenuringNursery.addressOfAllocatedSites();
  }

  void requestMinorGC(JS::GCReason reason);

  bool minorGCRequested() const {
    return minorGCTriggerReason_ != JS::GCReason::NO_REASON;
  }
  JS::GCReason minorGCTriggerReason() const { return minorGCTriggerReason_; }

  bool wantEagerCollection() const;

  bool enableProfiling() const { return enableProfiling_; }

  bool addMapWithNurseryIterators(MapObject* obj) {
    MOZ_ASSERT_IF(!mapsWithNurseryIterators_.empty(),
                  mapsWithNurseryIterators_.back() != obj);
    return mapsWithNurseryIterators_.append(obj);
  }
  bool addSetWithNurseryIterators(SetObject* obj) {
    MOZ_ASSERT_IF(!setsWithNurseryIterators_.empty(),
                  setsWithNurseryIterators_.back() != obj);
    return setsWithNurseryIterators_.append(obj);
  }

  void joinSweepTask();
  void joinDecommitTask();

#ifdef DEBUG
  bool sweepTaskIsIdle();
#endif

  mozilla::TimeStamp collectionStartTime() {
    return startTimes_[ProfileKey::Total];
  }

  bool canCreateAllocSite() { return pretenuringNursery.canCreateAllocSite(); }
  void noteAllocSiteCreated() { pretenuringNursery.noteAllocSiteCreated(); }
  bool reportPretenuring() const { return pretenuringReportFilter_.enabled; }
  void maybeStopPretenuring(gc::GCRuntime* gc) {
    pretenuringNursery.maybeStopPretenuring(gc);
  }

  void setAllocFlagsForZone(JS::Zone* zone);

  bool shouldTenureEverything(JS::GCReason reason);

  inline bool inCollectedRegion(const gc::Cell* cell) const;
  inline bool inCollectedRegion(void* ptr) const;

  void trackMallocedBufferOnPromotion(void* buffer, gc::Cell* owner,
                                      size_t nbytes, MemoryUse use);
  void trackBufferOnPromotion(void* buffer, gc::Cell* owner, size_t nbytes);
  void trackTrailerOnPromotion(void* buffer, gc::Cell* owner, size_t nbytes,
                               size_t overhead, MemoryUse use);

  // Round a size in bytes to the nearest valid nursery size.
  static size_t roundSize(size_t size);

  // The malloc'd block cache.
  gc::MallocedBlockCache& mallocedBlockCache() { return mallocedBlockCache_; }
  size_t sizeOfMallocedBlockCache(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocedBlockCache_.sizeOfExcludingThis(mallocSizeOf);
  }

  inline void addMallocedBufferBytes(size_t nbytes);

  mozilla::TimeStamp lastCollectionEndTime() const;

 private:
  struct Space;

  enum class ProfileKey {
#define DEFINE_TIME_KEY(name, text) name,
    FOR_EACH_NURSERY_PROFILE_TIME(DEFINE_TIME_KEY)
#undef DEFINE_TIME_KEY
        KeyCount
  };

  using ProfileTimes = mozilla::EnumeratedArray<ProfileKey, mozilla::TimeStamp,
                                                size_t(ProfileKey::KeyCount)>;
  using ProfileDurations =
      mozilla::EnumeratedArray<ProfileKey, mozilla::TimeDuration,
                               size_t(ProfileKey::KeyCount)>;

  size_t capacity() const { return capacity_; }

  // Total number of chunks and the capacity of the current nursery
  // space. Chunks will be lazily allocated and added to the chunks array up to
  // this limit. After that the nursery must be collected. This limit may be
  // changed at the end of collection by maybeResizeNursery.
  uint32_t maxChunkCount() const {
    MOZ_ASSERT(toSpace.maxChunkCount_);
    return toSpace.maxChunkCount_;
  }

  // Number of allocated (ready to use) chunks.
  unsigned allocatedChunkCount() const { return toSpace.chunks_.length(); }

  uint32_t currentChunk() const { return toSpace.currentChunk_; }
  uint32_t startChunk() const { return toSpace.startChunk_; }
  uintptr_t startPosition() const { return toSpace.startPosition_; }

  // Used and free space both include chunk headers for that part of the
  // nursery.
  MOZ_ALWAYS_INLINE size_t usedSpace() const {
    return capacity() - freeSpace();
  }
  MOZ_ALWAYS_INLINE size_t freeSpace() const {
    MOZ_ASSERT(isEnabled());
    MOZ_ASSERT(currentChunk() < maxChunkCount());
    return (currentEnd() - position()) +
           (maxChunkCount() - currentChunk() - 1) * gc::ChunkSize;
  }

  // Calculate the promotion rate of the most recent minor GC.
  // The valid_for_tenuring parameter is used to return whether this
  // promotion rate is accurate enough (the nursery was full enough) to be
  // used for tenuring and other decisions.
  //
  // Must only be called if the previousGC data is initialised.
  double calcPromotionRate(bool* validForTenuring) const;

  void freeTrailerBlocks(JS::GCOptions options, JS::GCReason reason);

  NurseryChunk& chunk(unsigned index) const { return *toSpace.chunks_[index]; }

  // Set the allocation position to the start of a chunk. This sets
  // currentChunk_, position_ and currentEnd_ values as appropriate.
  void moveToStartOfChunk(unsigned chunkno);

  bool initFirstChunk(AutoLockGCBgAlloc& lock);
  void setCapacity(size_t newCapacity);

  void poisonAndInitCurrentChunk();

  void setCurrentEnd();
  void setStartToCurrentPosition();

  // Allocate another chunk.
  [[nodiscard]] bool allocateNextChunk(AutoLockGCBgAlloc& lock);

  uintptr_t position() const { return toSpace.position_; }
  uintptr_t currentEnd() const { return toSpace.currentEnd_; }

  MOZ_ALWAYS_INLINE bool isSubChunkMode() const;

  JSRuntime* runtime() const;
  gcstats::Statistics& stats() const;

  const js::gc::GCSchedulingTunables& tunables() const;

  void getAllocFlagsForZone(JS::Zone* zone, bool* allocObjectsOut,
                            bool* allocStringsOut, bool* allocBigIntsOut);
  void updateAllZoneAllocFlags();
  void updateAllocFlagsForZone(JS::Zone* zone);
  void discardCodeAndSetJitFlagsForZone(JS::Zone* zone);

  void* allocate(size_t size);

  // Common internal allocator function. If this fails, call
  // handleAllocationFailure to see whether it's possible to retry.
  inline void* tryAllocate(size_t size);

  [[nodiscard]] bool moveToNextChunk();

  bool freeSpaceIsBelowEagerThreshold() const;
  bool isUnderused() const;

  struct CollectionResult {
    size_t tenuredBytes;
    size_t tenuredCells;
  };
  CollectionResult doCollection(gc::AutoGCSession& session,
                                JS::GCOptions options, JS::GCReason reason);
  void swapSpaces();
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
  bool checkForwardingPointerInsideNursery(void* ptr);
#endif

  // Discard pointers to objects that have been freed.
  void sweep();

  // In a minor GC, resets the start and end positions, the current chunk and
  // current position.
  void setNewExtentAndPosition();

  // the nursery on debug & nightly builds.
  void clear();

  void clearMapAndSetNurseryIterators();
  void sweepMapAndSetObjects();

  void sweepStringsWithBuffer();

  void sweepBuffers();

  // Get per-space size limits.
  size_t maxSpaceSize() const;
  size_t minSpaceSize() const;

  // Change the allocable space provided by the nursery.
  void maybeResizeNursery(JS::GCOptions options, JS::GCReason reason);
  size_t targetSize(JS::GCOptions options, JS::GCReason reason);
  void clearRecentGrowthData();
  void growAllocableSpace(size_t newCapacity);
  void shrinkAllocableSpace(size_t newCapacity);
  void minimizeAllocableSpace();

  // Free the chunks starting at firstFreeChunk until the end of the chunks
  // vector. Shrinks the vector but does not update maxChunkCount().
  void freeChunksFrom(Space& space, unsigned firstFreeChunk);

  // During a semispace nursery collection, return whether a cell in fromspace
  // was in the tospace of the previous collection, meaning that it should be
  // tenured in this collection.
  inline bool shouldTenure(gc::Cell* cell);

  void sendTelemetry(JS::GCReason reason, mozilla::TimeDuration totalTime,
                     bool wasEmpty, double promotionRate,
                     size_t sitesPretenured);

  void printCollectionProfile(JS::GCReason reason, double promotionRate);
  void printDeduplicationData(js::StringStats& prev, js::StringStats& curr);

  // Profile recording and printing.
  void maybeClearProfileDurations();
  void startProfile(ProfileKey key);
  void endProfile(ProfileKey key);
  static void printProfileDurations(const ProfileDurations& times,
                                    Sprinter& sprinter);

  mozilla::TimeStamp collectionStartTime() const;

 private:
  using BufferRelocationOverlay = void*;
  using BufferSet = HashSet<void*, PointerHasher<void*>, SystemAllocPolicy>;

  struct Space {
    // Fields used during allocation fast path go first:

    // Pointer to the first unallocated byte in the nursery.
    uintptr_t position_ = 0;

    // Pointer to the last byte of space in the current chunk.
    uintptr_t currentEnd_ = 0;

    // Vector of allocated chunks to allocate from.
    Vector<NurseryChunk*, 0, SystemAllocPolicy> chunks_;

    // The index of the chunk that is currently being allocated from.
    uint32_t currentChunk_ = 0;

    // The maximum number of chunks to allocate based on capacity_.
    uint32_t maxChunkCount_ = 0;

    // These fields refer to the beginning of the nursery. They're normally 0
    // and chunk(0).start() respectively. Except when a generational GC zeal
    // mode is active, then they may be arbitrary (see Nursery::clear()).
    uint32_t startChunk_ = 0;
    uintptr_t startPosition_ = 0;

    // The set of malloc-allocated buffers owned by nursery objects. Any
    // buffers that do not belong to a promoted thing at the end of a minor GC
    // must be freed.
    BufferSet mallocedBuffers;
    size_t mallocedBufferBytes = 0;

    // Wasm "trailer" (C++-heap-allocated) blocks.  See comments above on
    // ::registerTrailer and ::unregisterTrailer.
    Vector<PointerAndUint7, 0, SystemAllocPolicy> trailersAdded_;
    Vector<void*, 0, SystemAllocPolicy> trailersRemoved_;
    size_t trailersRemovedUsed_ = 0;
    size_t trailerBytes_ = 0;

    gc::ChunkKind kind;

    explicit Space(gc::ChunkKind kind);

    inline bool isEmpty() const;
    inline bool isInside(const void* p) const;

    // Return the logical offset within the nursery of an address in a nursery
    // chunk (chunks are discontiguous in memory).
    inline size_t offsetFromAddress(uintptr_t addr) const;
    inline size_t offsetFromExclusiveAddress(uintptr_t addr) const;

    void setKind(gc::ChunkKind newKind);

    void clear(Nursery* nursery);
    void moveToStartOfChunk(Nursery* nursery, unsigned chunkno);
    void setCurrentEnd(Nursery* nursery);
    void setStartToCurrentPosition();
    bool commitSubChunkRegion(size_t oldCapacity, size_t newCapacity);
    void decommitSubChunkRegion(Nursery* nursery, size_t oldCapacity,
                                size_t newCapacity);
    void freeTrailerBlocks(gc::MallocedBlockCache& mallocedBlockCache);

#ifdef DEBUG
    void checkKind(gc::ChunkKind expected) const;
    size_t findChunkIndex(uintptr_t chunkAddr) const;
#endif
  };

  Space toSpace;
  Space fromSpace;

  gc::GCRuntime* const gc;

  // The current nursery capacity measured in bytes. It may grow up to this
  // value without a collection, allocating chunks on demand. This limit may be
  // changed by maybeResizeNursery() each collection. It includes chunk headers.
  size_t capacity_;

  uintptr_t tenureThreshold_ = 0;

  gc::PretenuringNursery pretenuringNursery;

  mozilla::TimeDuration timeInChunkAlloc_;

  // Report minor collections taking at least this long, if enabled.
  bool enableProfiling_ = false;
  bool profileWorkers_ = false;

  mozilla::TimeDuration profileThreshold_;

  // Whether to use semispace collection.
  bool semispaceEnabled_;

  // Whether we will nursery-allocate strings.
  bool canAllocateStrings_;

  // Whether we will nursery-allocate BigInts.
  bool canAllocateBigInts_;

  // Report how many strings were deduplicated.
  bool reportDeduplications_;

#ifdef JS_GC_ZEAL
  // Report on the kinds of things promoted.
  bool reportPromotion_;
#endif

  // Whether to report information on pretenuring, and if so the allocation
  // threshold at which to report details of each allocation site.
  gc::AllocSiteFilter pretenuringReportFilter_;

  // Whether and why a collection of this nursery has been requested. When this
  // happens |prevPosition_| is set to the current position and |position_| set
  // to the end of the chunk to force the next allocation to fail.
  JS::GCReason minorGCTriggerReason_;
  uintptr_t prevPosition_;

  // Profiling data.

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

  // During a collection most hoisted slot and element buffers indicate their
  // new location with a forwarding pointer at the base. This does not work
  // for buffers whose length is less than pointer width, or when different
  // buffers might overlap each other. For these, an entry in the following
  // table is used.
  using ForwardedBufferMap =
      HashMap<void*, void*, PointerHasher<void*>, SystemAllocPolicy>;
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
  using CellsWithUniqueIdVector = JS::GCVector<gc::Cell*, 8, SystemAllocPolicy>;
  CellsWithUniqueIdVector cellsWithUid_;

  // Lists of map and set objects with iterators allocated in the nursery. Such
  // objects need to be swept after minor GC.
  using MapObjectVector = Vector<MapObject*, 0, SystemAllocPolicy>;
  MapObjectVector mapsWithNurseryIterators_;
  using SetObjectVector = Vector<SetObject*, 0, SystemAllocPolicy>;
  SetObjectVector setsWithNurseryIterators_;

  // List of strings with StringBuffers allocated in the nursery. References
  // to the buffers are dropped after minor GC. The list stores both the JS
  // string and the StringBuffer to simplify interaction with AtomRefs and
  // string deduplication.
  using StringAndBuffer = std::pair<JSLinearString*, mozilla::StringBuffer*>;
  using StringAndBufferVector =
      JS::GCVector<StringAndBuffer, 8, SystemAllocPolicy>;
  StringAndBufferVector stringBuffers_;

  // Like stringBuffers_, but for extensible strings for flattened ropes. This
  // requires a HashMap instead of a Vector because we need to remove the entry
  // when transferring the buffer to a new extensible string during flattening.
  using ExtensibleStringBuffers =
      HashMap<JSLinearString*, mozilla::StringBuffer*,
              js::PointerHasher<JSLinearString*>, js::SystemAllocPolicy>;
  ExtensibleStringBuffers extensibleStringBuffers_;

  // List of StringBuffers to release off-thread.
  using StringBufferVector =
      Vector<mozilla::StringBuffer*, 8, SystemAllocPolicy>;
  StringBufferVector stringBuffersToReleaseAfterMinorGC_;

  UniquePtr<NurserySweepTask> sweepTask;
  UniquePtr<NurseryDecommitTask> decommitTask;

  // A cache of small C++-heap allocated blocks associated with this Nursery.
  // This provided so as to provide cheap allocation/deallocation of
  // out-of-line storage areas as used by WasmStructObject and
  // WasmArrayObject, although the mechanism is general and not specific to
  // these object types.  Regarding lifetimes, because the cache holds only
  // blocks that are not currently in use, it can be flushed at any point with
  // no correctness impact, only a performance impact.
  gc::MallocedBlockCache mallocedBlockCache_;

  // Whether the previous collection tenured everything. This may be false if
  // semispace is in use.
  bool tenuredEverything;

  friend class gc::GCRuntime;
  friend class gc::TenuringTracer;
  friend struct NurseryChunk;
};

MOZ_ALWAYS_INLINE bool Nursery::isInside(const void* p) const {
  // TODO: Split this into separate methods.
  // TODO: Do we ever need to check both?
  return toSpace.isInside(p) || fromSpace.isInside(p);
}

MOZ_ALWAYS_INLINE bool Nursery::Space::isInside(const void* p) const {
  for (auto* chunk : chunks_) {
    if (uintptr_t(p) - uintptr_t(chunk) < gc::ChunkSize) {
      return true;
    }
  }
  return false;
}

}  // namespace js

#endif  // gc_Nursery_h
