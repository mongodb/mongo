/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Inline definitions of the CellAllocator methods.
 *
 * This is included from JSContext-inl.h for the definiton of JSContext::newCell
 * and shouldn't need to be included elsewhere.
 */

#ifndef gc_Allocator_inl_h
#define gc_Allocator_inl_h

#include "gc/Allocator.h"

#include "gc/Cell.h"
#include "gc/Zone.h"
#include "js/Class.h"
#include "js/RootingAPI.h"

#include "gc/Nursery-inl.h"

namespace js {
namespace gc {

template <typename T, AllowGC allowGC, typename... Args>
T* CellAllocator::NewCell(JSContext* cx, Args&&... args) {
  static_assert(std::is_base_of_v<gc::Cell, T>);

  // Objects. See the valid parameter list in NewObject, above.
  if constexpr (std::is_base_of_v<JSObject, T>) {
    return NewObject<T, allowGC>(cx, std::forward<Args>(args)...);
  }

  // BigInt
  else if constexpr (std::is_base_of_v<JS::BigInt, T>) {
    return NewBigInt<T, allowGC>(cx, std::forward<Args>(args)...);
  }

  // "Normal" strings (all of which can be nursery allocated). Atoms and
  // external strings will fall through to the generic code below. All other
  // strings go through NewString, which will forward the arguments to the
  // appropriate string class's constructor.
  else if constexpr (std::is_base_of_v<JSString, T> &&
                     !std::is_base_of_v<JSAtom, T> &&
                     !std::is_base_of_v<JSExternalString, T>) {
    return NewString<T, allowGC>(cx, std::forward<Args>(args)...);
  }

  else {
    // Allocate a new tenured GC thing that's not nursery-allocatable. Use
    // cx->newCell<T>(...), where the parameters are forwarded to the type's
    // constructor.
    return NewTenuredCell<T, allowGC>(cx, std::forward<Args>(args)...);
  }
}

template <typename T, AllowGC allowGC, typename... Args>
/* static */
T* CellAllocator::NewString(JSContext* cx, gc::Heap heap, Args&&... args) {
  static_assert(std::is_base_of_v<JSString, T>);
  gc::AllocKind kind = gc::MapTypeToAllocKind<T>::kind;
  void* ptr = AllocNurseryOrTenuredCell<JS::TraceKind::String, allowGC>(
      cx, kind, sizeof(T), heap, nullptr);
  if (MOZ_UNLIKELY(!ptr)) {
    return nullptr;
  }
  return new (mozilla::KnownNotNull, ptr) T(std::forward<Args>(args)...);
}

template <typename T, AllowGC allowGC>
/* static */
T* CellAllocator::NewBigInt(JSContext* cx, Heap heap) {
  void* ptr = AllocNurseryOrTenuredCell<JS::TraceKind::BigInt, allowGC>(
      cx, gc::AllocKind::BIGINT, sizeof(T), heap, nullptr);
  if (MOZ_UNLIKELY(!ptr)) {
    return nullptr;
  }
  return new (mozilla::KnownNotNull, ptr) T();
}

template <typename T, AllowGC allowGC>
/* static */
T* CellAllocator::NewObject(JSContext* cx, gc::AllocKind kind, gc::Heap heap,
                            const JSClass* clasp, gc::AllocSite* site) {
  MOZ_ASSERT(IsObjectAllocKind(kind));
  MOZ_ASSERT_IF(heap != gc::Heap::Tenured && clasp->hasFinalize() &&
                    !clasp->isProxyObject(),
                CanNurseryAllocateFinalizedClass(clasp));
  size_t thingSize = JSObject::thingSize(kind);
  void* cell = AllocNurseryOrTenuredCell<JS::TraceKind::Object, allowGC>(
      cx, kind, thingSize, heap, site);
  if (MOZ_UNLIKELY(!cell)) {
    return nullptr;
  }
  return new (mozilla::KnownNotNull, cell) T();
}

template <typename T, AllowGC allowGC, typename... Args>
/* static */
T* CellAllocator::NewTenuredCell(JSContext* cx, Args&&... args) {
  gc::AllocKind kind = gc::MapTypeToAllocKind<T>::kind;
  MOZ_ASSERT(Arena::thingSize(kind) == sizeof(T));
  void* cell = AllocTenuredCell<allowGC>(cx, kind);
  if (MOZ_UNLIKELY(!cell)) {
    return nullptr;
  }
  return new (mozilla::KnownNotNull, cell) T(std::forward<Args>(args)...);
}

#if defined(DEBUG) || defined(JS_GC_ZEAL) || defined(JS_OOM_BREAKPOINT)

// This serves as a single point to perform some unrelated checks that happens
// before every allocation. Performs the following:
//
//  - checks we can't GC inside a JS::AutoAssertNoGC region
//  - runs a zeal GC if needed
//
// This is a no-op in release builds.
//
// This is only called on paths where GC is allowed.
inline void PreAllocGCChecks(JSContext* cx) {
  // Crash if we could perform a GC action when it is not safe.
  if (!cx->suppressGC) {
    cx->verifyIsSafeToGC();
  }

#  ifdef JS_GC_ZEAL
  GCRuntime* gc = &cx->runtime()->gc;
  if (gc->needZealousGC()) {
    gc->runDebugGC();
  }
#  endif
}

inline bool CheckForSimulatedFailure(JSContext* cx, AllowGC allowGC) {
  // For testing out of memory conditions.
  if (js::oom::ShouldFailWithOOM()) {
    // If we are doing a fallible allocation, percolate up the OOM instead of
    // reporting it.
    if (allowGC) {
      ReportOutOfMemory(cx);
    }
    return false;
  }

  return true;
}
#else

inline void PreAllocGCChecks(JSContext* cx) {}
inline bool CheckForSimulatedFailure(JSContext* cx, AllowGC allowGC) {
  return true;
}

#endif  // DEBUG || JS_GC_ZEAL || JS_OOM_BREAKPOINT

template <JS::TraceKind traceKind, AllowGC allowGC>
/* static */
void* CellAllocator::AllocNurseryOrTenuredCell(JSContext* cx,
                                               gc::AllocKind allocKind,
                                               size_t thingSize, gc::Heap heap,
                                               AllocSite* site) {
  MOZ_ASSERT(IsNurseryAllocable(allocKind));
  MOZ_ASSERT(MapAllocToTraceKind(allocKind) == traceKind);
  MOZ_ASSERT(thingSize == Arena::thingSize(allocKind));
  MOZ_ASSERT_IF(site && site->initialHeap() == Heap::Tenured,
                heap == Heap::Tenured);
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));

  if constexpr (allowGC) {
    PreAllocGCChecks(cx);
  }

  if (!CheckForSimulatedFailure(cx, allowGC)) {
    return nullptr;
  }

  JS::Zone* zone = cx->zone();
  gc::Heap minHeapToTenure = CheckedHeap(zone->minHeapToTenure(traceKind));
  if (CheckedHeap(heap) < minHeapToTenure) {
    if (!site) {
      site = zone->unknownAllocSite(traceKind);
    }

#ifdef JS_GC_ZEAL
    site = MaybeGenerateMissingAllocSite(cx, traceKind, site);
#endif

    void* ptr = cx->nursery().tryAllocateCell(site, thingSize, traceKind);
    if (MOZ_LIKELY(ptr)) {
      return ptr;
    }

    return RetryNurseryAlloc<allowGC>(cx, traceKind, allocKind, thingSize,
                                      site);
  }

  return AllocTenuredCellForNurseryAlloc<allowGC>(cx, allocKind);
}

/* static */
MOZ_ALWAYS_INLINE gc::Heap CellAllocator::CheckedHeap(gc::Heap heap) {
  if (heap > Heap::Tenured) {
    // This helps the compiler to see that nursery allocation is never
    // possible if Heap::Tenured is specified.
    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("Bad gc::Heap value");
  }

  return heap;
}

}  // namespace gc
}  // namespace js

#endif  // gc_Allocator_inl_h
