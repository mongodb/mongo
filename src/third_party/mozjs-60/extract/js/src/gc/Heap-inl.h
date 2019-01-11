/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Heap_inl_h
#define gc_Heap_inl_h

#include "gc/Heap.h"

#include "gc/StoreBuffer.h"
#include "gc/Zone.h"

inline void
js::gc::Arena::init(JS::Zone* zoneArg, AllocKind kind)
{
    MOZ_ASSERT(firstFreeSpan.isEmpty());
    MOZ_ASSERT(!zone);
    MOZ_ASSERT(!allocated());
    MOZ_ASSERT(!hasDelayedMarking);
    MOZ_ASSERT(!markOverflow);
    MOZ_ASSERT(!auxNextLink);

    zone = zoneArg;
    allocKind = size_t(kind);
    setAsFullyUnused();
    if (zone->isAtomsZone())
        zone->runtimeFromAnyThread()->gc.atomMarking.registerArena(this);
    else
        bufferedCells() = &ArenaCellSet::Empty;
}

inline void
js::gc::Arena::release()
{
    if (zone->isAtomsZone())
        zone->runtimeFromAnyThread()->gc.atomMarking.unregisterArena(this);
    setAsNotAllocated();
}

inline js::gc::ArenaCellSet*&
js::gc::Arena::bufferedCells()
{
    MOZ_ASSERT(zone && !zone->isAtomsZone());
    return bufferedCells_;
}

inline size_t&
js::gc::Arena::atomBitmapStart()
{
    MOZ_ASSERT(zone && zone->isAtomsZone());
    return atomBitmapStart_;
}

#endif
