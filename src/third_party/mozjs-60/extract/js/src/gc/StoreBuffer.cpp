/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/StoreBuffer-inl.h"

#include "mozilla/Assertions.h"

#include "gc/Statistics.h"
#include "vm/ArgumentsObject.h"
#include "vm/Runtime.h"

#include "gc/GC-inl.h"

using namespace js;
using namespace js::gc;

void
StoreBuffer::GenericBuffer::trace(StoreBuffer* owner, JSTracer* trc)
{
    mozilla::ReentrancyGuard g(*owner);
    MOZ_ASSERT(owner->isEnabled());
    if (!storage_)
        return;

    for (LifoAlloc::Enum e(*storage_); !e.empty();) {
        unsigned size = *e.read<unsigned>();
        BufferableRef* edge = e.read<BufferableRef>(size);
        edge->trace(trc);
    }
}

bool
StoreBuffer::enable()
{
    if (enabled_)
        return true;

    if (!bufferVal.init() ||
        !bufferCell.init() ||
        !bufferSlot.init() ||
        !bufferGeneric.init())
    {
        return false;
    }

    enabled_ = true;
    return true;
}

void
StoreBuffer::disable()
{
    if (!enabled_)
        return;

    aboutToOverflow_ = false;

    enabled_ = false;
}

void
StoreBuffer::clear()
{
    if (!enabled_)
        return;

    aboutToOverflow_ = false;
    cancelIonCompilations_ = false;

    bufferVal.clear();
    bufferCell.clear();
    bufferSlot.clear();
    bufferGeneric.clear();

    for (ArenaCellSet* set = bufferWholeCell; set; set = set->next)
        set->arena->bufferedCells() = nullptr;
    bufferWholeCell = nullptr;
}

void
StoreBuffer::setAboutToOverflow(JS::gcreason::Reason reason)
{
    if (!aboutToOverflow_) {
        aboutToOverflow_ = true;
        runtime_->gc.stats().count(gcstats::STAT_STOREBUFFER_OVERFLOW);
    }
    nursery_.requestMinorGC(reason);
}

void
StoreBuffer::addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf, JS::GCSizes
*sizes)
{
    sizes->storeBufferVals       += bufferVal.sizeOfExcludingThis(mallocSizeOf);
    sizes->storeBufferCells      += bufferCell.sizeOfExcludingThis(mallocSizeOf);
    sizes->storeBufferSlots      += bufferSlot.sizeOfExcludingThis(mallocSizeOf);
    sizes->storeBufferGenerics   += bufferGeneric.sizeOfExcludingThis(mallocSizeOf);

    for (ArenaCellSet* set = bufferWholeCell; set; set = set->next)
        sizes->storeBufferWholeCells += sizeof(ArenaCellSet);
}

void
StoreBuffer::addToWholeCellBuffer(ArenaCellSet* set)
{
    set->next = bufferWholeCell;
    bufferWholeCell = set;
}

ArenaCellSet ArenaCellSet::Empty(nullptr);

ArenaCellSet::ArenaCellSet(Arena* arena)
  : arena(arena), next(nullptr)
{
    bits.clear(false);
}

ArenaCellSet*
js::gc::AllocateWholeCellSet(Arena* arena)
{
    Zone* zone = arena->zone;
    if (!zone->group()->nursery().isEnabled())
        return nullptr;

    AutoEnterOOMUnsafeRegion oomUnsafe;
    Nursery& nursery = zone->group()->nursery();
    void* data = nursery.allocateBuffer(zone, sizeof(ArenaCellSet));
    if (!data)
        oomUnsafe.crash("Failed to allocate WholeCellSet");

    if (nursery.freeSpace() < ArenaCellSet::NurseryFreeThresholdBytes)
        zone->group()->storeBuffer().setAboutToOverflow(JS::gcreason::FULL_WHOLE_CELL_BUFFER);

    auto cells = static_cast<ArenaCellSet*>(data);
    new (cells) ArenaCellSet(arena);
    arena->bufferedCells() = cells;
    zone->group()->storeBuffer().addToWholeCellBuffer(cells);
    return cells;
}

template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::ValueEdge>;
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::CellPtrEdge>;
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::SlotsEdge>;
