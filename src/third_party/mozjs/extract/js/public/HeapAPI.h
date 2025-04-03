/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_HeapAPI_h
#define js_HeapAPI_h

#include "mozilla/Atomics.h"
#include "mozilla/BitSet.h"

#include <limits.h>
#include <type_traits>

#include "js/AllocPolicy.h"
#include "js/GCAnnotations.h"
#include "js/HashTable.h"
#include "js/shadow/String.h"  // JS::shadow::String
#include "js/shadow/Symbol.h"  // JS::shadow::Symbol
#include "js/shadow/Zone.h"    // JS::shadow::Zone
#include "js/TraceKind.h"
#include "js/TypeDecls.h"

/* These values are private to the JS engine. */
namespace js {

class NurseryDecommitTask;

JS_PUBLIC_API bool CurrentThreadCanAccessZone(JS::Zone* zone);

namespace gc {

class Arena;
struct Cell;
class TenuredChunk;
class StoreBuffer;
class TenuredCell;

const size_t ArenaShift = 12;
const size_t ArenaSize = size_t(1) << ArenaShift;
const size_t ArenaMask = ArenaSize - 1;

#if defined(XP_MACOSX) && defined(__aarch64__)
const size_t PageShift = 14;
#else
const size_t PageShift = 12;
#endif
// Expected page size, so we could initialze ArenasPerPage at compile-time.
// The actual system page size should be queried by SystemPageSize().
const size_t PageSize = size_t(1) << PageShift;
constexpr size_t ArenasPerPage = PageSize / ArenaSize;

#ifdef JS_GC_SMALL_CHUNK_SIZE
const size_t ChunkShift = 18;
#else
const size_t ChunkShift = 20;
#endif
const size_t ChunkSize = size_t(1) << ChunkShift;
const size_t ChunkMask = ChunkSize - 1;

const size_t CellAlignShift = 3;
const size_t CellAlignBytes = size_t(1) << CellAlignShift;
const size_t CellAlignMask = CellAlignBytes - 1;

const size_t CellBytesPerMarkBit = CellAlignBytes;
const size_t MarkBitsPerCell = 2;

/*
 * The mark bitmap has one bit per each possible cell start position. This
 * wastes some space for larger GC things but allows us to avoid division by the
 * cell's size when accessing the bitmap.
 */
const size_t ArenaBitmapBits = ArenaSize / CellBytesPerMarkBit;
const size_t ArenaBitmapBytes = HowMany(ArenaBitmapBits, 8);
const size_t ArenaBitmapWords = HowMany(ArenaBitmapBits, JS_BITS_PER_WORD);

// The base class for all GC chunks, either in the nursery or in the tenured
// heap memory. This structure is locatable from any GC pointer by aligning to
// the chunk size.
class alignas(CellAlignBytes) ChunkBase {
 protected:
  ChunkBase(JSRuntime* rt, StoreBuffer* sb) {
    MOZ_ASSERT((uintptr_t(this) & ChunkMask) == 0);
    initBase(rt, sb);
  }

  void initBase(JSRuntime* rt, StoreBuffer* sb) {
    runtime = rt;
    storeBuffer = sb;
  }

 public:
  // The store buffer for pointers from tenured things to things in this
  // chunk. Will be non-null if and only if this is a nursery chunk.
  StoreBuffer* storeBuffer;

  // Provide quick access to the runtime from absolutely anywhere.
  JSRuntime* runtime;
};

// Information about tenured heap chunks.
struct TenuredChunkInfo {
 private:
  friend class ChunkPool;
  TenuredChunk* next = nullptr;
  TenuredChunk* prev = nullptr;

 public:
  /* Number of free arenas, either committed or decommitted. */
  uint32_t numArenasFree;

  /* Number of free, committed arenas. */
  uint32_t numArenasFreeCommitted;
};

/*
 * Calculating ArenasPerChunk:
 *
 * To figure out how many Arenas will fit in a chunk we need to know how much
 * extra space is available after we allocate the header data. This is a problem
 * because the header size depends on the number of arenas in the chunk.
 *
 * The dependent fields are markBits, decommittedPages and
 * freeCommittedArenas. markBits needs ArenaBitmapBytes bytes per arena,
 * decommittedPages needs one bit per page and freeCommittedArenas needs one
 * bit per arena.
 *
 * We can calculate an approximate value by dividing the number of bits of free
 * space in the chunk by the number of bits needed per arena. This is an
 * approximation because it doesn't take account of the fact that the variable
 * sized fields must be rounded up to a whole number of words, or any padding
 * the compiler adds between fields.
 *
 * Fortunately, for the chunk and arena size parameters we use this
 * approximation turns out to be correct. If it were not we might need to adjust
 * the arena count down by one to allow more space for the padding.
 */
const size_t BitsPerPageWithHeaders =
    (ArenaSize + ArenaBitmapBytes) * ArenasPerPage * CHAR_BIT + ArenasPerPage +
    1;
const size_t ChunkBitsAvailable =
    (ChunkSize - sizeof(ChunkBase) - sizeof(TenuredChunkInfo)) * CHAR_BIT;
const size_t PagesPerChunk = ChunkBitsAvailable / BitsPerPageWithHeaders;
const size_t ArenasPerChunk = PagesPerChunk * ArenasPerPage;
const size_t FreeCommittedBits = ArenasPerChunk;
const size_t DecommitBits = PagesPerChunk;
const size_t BitsPerArenaWithHeaders =
    (ArenaSize + ArenaBitmapBytes) * CHAR_BIT +
    (DecommitBits / ArenasPerChunk) + 1;

const size_t CalculatedChunkSizeRequired =
    sizeof(ChunkBase) + sizeof(TenuredChunkInfo) +
    RoundUp(ArenasPerChunk * ArenaBitmapBytes, sizeof(uintptr_t)) +
    RoundUp(FreeCommittedBits, sizeof(uint32_t) * CHAR_BIT) / CHAR_BIT +
    RoundUp(DecommitBits, sizeof(uint32_t) * CHAR_BIT) / CHAR_BIT +
    ArenasPerChunk * ArenaSize;
static_assert(CalculatedChunkSizeRequired <= ChunkSize,
              "Calculated ArenasPerChunk is too large");

const size_t CalculatedChunkPadSize = ChunkSize - CalculatedChunkSizeRequired;
static_assert(CalculatedChunkPadSize * CHAR_BIT < BitsPerArenaWithHeaders,
              "Calculated ArenasPerChunk is too small");

// Define a macro for the expected number of arenas so its value appears in the
// error message if the assertion fails.
#ifdef JS_GC_SMALL_CHUNK_SIZE
#  define EXPECTED_ARENA_COUNT 63
#else
#  define EXPECTED_ARENA_COUNT 252
#endif
static_assert(ArenasPerChunk == EXPECTED_ARENA_COUNT,
              "Do not accidentally change our heap's density.");
#undef EXPECTED_ARENA_COUNT

// Mark bitmaps are atomic because they can be written by gray unmarking on the
// main thread while read by sweeping on a background thread. The former does
// not affect the result of the latter.
using MarkBitmapWord = mozilla::Atomic<uintptr_t, mozilla::Relaxed>;

/*
 * Live objects are marked black or gray. Everything reachable from a JS root is
 * marked black. Objects marked gray are eligible for cycle collection.
 *
 *    BlackBit:     GrayOrBlackBit:  Color:
 *       0               0           white
 *       0               1           gray
 *       1               0           black
 *       1               1           black
 */
enum class ColorBit : uint32_t { BlackBit = 0, GrayOrBlackBit = 1 };

// Mark colors. Order is important here: the greater value the 'more marked' a
// cell is.
enum class MarkColor : uint8_t { Gray = 1, Black = 2 };

// Mark bitmap for a tenured heap chunk.
struct MarkBitmap {
  static constexpr size_t WordCount = ArenaBitmapWords * ArenasPerChunk;
  MarkBitmapWord bitmap[WordCount];

  inline void getMarkWordAndMask(const TenuredCell* cell, ColorBit colorBit,
                                 MarkBitmapWord** wordp, uintptr_t* maskp);

  // The following are not exported and are defined in gc/Heap.h:
  inline bool markBit(const TenuredCell* cell, ColorBit colorBit);
  inline bool isMarkedAny(const TenuredCell* cell);
  inline bool isMarkedBlack(const TenuredCell* cell);
  inline bool isMarkedGray(const TenuredCell* cell);
  inline bool markIfUnmarked(const TenuredCell* cell, MarkColor color);
  inline bool markIfUnmarkedAtomic(const TenuredCell* cell, MarkColor color);
  inline void markBlack(const TenuredCell* cell);
  inline void markBlackAtomic(const TenuredCell* cell);
  inline void copyMarkBit(TenuredCell* dst, const TenuredCell* src,
                          ColorBit colorBit);
  inline void unmark(const TenuredCell* cell);
  inline MarkBitmapWord* arenaBits(Arena* arena);
};

static_assert(ArenaBitmapBytes * ArenasPerChunk == sizeof(MarkBitmap),
              "Ensure our MarkBitmap actually covers all arenas.");

// Bitmap with one bit per page used for decommitted page set.
using ChunkPageBitmap = mozilla::BitSet<PagesPerChunk, uint32_t>;

// Bitmap with one bit per arena used for free committed arena set.
using ChunkArenaBitmap = mozilla::BitSet<ArenasPerChunk, uint32_t>;

// Base class containing data members for a tenured heap chunk.
class TenuredChunkBase : public ChunkBase {
 public:
  TenuredChunkInfo info;
  MarkBitmap markBits;
  ChunkArenaBitmap freeCommittedArenas;
  ChunkPageBitmap decommittedPages;

 protected:
  explicit TenuredChunkBase(JSRuntime* runtime) : ChunkBase(runtime, nullptr) {
    info.numArenasFree = ArenasPerChunk;
  }

  void initAsDecommitted();
};

/*
 * We sometimes use an index to refer to a cell in an arena. The index for a
 * cell is found by dividing by the cell alignment so not all indices refer to
 * valid cells.
 */
const size_t ArenaCellIndexBytes = CellAlignBytes;
const size_t MaxArenaCellIndex = ArenaSize / CellAlignBytes;

const size_t MarkBitmapWordBits = sizeof(MarkBitmapWord) * CHAR_BIT;

constexpr size_t FirstArenaAdjustmentBits =
    RoundUp(sizeof(gc::TenuredChunkBase), ArenaSize) / gc::CellBytesPerMarkBit;

static_assert((FirstArenaAdjustmentBits % MarkBitmapWordBits) == 0);
constexpr size_t FirstArenaAdjustmentWords =
    FirstArenaAdjustmentBits / MarkBitmapWordBits;

const size_t ChunkStoreBufferOffset = offsetof(ChunkBase, storeBuffer);
const size_t ChunkMarkBitmapOffset = offsetof(TenuredChunkBase, markBits);

// Hardcoded offsets into Arena class.
const size_t ArenaZoneOffset = 2 * sizeof(uint32_t);
const size_t ArenaHeaderSize = ArenaZoneOffset + 2 * sizeof(uintptr_t) +
                               sizeof(size_t) + sizeof(uintptr_t);

// The first word of a GC thing has certain requirements from the GC and is used
// to store flags in the low bits.
const size_t CellFlagBitsReservedForGC = 3;

// The first word can be used to store JSClass pointers for some thing kinds, so
// these must be suitably aligned.
const size_t JSClassAlignBytes = size_t(1) << CellFlagBitsReservedForGC;

#ifdef JS_DEBUG
/* When downcasting, ensure we are actually the right type. */
extern JS_PUBLIC_API void AssertGCThingHasType(js::gc::Cell* cell,
                                               JS::TraceKind kind);
#else
inline void AssertGCThingHasType(js::gc::Cell* cell, JS::TraceKind kind) {}
#endif

MOZ_ALWAYS_INLINE bool IsInsideNursery(const js::gc::Cell* cell);
MOZ_ALWAYS_INLINE bool IsInsideNursery(const js::gc::TenuredCell* cell);

} /* namespace gc */
} /* namespace js */

namespace JS {

enum class HeapState {
  Idle,             // doing nothing with the GC heap
  Tracing,          // tracing the GC heap without collecting, e.g.
                    // IterateCompartments()
  MajorCollecting,  // doing a GC of the major heap
  MinorCollecting,  // doing a GC of the minor heap (nursery)
  CycleCollecting   // in the "Unlink" phase of cycle collection
};

JS_PUBLIC_API HeapState RuntimeHeapState();

static inline bool RuntimeHeapIsBusy() {
  return RuntimeHeapState() != HeapState::Idle;
}

static inline bool RuntimeHeapIsTracing() {
  return RuntimeHeapState() == HeapState::Tracing;
}

static inline bool RuntimeHeapIsMajorCollecting() {
  return RuntimeHeapState() == HeapState::MajorCollecting;
}

static inline bool RuntimeHeapIsMinorCollecting() {
  return RuntimeHeapState() == HeapState::MinorCollecting;
}

static inline bool RuntimeHeapIsCollecting(HeapState state) {
  return state == HeapState::MajorCollecting ||
         state == HeapState::MinorCollecting;
}

static inline bool RuntimeHeapIsCollecting() {
  return RuntimeHeapIsCollecting(RuntimeHeapState());
}

static inline bool RuntimeHeapIsCycleCollecting() {
  return RuntimeHeapState() == HeapState::CycleCollecting;
}

/*
 * This list enumerates the different types of conceptual stacks we have in
 * SpiderMonkey. In reality, they all share the C stack, but we allow different
 * stack limits depending on the type of code running.
 */
enum StackKind {
  StackForSystemCode,       // C++, such as the GC, running on behalf of the VM.
  StackForTrustedScript,    // Script running with trusted principals.
  StackForUntrustedScript,  // Script running with untrusted principals.
  StackKindCount
};

/*
 * Default maximum size for the generational nursery in bytes. This is the
 * initial value. In the browser this configured by the
 * javascript.options.mem.nursery.max_kb pref.
 */
const uint32_t DefaultNurseryMaxBytes = 16 * js::gc::ChunkSize;

/* Default maximum heap size in bytes to pass to JS_NewContext(). */
const uint32_t DefaultHeapMaxBytes = 32 * 1024 * 1024;

/**
 * A GC pointer, tagged with the trace kind.
 *
 * In general, a GC pointer should be stored with an exact type. This class
 * is for use when that is not possible because a single pointer must point
 * to several kinds of GC thing.
 */
class JS_PUBLIC_API GCCellPtr {
 public:
  GCCellPtr() : GCCellPtr(nullptr) {}

  // Construction from a void* and trace kind.
  GCCellPtr(void* gcthing, JS::TraceKind traceKind)
      : ptr(checkedCast(gcthing, traceKind)) {}

  // Automatically construct a null GCCellPtr from nullptr.
  MOZ_IMPLICIT GCCellPtr(decltype(nullptr))
      : ptr(checkedCast(nullptr, JS::TraceKind::Null)) {}

  // Construction from an explicit type.
  template <typename T>
  explicit GCCellPtr(T* p)
      : ptr(checkedCast(p, JS::MapTypeToTraceKind<T>::kind)) {}
  explicit GCCellPtr(JSFunction* p)
      : ptr(checkedCast(p, JS::TraceKind::Object)) {}
  explicit GCCellPtr(JSScript* p)
      : ptr(checkedCast(p, JS::TraceKind::Script)) {}
  explicit GCCellPtr(const Value& v);

  JS::TraceKind kind() const {
    uintptr_t kindBits = ptr & OutOfLineTraceKindMask;
    if (kindBits != OutOfLineTraceKindMask) {
      return JS::TraceKind(kindBits);
    }
    return outOfLineKind();
  }

  // Allow GCCellPtr to be used in a boolean context.
  explicit operator bool() const {
    MOZ_ASSERT(bool(asCell()) == (kind() != JS::TraceKind::Null));
    return asCell();
  }

  // Simplify checks to the kind.
  template <typename T, typename = std::enable_if_t<JS::IsBaseTraceType_v<T>>>
  bool is() const {
    return kind() == JS::MapTypeToTraceKind<T>::kind;
  }

  // Conversions to more specific types must match the kind. Access to
  // further refined types is not allowed directly from a GCCellPtr.
  template <typename T, typename = std::enable_if_t<JS::IsBaseTraceType_v<T>>>
  T& as() const {
    MOZ_ASSERT(kind() == JS::MapTypeToTraceKind<T>::kind);
    // We can't use static_cast here, because the fact that JSObject
    // inherits from js::gc::Cell is not part of the public API.
    return *reinterpret_cast<T*>(asCell());
  }

  // Return a pointer to the cell this |GCCellPtr| refers to, or |nullptr|.
  // (It would be more symmetrical with |to| for this to return a |Cell&|, but
  // the result can be |nullptr|, and null references are undefined behavior.)
  js::gc::Cell* asCell() const {
    return reinterpret_cast<js::gc::Cell*>(ptr & ~OutOfLineTraceKindMask);
  }

  // The CC's trace logger needs an identity that is XPIDL serializable.
  uint64_t unsafeAsInteger() const {
    return static_cast<uint64_t>(unsafeAsUIntPtr());
  }
  // Inline mark bitmap access requires direct pointer arithmetic.
  uintptr_t unsafeAsUIntPtr() const {
    MOZ_ASSERT(asCell());
    MOZ_ASSERT(!js::gc::IsInsideNursery(asCell()));
    return reinterpret_cast<uintptr_t>(asCell());
  }

  MOZ_ALWAYS_INLINE bool mayBeOwnedByOtherRuntime() const {
    if (!is<JSString>() && !is<JS::Symbol>()) {
      return false;
    }
    if (is<JSString>()) {
      return JS::shadow::String::isPermanentAtom(asCell());
    }
    MOZ_ASSERT(is<JS::Symbol>());
    return JS::shadow::Symbol::isWellKnownSymbol(asCell());
  }

 private:
  static uintptr_t checkedCast(void* p, JS::TraceKind traceKind) {
    auto* cell = static_cast<js::gc::Cell*>(p);
    MOZ_ASSERT((uintptr_t(p) & OutOfLineTraceKindMask) == 0);
    AssertGCThingHasType(cell, traceKind);
    // Store trace in the bottom bits of pointer for common kinds.
    uintptr_t kindBits = uintptr_t(traceKind);
    if (kindBits >= OutOfLineTraceKindMask) {
      kindBits = OutOfLineTraceKindMask;
    }
    return uintptr_t(p) | kindBits;
  }

  JS::TraceKind outOfLineKind() const;

  uintptr_t ptr;
} JS_HAZ_GC_POINTER;

// Unwraps the given GCCellPtr, calls the functor |f| with a template argument
// of the actual type of the pointer, and returns the result.
template <typename F>
auto MapGCThingTyped(GCCellPtr thing, F&& f) {
  switch (thing.kind()) {
#define JS_EXPAND_DEF(name, type, _, _1) \
  case JS::TraceKind::name:              \
    return f(&thing.as<type>());
    JS_FOR_EACH_TRACEKIND(JS_EXPAND_DEF);
#undef JS_EXPAND_DEF
    default:
      MOZ_CRASH("Invalid trace kind in MapGCThingTyped for GCCellPtr.");
  }
}

// Unwraps the given GCCellPtr and calls the functor |f| with a template
// argument of the actual type of the pointer. Doesn't return anything.
template <typename F>
void ApplyGCThingTyped(GCCellPtr thing, F&& f) {
  // This function doesn't do anything but is supplied for symmetry with other
  // MapGCThingTyped/ApplyGCThingTyped implementations that have to wrap the
  // functor to return a dummy value that is ignored.
  MapGCThingTyped(thing, f);
}

} /* namespace JS */

// These are defined in the toplevel namespace instead of within JS so that
// they won't shadow other operator== overloads (see bug 1456512.)

inline bool operator==(JS::GCCellPtr ptr1, JS::GCCellPtr ptr2) {
  return ptr1.asCell() == ptr2.asCell();
}

inline bool operator!=(JS::GCCellPtr ptr1, JS::GCCellPtr ptr2) {
  return !(ptr1 == ptr2);
}

namespace js {
namespace gc {

/* static */
MOZ_ALWAYS_INLINE void MarkBitmap::getMarkWordAndMask(const TenuredCell* cell,
                                                      ColorBit colorBit,
                                                      MarkBitmapWord** wordp,
                                                      uintptr_t* maskp) {
  // Note: the JIT pre-barrier trampolines inline this code. Update
  // MacroAssembler::emitPreBarrierFastPath code too when making changes here!

  MOZ_ASSERT(size_t(colorBit) < MarkBitsPerCell);

  size_t offset = uintptr_t(cell) & ChunkMask;
  const size_t bit = offset / CellBytesPerMarkBit + size_t(colorBit);
  size_t word = bit / MarkBitmapWordBits - FirstArenaAdjustmentWords;
  MOZ_ASSERT(word < WordCount);
  *wordp = &bitmap[word];
  *maskp = uintptr_t(1) << (bit % MarkBitmapWordBits);
}

namespace detail {

static MOZ_ALWAYS_INLINE ChunkBase* GetCellChunkBase(const Cell* cell) {
  MOZ_ASSERT(cell);
  return reinterpret_cast<ChunkBase*>(uintptr_t(cell) & ~ChunkMask);
}

static MOZ_ALWAYS_INLINE TenuredChunkBase* GetCellChunkBase(
    const TenuredCell* cell) {
  MOZ_ASSERT(cell);
  return reinterpret_cast<TenuredChunkBase*>(uintptr_t(cell) & ~ChunkMask);
}

static MOZ_ALWAYS_INLINE JS::Zone* GetTenuredGCThingZone(const uintptr_t addr) {
  MOZ_ASSERT(addr);
  const uintptr_t zone_addr = (addr & ~ArenaMask) | ArenaZoneOffset;
  return *reinterpret_cast<JS::Zone**>(zone_addr);
}

static MOZ_ALWAYS_INLINE bool TenuredCellIsMarkedBlack(
    const TenuredCell* cell) {
  // Return true if BlackBit is set.

  MOZ_ASSERT(cell);
  MOZ_ASSERT(!js::gc::IsInsideNursery(cell));

  MarkBitmapWord* blackWord;
  uintptr_t blackMask;
  TenuredChunkBase* chunk = GetCellChunkBase(cell);
  chunk->markBits.getMarkWordAndMask(cell, js::gc::ColorBit::BlackBit,
                                     &blackWord, &blackMask);
  return *blackWord & blackMask;
}

static MOZ_ALWAYS_INLINE bool NonBlackCellIsMarkedGray(
    const TenuredCell* cell) {
  // Return true if GrayOrBlackBit is set. Callers should check BlackBit first.

  MOZ_ASSERT(cell);
  MOZ_ASSERT(!js::gc::IsInsideNursery(cell));
  MOZ_ASSERT(!TenuredCellIsMarkedBlack(cell));

  MarkBitmapWord* grayWord;
  uintptr_t grayMask;
  TenuredChunkBase* chunk = GetCellChunkBase(cell);
  chunk->markBits.getMarkWordAndMask(cell, js::gc::ColorBit::GrayOrBlackBit,
                                     &grayWord, &grayMask);
  return *grayWord & grayMask;
}

static MOZ_ALWAYS_INLINE bool TenuredCellIsMarkedGray(const TenuredCell* cell) {
  return !TenuredCellIsMarkedBlack(cell) && NonBlackCellIsMarkedGray(cell);
}

static MOZ_ALWAYS_INLINE bool CellIsMarkedGray(const Cell* cell) {
  MOZ_ASSERT(cell);
  if (js::gc::IsInsideNursery(cell)) {
    return false;
  }
  return TenuredCellIsMarkedGray(reinterpret_cast<const TenuredCell*>(cell));
}

extern JS_PUBLIC_API bool CanCheckGrayBits(const TenuredCell* cell);

extern JS_PUBLIC_API bool CellIsMarkedGrayIfKnown(const TenuredCell* cell);

#ifdef DEBUG
extern JS_PUBLIC_API void AssertCellIsNotGray(const Cell* cell);

extern JS_PUBLIC_API bool ObjectIsMarkedBlack(const JSObject* obj);
#endif

MOZ_ALWAYS_INLINE bool CellHasStoreBuffer(const Cell* cell) {
  return GetCellChunkBase(cell)->storeBuffer;
}

} /* namespace detail */

MOZ_ALWAYS_INLINE bool IsInsideNursery(const Cell* cell) {
  MOZ_ASSERT(cell);
  return detail::CellHasStoreBuffer(cell);
}

MOZ_ALWAYS_INLINE bool IsInsideNursery(const TenuredCell* cell) {
  MOZ_ASSERT(cell);
  MOZ_ASSERT(!IsInsideNursery(reinterpret_cast<const Cell*>(cell)));
  return false;
}

// Allow use before the compiler knows the derivation of JSObject, JSString, and
// JS::BigInt.
MOZ_ALWAYS_INLINE bool IsInsideNursery(const JSObject* obj) {
  return IsInsideNursery(reinterpret_cast<const Cell*>(obj));
}
MOZ_ALWAYS_INLINE bool IsInsideNursery(const JSString* str) {
  return IsInsideNursery(reinterpret_cast<const Cell*>(str));
}
MOZ_ALWAYS_INLINE bool IsInsideNursery(const JS::BigInt* bi) {
  return IsInsideNursery(reinterpret_cast<const Cell*>(bi));
}

MOZ_ALWAYS_INLINE bool IsCellPointerValid(const void* ptr) {
  auto addr = uintptr_t(ptr);
  if (addr < ChunkSize || addr % CellAlignBytes != 0) {
    return false;
  }

  auto* cell = reinterpret_cast<const Cell*>(ptr);
  if (!IsInsideNursery(cell)) {
    return detail::GetTenuredGCThingZone(addr) != nullptr;
  }

  return true;
}

MOZ_ALWAYS_INLINE bool IsCellPointerValidOrNull(const void* cell) {
  if (!cell) {
    return true;
  }
  return IsCellPointerValid(cell);
}

} /* namespace gc */
} /* namespace js */

namespace JS {

static MOZ_ALWAYS_INLINE Zone* GetTenuredGCThingZone(GCCellPtr thing) {
  MOZ_ASSERT(!js::gc::IsInsideNursery(thing.asCell()));
  return js::gc::detail::GetTenuredGCThingZone(thing.unsafeAsUIntPtr());
}

extern JS_PUBLIC_API Zone* GetNurseryCellZone(js::gc::Cell* cell);

static MOZ_ALWAYS_INLINE Zone* GetGCThingZone(GCCellPtr thing) {
  if (!js::gc::IsInsideNursery(thing.asCell())) {
    return js::gc::detail::GetTenuredGCThingZone(thing.unsafeAsUIntPtr());
  }

  return GetNurseryCellZone(thing.asCell());
}

static MOZ_ALWAYS_INLINE Zone* GetStringZone(JSString* str) {
  if (!js::gc::IsInsideNursery(str)) {
    return js::gc::detail::GetTenuredGCThingZone(
        reinterpret_cast<uintptr_t>(str));
  }
  return GetNurseryCellZone(reinterpret_cast<js::gc::Cell*>(str));
}

extern JS_PUBLIC_API Zone* GetObjectZone(JSObject* obj);

static MOZ_ALWAYS_INLINE bool GCThingIsMarkedGray(GCCellPtr thing) {
  js::gc::Cell* cell = thing.asCell();
  if (IsInsideNursery(cell)) {
    return false;
  }

  auto* tenuredCell = reinterpret_cast<js::gc::TenuredCell*>(cell);
  return js::gc::detail::CellIsMarkedGrayIfKnown(tenuredCell);
}

// Specialised gray marking check for use by the cycle collector. This is not
// called during incremental GC or when the gray bits are invalid.
static MOZ_ALWAYS_INLINE bool GCThingIsMarkedGrayInCC(GCCellPtr thing) {
  js::gc::Cell* cell = thing.asCell();
  if (IsInsideNursery(cell)) {
    return false;
  }

  auto* tenuredCell = reinterpret_cast<js::gc::TenuredCell*>(cell);
  if (!js::gc::detail::TenuredCellIsMarkedGray(tenuredCell)) {
    return false;
  }

  MOZ_ASSERT(js::gc::detail::CanCheckGrayBits(tenuredCell));

  return true;
}

extern JS_PUBLIC_API JS::TraceKind GCThingTraceKind(void* thing);

extern JS_PUBLIC_API void EnableNurseryStrings(JSContext* cx);

extern JS_PUBLIC_API void DisableNurseryStrings(JSContext* cx);

extern JS_PUBLIC_API void EnableNurseryBigInts(JSContext* cx);

extern JS_PUBLIC_API void DisableNurseryBigInts(JSContext* cx);

/*
 * Returns true when writes to GC thing pointers (and reads from weak pointers)
 * must call an incremental barrier. This is generally only true when running
 * mutator code in-between GC slices. At other times, the barrier may be elided
 * for performance.
 */
extern JS_PUBLIC_API bool IsIncrementalBarrierNeeded(JSContext* cx);

/*
 * Notify the GC that a reference to a JSObject is about to be overwritten.
 * This method must be called if IsIncrementalBarrierNeeded.
 */
extern JS_PUBLIC_API void IncrementalPreWriteBarrier(JSObject* obj);

/*
 * Notify the GC that a reference to a tenured GC cell is about to be
 * overwritten. This method must be called if IsIncrementalBarrierNeeded.
 */
extern JS_PUBLIC_API void IncrementalPreWriteBarrier(GCCellPtr thing);

/**
 * Unsets the gray bit for anything reachable from |thing|. |kind| should not be
 * JS::TraceKind::Shape. |thing| should be non-null. The return value indicates
 * if anything was unmarked.
 */
extern JS_PUBLIC_API bool UnmarkGrayGCThingRecursively(GCCellPtr thing);

}  // namespace JS

namespace js {
namespace gc {

extern JS_PUBLIC_API void PerformIncrementalReadBarrier(JS::GCCellPtr thing);

static MOZ_ALWAYS_INLINE void ExposeGCThingToActiveJS(JS::GCCellPtr thing) {
  // TODO: I'd like to assert !RuntimeHeapIsBusy() here but this gets
  // called while we are tracing the heap, e.g. during memory reporting
  // (see bug 1313318).
  MOZ_ASSERT(!JS::RuntimeHeapIsCollecting());

  // GC things residing in the nursery cannot be gray: they have no mark bits.
  // All live objects in the nursery are moved to tenured at the beginning of
  // each GC slice, so the gray marker never sees nursery things.
  if (IsInsideNursery(thing.asCell())) {
    return;
  }

  auto* cell = reinterpret_cast<TenuredCell*>(thing.asCell());
  if (detail::TenuredCellIsMarkedBlack(cell)) {
    return;
  }

  // GC things owned by other runtimes are always black.
  MOZ_ASSERT(!thing.mayBeOwnedByOtherRuntime());

  auto* zone = JS::shadow::Zone::from(JS::GetTenuredGCThingZone(thing));
  if (zone->needsIncrementalBarrier()) {
    PerformIncrementalReadBarrier(thing);
  } else if (!zone->isGCPreparing() && detail::NonBlackCellIsMarkedGray(cell)) {
    MOZ_ALWAYS_TRUE(JS::UnmarkGrayGCThingRecursively(thing));
  }

  MOZ_ASSERT_IF(!zone->isGCPreparing(), !detail::TenuredCellIsMarkedGray(cell));
}

static MOZ_ALWAYS_INLINE void IncrementalReadBarrier(JS::GCCellPtr thing) {
  // This is a lighter version of ExposeGCThingToActiveJS that doesn't do gray
  // unmarking.

  if (IsInsideNursery(thing.asCell())) {
    return;
  }

  auto* zone = JS::shadow::Zone::from(JS::GetTenuredGCThingZone(thing));
  auto* cell = reinterpret_cast<TenuredCell*>(thing.asCell());
  if (zone->needsIncrementalBarrier() &&
      !detail::TenuredCellIsMarkedBlack(cell)) {
    // GC things owned by other runtimes are always black.
    MOZ_ASSERT(!thing.mayBeOwnedByOtherRuntime());
    PerformIncrementalReadBarrier(thing);
  }
}

template <typename T>
extern JS_PUBLIC_API bool EdgeNeedsSweepUnbarrieredSlow(T* thingp);

static MOZ_ALWAYS_INLINE bool EdgeNeedsSweepUnbarriered(JSObject** objp) {
  // This function does not handle updating nursery pointers. Raw JSObject
  // pointers should be updated separately or replaced with
  // JS::Heap<JSObject*> which handles this automatically.
  MOZ_ASSERT(!JS::RuntimeHeapIsMinorCollecting());
  if (IsInsideNursery(*objp)) {
    return false;
  }

  auto zone =
      JS::shadow::Zone::from(detail::GetTenuredGCThingZone(uintptr_t(*objp)));
  if (!zone->isGCSweepingOrCompacting()) {
    return false;
  }

  return EdgeNeedsSweepUnbarrieredSlow(objp);
}

}  // namespace gc
}  // namespace js

namespace JS {

/*
 * This should be called when an object that is marked gray is exposed to the JS
 * engine (by handing it to running JS code or writing it into live JS
 * data). During incremental GC, since the gray bits haven't been computed yet,
 * we conservatively mark the object black.
 */
static MOZ_ALWAYS_INLINE void ExposeObjectToActiveJS(JSObject* obj) {
  MOZ_ASSERT(obj);
  MOZ_ASSERT(!js::gc::EdgeNeedsSweepUnbarrieredSlow(&obj));
  js::gc::ExposeGCThingToActiveJS(GCCellPtr(obj));
}

} /* namespace JS */

#endif /* js_HeapAPI_h */
