/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=79 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Nursery_inl_h
#define gc_Nursery_inl_h

#include "gc/Nursery.h"

#include "gc/Heap.h"
#include "gc/RelocationOverlay.h"
#include "gc/Zone.h"
#include "js/TracingAPI.h"
#include "vm/JSContext.h"
#include "vm/Runtime.h"
#include "vm/SharedMem.h"

template<typename T>
bool
js::Nursery::isInside(const SharedMem<T>& p) const
{
    return isInside(p.unwrap(/*safe - used for value in comparison above*/));
}

MOZ_ALWAYS_INLINE /* static */ bool
js::Nursery::getForwardedPointer(js::gc::Cell** ref)
{
    MOZ_ASSERT(ref);
    MOZ_ASSERT(IsInsideNursery(*ref));
    const gc::RelocationOverlay* overlay = reinterpret_cast<const gc::RelocationOverlay*>(*ref);
    if (!overlay->isForwarded())
        return false;
    *ref = overlay->forwardingAddress();
    return true;
}

inline void
js::Nursery::maybeSetForwardingPointer(JSTracer* trc, void* oldData, void* newData, bool direct)
{
    if (trc->isTenuringTracer())
        setForwardingPointerWhileTenuring(oldData, newData, direct);
}

inline void
js::Nursery::setForwardingPointerWhileTenuring(void* oldData, void* newData, bool direct)
{
    if (isInside(oldData))
        setForwardingPointer(oldData, newData, direct);
}

inline void
js::Nursery::setSlotsForwardingPointer(HeapSlot* oldSlots, HeapSlot* newSlots, uint32_t nslots)
{
    // Slot arrays always have enough space for a forwarding pointer, since the
    // number of slots is never zero.
    MOZ_ASSERT(nslots > 0);
    setDirectForwardingPointer(oldSlots, newSlots);
}

inline void
js::Nursery::setElementsForwardingPointer(ObjectElements* oldHeader, ObjectElements* newHeader,
                                          uint32_t capacity)
{
    // Only use a direct forwarding pointer if there is enough space for one.
    setForwardingPointer(oldHeader->elements(), newHeader->elements(),
                         capacity > 0);
}

inline void
js::Nursery::setForwardingPointer(void* oldData, void* newData, bool direct)
{
    if (direct) {
        setDirectForwardingPointer(oldData, newData);
        return;
    }

    setIndirectForwardingPointer(oldData, newData);
}

inline void
js::Nursery::setDirectForwardingPointer(void* oldData, void* newData)
{
    MOZ_ASSERT(isInside(oldData));

    // Bug 1196210: If a zero-capacity header lands in the last 2 words of a
    // jemalloc chunk abutting the start of a nursery chunk, the (invalid)
    // newData pointer will appear to be "inside" the nursery.
    MOZ_ASSERT(!isInside(newData) || (uintptr_t(newData) & js::gc::ChunkMask) == 0);

    *reinterpret_cast<void**>(oldData) = newData;
}

namespace js {

// The allocation methods below will not run the garbage collector. If the
// nursery cannot accomodate the allocation, the malloc heap will be used
// instead.

template <typename T>
static inline T*
AllocateObjectBuffer(JSContext* cx, uint32_t count)
{
    size_t nbytes = JS_ROUNDUP(count * sizeof(T), sizeof(Value));
    T* buffer = static_cast<T*>(cx->nursery().allocateBuffer(cx->zone(), nbytes));
    if (!buffer)
        ReportOutOfMemory(cx);
    return buffer;
}

template <typename T>
static inline T*
AllocateObjectBuffer(JSContext* cx, JSObject* obj, uint32_t count)
{
    if (cx->helperThread())
        return cx->zone()->pod_malloc<T>(count);
    size_t nbytes = JS_ROUNDUP(count * sizeof(T), sizeof(Value));
    T* buffer = static_cast<T*>(cx->nursery().allocateBuffer(obj, nbytes));
    if (!buffer)
        ReportOutOfMemory(cx);
    return buffer;
}

// If this returns null then the old buffer will be left alone.
template <typename T>
static inline T*
ReallocateObjectBuffer(JSContext* cx, JSObject* obj, T* oldBuffer,
                       uint32_t oldCount, uint32_t newCount)
{
    if (cx->helperThread())
        return obj->zone()->pod_realloc<T>(oldBuffer, oldCount, newCount);
    T* buffer =  static_cast<T*>(cx->nursery().reallocateBuffer(obj, oldBuffer,
                                                                oldCount * sizeof(T),
                                                                newCount * sizeof(T)));
    if (!buffer)
        ReportOutOfMemory(cx);
    return buffer;
}

static inline void
EvictAllNurseries(JSRuntime* rt, JS::gcreason::Reason reason = JS::gcreason::EVICT_NURSERY)
{
    rt->gc.evictNursery(reason);
}

} // namespace js

#endif /* gc_Nursery_inl_h */
