/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_StoreBuffer_h
#define gc_StoreBuffer_h

#include "mozilla/Attributes.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/ReentrancyGuard.h"

#include <algorithm>

#include "ds/BitArray.h"
#include "ds/LifoAlloc.h"
#include "gc/Nursery.h"
#include "js/AllocPolicy.h"
#include "js/MemoryMetrics.h"
#include "js/UniquePtr.h"
#include "threading/Mutex.h"

namespace js {

#ifdef DEBUG
extern bool CurrentThreadIsGCMarking();
#endif

namespace gc {

// Map from all trace kinds to the base GC type.
template <JS::TraceKind kind>
struct MapTraceKindToType {};

#define DEFINE_TRACE_KIND_MAP(name, type, _, _1)   \
  template <>                                      \
  struct MapTraceKindToType<JS::TraceKind::name> { \
    using Type = type;                             \
  };
JS_FOR_EACH_TRACEKIND(DEFINE_TRACE_KIND_MAP);
#undef DEFINE_TRACE_KIND_MAP

// Map from a possibly-derived type to the base GC type.
template <typename T>
struct BaseGCType {
  using type =
      typename MapTraceKindToType<JS::MapTypeToTraceKind<T>::kind>::Type;
  static_assert(std::is_base_of_v<type, T>, "Failed to find base type");
};

class Arena;
class ArenaCellSet;

#ifdef DEBUG
extern bool CurrentThreadHasLockedGC();
#endif

/*
 * BufferableRef represents an abstract reference for use in the generational
 * GC's remembered set. Entries in the store buffer that cannot be represented
 * with the simple pointer-to-a-pointer scheme must derive from this class and
 * use the generic store buffer interface.
 *
 * A single BufferableRef entry in the generic buffer can represent many entries
 * in the remembered set.  For example js::OrderedHashTableRef represents all
 * the incoming edges corresponding to keys in an ordered hash table.
 */
class BufferableRef {
 public:
  virtual void trace(JSTracer* trc) = 0;
  bool maybeInRememberedSet(const Nursery&) const { return true; }
};

typedef HashSet<void*, PointerHasher<void*>, SystemAllocPolicy> EdgeSet;

/* The size of a single block of store buffer storage space. */
static const size_t LifoAllocBlockSize = 8 * 1024;

/*
 * The StoreBuffer observes all writes that occur in the system and performs
 * efficient filtering of them to derive a remembered set for nursery GC.
 */
class StoreBuffer {
  friend class mozilla::ReentrancyGuard;

  /* The size at which a block is about to overflow for the generic buffer. */
  static const size_t GenericBufferLowAvailableThreshold =
      LifoAllocBlockSize / 2;

  /* The size at which other store buffers are about to overflow. */
  static const size_t BufferOverflowThresholdBytes = 128 * 1024;

  /*
   * This buffer holds only a single type of edge. Using this buffer is more
   * efficient than the generic buffer when many writes will be to the same
   * type of edge: e.g. Value or Cell*.
   */
  template <typename T>
  struct MonoTypeBuffer {
    /* The canonical set of stores. */
    typedef HashSet<T, typename T::Hasher, SystemAllocPolicy> StoreSet;
    StoreSet stores_;

    /*
     * A one element cache in front of the canonical set to speed up
     * temporary instances of HeapPtr.
     */
    T last_;

    StoreBuffer* owner_;

    JS::GCReason gcReason_;

    /* Maximum number of entries before we request a minor GC. */
    const static size_t MaxEntries = BufferOverflowThresholdBytes / sizeof(T);

    explicit MonoTypeBuffer(StoreBuffer* owner, JS::GCReason reason)
        : last_(T()), owner_(owner), gcReason_(reason) {}

    void clear() {
      last_ = T();
      stores_.clear();
    }

    /* Add one item to the buffer. */
    void put(const T& t) {
      sinkStore();
      last_ = t;
    }

    /* Remove an item from the store buffer. */
    void unput(const T& v) {
      // Fast, hashless remove of last put.
      if (last_ == v) {
        last_ = T();
        return;
      }
      stores_.remove(v);
    }

    /* Move any buffered stores to the canonical store set. */
    void sinkStore() {
      if (last_) {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!stores_.put(last_)) {
          oomUnsafe.crash("Failed to allocate for MonoTypeBuffer::put.");
        }
      }
      last_ = T();

      if (MOZ_UNLIKELY(stores_.count() > MaxEntries)) {
        owner_->setAboutToOverflow(gcReason_);
      }
    }

    /* Trace the source of all edges in the store buffer. */
    void trace(TenuringTracer& mover);

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) {
      return stores_.shallowSizeOfExcludingThis(mallocSizeOf);
    }

    bool isEmpty() const { return last_ == T() && stores_.empty(); }

   private:
    MonoTypeBuffer(const MonoTypeBuffer& other) = delete;
    MonoTypeBuffer& operator=(const MonoTypeBuffer& other) = delete;
  };

  struct WholeCellBuffer {
    UniquePtr<LifoAlloc> storage_;
    ArenaCellSet* stringHead_;
    ArenaCellSet* nonStringHead_;
    StoreBuffer* owner_;

    explicit WholeCellBuffer(StoreBuffer* owner)
        : storage_(nullptr),
          stringHead_(nullptr),
          nonStringHead_(nullptr),
          owner_(owner) {}

    [[nodiscard]] bool init();

    void clear();

    bool isAboutToOverflow() const {
      return !storage_->isEmpty() &&
             storage_->used() > BufferOverflowThresholdBytes;
    }

    void trace(TenuringTracer& mover);

    inline void put(const Cell* cell);

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) {
      return storage_ ? storage_->sizeOfIncludingThis(mallocSizeOf) : 0;
    }

    bool isEmpty() const {
      MOZ_ASSERT_IF(!stringHead_ && !nonStringHead_,
                    !storage_ || storage_->isEmpty());
      return !stringHead_ && !nonStringHead_;
    }

   private:
    ArenaCellSet* allocateCellSet(Arena* arena);

    WholeCellBuffer(const WholeCellBuffer& other) = delete;
    WholeCellBuffer& operator=(const WholeCellBuffer& other) = delete;
  };

  struct GenericBuffer {
    UniquePtr<LifoAlloc> storage_;
    StoreBuffer* owner_;

    explicit GenericBuffer(StoreBuffer* owner)
        : storage_(nullptr), owner_(owner) {}

    [[nodiscard]] bool init();

    void clear() {
      if (storage_) {
        storage_->used() ? storage_->releaseAll() : storage_->freeAll();
      }
    }

    bool isAboutToOverflow() const {
      return !storage_->isEmpty() && storage_->availableInCurrentChunk() <
                                         GenericBufferLowAvailableThreshold;
    }

    /* Trace all generic edges. */
    void trace(JSTracer* trc);

    template <typename T>
    void put(const T& t) {
      MOZ_ASSERT(storage_);

      /* Ensure T is derived from BufferableRef. */
      (void)static_cast<const BufferableRef*>(&t);

      AutoEnterOOMUnsafeRegion oomUnsafe;
      unsigned size = sizeof(T);
      unsigned* sizep = storage_->pod_malloc<unsigned>();
      if (!sizep) {
        oomUnsafe.crash("Failed to allocate for GenericBuffer::put.");
      }
      *sizep = size;

      T* tp = storage_->new_<T>(t);
      if (!tp) {
        oomUnsafe.crash("Failed to allocate for GenericBuffer::put.");
      }

      if (isAboutToOverflow()) {
        owner_->setAboutToOverflow(JS::GCReason::FULL_GENERIC_BUFFER);
      }
    }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) {
      return storage_ ? storage_->sizeOfIncludingThis(mallocSizeOf) : 0;
    }

    bool isEmpty() const { return !storage_ || storage_->isEmpty(); }

   private:
    GenericBuffer(const GenericBuffer& other) = delete;
    GenericBuffer& operator=(const GenericBuffer& other) = delete;
  };

  template <typename Edge>
  struct PointerEdgeHasher {
    using Lookup = Edge;
    static HashNumber hash(const Lookup& l) {
      return mozilla::HashGeneric(l.edge);
    }
    static bool match(const Edge& k, const Lookup& l) { return k == l; }
  };

  template <typename T>
  struct CellPtrEdge {
    T** edge = nullptr;

    CellPtrEdge() = default;
    explicit CellPtrEdge(T** v) : edge(v) {}
    bool operator==(const CellPtrEdge& other) const {
      return edge == other.edge;
    }
    bool operator!=(const CellPtrEdge& other) const {
      return edge != other.edge;
    }

    bool maybeInRememberedSet(const Nursery& nursery) const {
      MOZ_ASSERT(IsInsideNursery(*edge));
      return !nursery.isInside(edge);
    }

    void trace(TenuringTracer& mover) const;

    explicit operator bool() const { return edge != nullptr; }

    using Hasher = PointerEdgeHasher<CellPtrEdge<T>>;
  };

  using ObjectPtrEdge = CellPtrEdge<JSObject>;
  using StringPtrEdge = CellPtrEdge<JSString>;
  using BigIntPtrEdge = CellPtrEdge<JS::BigInt>;

  struct ValueEdge {
    JS::Value* edge;

    ValueEdge() : edge(nullptr) {}
    explicit ValueEdge(JS::Value* v) : edge(v) {}
    bool operator==(const ValueEdge& other) const { return edge == other.edge; }
    bool operator!=(const ValueEdge& other) const { return edge != other.edge; }

    Cell* deref() const {
      return edge->isGCThing() ? static_cast<Cell*>(edge->toGCThing())
                               : nullptr;
    }

    bool maybeInRememberedSet(const Nursery& nursery) const {
      MOZ_ASSERT(IsInsideNursery(deref()));
      return !nursery.isInside(edge);
    }

    void trace(TenuringTracer& mover) const;

    explicit operator bool() const { return edge != nullptr; }

    using Hasher = PointerEdgeHasher<ValueEdge>;
  };

  struct SlotsEdge {
    // These definitions must match those in HeapSlot::Kind.
    const static int SlotKind = 0;
    const static int ElementKind = 1;

    uintptr_t objectAndKind_;  // NativeObject* | Kind
    uint32_t start_;
    uint32_t count_;

    SlotsEdge() : objectAndKind_(0), start_(0), count_(0) {}
    SlotsEdge(NativeObject* object, int kind, uint32_t start, uint32_t count)
        : objectAndKind_(uintptr_t(object) | kind),
          start_(start),
          count_(count) {
      MOZ_ASSERT((uintptr_t(object) & 1) == 0);
      MOZ_ASSERT(kind <= 1);
      MOZ_ASSERT(count > 0);
      MOZ_ASSERT(start + count > start);
    }

    NativeObject* object() const {
      return reinterpret_cast<NativeObject*>(objectAndKind_ & ~1);
    }
    int kind() const { return (int)(objectAndKind_ & 1); }

    bool operator==(const SlotsEdge& other) const {
      return objectAndKind_ == other.objectAndKind_ && start_ == other.start_ &&
             count_ == other.count_;
    }

    bool operator!=(const SlotsEdge& other) const { return !(*this == other); }

    // True if this SlotsEdge range overlaps with the other SlotsEdge range,
    // false if they do not overlap.
    bool overlaps(const SlotsEdge& other) const {
      if (objectAndKind_ != other.objectAndKind_) {
        return false;
      }

      // Widen our range by one on each side so that we consider
      // adjacent-but-not-actually-overlapping ranges as overlapping. This
      // is particularly useful for coalescing a series of increasing or
      // decreasing single index writes 0, 1, 2, ..., N into a SlotsEdge
      // range of elements [0, N].
      uint32_t end = start_ + count_ + 1;
      uint32_t start = start_ > 0 ? start_ - 1 : 0;
      MOZ_ASSERT(start < end);

      uint32_t otherEnd = other.start_ + other.count_;
      MOZ_ASSERT(other.start_ <= otherEnd);
      return (start <= other.start_ && other.start_ <= end) ||
             (start <= otherEnd && otherEnd <= end);
    }

    // Destructively make this SlotsEdge range the union of the other
    // SlotsEdge range and this one. A precondition is that the ranges must
    // overlap.
    void merge(const SlotsEdge& other) {
      MOZ_ASSERT(overlaps(other));
      uint32_t end = std::max(start_ + count_, other.start_ + other.count_);
      start_ = std::min(start_, other.start_);
      count_ = end - start_;
    }

    bool maybeInRememberedSet(const Nursery& n) const {
      return !IsInsideNursery(reinterpret_cast<Cell*>(object()));
    }

    void trace(TenuringTracer& mover) const;

    explicit operator bool() const { return objectAndKind_ != 0; }

    typedef struct Hasher {
      using Lookup = SlotsEdge;
      static HashNumber hash(const Lookup& l) {
        return mozilla::HashGeneric(l.objectAndKind_, l.start_, l.count_);
      }
      static bool match(const SlotsEdge& k, const Lookup& l) { return k == l; }
    } Hasher;
  };

  // The GC runs tasks that may access the storebuffer in parallel and so must
  // take a lock. The mutator may only access the storebuffer from the main
  // thread.
  inline void CheckAccess() const {
#ifdef DEBUG
    if (JS::RuntimeHeapIsBusy()) {
      MOZ_ASSERT(!CurrentThreadIsGCMarking());
      lock_.assertOwnedByCurrentThread();
    } else {
      MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime_));
    }
#endif
  }

  template <typename Buffer, typename Edge>
  void unput(Buffer& buffer, const Edge& edge) {
    CheckAccess();
    if (!isEnabled()) {
      return;
    }
    mozilla::ReentrancyGuard g(*this);
    buffer.unput(edge);
  }

  template <typename Buffer, typename Edge>
  void put(Buffer& buffer, const Edge& edge) {
    CheckAccess();
    if (!isEnabled()) {
      return;
    }
    mozilla::ReentrancyGuard g(*this);
    if (edge.maybeInRememberedSet(nursery_)) {
      buffer.put(edge);
    }
  }

  Mutex lock_;

  MonoTypeBuffer<ValueEdge> bufferVal;
  MonoTypeBuffer<StringPtrEdge> bufStrCell;
  MonoTypeBuffer<BigIntPtrEdge> bufBigIntCell;
  MonoTypeBuffer<ObjectPtrEdge> bufObjCell;
  MonoTypeBuffer<SlotsEdge> bufferSlot;
  WholeCellBuffer bufferWholeCell;
  GenericBuffer bufferGeneric;

  JSRuntime* runtime_;
  const Nursery& nursery_;

  bool aboutToOverflow_;
  bool enabled_;
  bool mayHavePointersToDeadCells_;
#ifdef DEBUG
  bool mEntered; /* For ReentrancyGuard. */
#endif

 public:
#ifdef DEBUG
  bool markingNondeduplicatable;
#endif

  explicit StoreBuffer(JSRuntime* rt, const Nursery& nursery);
  [[nodiscard]] bool enable();

  void disable();
  bool isEnabled() const { return enabled_; }

  bool isEmpty() const;
  void clear();

  const Nursery& nursery() const { return nursery_; }

  /* Get the overflowed status. */
  bool isAboutToOverflow() const { return aboutToOverflow_; }

  /*
   * Brain transplants may add whole cell buffer entires for dead cells. We must
   * evict the nursery prior to sweeping arenas if any such entries are present.
   */
  bool mayHavePointersToDeadCells() const {
    return mayHavePointersToDeadCells_;
  }

  /* Insert a single edge into the buffer/remembered set. */
  void putValue(JS::Value* vp) { put(bufferVal, ValueEdge(vp)); }
  void unputValue(JS::Value* vp) { unput(bufferVal, ValueEdge(vp)); }

  void putCell(JSString** strp) { put(bufStrCell, StringPtrEdge(strp)); }
  void unputCell(JSString** strp) { unput(bufStrCell, StringPtrEdge(strp)); }

  void putCell(JS::BigInt** bip) { put(bufBigIntCell, BigIntPtrEdge(bip)); }
  void unputCell(JS::BigInt** bip) { unput(bufBigIntCell, BigIntPtrEdge(bip)); }

  void putCell(JSObject** strp) { put(bufObjCell, ObjectPtrEdge(strp)); }
  void unputCell(JSObject** strp) { unput(bufObjCell, ObjectPtrEdge(strp)); }

  void putSlot(NativeObject* obj, int kind, uint32_t start, uint32_t count) {
    SlotsEdge edge(obj, kind, start, count);
    if (bufferSlot.last_.overlaps(edge)) {
      bufferSlot.last_.merge(edge);
    } else {
      put(bufferSlot, edge);
    }
  }

  inline void putWholeCell(Cell* cell);

  /* Insert an entry into the generic buffer. */
  template <typename T>
  void putGeneric(const T& t) {
    put(bufferGeneric, t);
  }

  void setMayHavePointersToDeadCells() { mayHavePointersToDeadCells_ = true; }

  /* Methods to trace the source of all edges in the store buffer. */
  void traceValues(TenuringTracer& mover) { bufferVal.trace(mover); }
  void traceCells(TenuringTracer& mover) {
    bufStrCell.trace(mover);
    bufBigIntCell.trace(mover);
    bufObjCell.trace(mover);
  }
  void traceSlots(TenuringTracer& mover) { bufferSlot.trace(mover); }
  void traceWholeCells(TenuringTracer& mover) { bufferWholeCell.trace(mover); }
  void traceGenericEntries(JSTracer* trc) { bufferGeneric.trace(trc); }

  /* For use by our owned buffers and for testing. */
  void setAboutToOverflow(JS::GCReason);

  void addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                              JS::GCSizes* sizes);

  void checkEmpty() const;

  // For use by the GC only.
  void lock() { lock_.lock(); }
  void unlock() { lock_.unlock(); }
};

// A set of cells in an arena used to implement the whole cell store buffer.
class ArenaCellSet {
  friend class StoreBuffer;

  using ArenaCellBits = BitArray<MaxArenaCellIndex>;

  // The arena this relates to.
  Arena* arena;

  // Pointer to next set forming a linked list.
  ArenaCellSet* next;

  // Bit vector for each possible cell start position.
  ArenaCellBits bits;

#ifdef DEBUG
  // The minor GC number when this was created. This object should not survive
  // past the next minor collection.
  const uint64_t minorGCNumberAtCreation;
#endif

  // Construct the empty sentinel object.
  constexpr ArenaCellSet()
      : arena(nullptr),
        next(nullptr)
#ifdef DEBUG
        ,
        minorGCNumberAtCreation(0)
#endif
  {
  }

 public:
  using WordT = ArenaCellBits::WordT;
  const size_t BitsPerWord = ArenaCellBits::bitsPerElement;
  const size_t NumWords = ArenaCellBits::numSlots;

  ArenaCellSet(Arena* arena, ArenaCellSet* next);

  bool hasCell(const TenuredCell* cell) const {
    return hasCell(getCellIndex(cell));
  }

  void putCell(const TenuredCell* cell) { putCell(getCellIndex(cell)); }

  bool isEmpty() const { return this == &Empty; }

  bool hasCell(size_t cellIndex) const;

  void putCell(size_t cellIndex);

  void check() const;

  WordT getWord(size_t wordIndex) const { return bits.getWord(wordIndex); }

  void trace(TenuringTracer& mover);

  // Sentinel object used for all empty sets.
  //
  // We use a sentinel because it simplifies the JIT code slightly as we can
  // assume all arenas have a cell set.
  static ArenaCellSet Empty;

  static size_t getCellIndex(const TenuredCell* cell);
  static void getWordIndexAndMask(size_t cellIndex, size_t* wordp,
                                  uint32_t* maskp);

  // Attempt to trigger a minor GC if free space in the nursery (where these
  // objects are allocated) falls below this threshold.
  static const size_t NurseryFreeThresholdBytes = 64 * 1024;

  static size_t offsetOfArena() { return offsetof(ArenaCellSet, arena); }
  static size_t offsetOfBits() { return offsetof(ArenaCellSet, bits); }
};

// Post-write barrier implementation for GC cells.

// Implement the post-write barrier for nursery allocateable cell type |T|. Call
// this from |T::postWriteBarrier|.
template <typename T>
MOZ_ALWAYS_INLINE void PostWriteBarrierImpl(void* cellp, T* prev, T* next) {
  MOZ_ASSERT(cellp);

  // If the target needs an entry, add it.
  StoreBuffer* buffer;
  if (next && (buffer = next->storeBuffer())) {
    // If we know that the prev has already inserted an entry, we can skip
    // doing the lookup to add the new entry. Note that we cannot safely
    // assert the presence of the entry because it may have been added
    // via a different store buffer.
    if (prev && prev->storeBuffer()) {
      return;
    }
    buffer->putCell(static_cast<T**>(cellp));
    return;
  }

  // Remove the prev entry if the new value does not need it. There will only
  // be a prev entry if the prev value was in the nursery.
  if (prev && (buffer = prev->storeBuffer())) {
    buffer->unputCell(static_cast<T**>(cellp));
  }
}

template <typename T>
MOZ_ALWAYS_INLINE void PostWriteBarrier(T** vp, T* prev, T* next) {
  static_assert(std::is_base_of_v<Cell, T>);
  static_assert(!std::is_same_v<Cell, T> && !std::is_same_v<TenuredCell, T>);

  if constexpr (!std::is_base_of_v<TenuredCell, T>) {
    using BaseT = typename BaseGCType<T>::type;
    PostWriteBarrierImpl<BaseT>(vp, prev, next);
    return;
  }

  MOZ_ASSERT(!IsInsideNursery(next));
}

// Used when we don't have a specific edge to put in the store buffer.
void PostWriteBarrierCell(Cell* cell, Cell* prev, Cell* next);

} /* namespace gc */
} /* namespace js */

#endif /* gc_StoreBuffer_h */
