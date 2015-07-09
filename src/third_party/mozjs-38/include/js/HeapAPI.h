/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_HeapAPI_h
#define js_HeapAPI_h

#include <limits.h>

#include "js/TracingAPI.h"
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
const size_t ChunkLocationOffset = ChunkSize - 2 * sizeof(void*) - sizeof(uint64_t);
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
AssertGCThingHasType(js::gc::Cell* cell, JSGCTraceKind kind);
#else
inline void
AssertGCThingHasType(js::gc::Cell* cell, JSGCTraceKind kind) {}
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

// A GC pointer, tagged with the trace kind.
//
// In general, a GC pointer should be stored with an exact type. This class
// is for use when that is not possible because a single pointer must point
// to several kinds of GC thing.
class JS_FRIEND_API(GCCellPtr)
{
  public:
    // Construction from a void* and trace kind.
    GCCellPtr(void* gcthing, JSGCTraceKind traceKind) : ptr(checkedCast(gcthing, traceKind)) {}

    // Construction from an explicit type.
    explicit GCCellPtr(JSObject* obj) : ptr(checkedCast(obj, JSTRACE_OBJECT)) { }
    explicit GCCellPtr(JSFunction* fun) : ptr(checkedCast(fun, JSTRACE_OBJECT)) { }
    explicit GCCellPtr(JSString* str) : ptr(checkedCast(str, JSTRACE_STRING)) { }
    explicit GCCellPtr(JSFlatString* str) : ptr(checkedCast(str, JSTRACE_STRING)) { }
    explicit GCCellPtr(JSScript* script) : ptr(checkedCast(script, JSTRACE_SCRIPT)) { }
    explicit GCCellPtr(const Value& v);

    // Not all compilers have nullptr_t yet, so use this instead of GCCellPtr(nullptr).
    static GCCellPtr NullPtr() { return GCCellPtr(nullptr, JSTRACE_NULL); }

    JSGCTraceKind kind() const {
        JSGCTraceKind traceKind = JSGCTraceKind(ptr & JSTRACE_OUTOFLINE);
        if (traceKind != JSTRACE_OUTOFLINE)
            return traceKind;
        return outOfLineKind();
    }

    // Allow GCCellPtr to be used in a boolean context.
    explicit operator bool() const {
        MOZ_ASSERT(bool(asCell()) == (kind() != JSTRACE_NULL));
        return asCell();
    }

    // Simplify checks to the kind.
    bool isObject() const { return kind() == JSTRACE_OBJECT; }
    bool isScript() const { return kind() == JSTRACE_SCRIPT; }
    bool isString() const { return kind() == JSTRACE_STRING; }
    bool isSymbol() const { return kind() == JSTRACE_SYMBOL; }
    bool isShape() const { return kind() == JSTRACE_SHAPE; }

    // Conversions to more specific types must match the kind. Access to
    // further refined types is not allowed directly from a GCCellPtr.
    JSObject* toObject() const {
        MOZ_ASSERT(kind() == JSTRACE_OBJECT);
        return reinterpret_cast<JSObject*>(asCell());
    }
    JSString* toString() const {
        MOZ_ASSERT(kind() == JSTRACE_STRING);
        return reinterpret_cast<JSString*>(asCell());
    }
    JSScript* toScript() const {
        MOZ_ASSERT(kind() == JSTRACE_SCRIPT);
        return reinterpret_cast<JSScript*>(asCell());
    }
    Symbol* toSymbol() const {
        MOZ_ASSERT(kind() == JSTRACE_SYMBOL);
        return reinterpret_cast<Symbol*>(asCell());
    }
    js::gc::Cell* asCell() const {
        return reinterpret_cast<js::gc::Cell*>(ptr & ~JSTRACE_OUTOFLINE);
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

  private:
    uintptr_t checkedCast(void* p, JSGCTraceKind traceKind) {
        js::gc::Cell* cell = static_cast<js::gc::Cell*>(p);
        MOZ_ASSERT((uintptr_t(p) & JSTRACE_OUTOFLINE) == 0);
        AssertGCThingHasType(cell, traceKind);
        // Note: the JSTRACE_OUTOFLINE bits are set on all out-of-line kinds
        // so that we can mask instead of branching.
        MOZ_ASSERT_IF(traceKind >= JSTRACE_OUTOFLINE,
                      (traceKind & JSTRACE_OUTOFLINE) == JSTRACE_OUTOFLINE);
        return uintptr_t(p) | (traceKind & JSTRACE_OUTOFLINE);
    }

    JSGCTraceKind outOfLineKind() const;

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
GetTenuredGCThingZone(void* thing)
{
    MOZ_ASSERT(!js::gc::IsInsideNursery((js::gc::Cell*)thing));
    return js::gc::detail::GetGCThingZone(uintptr_t(thing));
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
    if (!rt->needsIncrementalBarrier())
        return false;
    JS::Zone* zone = JS::GetTenuredGCThingZone(thing.asCell());
    return JS::shadow::Zone::asShadowZone(zone)->needsIncrementalBarrier();
}

} /* namespace gc */
} /* namespace js */

#endif /* js_HeapAPI_h */
