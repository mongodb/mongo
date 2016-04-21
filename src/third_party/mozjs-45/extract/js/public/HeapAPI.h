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

// Whether the current thread is permitted access to any part of the specified
// runtime or zone.
JS_FRIEND_API(bool)
CurrentThreadCanAccessRuntime(JSRuntime* rt);

JS_FRIEND_API(bool)
CurrentThreadCanAccessZone(JS::Zone* zone);

namespace gc {

struct Cell;

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

const size_t CellShift = 3;
const size_t CellSize = size_t(1) << CellShift;
const size_t CellMask = CellSize - 1;

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
const size_t ArenaZoneOffset = 0;

/*
 * Live objects are marked black. How many other additional colors are available
 * depends on the size of the GCThing. Objects marked gray are eligible for
 * cycle collection.
 */
static const uint32_t BLACK = 0;
static const uint32_t GRAY = 1;

/*
 * The "location" field in the Chunk trailer is a bit vector indicting various
 * roles of the chunk.
 *
 * The value 0 for the "location" field is invalid, at least one bit must be
 * set.
 *
 * Some bits preclude others, for example, any "nursery" bit precludes any
 * "tenured" or "middle generation" bit.
 */
const uintptr_t ChunkLocationBitNursery = 1;       // Standard GGC nursery
const uintptr_t ChunkLocationBitTenuredHeap = 2;   // Standard GGC tenured generation

const uintptr_t ChunkLocationAnyNursery = ChunkLocationBitNursery;

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
struct Zone;

/* Default size for the generational nursery in bytes. */
const uint32_t DefaultNurseryBytes = 16 * js::gc::ChunkSize;

/* Default maximum heap size in bytes to pass to JS_NewRuntime(). */
const uint32_t DefaultHeapMaxBytes = 32 * 1024 * 1024;

namespace shadow {

struct Zone
{
  protected:
    JSRuntime* const runtime_;
    JSTracer* const barrierTracer_;     // A pointer to the JSRuntime's |gcMarker|.

  public:
    bool needsIncrementalBarrier_;

    Zone(JSRuntime* runtime, JSTracer* barrierTracerArg)
      : runtime_(runtime),
        barrierTracer_(barrierTracerArg),
        needsIncrementalBarrier_(false)
    {}

    bool needsIncrementalBarrier() const {
        return needsIncrementalBarrier_;
    }

    JSTracer* barrierTracer() {
        MOZ_ASSERT(needsIncrementalBarrier_);
        MOZ_ASSERT(js::CurrentThreadCanAccessRuntime(runtime_));
        return barrierTracer_;
    }

    JSRuntime* runtimeFromMainThread() const {
        MOZ_ASSERT(js::CurrentThreadCanAccessRuntime(runtime_));
        return runtime_;
    }

    // Note: Unrestricted access to the zone's runtime from an arbitrary
    // thread can easily lead to races. Use this method very carefully.
    JSRuntime* runtimeFromAnyThread() const {
        return runtime_;
    }

    static JS::shadow::Zone* asShadowZone(JS::Zone* zone) {
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

    bool mayBeOwnedByOtherRuntime() const;

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
    MOZ_ASSERT(addr);
    const uintptr_t bmap_addr = (addr & ~ChunkMask) | ChunkMarkBitmapOffset;
    return reinterpret_cast<uintptr_t*>(bmap_addr);
}

static MOZ_ALWAYS_INLINE JS::shadow::Runtime*
GetGCThingRuntime(const uintptr_t addr)
{
    MOZ_ASSERT(addr);
    const uintptr_t rt_addr = (addr & ~ChunkMask) | ChunkRuntimeOffset;
    return *reinterpret_cast<JS::shadow::Runtime**>(rt_addr);
}

static MOZ_ALWAYS_INLINE void
GetGCThingMarkWordAndMask(const uintptr_t addr, uint32_t color,
                          uintptr_t** wordp, uintptr_t* maskp)
{
    MOZ_ASSERT(addr);
    const size_t bit = (addr & js::gc::ChunkMask) / js::gc::CellSize + color;
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
CellIsMarkedGray(const Cell* cell)
{
    MOZ_ASSERT(cell);
    MOZ_ASSERT(!js::gc::IsInsideNursery(cell));
    uintptr_t* word, mask;
    js::gc::detail::GetGCThingMarkWordAndMask(uintptr_t(cell), js::gc::GRAY, &word, &mask);
    return *word & mask;
}

} /* namespace detail */

MOZ_ALWAYS_INLINE bool
IsInsideNursery(const js::gc::Cell* cell)
{
    if (!cell)
        return false;
    uintptr_t addr = uintptr_t(cell);
    addr &= ~js::gc::ChunkMask;
    addr |= js::gc::ChunkLocationOffset;
    uint32_t location = *reinterpret_cast<uint32_t*>(addr);
    MOZ_ASSERT(location != 0);
    return location & ChunkLocationAnyNursery;
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

static MOZ_ALWAYS_INLINE Zone*
GetStringZone(JSString* str)
{
    return js::gc::detail::GetGCThingZone(uintptr_t(str));
}

extern JS_PUBLIC_API(Zone*)
GetObjectZone(JSObject* obj);

static MOZ_ALWAYS_INLINE bool
ObjectIsTenured(JSObject* obj)
{
    return !js::gc::IsInsideNursery(reinterpret_cast<js::gc::Cell*>(obj));
}

static MOZ_ALWAYS_INLINE bool
ObjectIsMarkedGray(JSObject* obj)
{
    /*
     * GC things residing in the nursery cannot be gray: they have no mark bits.
     * All live objects in the nursery are moved to tenured at the beginning of
     * each GC slice, so the gray marker never sees nursery things.
     */
    if (js::gc::IsInsideNursery(reinterpret_cast<js::gc::Cell*>(obj)))
        return false;
    return js::gc::detail::CellIsMarkedGray(reinterpret_cast<js::gc::Cell*>(obj));
}

static MOZ_ALWAYS_INLINE bool
ScriptIsMarkedGray(JSScript* script)
{
    return js::gc::detail::CellIsMarkedGray(reinterpret_cast<js::gc::Cell*>(script));
}

static MOZ_ALWAYS_INLINE bool
GCThingIsMarkedGray(GCCellPtr thing)
{
    if (js::gc::IsInsideNursery(thing.asCell()))
        return false;
    if (thing.mayBeOwnedByOtherRuntime())
        return false;
    return js::gc::detail::CellIsMarkedGray(thing.asCell());
}

} /* namespace JS */

namespace js {
namespace gc {

static MOZ_ALWAYS_INLINE bool
IsIncrementalBarrierNeededOnTenuredGCThing(JS::shadow::Runtime* rt, const JS::GCCellPtr thing)
{
    MOZ_ASSERT(thing);
    MOZ_ASSERT(!js::gc::IsInsideNursery(thing.asCell()));
    if (rt->isHeapBusy())
        return false;
    JS::Zone* zone = JS::GetTenuredGCThingZone(thing);
    return JS::shadow::Zone::asShadowZone(zone)->needsIncrementalBarrier();
}

/**
 * Create an object providing access to the garbage collector's internal notion
 * of the current state of memory (both GC heap memory and GCthing-controlled
 * malloc memory.
 */
extern JS_PUBLIC_API(JSObject*)
NewMemoryInfoObject(JSContext* cx);

} /* namespace gc */
} /* namespace js */

#endif /* js_HeapAPI_h */
