/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Allocator_h
#define gc_Allocator_h

#include <stdint.h>

#include "gc/AllocKind.h"
#include "js/RootingAPI.h"

class JSFatInlineString;

namespace js {

namespace gc {
class AllocSite;
}  // namespace gc

enum AllowGC { NoGC = 0, CanGC = 1 };

// Allocate a new GC thing that's not a JSObject or a string.
//
// After a successful allocation the caller must fully initialize the thing
// before calling any function that can potentially trigger GC. This will ensure
// that GC tracing never sees junk values stored in the partially initialized
// thing.
template <typename T, AllowGC allowGC = CanGC>
T* Allocate(JSContext* cx);

// Allocate a JSObject.
//
// A longer signature that includes additional information in support of various
// optimizations. If dynamic slots are requested they will be allocated and the
// pointer stored directly in |NativeObject::slots_|.
template <AllowGC allowGC = CanGC>
JSObject* AllocateObject(JSContext* cx, gc::AllocKind kind,
                         size_t nDynamicSlots, gc::InitialHeap heap,
                         const JSClass* clasp, gc::AllocSite* site = nullptr);

// Internal function used for nursery-allocatable strings.
template <typename StringAllocT, AllowGC allowGC = CanGC>
StringAllocT* AllocateStringImpl(JSContext* cx, gc::InitialHeap heap);

// Allocate a string.
//
// Use for nursery-allocatable strings. Returns a value cast to the correct
// type.
template <typename StringT, AllowGC allowGC = CanGC>
StringT* AllocateString(JSContext* cx, gc::InitialHeap heap) {
  return static_cast<StringT*>(AllocateStringImpl<JSString, allowGC>(cx, heap));
}

// Specialization for JSFatInlineString that must use a different allocation
// type. Note that we have to explicitly specialize for both values of AllowGC
// because partial function specialization is not allowed.
template <>
inline JSFatInlineString* AllocateString<JSFatInlineString, CanGC>(
    JSContext* cx, gc::InitialHeap heap) {
  return static_cast<JSFatInlineString*>(
      js::AllocateStringImpl<JSFatInlineString, CanGC>(cx, heap));
}

template <>
inline JSFatInlineString* AllocateString<JSFatInlineString, NoGC>(
    JSContext* cx, gc::InitialHeap heap) {
  return static_cast<JSFatInlineString*>(
      js::AllocateStringImpl<JSFatInlineString, NoGC>(cx, heap));
}

// Allocate a BigInt.
//
// Use for nursery-allocatable BigInt.
template <AllowGC allowGC = CanGC>
JS::BigInt* AllocateBigInt(JSContext* cx, gc::InitialHeap heap);

}  // namespace js

#endif  // gc_Allocator_h
