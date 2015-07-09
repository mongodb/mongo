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
#include "js/TracingAPI.h"
#include "vm/Runtime.h"

template <typename T>
MOZ_ALWAYS_INLINE bool
js::Nursery::getForwardedPointer(T** ref)
{
    MOZ_ASSERT(ref);
    MOZ_ASSERT(isInside((void*)*ref));
    const gc::RelocationOverlay* overlay = reinterpret_cast<const gc::RelocationOverlay*>(*ref);
    if (!overlay->isForwarded())
        return false;
    /* This static cast from Cell* restricts T to valid (GC thing) types. */
    *ref = static_cast<T*>(overlay->forwardingAddress());
    return true;
}

#endif /* gc_Nursery_inl_h */
