/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Cell_h
#define gc_Cell_h

#include "mozilla/Atomics.h"
#include "mozilla/EndianUtils.h"

#include <functional>
#include <type_traits>

#include "gc/GCEnum.h"
#include "gc/Heap.h"
#include "js/GCAnnotations.h"
#include "js/shadow/Zone.h"  // JS::shadow::Zone
#include "js/TraceKind.h"
#include "js/TypeDecls.h"

namespace JS {
enum class TraceKind;
} /* namespace JS */

namespace js {

class GenericPrinter;

extern bool RuntimeFromMainThreadIsHeapMajorCollecting(
    JS::shadow::Zone* shadowZone);

#ifdef DEBUG

// Barriers can't be triggered during backend Ion compilation, which may run on
// a helper thread.
extern bool CurrentThreadIsIonCompiling();

extern bool CurrentThreadIsGCMarking();
extern bool CurrentThreadIsGCSweeping();
extern bool CurrentThreadIsGCFinalizing();
extern bool RuntimeIsVerifyingPreBarriers(JSRuntime* runtime);

#endif

extern void TraceManuallyBarrieredGenericPointerEdge(JSTracer* trc,
                                                     gc::Cell** thingp,
                                                     const char* name);

namespace gc {

class Arena;
enum class AllocKind : uint8_t;
class StoreBuffer;
class TenuredCell;

extern void PerformIncrementalBarrier(TenuredCell* cell);
extern void PerformIncrementalBarrierDuringFlattening(JSString* str);
extern void UnmarkGrayGCThingRecursively(TenuredCell* cell);

// Like gc::MarkColor but allows the possibility of the cell being unmarked.
//
// This class mimics an enum class, but supports operator overloading.
class CellColor {
 public:
  enum Color { White = 0, Gray = 1, Black = 2 };

  CellColor() : color(White) {}

  MOZ_IMPLICIT CellColor(MarkColor markColor)
      : color(markColor == MarkColor::Black ? Black : Gray) {}

  MOZ_IMPLICIT constexpr CellColor(Color c) : color(c) {}

  MarkColor asMarkColor() const {
    MOZ_ASSERT(color != White);
    return color == Black ? MarkColor::Black : MarkColor::Gray;
  }

  // Implement a total ordering for CellColor, with white being 'least marked'
  // and black being 'most marked'.
  bool operator<(const CellColor other) const { return color < other.color; }
  bool operator>(const CellColor other) const { return color > other.color; }
  bool operator<=(const CellColor other) const { return color <= other.color; }
  bool operator>=(const CellColor other) const { return color >= other.color; }
  bool operator!=(const CellColor other) const { return color != other.color; }
  bool operator==(const CellColor other) const { return color == other.color; }
  explicit operator bool() const { return color != White; }

#if defined(JS_GC_ZEAL) || defined(DEBUG)
  const char* name() const {
    switch (color) {
      case CellColor::White:
        return "white";
      case CellColor::Black:
        return "black";
      case CellColor::Gray:
        return "gray";
      default:
        MOZ_CRASH("Unexpected cell color");
    }
  }
#endif

 private:
  Color color;
};

// [SMDOC] GC Cell
//
// A GC cell is the ultimate base class for all GC things. All types allocated
// on the GC heap extend either gc::Cell or gc::TenuredCell. If a type is always
// tenured, prefer the TenuredCell class as base.
//
// The first word of Cell is a uintptr_t that reserves the low three bits for GC
// purposes. The remaining bits are available to sub-classes and can be used
// store a pointer to another gc::Cell. It can also be used for temporary
// storage (see setTemporaryGCUnsafeData). To make use of the remaining space,
// sub-classes derive from a helper class such as TenuredCellWithNonGCPointer.
//
// During moving GC operation a Cell may be marked as forwarded. This indicates
// that a gc::RelocationOverlay is currently stored in the Cell's memory and
// should be used to find the new location of the Cell.
struct Cell {
 protected:
  // Cell header word. Stores GC flags and derived class data.
  //
  // This is atomic since it can be read from and written to by different
  // threads during compacting GC, in a limited way. Specifically, writes that
  // update the derived class data can race with reads that check the forwarded
  // flag. The writes do not change the forwarded flag (which is always false in
  // this situation).
  mozilla::Atomic<uintptr_t, mozilla::MemoryOrdering::Relaxed> header_;

 public:
  static_assert(gc::CellFlagBitsReservedForGC >= 3,
                "Not enough flag bits reserved for GC");
  static constexpr uintptr_t RESERVED_MASK =
      BitMask(gc::CellFlagBitsReservedForGC);

  // Indicates whether the cell has been forwarded (moved) by generational or
  // compacting GC and is now a RelocationOverlay.
  static constexpr uintptr_t FORWARD_BIT = Bit(0);

  // Bits 1 and 2 are reserved for future use by the GC.

  bool isForwarded() const { return header_ & FORWARD_BIT; }
  uintptr_t flags() const { return header_ & RESERVED_MASK; }

  MOZ_ALWAYS_INLINE bool isTenured() const { return !IsInsideNursery(this); }
  MOZ_ALWAYS_INLINE const TenuredCell& asTenured() const;
  MOZ_ALWAYS_INLINE TenuredCell& asTenured();

  MOZ_ALWAYS_INLINE bool isMarkedAny() const;
  MOZ_ALWAYS_INLINE bool isMarkedBlack() const;
  MOZ_ALWAYS_INLINE bool isMarkedGray() const;
  MOZ_ALWAYS_INLINE bool isMarked(gc::MarkColor color) const;
  MOZ_ALWAYS_INLINE bool isMarkedAtLeast(gc::MarkColor color) const;

  MOZ_ALWAYS_INLINE CellColor color() const {
    return isMarkedBlack()  ? CellColor::Black
           : isMarkedGray() ? CellColor::Gray
                            : CellColor::White;
  }

  inline JSRuntime* runtimeFromMainThread() const;

  // Note: Unrestricted access to the runtime of a GC thing from an arbitrary
  // thread can easily lead to races. Use this method very carefully.
  inline JSRuntime* runtimeFromAnyThread() const;

  // May be overridden by GC thing kinds that have a compartment pointer.
  inline JS::Compartment* maybeCompartment() const { return nullptr; }

  // The StoreBuffer used to record incoming pointers from the tenured heap.
  // This will return nullptr for a tenured cell.
  inline StoreBuffer* storeBuffer() const;

  inline JS::TraceKind getTraceKind() const;

  static MOZ_ALWAYS_INLINE bool needPreWriteBarrier(JS::Zone* zone);

  template <typename T, typename = std::enable_if_t<JS::IsBaseTraceType_v<T>>>
  inline bool is() const {
    return getTraceKind() == JS::MapTypeToTraceKind<T>::kind;
  }

  template <typename T, typename = std::enable_if_t<JS::IsBaseTraceType_v<T>>>
  inline T* as() {
    // |this|-qualify the |is| call below to avoid compile errors with even
    // fairly recent versions of gcc, e.g. 7.1.1 according to bz.
    MOZ_ASSERT(this->is<T>());
    return static_cast<T*>(this);
  }

  template <typename T, typename = std::enable_if_t<JS::IsBaseTraceType_v<T>>>
  inline const T* as() const {
    // |this|-qualify the |is| call below to avoid compile errors with even
    // fairly recent versions of gcc, e.g. 7.1.1 according to bz.
    MOZ_ASSERT(this->is<T>());
    return static_cast<const T*>(this);
  }

  inline JS::Zone* zone() const;
  inline JS::Zone* zoneFromAnyThread() const;

  // Get the zone for a cell known to be in the nursery.
  inline JS::Zone* nurseryZone() const;
  inline JS::Zone* nurseryZoneFromAnyThread() const;

  // Default implementation for kinds that cannot be permanent. This may be
  // overriden by derived classes.
  MOZ_ALWAYS_INLINE bool isPermanentAndMayBeShared() const { return false; }

#ifdef DEBUG
  static inline void assertThingIsNotGray(Cell* cell);
  inline bool isAligned() const;
  void dump(GenericPrinter& out) const;
  void dump() const;
#endif

 protected:
  uintptr_t address() const;
  inline TenuredChunk* chunk() const;

  // Cells are destroyed by the GC. Do not delete them directly.
  void operator delete(void*) { MOZ_CRASH("This path is unreachable."); };
} JS_HAZ_GC_THING;

// A GC TenuredCell gets behaviors that are valid for things in the Tenured
// heap, such as access to the arena and mark bits.
class TenuredCell : public Cell {
 public:
  MOZ_ALWAYS_INLINE bool isTenured() const {
    MOZ_ASSERT(!IsInsideNursery(this));
    return true;
  }

  // Mark bit management.
  MOZ_ALWAYS_INLINE bool isMarkedAny() const;
  MOZ_ALWAYS_INLINE bool isMarkedBlack() const;
  MOZ_ALWAYS_INLINE bool isMarkedGray() const;

  // Same as Cell::color, but skips nursery checks.
  MOZ_ALWAYS_INLINE CellColor color() const {
    return isMarkedBlack()  ? CellColor::Black
           : isMarkedGray() ? CellColor::Gray
                            : CellColor::White;
  }

  // The return value indicates if the cell went from unmarked to marked.
  MOZ_ALWAYS_INLINE bool markIfUnmarked(
      MarkColor color = MarkColor::Black) const;
  MOZ_ALWAYS_INLINE void markBlack() const;
  MOZ_ALWAYS_INLINE void copyMarkBitsFrom(const TenuredCell* src);
  MOZ_ALWAYS_INLINE void unmark();

  // Access to the arena.
  inline Arena* arena() const;
  inline AllocKind getAllocKind() const;
  inline JS::TraceKind getTraceKind() const;
  inline JS::Zone* zone() const;
  inline JS::Zone* zoneFromAnyThread() const;
  inline bool isInsideZone(JS::Zone* zone) const;

  MOZ_ALWAYS_INLINE JS::shadow::Zone* shadowZone() const {
    return JS::shadow::Zone::from(zone());
  }
  MOZ_ALWAYS_INLINE JS::shadow::Zone* shadowZoneFromAnyThread() const {
    return JS::shadow::Zone::from(zoneFromAnyThread());
  }

  template <typename T, typename = std::enable_if_t<JS::IsBaseTraceType_v<T>>>
  inline bool is() const {
    return getTraceKind() == JS::MapTypeToTraceKind<T>::kind;
  }

  template <typename T, typename = std::enable_if_t<JS::IsBaseTraceType_v<T>>>
  inline T* as() {
    // |this|-qualify the |is| call below to avoid compile errors with even
    // fairly recent versions of gcc, e.g. 7.1.1 according to bz.
    MOZ_ASSERT(this->is<T>());
    return static_cast<T*>(this);
  }

  template <typename T, typename = std::enable_if_t<JS::IsBaseTraceType_v<T>>>
  inline const T* as() const {
    // |this|-qualify the |is| call below to avoid compile errors with even
    // fairly recent versions of gcc, e.g. 7.1.1 according to bz.
    MOZ_ASSERT(this->is<T>());
    return static_cast<const T*>(this);
  }

  // Default implementation for kinds that don't require fixup.
  void fixupAfterMovingGC() {}

#ifdef DEBUG
  inline bool isAligned() const;
#endif
};

MOZ_ALWAYS_INLINE const TenuredCell& Cell::asTenured() const {
  MOZ_ASSERT(isTenured());
  return *static_cast<const TenuredCell*>(this);
}

MOZ_ALWAYS_INLINE TenuredCell& Cell::asTenured() {
  MOZ_ASSERT(isTenured());
  return *static_cast<TenuredCell*>(this);
}

MOZ_ALWAYS_INLINE bool Cell::isMarkedAny() const {
  return !isTenured() || asTenured().isMarkedAny();
}

MOZ_ALWAYS_INLINE bool Cell::isMarkedBlack() const {
  return !isTenured() || asTenured().isMarkedBlack();
}

MOZ_ALWAYS_INLINE bool Cell::isMarkedGray() const {
  return isTenured() && asTenured().isMarkedGray();
}

MOZ_ALWAYS_INLINE bool Cell::isMarked(gc::MarkColor color) const {
  return color == MarkColor::Gray ? isMarkedGray() : isMarkedBlack();
}

MOZ_ALWAYS_INLINE bool Cell::isMarkedAtLeast(gc::MarkColor color) const {
  return color == MarkColor::Gray ? isMarkedAny() : isMarkedBlack();
}

inline JSRuntime* Cell::runtimeFromMainThread() const {
  JSRuntime* rt = chunk()->runtime;
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
  return rt;
}

inline JSRuntime* Cell::runtimeFromAnyThread() const {
  return chunk()->runtime;
}

inline uintptr_t Cell::address() const {
  uintptr_t addr = uintptr_t(this);
  MOZ_ASSERT(addr % CellAlignBytes == 0);
  MOZ_ASSERT(TenuredChunk::withinValidRange(addr));
  return addr;
}

TenuredChunk* Cell::chunk() const {
  uintptr_t addr = uintptr_t(this);
  MOZ_ASSERT(addr % CellAlignBytes == 0);
  addr &= ~ChunkMask;
  return reinterpret_cast<TenuredChunk*>(addr);
}

inline StoreBuffer* Cell::storeBuffer() const { return chunk()->storeBuffer; }

JS::Zone* Cell::zone() const {
  if (isTenured()) {
    return asTenured().zone();
  }

  return nurseryZone();
}

JS::Zone* Cell::zoneFromAnyThread() const {
  if (isTenured()) {
    return asTenured().zoneFromAnyThread();
  }

  return nurseryZoneFromAnyThread();
}

JS::Zone* Cell::nurseryZone() const {
  JS::Zone* zone = nurseryZoneFromAnyThread();
  MOZ_ASSERT(CurrentThreadIsGCMarking() || CurrentThreadCanAccessZone(zone));
  return zone;
}

JS::Zone* Cell::nurseryZoneFromAnyThread() const {
  return NurseryCellHeader::from(this)->zone();
}

#ifdef DEBUG
extern Cell* UninlinedForwarded(const Cell* cell);
#endif

inline JS::TraceKind Cell::getTraceKind() const {
  if (isTenured()) {
    MOZ_ASSERT_IF(isForwarded(), UninlinedForwarded(this)->getTraceKind() ==
                                     asTenured().getTraceKind());
    return asTenured().getTraceKind();
  }

  return NurseryCellHeader::from(this)->traceKind();
}

/* static */ MOZ_ALWAYS_INLINE bool Cell::needPreWriteBarrier(JS::Zone* zone) {
  return JS::shadow::Zone::from(zone)->needsIncrementalBarrier();
}

bool TenuredCell::isMarkedAny() const {
  MOZ_ASSERT(arena()->allocated());
  return chunk()->markBits.isMarkedAny(this);
}

bool TenuredCell::isMarkedBlack() const {
  MOZ_ASSERT(arena()->allocated());
  return chunk()->markBits.isMarkedBlack(this);
}

bool TenuredCell::isMarkedGray() const {
  MOZ_ASSERT(arena()->allocated());
  return chunk()->markBits.isMarkedGray(this);
}

bool TenuredCell::markIfUnmarked(MarkColor color /* = Black */) const {
  return chunk()->markBits.markIfUnmarked(this, color);
}

void TenuredCell::markBlack() const { chunk()->markBits.markBlack(this); }

void TenuredCell::copyMarkBitsFrom(const TenuredCell* src) {
  MarkBitmap& markBits = chunk()->markBits;
  markBits.copyMarkBit(this, src, ColorBit::BlackBit);
  markBits.copyMarkBit(this, src, ColorBit::GrayOrBlackBit);
}

void TenuredCell::unmark() { chunk()->markBits.unmark(this); }

inline Arena* TenuredCell::arena() const {
  MOZ_ASSERT(isTenured());
  uintptr_t addr = address();
  addr &= ~ArenaMask;
  return reinterpret_cast<Arena*>(addr);
}

AllocKind TenuredCell::getAllocKind() const { return arena()->getAllocKind(); }

JS::TraceKind TenuredCell::getTraceKind() const {
  return MapAllocToTraceKind(getAllocKind());
}

JS::Zone* TenuredCell::zone() const {
  JS::Zone* zone = arena()->zone;
  MOZ_ASSERT(CurrentThreadIsGCMarking() || CurrentThreadCanAccessZone(zone));
  return zone;
}

JS::Zone* TenuredCell::zoneFromAnyThread() const { return arena()->zone; }

bool TenuredCell::isInsideZone(JS::Zone* zone) const {
  return zone == arena()->zone;
}

// Read barrier and pre-write barrier implementation for GC cells.

template <typename T>
MOZ_ALWAYS_INLINE void ReadBarrier(T* thing) {
  static_assert(std::is_base_of_v<Cell, T>);
  static_assert(!std::is_same_v<Cell, T> && !std::is_same_v<TenuredCell, T>);

  if (thing && !thing->isPermanentAndMayBeShared()) {
    ReadBarrierImpl(thing);
  }
}

MOZ_ALWAYS_INLINE void ReadBarrierImpl(TenuredCell* thing) {
  MOZ_ASSERT(!CurrentThreadIsIonCompiling());
  MOZ_ASSERT(!CurrentThreadIsGCMarking());
  MOZ_ASSERT(thing);
  MOZ_ASSERT(CurrentThreadCanAccessZone(thing->zoneFromAnyThread()));

  // Barriers should not be triggered on main thread while collecting.
  mozilla::DebugOnly<JSRuntime*> runtime = thing->runtimeFromAnyThread();
  MOZ_ASSERT_IF(CurrentThreadCanAccessRuntime(runtime),
                !JS::RuntimeHeapIsCollecting());

  JS::shadow::Zone* shadowZone = thing->shadowZoneFromAnyThread();
  if (shadowZone->needsIncrementalBarrier()) {
    // We should only observe barriers being enabled on the main thread.
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime));
    PerformIncrementalBarrier(thing);
    return;
  }

  if (thing->isMarkedGray()) {
    // There shouldn't be anything marked gray unless we're on the main thread.
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime));
    UnmarkGrayGCThingRecursively(thing);
  }
}

MOZ_ALWAYS_INLINE void ReadBarrierImpl(Cell* thing) {
  MOZ_ASSERT(!CurrentThreadIsGCMarking());
  if (thing->isTenured()) {
    ReadBarrierImpl(&thing->asTenured());
  }
}

MOZ_ALWAYS_INLINE void PreWriteBarrierImpl(TenuredCell* thing) {
  MOZ_ASSERT(!CurrentThreadIsIonCompiling());
  MOZ_ASSERT(!CurrentThreadIsGCMarking());

  if (!thing) {
    return;
  }

  // Barriers can be triggered on the main thread while collecting, but are
  // disabled. For example, this happens when destroying HeapPtr wrappers.

  JS::shadow::Zone* zone = thing->shadowZoneFromAnyThread();
  if (!zone->needsIncrementalBarrier()) {
    return;
  }

  // Barriers can be triggered on off the main thread in two situations:
  //  - background finalization of HeapPtrs to the atoms zone
  //  - while we are verifying pre-barriers for a worker runtime
  // The barrier is not required in either case.
  bool checkThread = zone->isAtomsZone();
#ifdef JS_GC_ZEAL
  checkThread = checkThread || zone->isSelfHostingZone();
#endif
  JSRuntime* runtime = thing->runtimeFromAnyThread();
  if (checkThread && !CurrentThreadCanAccessRuntime(runtime)) {
    MOZ_ASSERT(CurrentThreadIsGCFinalizing() ||
               RuntimeIsVerifyingPreBarriers(runtime));
    return;
  }

  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime));
  MOZ_ASSERT(!RuntimeFromMainThreadIsHeapMajorCollecting(zone));
  PerformIncrementalBarrier(thing);
}

MOZ_ALWAYS_INLINE void PreWriteBarrierImpl(Cell* thing) {
  MOZ_ASSERT(!CurrentThreadIsGCMarking());
  if (thing && thing->isTenured()) {
    PreWriteBarrierImpl(&thing->asTenured());
  }
}

template <typename T>
MOZ_ALWAYS_INLINE void PreWriteBarrier(T* thing) {
  static_assert(std::is_base_of_v<Cell, T>);
  static_assert(!std::is_same_v<Cell, T> && !std::is_same_v<TenuredCell, T>);

  if (thing && !thing->isPermanentAndMayBeShared()) {
    PreWriteBarrierImpl(thing);
  }
}

// Pre-write barrier implementation for structures containing GC cells, taking a
// functor to trace the structure.
template <typename T, typename F>
MOZ_ALWAYS_INLINE void PreWriteBarrier(JS::Zone* zone, T* data,
                                       const F& traceFn) {
  MOZ_ASSERT(!CurrentThreadIsIonCompiling());
  MOZ_ASSERT(!CurrentThreadIsGCMarking());

  auto* shadowZone = JS::shadow::Zone::from(zone);
  if (!shadowZone->needsIncrementalBarrier()) {
    return;
  }

  MOZ_ASSERT(CurrentThreadCanAccessRuntime(shadowZone->runtimeFromAnyThread()));
  MOZ_ASSERT(!RuntimeFromMainThreadIsHeapMajorCollecting(shadowZone));

  traceFn(shadowZone->barrierTracer(), data);
}

// Pre-write barrier implementation for structures containing GC cells. T must
// support a |trace| method.
template <typename T>
MOZ_ALWAYS_INLINE void PreWriteBarrier(JS::Zone* zone, T* data) {
  PreWriteBarrier(zone, data, [](JSTracer* trc, T* data) { data->trace(trc); });
}

#ifdef DEBUG

/* static */ void Cell::assertThingIsNotGray(Cell* cell) {
  JS::AssertCellIsNotGray(cell);
}

bool Cell::isAligned() const {
  if (!isTenured()) {
    return true;
  }
  return asTenured().isAligned();
}

bool TenuredCell::isAligned() const {
  return Arena::isAligned(address(), arena()->getThingSize());
}

#endif

// Base class for nusery-allocatable GC things that have 32-bit length and
// 32-bit flags (currently JSString and BigInt).
//
// This tries to store both in Cell::header_, but if that isn't large enough the
// length is stored separately.
//
//          32       0
//  ------------------
//  | Length | Flags |
//  ------------------
//
// The low bits of the flags word (see CellFlagBitsReservedForGC) are reserved
// for GC. Derived classes must ensure they don't use these flags for non-GC
// purposes.
class alignas(gc::CellAlignBytes) CellWithLengthAndFlags : public Cell {
#if JS_BITS_PER_WORD == 32
  // Additional storage for length if |header_| is too small to fit both.
  uint32_t length_;
#endif

 protected:
  uint32_t headerLengthField() const {
#if JS_BITS_PER_WORD == 32
    return length_;
#else
    return uint32_t(header_ >> 32);
#endif
  }

  uint32_t headerFlagsField() const { return uint32_t(header_); }

  void setHeaderFlagBit(uint32_t flag) {
    MOZ_ASSERT((flag & RESERVED_MASK) == 0);
    header_ |= uintptr_t(flag);
  }
  void clearHeaderFlagBit(uint32_t flag) {
    MOZ_ASSERT((flag & RESERVED_MASK) == 0);
    header_ &= ~uintptr_t(flag);
  }
  void toggleHeaderFlagBit(uint32_t flag) {
    MOZ_ASSERT((flag & RESERVED_MASK) == 0);
    header_ ^= uintptr_t(flag);
  }

  void setHeaderLengthAndFlags(uint32_t len, uint32_t flags) {
    MOZ_ASSERT((flags & RESERVED_MASK) == 0);
#if JS_BITS_PER_WORD == 32
    header_ = flags;
    length_ = len;
#else
    header_ = (uint64_t(len) << 32) | uint64_t(flags);
#endif
  }

 public:
  // Returns the offset of header_. JIT code should use offsetOfFlags
  // below.
  static constexpr size_t offsetOfRawHeaderFlagsField() {
    return offsetof(CellWithLengthAndFlags, header_);
  }

  // Offsets for direct field from jit code. A number of places directly
  // access 32-bit length and flags fields so do endian trickery here.
#if JS_BITS_PER_WORD == 32
  static constexpr size_t offsetOfHeaderFlags() {
    return offsetof(CellWithLengthAndFlags, header_);
  }
  static constexpr size_t offsetOfHeaderLength() {
    return offsetof(CellWithLengthAndFlags, length_);
  }
#elif MOZ_LITTLE_ENDIAN()
  static constexpr size_t offsetOfHeaderFlags() {
    return offsetof(CellWithLengthAndFlags, header_);
  }
  static constexpr size_t offsetOfHeaderLength() {
    return offsetof(CellWithLengthAndFlags, header_) + sizeof(uint32_t);
  }
#else
  static constexpr size_t offsetOfHeaderFlags() {
    return offsetof(CellWithLengthAndFlags, header_) + sizeof(uint32_t);
  }
  static constexpr size_t offsetOfHeaderLength() {
    return offsetof(CellWithLengthAndFlags, header_);
  }
#endif
};

// Base class for non-nursery-allocatable GC things that allows storing a non-GC
// thing pointer in the first word.
//
// The low bits of the word (see CellFlagBitsReservedForGC) are reserved for GC.
template <class PtrT>
class alignas(gc::CellAlignBytes) TenuredCellWithNonGCPointer
    : public TenuredCell {
  static_assert(!std::is_pointer_v<PtrT>,
                "PtrT should be the type of the referent, not of the pointer");
  static_assert(
      !std::is_base_of_v<Cell, PtrT>,
      "Don't use TenuredCellWithNonGCPointer for pointers to GC things");

 protected:
  TenuredCellWithNonGCPointer() = default;
  explicit TenuredCellWithNonGCPointer(PtrT* initial) {
    uintptr_t data = uintptr_t(initial);
    MOZ_ASSERT((data & RESERVED_MASK) == 0);
    header_ = data;
  }

  PtrT* headerPtr() const {
    MOZ_ASSERT(flags() == 0);
    return reinterpret_cast<PtrT*>(uintptr_t(header_));
  }

  void setHeaderPtr(PtrT* newValue) {
    // As above, no flags are expected to be set here.
    uintptr_t data = uintptr_t(newValue);
    MOZ_ASSERT(flags() == 0);
    MOZ_ASSERT((data & RESERVED_MASK) == 0);
    header_ = data;
  }

 public:
  static constexpr size_t offsetOfHeaderPtr() {
    return offsetof(TenuredCellWithNonGCPointer, header_);
  }
};

// Base class for non-nursery-allocatable GC things that allows storing flags
// in the first word.
//
// The low bits of the flags word (see CellFlagBitsReservedForGC) are reserved
// for GC.
class alignas(gc::CellAlignBytes) TenuredCellWithFlags : public TenuredCell {
 protected:
  TenuredCellWithFlags() = default;
  explicit TenuredCellWithFlags(uintptr_t initial) {
    MOZ_ASSERT((initial & RESERVED_MASK) == 0);
    header_ = initial;
  }

  uintptr_t headerFlagsField() const {
    MOZ_ASSERT(flags() == 0);
    return header_;
  }

  void setHeaderFlagBits(uintptr_t flags) {
    MOZ_ASSERT((flags & RESERVED_MASK) == 0);
    header_ |= flags;
  }
  void clearHeaderFlagBits(uintptr_t flags) {
    MOZ_ASSERT((flags & RESERVED_MASK) == 0);
    header_ &= ~flags;
  }
};

// Base class for GC things that have a tenured GC pointer as their first word.
//
// The low bits of the first word (see CellFlagBitsReservedForGC) are reserved
// for GC.
//
// This includes a pre write barrier when the pointer is update. No post barrier
// is necessary as the pointer is always tenured.
template <class BaseCell, class PtrT>
class alignas(gc::CellAlignBytes) CellWithTenuredGCPointer : public BaseCell {
  static void staticAsserts() {
    // These static asserts are not in class scope because the PtrT may not be
    // defined when this class template is instantiated.
    static_assert(
        std::is_same_v<BaseCell, Cell> || std::is_same_v<BaseCell, TenuredCell>,
        "BaseCell must be either Cell or TenuredCell");
    static_assert(
        !std::is_pointer_v<PtrT>,
        "PtrT should be the type of the referent, not of the pointer");
    static_assert(
        std::is_base_of_v<Cell, PtrT>,
        "Only use CellWithTenuredGCPointer for pointers to GC things");
  }

 protected:
  CellWithTenuredGCPointer() = default;
  explicit CellWithTenuredGCPointer(PtrT* initial) { initHeaderPtr(initial); }

  void initHeaderPtr(PtrT* initial) {
    MOZ_ASSERT(!IsInsideNursery(initial));
    uintptr_t data = uintptr_t(initial);
    MOZ_ASSERT((data & Cell::RESERVED_MASK) == 0);
    this->header_ = data;
  }

  void setHeaderPtr(PtrT* newValue) {
    // As above, no flags are expected to be set here.
    MOZ_ASSERT(!IsInsideNursery(newValue));
    PreWriteBarrier(headerPtr());
    unbarrieredSetHeaderPtr(newValue);
  }

 public:
  PtrT* headerPtr() const {
    staticAsserts();
    MOZ_ASSERT(this->flags() == 0);
    return reinterpret_cast<PtrT*>(uintptr_t(this->header_));
  }

  void unbarrieredSetHeaderPtr(PtrT* newValue) {
    uintptr_t data = uintptr_t(newValue);
    MOZ_ASSERT(this->flags() == 0);
    MOZ_ASSERT((data & Cell::RESERVED_MASK) == 0);
    this->header_ = data;
  }

  static constexpr size_t offsetOfHeaderPtr() {
    return offsetof(CellWithTenuredGCPointer, header_);
  }
};

void CellHeaderPostWriteBarrier(JSObject** ptr, JSObject* prev, JSObject* next);

template <class PtrT>
class alignas(gc::CellAlignBytes) TenuredCellWithGCPointer
    : public TenuredCell {
  static void staticAsserts() {
    // These static asserts are not in class scope because the PtrT may not be
    // defined when this class template is instantiated.
    static_assert(
        !std::is_pointer_v<PtrT>,
        "PtrT should be the type of the referent, not of the pointer");
    static_assert(
        std::is_base_of_v<Cell, PtrT>,
        "Only use TenuredCellWithGCPointer for pointers to GC things");
    static_assert(
        !std::is_base_of_v<TenuredCell, PtrT>,
        "Don't use TenuredCellWithGCPointer for always-tenured GC things");
  }

 protected:
  TenuredCellWithGCPointer() = default;
  explicit TenuredCellWithGCPointer(PtrT* initial) { initHeaderPtr(initial); }

  void initHeaderPtr(PtrT* initial) {
    uintptr_t data = uintptr_t(initial);
    MOZ_ASSERT((data & Cell::RESERVED_MASK) == 0);
    this->header_ = data;
    if (IsInsideNursery(initial)) {
      CellHeaderPostWriteBarrier(headerPtrAddress(), nullptr, initial);
    }
  }

  PtrT** headerPtrAddress() {
    MOZ_ASSERT(this->flags() == 0);
    return reinterpret_cast<PtrT**>(&this->header_);
  }

 public:
  PtrT* headerPtr() const {
    MOZ_ASSERT(this->flags() == 0);
    return reinterpret_cast<PtrT*>(uintptr_t(this->header_));
  }

  void unbarrieredSetHeaderPtr(PtrT* newValue) {
    uintptr_t data = uintptr_t(newValue);
    MOZ_ASSERT(this->flags() == 0);
    MOZ_ASSERT((data & Cell::RESERVED_MASK) == 0);
    this->header_ = data;
  }

  static constexpr size_t offsetOfHeaderPtr() {
    return offsetof(TenuredCellWithGCPointer, header_);
  }
};

} /* namespace gc */
} /* namespace js */

#endif /* gc_Cell_h */
