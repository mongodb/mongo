/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Allocator_h
#define gc_Allocator_h

#include "mozilla/OperatorNewExtensions.h"

#include <stdint.h>

#include "gc/AllocKind.h"
#include "gc/GCEnum.h"
#include "js/TypeDecls.h"

namespace js {
namespace gc {

class AllocSite;
struct Cell;
class TenuredCell;
class TenuringTracer;

// Allocator implementation functions. SpiderMonkey code outside this file
// should use:
//
//     cx->newCell<T>(...)
//
// or optionally:
//
//     cx->newCell<T, AllowGC::NoGC>(...)
//
// `friend` js::gc::CellAllocator in a subtype T of Cell in order to allow it to
// be allocated with cx->newCell<T>(...). The friend declaration will allow
// calling T's constructor.
//
// The parameters will be passed to a type-specific function or constructor. For
// nursery-allocatable types, see e.g. the NewString, NewObject, and NewBigInt
// methods. For all other types, the parameters will be forwarded to the
// constructor.
class CellAllocator {
 public:
  // This is the entry point for all allocation, though callers should still not
  // use this directly. Use cx->newCell<T>(...) instead.
  //
  // After a successful allocation the caller must fully initialize the thing
  // before calling any function that can potentially trigger GC. This will
  // ensure that GC tracing never sees junk values stored in the partially
  // initialized thing.
  template <typename T, js::AllowGC allowGC = CanGC, typename... Args>
  static inline T* NewCell(JSContext* cx, Args&&... args);

 private:
  template <AllowGC allowGC>
  static void* RetryNurseryAlloc(JSContext* cx, JS::TraceKind traceKind,
                                 AllocKind allocKind, size_t thingSize,
                                 AllocSite* site);
  template <AllowGC allowGC>
  static void* TryNewTenuredCell(JSContext* cx, AllocKind kind,
                                 size_t thingSize);

#if defined(DEBUG) || defined(JS_GC_ZEAL) || defined(JS_OOM_BREAKPOINT)
  template <AllowGC allowGC>
  static bool PreAllocChecks(JSContext* cx, AllocKind kind);
#else
  template <AllowGC allowGC>
  static bool PreAllocChecks(JSContext* cx, AllocKind kind) {
    return true;
  }
#endif

#ifdef JS_GC_ZEAL
  static AllocSite* MaybeGenerateMissingAllocSite(JSContext* cx,
                                                  JS::TraceKind traceKind,
                                                  AllocSite* site);
#endif

#ifdef DEBUG
  static void CheckIncrementalZoneState(JSContext* cx, void* ptr);
#endif

  static inline gc::Heap CheckedHeap(gc::Heap heap);

  // Allocate a cell in the nursery, unless |heap| is Heap::Tenured or nursery
  // allocation is disabled for |traceKind| in the current zone.
  template <JS::TraceKind traceKind, AllowGC allowGC>
  static void* AllocNurseryOrTenuredCell(JSContext* cx, gc::AllocKind allocKind,
                                         size_t thingSize, gc::Heap heap,
                                         AllocSite* site);
  friend class TenuringTracer;

  // Allocate a cell in the tenured heap.
  template <AllowGC allowGC>
  static void* AllocTenuredCell(JSContext* cx, gc::AllocKind kind, size_t size);

  // Allocate a string. Use cx->newCell<T>([heap]).
  //
  // Use for nursery-allocatable strings. Returns a value cast to the correct
  // type. Non-nursery-allocatable strings will go through the fallback
  // tenured-only allocation path.
  template <typename T, AllowGC allowGC, typename... Args>
  static T* NewString(JSContext* cx, gc::Heap heap, Args&&... args);

  template <typename T, AllowGC allowGC>
  static T* NewBigInt(JSContext* cx, Heap heap);

  template <typename T, AllowGC allowGC>
  static T* NewObject(JSContext* cx, gc::AllocKind kind, gc::Heap heap,
                      const JSClass* clasp, gc::AllocSite* site = nullptr);

  // Allocate all other kinds of GC thing.
  template <typename T, AllowGC allowGC, typename... Args>
  static T* NewTenuredCell(JSContext* cx, Args&&... args);
};

}  // namespace gc
}  // namespace js

#endif  // gc_Allocator_h
