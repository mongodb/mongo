/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// SpiderMonkey GC allocation API.

#ifndef gc_Allocator_h
#define gc_Allocator_h

#include "mozilla/OperatorNewExtensions.h"

#include <stdint.h>

#include "gc/AllocKind.h"
#include "gc/GCEnum.h"
#include "js/HeapAPI.h"
#include "js/TypeDecls.h"

namespace js {
namespace gc {

class AllocSite;
struct Cell;
class BufferAllocator;
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
  template <typename T, AllowGC allowGC = CanGC, typename... Args>
  static inline T* NewCell(JSContext* cx, Args&&... args);
  friend class BufferAllocator;

 private:
  // Allocate a string. Use cx->newCell<T>([heap]).
  //
  // Use for nursery-allocatable strings. Returns a value cast to the correct
  // type. Non-nursery-allocatable strings will go through the fallback
  // tenured-only allocation path.
  template <typename T, AllowGC allowGC, typename... Args>
  static T* NewString(JSContext* cx, Heap heap, Args&&... args);

  template <typename T, AllowGC allowGC>
  static T* NewBigInt(JSContext* cx, Heap heap);

  template <typename T, AllowGC allowGC>
  static T* NewObject(JSContext* cx, AllocKind kind, Heap heap,
                      const JSClass* clasp, AllocSite* site = nullptr);

  // Allocate all other kinds of GC thing.
  template <typename T, AllowGC allowGC, typename... Args>
  static T* NewTenuredCell(JSContext* cx, Args&&... args);

  // Allocate a cell in the nursery, unless |heap| is Heap::Tenured or nursery
  // allocation is disabled for |traceKind| in the current zone.
  template <JS::TraceKind traceKind, AllowGC allowGC>
  static void* AllocNurseryOrTenuredCell(JSContext* cx, AllocKind allocKind,
                                         size_t thingSize, Heap heap,
                                         AllocSite* site);
  friend class TenuringTracer;

  template <AllowGC allowGC>
  static void* RetryNurseryAlloc(JSContext* cx, JS::TraceKind traceKind,
                                 AllocKind allocKind, size_t thingSize,
                                 AllocSite* site);
  template <AllowGC allowGC>
  static void* AllocTenuredCellForNurseryAlloc(JSContext* cx, AllocKind kind);

  // Allocate a cell in the tenured heap.
  template <AllowGC allowGC>
  static void* AllocTenuredCell(JSContext* cx, AllocKind kind);

  template <AllowGC allowGC>
  static void* AllocTenuredCellUnchecked(JS::Zone* zone, AllocKind kind);

  static void* RetryTenuredAlloc(JS::Zone* zone, AllocKind kind);

#ifdef JS_GC_ZEAL
  static AllocSite* MaybeGenerateMissingAllocSite(JSContext* cx,
                                                  JS::TraceKind traceKind,
                                                  AllocSite* site);
#endif

#ifdef DEBUG
  static void CheckIncrementalZoneState(JS::Zone* zone, void* ptr);
#endif

  static inline Heap CheckedHeap(Heap heap);
};

// Buffer allocator public API.

size_t GetGoodAllocSize(size_t requiredBytes);
size_t GetGoodPower2AllocSize(size_t requiredBytes);
size_t GetGoodElementCount(size_t requiredCount, size_t elementSize);
size_t GetGoodPower2ElementCount(size_t requiredCount, size_t elementSize);
void* AllocBuffer(JS::Zone* zone, size_t bytes, bool nurseryOwned);
void* ReallocBuffer(JS::Zone* zone, void* alloc, size_t bytes,
                    bool nurseryOwned);
void FreeBuffer(JS::Zone* zone, void* alloc);

// Indicate whether |alloc| is a buffer allocation as opposed to a fixed size GC
// cell. Does not work for malloced memory.
bool IsBufferAlloc(void* alloc);

bool IsNurseryOwned(JS::Zone* zone, void* alloc);

size_t GetAllocSize(JS::Zone* zone, void* alloc);

// Buffer allocator GC-internal API.

void* AllocBufferInGC(JS::Zone* zone, size_t bytes, bool nurseryOwned);
bool IsBufferAllocMarkedBlack(JS::Zone* zone, void* alloc);
void TraceBufferEdgeInternal(JSTracer* trc, Cell* owner, void** bufferp,
                             const char* name);
void MarkTenuredBuffer(JS::Zone* zone, void* alloc);

}  // namespace gc
}  // namespace js

#endif  // gc_Allocator_h
