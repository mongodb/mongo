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
#include "gc/Cell.h"
#include "js/Class.h"
#include "js/TypeDecls.h"

namespace js {

// [SMDOC] AllowGC template parameter
//
// AllowGC is a template parameter for functions that support both with and
// without GC operation.
//
// The CanGC variant of the function can trigger a garbage collection, and
// should set a pending exception on failure.
//
// The NoGC variant of the function cannot trigger a garbage collection, and
// should not set any pending exception on failure.  This variant can be called
// in fast paths where the caller has unrooted pointers.  The failure means we
// need to perform GC to allocate an object. The caller can fall back to a slow
// path that roots pointers before calling a CanGC variant of the function,
// without having to clear a pending exception.
enum AllowGC { NoGC = 0, CanGC = 1 };

namespace gc {

class AllocSite;
struct Cell;
class TenuredCell;

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
  template <typename T, js::AllowGC allowGC = CanGC, typename... Args>
  static T* NewCell(JSContext* cx, Args&&... args);

 private:
  // Allocate a cell in the nursery, unless |heap| is Heap::Tenured or nursery
  // allocation is disabled for |traceKind| in the current zone.
  template <JS::TraceKind traceKind, AllowGC allowGC = CanGC>
  static void* AllocNurseryOrTenuredCell(JSContext* cx, gc::AllocKind allocKind,
                                         gc::Heap heap, AllocSite* site);

  // Allocate a cell in the tenured heap.
  template <AllowGC allowGC = CanGC>
  static void* AllocTenuredCell(JSContext* cx, gc::AllocKind kind, size_t size);

  // Allocate a string. Use cx->newCell<T>([heap]).
  //
  // Use for nursery-allocatable strings. Returns a value cast to the correct
  // type. Non-nursery-allocatable strings will go through the fallback
  // tenured-only allocation path.
  template <typename T, AllowGC allowGC = CanGC, typename... Args>
  static T* NewString(JSContext* cx, gc::Heap heap, Args&&... args) {
    static_assert(std::is_base_of_v<JSString, T>);
    gc::AllocKind kind = gc::MapTypeToAllocKind<T>::kind;
    void* ptr = AllocNurseryOrTenuredCell<JS::TraceKind::String, allowGC>(
        cx, kind, heap, nullptr);
    if (!ptr) {
      return nullptr;
    }
    return new (mozilla::KnownNotNull, ptr) T(std::forward<Args>(args)...);
  }

  template <typename T, AllowGC allowGC /* = CanGC */>
  static T* NewBigInt(JSContext* cx, Heap heap) {
    void* ptr = AllocNurseryOrTenuredCell<JS::TraceKind::BigInt, allowGC>(
        cx, gc::AllocKind::BIGINT, heap, nullptr);
    if (ptr) {
      return new (mozilla::KnownNotNull, ptr) T();
    }
    return nullptr;
  }

  template <typename T, AllowGC allowGC = CanGC>
  static T* NewObject(JSContext* cx, gc::AllocKind kind, gc::Heap heap,
                      const JSClass* clasp, gc::AllocSite* site = nullptr) {
    MOZ_ASSERT(IsObjectAllocKind(kind));
    MOZ_ASSERT_IF(heap != gc::Heap::Tenured && clasp->hasFinalize() &&
                      !clasp->isProxyObject(),
                  CanNurseryAllocateFinalizedClass(clasp));
    void* cell = AllocNurseryOrTenuredCell<JS::TraceKind::Object, allowGC>(
        cx, kind, heap, site);
    if (!cell) {
      return nullptr;
    }
    return new (mozilla::KnownNotNull, cell) T();
  }

  // Allocate all other kinds of GC thing.
  template <typename T, AllowGC allowGC = CanGC, typename... Args>
  static T* NewTenuredCell(JSContext* cx, Args&&... args) {
    gc::AllocKind kind = gc::MapTypeToAllocKind<T>::kind;
    void* cell = AllocTenuredCell<allowGC>(cx, kind, sizeof(T));
    if (!cell) {
      return nullptr;
    }
    return new (mozilla::KnownNotNull, cell) T(std::forward<Args>(args)...);
  }
};

}  // namespace gc

// This is the entry point for all allocation, though callers should still not
// use this directly. Use cx->newCell<T>(...) instead.
//
// After a successful allocation the caller must fully initialize the thing
// before calling any function that can potentially trigger GC. This will
// ensure that GC tracing never sees junk values stored in the partially
// initialized thing.
template <typename T, AllowGC allowGC, typename... Args>
T* gc::CellAllocator::NewCell(JSContext* cx, Args&&... args) {
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

}  // namespace js

#endif  // gc_Allocator_h
