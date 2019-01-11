/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_HeapAPI_h
#define js_HeapAPI_h

#include <limits.h>

#include "jspubtd.h"

#include "js/TraceKind.h"
#include "js/Utility.h"

/* These values are private to the JS engine. */
namespace js {

JS_FRIEND_API(bool)
CurrentThreadCanAccessZone(JS::Zone* zone);

namespace gc {

struct Cell;

/*
 * The low bit is set so this should never equal a normal pointer, and the high
 * bit is set so this should never equal the upper 32 bits of a 64-bit pointer.
 */
const uint32_t Relocated = uintptr_t(0xbad0bad1);

const size_t ArenaShift = 12;
const size_t ArenaSize = size_t(1) << ArenaShift;
const size_t ArenaMask = ArenaSize - 1;

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

/*
 * We sometimes use an index to refer to a cell in an arena. The index for a
 * cell is found by dividing by the cell alignment so not all indicies refer to
 * valid cells.
 */
const size_t ArenaCellIndexBytes = CellAlignBytes;
const size_t MaxArenaCellIndex = ArenaSize / CellAlignBytes;

/* These are magic constants derived from actual offsets in gc/Heap.h. */
#ifdef JS_GC_SMALL_CHUNK_SIZE
const size_t ChunkMarkBitmapOffset = 258104;
const size_t ChunkMarkBitmapBits = 31744;
#else
const size_t ChunkMarkBitmapOffset = 1032352;
const size_t ChunkMarkBitmapBits = 129024;
#endif
const size_t ChunkRuntimeOffset = ChunkSize - sizeof(void*);
const size_t ChunkTrailerSize = 2 * sizeof(uintptr_t) + sizeof(uint64_t);
const size_t ChunkLocationOffset = ChunkSize - ChunkTrailerSize;
const size_t ChunkStoreBufferOffset = ChunkSize - ChunkTrailerSize + sizeof(uint64_t);
const size_t ArenaZoneOffset = sizeof(size_t);
const size_t ArenaHeaderSize = sizeof(size_t) + 2 * sizeof(uintptr_t) +
                               sizeof(size_t) + sizeof(uintptr_t);

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
enum class ColorBit : uint32_t
{
    BlackBit = 0,
    GrayOrBlackBit = 1
};

/*
 * The "location" field in the Chunk trailer is a enum indicating various roles
 * of the chunk.
 */
enum class ChunkLocation : uint32_t
{
    Invalid = 0,
    Nursery = 1,
    TenuredHeap = 2
};

#ifdef JS_DEBUG
/* When downcasting, ensure we are actually the right type. */
extern JS_FRIEND_API(void)
AssertGCThingHasType(js::gc::Cell* cell, JS::TraceKind kind);
#else
inline void
AssertGCThingHasType(js::gc::Cell* cell, JS::TraceKind kind) {}
#endif

MOZ_ALWAYS_INLINE bool IsInsideNursery(const js::gc::Cell* cell);

} /* namespace gc */
} /* namespace js */

namespace JS {

/*
 * This list enumerates the different types of conceptual stacks we have in
 * SpiderMonkey. In reality, they all share the C stack, but we allow different
 * stack limits depending on the type of code running.
 */
enum StackKind
{
    StackForSystemCode,      // C++, such as the GC, running on behalf of the VM.
    StackForTrustedScript,   // Script running with trusted principals.
    StackForUntrustedScript, // Script running with untrusted principals.
    StackKindCount
};

/*
 * Default size for the generational nursery in bytes.
 * This is the initial nursery size, when running in the browser this is
 * updated by JS_SetGCParameter().
 */
const uint32_t DefaultNurseryBytes = 16 * js::gc::ChunkSize;

/* Default maximum heap size in bytes to pass to JS_NewContext(). */
const uint32_t DefaultHeapMaxBytes = 32 * 1024 * 1024;

namespace shadow {

struct Zone
{
    enum GCState : uint8_t {
        NoGC,
        Mark,
        MarkGray,
        Sweep,
        Finished,
        Compact
    };

  protected:
    JSRuntime* const runtime_;
    JSTracer* const barrierTracer_;     // A pointer to the JSRuntime's |gcMarker|.
    uint32_t needsIncrementalBarrier_;
    GCState gcState_;

    Zone(JSRuntime* runtime, JSTracer* barrierTracerArg)
      : runtime_(runtime),
        barrierTracer_(barrierTracerArg),
        needsIncrementalBarrier_(0),
        gcState_(NoGC)
    {}

  public:
    bool needsIncrementalBarrier() const {
        return needsIncrementalBarrier_;
    }

    JSTracer* barrierTracer() {
        MOZ_ASSERT(needsIncrementalBarrier_);
        MOZ_ASSERT(js::CurrentThreadCanAccessRuntime(runtime_));
        return barrierTracer_;
    }

    JSRuntime* runtimeFromActiveCooperatingThread() const {
        MOZ_ASSERT(js::CurrentThreadCanAccessRuntime(runtime_));
        return runtime_;
    }

    // Note: Unrestricted access to the zone's runtime from an arbitrary
    // thread can easily lead to races. Use this method very carefully.
    JSRuntime* runtimeFromAnyThread() const {
        return runtime_;
    }

    GCState gcState() const { return gcState_; }
    bool wasGCStarted() const { return gcState_ != NoGC; }
    bool isGCMarkingBlack() const { return gcState_ == Mark; }
    bool isGCMarkingGray() const { return gcState_ == MarkGray; }
    bool isGCSweeping() const { return gcState_ == Sweep; }
    bool isGCFinished() const { return gcState_ == Finished; }
    bool isGCCompacting() const { return gcState_ == Compact; }
    bool isGCMarking() const { return gcState_ == Mark || gcState_ == MarkGray; }
    bool isGCSweepingOrCompacting() const { return gcState_ == Sweep || gcState_ == Compact; }

    static MOZ_ALWAYS_INLINE JS::shadow::Zone* asShadowZone(JS::Zone* zone) {
        return reinterpret_cast<JS::shadow::Zone*>(zone);
    }
};

} /* namespace shadow */

/**
 * A GC pointer, tagged with the trace kind.
 *
 * In general, a GC pointer should be stored with an exact type. This class
 * is for use when that is not possible because a single pointer must point
 * to several kinds of GC thing.
 */
class JS_FRIEND_API(GCCellPtr)
{
  public:
    // Construction from a void* and trace kind.
    GCCellPtr(void* gcthing, JS::TraceKind traceKind) : ptr(checkedCast(gcthing, traceKind)) {}

    // Automatically construct a null GCCellPtr from nullptr.
    MOZ_IMPLICIT GCCellPtr(decltype(nullptr)) : ptr(checkedCast(nullptr, JS::TraceKind::Null)) {}

    // Construction from an explicit type.
    template <typename T>
    explicit GCCellPtr(T* p) : ptr(checkedCast(p, JS::MapTypeToTraceKind<T>::kind)) { }
    explicit GCCellPtr(JSFunction* p) : ptr(checkedCast(p, JS::TraceKind::Object)) { }
    explicit GCCellPtr(JSFlatString* str) : ptr(checkedCast(str, JS::TraceKind::String)) { }
    explicit GCCellPtr(const Value& v);

    JS::TraceKind kind() const {
        JS::TraceKind traceKind = JS::TraceKind(ptr & OutOfLineTraceKindMask);
        if (uintptr_t(traceKind) != OutOfLineTraceKindMask)
            return traceKind;
        return outOfLineKind();
    }

    // Allow GCCellPtr to be used in a boolean context.
    explicit operator bool() const {
        MOZ_ASSERT(bool(asCell()) == (kind() != JS::TraceKind::Null));
        return asCell();
    }

    // Simplify checks to the kind.
    template <typename T>
    bool is() const { return kind() == JS::MapTypeToTraceKind<T>::kind; }

    // Conversions to more specific types must match the kind. Access to
    // further refined types is not allowed directly from a GCCellPtr.
    template <typename T>
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
        if (is<JSString>() || is<JS::Symbol>())
            return mayBeOwnedByOtherRuntimeSlow();
        return false;
    }

  private:
    static uintptr_t checkedCast(void* p, JS::TraceKind traceKind) {
        js::gc::Cell* cell = static_cast<js::gc::Cell*>(p);
        MOZ_ASSERT((uintptr_t(p) & OutOfLineTraceKindMask) == 0);
        AssertGCThingHasType(cell, traceKind);
        // Note: the OutOfLineTraceKindMask bits are set on all out-of-line kinds
        // so that we can mask instead of branching.
        MOZ_ASSERT_IF(uintptr_t(traceKind) >= OutOfLineTraceKindMask,
                      (uintptr_t(traceKind) & OutOfLineTraceKindMask) == OutOfLineTraceKindMask);
        return uintptr_t(p) | (uintptr_t(traceKind) & OutOfLineTraceKindMask);
    }

    bool mayBeOwnedByOtherRuntimeSlow() const;

    JS::TraceKind outOfLineKind() const;

    uintptr_t ptr;
};

inline bool
operator==(const GCCellPtr& ptr1, const GCCellPtr& ptr2)
{
    return ptr1.asCell() == ptr2.asCell();
}

inline bool
operator!=(const GCCellPtr& ptr1, const GCCellPtr& ptr2)
{
    return !(ptr1 == ptr2);
}

// Unwraps the given GCCellPtr and calls the given functor with a template
// argument of the actual type of the pointer.
template <typename F, typename... Args>
auto
DispatchTyped(F f, GCCellPtr thing, Args&&... args)
  -> decltype(f(static_cast<JSObject*>(nullptr), mozilla::Forward<Args>(args)...))
{
    switch (thing.kind()) {
#define JS_EXPAND_DEF(name, type, _) \
      case JS::TraceKind::name: \
          return f(&thing.as<type>(), mozilla::Forward<Args>(args)...);
      JS_FOR_EACH_TRACEKIND(JS_EXPAND_DEF);
#undef JS_EXPAND_DEF
      default:
          MOZ_CRASH("Invalid trace kind in DispatchTyped for GCCellPtr.");
    }
}

} /* namespace JS */

namespace js {
namespace gc {
namespace detail {

static MOZ_ALWAYS_INLINE uintptr_t*
GetGCThingMarkBitmap(const uintptr_t addr)
{
    // Note: the JIT pre-barrier trampolines inline this code. Update that
    // code too when making changes here!
    MOZ_ASSERT(addr);
    const uintptr_t bmap_addr = (addr & ~ChunkMask) | ChunkMarkBitmapOffset;
    return reinterpret_cast<uintptr_t*>(bmap_addr);
}

static MOZ_ALWAYS_INLINE void
GetGCThingMarkWordAndMask(const uintptr_t addr, ColorBit colorBit,
                          uintptr_t** wordp, uintptr_t* maskp)
{
    // Note: the JIT pre-barrier trampolines inline this code. Update that
    // code too when making changes here!
    MOZ_ASSERT(addr);
    const size_t bit = (addr & js::gc::ChunkMask) / js::gc::CellBytesPerMarkBit +
                       static_cast<uint32_t>(colorBit);
    MOZ_ASSERT(bit < js::gc::ChunkMarkBitmapBits);
    uintptr_t* bitmap = GetGCThingMarkBitmap(addr);
    const uintptr_t nbits = sizeof(*bitmap) * CHAR_BIT;
    *maskp = uintptr_t(1) << (bit % nbits);
    *wordp = &bitmap[bit / nbits];
}

static MOZ_ALWAYS_INLINE JS::Zone*
GetGCThingZone(const uintptr_t addr)
{
    MOZ_ASSERT(addr);
    const uintptr_t zone_addr = (addr & ~ArenaMask) | ArenaZoneOffset;
    return *reinterpret_cast<JS::Zone**>(zone_addr);
}

static MOZ_ALWAYS_INLINE bool
TenuredCellIsMarkedGray(const Cell* cell)
{
    // Return true if GrayOrBlackBit is set and BlackBit is not set.
    MOZ_ASSERT(cell);
    MOZ_ASSERT(!js::gc::IsInsideNursery(cell));

    uintptr_t* grayWord, grayMask;
    js::gc::detail::GetGCThingMarkWordAndMask(uintptr_t(cell), js::gc::ColorBit::GrayOrBlackBit,
                                              &grayWord, &grayMask);
    if (!(*grayWord & grayMask))
        return false;

    uintptr_t* blackWord, blackMask;
    js::gc::detail::GetGCThingMarkWordAndMask(uintptr_t(cell), js::gc::ColorBit::BlackBit,
                                              &blackWord, &blackMask);
    return !(*blackWord & blackMask);
}

static MOZ_ALWAYS_INLINE bool
CellIsMarkedGray(const Cell* cell)
{
    MOZ_ASSERT(cell);
    if (js::gc::IsInsideNursery(cell))
        return false;
    return TenuredCellIsMarkedGray(cell);
}

extern JS_PUBLIC_API(bool)
CellIsMarkedGrayIfKnown(const Cell* cell);

#ifdef DEBUG
extern JS_PUBLIC_API(bool)
CellIsNotGray(const Cell* cell);

extern JS_PUBLIC_API(bool)
ObjectIsMarkedBlack(const JSObject* obj);
#endif

MOZ_ALWAYS_INLINE ChunkLocation
GetCellLocation(const void* cell)
{
    uintptr_t addr = uintptr_t(cell);
    addr &= ~js::gc::ChunkMask;
    addr |= js::gc::ChunkLocationOffset;
    return *reinterpret_cast<ChunkLocation*>(addr);
}

MOZ_ALWAYS_INLINE bool
NurseryCellHasStoreBuffer(const void* cell)
{
    uintptr_t addr = uintptr_t(cell);
    addr &= ~js::gc::ChunkMask;
    addr |= js::gc::ChunkStoreBufferOffset;
    return *reinterpret_cast<void**>(addr) != nullptr;
}

} /* namespace detail */

MOZ_ALWAYS_INLINE bool
IsInsideNursery(const js::gc::Cell* cell)
{
    if (!cell)
        return false;
    auto location = detail::GetCellLocation(cell);
    MOZ_ASSERT(location == ChunkLocation::Nursery || location == ChunkLocation::TenuredHeap);
    return location == ChunkLocation::Nursery;
}

MOZ_ALWAYS_INLINE bool
IsCellPointerValid(const void* cell)
{
    auto addr = uintptr_t(cell);
    if (addr < ChunkSize || addr % CellAlignBytes != 0)
        return false;
    auto location = detail::GetCellLocation(cell);
    if (location == ChunkLocation::TenuredHeap)
        return !!detail::GetGCThingZone(addr);
    if (location == ChunkLocation::Nursery)
        return detail::NurseryCellHasStoreBuffer(cell);
    return false;
}

MOZ_ALWAYS_INLINE bool
IsCellPointerValidOrNull(const void* cell)
{
    if (!cell)
        return true;
    return IsCellPointerValid(cell);
}

} /* namespace gc */
} /* namespace js */

namespace JS {

static MOZ_ALWAYS_INLINE Zone*
GetTenuredGCThingZone(GCCellPtr thing)
{
    MOZ_ASSERT(!js::gc::IsInsideNursery(thing.asCell()));
    return js::gc::detail::GetGCThingZone(thing.unsafeAsUIntPtr());
}

extern JS_PUBLIC_API(Zone*)
GetNurseryStringZone(JSString* str);

static MOZ_ALWAYS_INLINE Zone*
GetStringZone(JSString* str)
{
    if (!js::gc::IsInsideNursery(reinterpret_cast<js::gc::Cell*>(str)))
        return js::gc::detail::GetGCThingZone(reinterpret_cast<uintptr_t>(str));
    return GetNurseryStringZone(str);
}

extern JS_PUBLIC_API(Zone*)
GetObjectZone(JSObject* obj);

extern JS_PUBLIC_API(Zone*)
GetValueZone(const Value& value);

static MOZ_ALWAYS_INLINE bool
GCThingIsMarkedGray(GCCellPtr thing)
{
    if (thing.mayBeOwnedByOtherRuntime())
        return false;
    return js::gc::detail::CellIsMarkedGrayIfKnown(thing.asCell());
}

extern JS_PUBLIC_API(JS::TraceKind)
GCThingTraceKind(void* thing);

extern JS_PUBLIC_API(void)
EnableNurseryStrings(JSContext* cx);

extern JS_PUBLIC_API(void)
DisableNurseryStrings(JSContext* cx);

/*
 * Returns true when writes to GC thing pointers (and reads from weak pointers)
 * must call an incremental barrier. This is generally only true when running
 * mutator code in-between GC slices. At other times, the barrier may be elided
 * for performance.
 */
extern JS_PUBLIC_API(bool)
IsIncrementalBarrierNeeded(JSContext* cx);

/*
 * Notify the GC that a reference to a JSObject is about to be overwritten.
 * This method must be called if IsIncrementalBarrierNeeded.
 */
extern JS_PUBLIC_API(void)
IncrementalPreWriteBarrier(JSObject* obj);

/*
 * Notify the GC that a weak reference to a GC thing has been read.
 * This method must be called if IsIncrementalBarrierNeeded.
 */
extern JS_PUBLIC_API(void)
IncrementalReadBarrier(GCCellPtr thing);

/**
 * Unsets the gray bit for anything reachable from |thing|. |kind| should not be
 * JS::TraceKind::Shape. |thing| should be non-null. The return value indicates
 * if anything was unmarked.
 */
extern JS_FRIEND_API(bool)
UnmarkGrayGCThingRecursively(GCCellPtr thing);

} // namespace JS

namespace js {
namespace gc {

static MOZ_ALWAYS_INLINE bool
IsIncrementalBarrierNeededOnTenuredGCThing(const JS::GCCellPtr thing)
{
    MOZ_ASSERT(thing);
    MOZ_ASSERT(!js::gc::IsInsideNursery(thing.asCell()));

    // TODO: I'd like to assert !CurrentThreadIsHeapBusy() here but this gets
    // called while we are tracing the heap, e.g. during memory reporting
    // (see bug 1313318).
    MOZ_ASSERT(!JS::CurrentThreadIsHeapCollecting());

    JS::Zone* zone = JS::GetTenuredGCThingZone(thing);
    return JS::shadow::Zone::asShadowZone(zone)->needsIncrementalBarrier();
}

static MOZ_ALWAYS_INLINE void
ExposeGCThingToActiveJS(JS::GCCellPtr thing)
{
    // GC things residing in the nursery cannot be gray: they have no mark bits.
    // All live objects in the nursery are moved to tenured at the beginning of
    // each GC slice, so the gray marker never sees nursery things.
    if (IsInsideNursery(thing.asCell()))
        return;

    // There's nothing to do for permanent GC things that might be owned by
    // another runtime.
    if (thing.mayBeOwnedByOtherRuntime())
        return;

    if (IsIncrementalBarrierNeededOnTenuredGCThing(thing))
        JS::IncrementalReadBarrier(thing);
    else if (js::gc::detail::TenuredCellIsMarkedGray(thing.asCell()))
        JS::UnmarkGrayGCThingRecursively(thing);

    MOZ_ASSERT(!js::gc::detail::TenuredCellIsMarkedGray(thing.asCell()));
}

template <typename T>
extern JS_PUBLIC_API(bool)
EdgeNeedsSweepUnbarrieredSlow(T* thingp);

static MOZ_ALWAYS_INLINE bool
EdgeNeedsSweepUnbarriered(JSObject** objp)
{
    // This function does not handle updating nursery pointers. Raw JSObject
    // pointers should be updated separately or replaced with
    // JS::Heap<JSObject*> which handles this automatically.
    MOZ_ASSERT(!JS::CurrentThreadIsHeapMinorCollecting());
    if (IsInsideNursery(reinterpret_cast<Cell*>(*objp)))
        return false;

    auto zone = JS::shadow::Zone::asShadowZone(detail::GetGCThingZone(uintptr_t(*objp)));
    if (!zone->isGCSweepingOrCompacting())
        return false;

    return EdgeNeedsSweepUnbarrieredSlow(objp);
}

} // namespace gc
} // namesapce js

namespace JS {

/*
 * This should be called when an object that is marked gray is exposed to the JS
 * engine (by handing it to running JS code or writing it into live JS
 * data). During incremental GC, since the gray bits haven't been computed yet,
 * we conservatively mark the object black.
 */
static MOZ_ALWAYS_INLINE void
ExposeObjectToActiveJS(JSObject* obj)
{
    MOZ_ASSERT(obj);
    MOZ_ASSERT(!js::gc::EdgeNeedsSweepUnbarrieredSlow(&obj));
    js::gc::ExposeGCThingToActiveJS(GCCellPtr(obj));
}

static MOZ_ALWAYS_INLINE void
ExposeScriptToActiveJS(JSScript* script)
{
    MOZ_ASSERT(!js::gc::EdgeNeedsSweepUnbarrieredSlow(&script));
    js::gc::ExposeGCThingToActiveJS(GCCellPtr(script));
}

} /* namespace JS */

#endif /* js_HeapAPI_h */
