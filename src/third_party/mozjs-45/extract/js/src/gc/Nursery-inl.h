/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=79 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Nursery_inl_h
#define gc_Nursery_inl_h

#include "gc/Nursery.h"

#include "jscntxt.h"

#include "gc/Heap.h"
#include "gc/Zone.h"
#include "js/TracingAPI.h"
#include "vm/Runtime.h"

MOZ_ALWAYS_INLINE bool
js::Nursery::getForwardedPointer(JSObject** ref) const
{
    MOZ_ASSERT(ref);
    MOZ_ASSERT(isInside((void*)*ref));
    const gc::RelocationOverlay* overlay = reinterpret_cast<const gc::RelocationOverlay*>(*ref);
    if (!overlay->isForwarded())
        return false;
    *ref = static_cast<JSObject*>(overlay->forwardingAddress());
    return true;
}

namespace js {

// The allocation methods below will not run the garbage collector. If the
// nursery cannot accomodate the allocation, the malloc heap will be used
// instead.

template <typename T>
static inline T*
AllocateObjectBuffer(ExclusiveContext* cx, uint32_t count)
{
    if (cx->isJSContext()) {
        Nursery& nursery = cx->asJSContext()->runtime()->gc.nursery;
        size_t nbytes = JS_ROUNDUP(count * sizeof(T), sizeof(Value));
        T* buffer = static_cast<T*>(nursery.allocateBuffer(cx->zone(), nbytes));
        if (!buffer)
            ReportOutOfMemory(cx);
        return buffer;
    }
    return cx->zone()->pod_malloc<T>(count);
}

template <typename T>
static inline T*
AllocateObjectBuffer(ExclusiveContext* cx, JSObject* obj, uint32_t count)
{
    if (cx->isJSContext()) {
        Nursery& nursery = cx->asJSContext()->runtime()->gc.nursery;
        size_t nbytes = JS_ROUNDUP(count * sizeof(T), sizeof(Value));
        T* buffer = static_cast<T*>(nursery.allocateBuffer(obj, nbytes));
        if (!buffer)
            ReportOutOfMemory(cx);
        return buffer;
    }
    return obj->zone()->pod_malloc<T>(count);
}

// If this returns null then the old buffer will be left alone.
template <typename T>
static inline T*
ReallocateObjectBuffer(ExclusiveContext* cx, JSObject* obj, T* oldBuffer,
                       uint32_t oldCount, uint32_t newCount)
{
    if (cx->isJSContext()) {
        Nursery& nursery = cx->asJSContext()->runtime()->gc.nursery;
        T* buffer =  static_cast<T*>(nursery.reallocateBuffer(obj, oldBuffer,
                                                              oldCount * sizeof(T),
                                                              newCount * sizeof(T)));
        if (!buffer)
            ReportOutOfMemory(cx);
        return buffer;
    }
    return obj->zone()->pod_realloc<T>(oldBuffer, oldCount, newCount);
}

} // namespace js

#endif /* gc_Nursery_inl_h */
