/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Allocator_h
#define gc_Allocator_h

#include "gc/Heap.h"
#include "js/RootingAPI.h"

class JSFatInlineString;

namespace js {

struct Class;

// Allocate a new GC thing. After a successful allocation the caller must
// fully initialize the thing before calling any function that can potentially
// trigger GC. This will ensure that GC tracing never sees junk values stored
// in the partially initialized thing.

template <typename T, AllowGC allowGC = CanGC>
T*
Allocate(JSContext* cx);

// Use for JSObject. A longer signature that includes additional information in
// support of various optimizations. If dynamic slots are requested they will be
// allocated and the pointer stored directly in |NativeObject::slots_|.
template <typename, AllowGC allowGC = CanGC>
JSObject*
Allocate(JSContext* cx, gc::AllocKind kind, size_t nDynamicSlots, gc::InitialHeap heap,
         const Class* clasp);

// Internal function used for nursery-allocatable strings.
template <typename StringAllocT, AllowGC allowGC = CanGC>
StringAllocT*
AllocateString(JSContext* cx, gc::InitialHeap heap);

// Use for nursery-allocatable strings. Returns a value cast to the correct
// type.
template <typename StringT, AllowGC allowGC = CanGC>
StringT*
Allocate(JSContext* cx, gc::InitialHeap heap)
{
    return static_cast<StringT*>(js::AllocateString<JSString, allowGC>(cx, heap));
}

// Specialization for JSFatInlineString that must use a different allocation
// type. Note that we have to explicitly specialize for both values of AllowGC
// because partial function specialization is not allowed.
template <>
inline JSFatInlineString*
Allocate<JSFatInlineString, CanGC>(JSContext* cx, gc::InitialHeap heap)
{
    return static_cast<JSFatInlineString*>(js::AllocateString<JSFatInlineString, CanGC>(cx, heap));
}

template <>
inline JSFatInlineString*
Allocate<JSFatInlineString, NoGC>(JSContext* cx, gc::InitialHeap heap)
{
    return static_cast<JSFatInlineString*>(js::AllocateString<JSFatInlineString, NoGC>(cx, heap));
}

} // namespace js

#endif // gc_Allocator_h
