/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/StoreBuffer.h"

#include "mozilla/Assertions.h"

#include "jscompartment.h"

#include "gc/Statistics.h"
#include "vm/ArgumentsObject.h"
#include "vm/Runtime.h"

#include "jsgcinlines.h"

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
        unsigned size = *e.get<unsigned>();
        e.popFront<unsigned>();
        BufferableRef* edge = e.get<BufferableRef>(size);
        edge->trace(trc);
        e.popFront(size);
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
        !bufferWholeCell.init() ||
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

bool
StoreBuffer::clear()
{
    if (!enabled_)
        return true;

    aboutToOverflow_ = false;
    cancelIonCompilations_ = false;

    bufferVal.clear();
    bufferCell.clear();
    bufferSlot.clear();
    bufferWholeCell.clear();
    bufferGeneric.clear();

    return true;
}

void
StoreBuffer::setAboutToOverflow()
{
    if (!aboutToOverflow_) {
        aboutToOverflow_ = true;
        runtime_->gc.stats.count(gcstats::STAT_STOREBUFFER_OVERFLOW);
    }
    runtime_->gc.requestMinorGC(JS::gcreason::FULL_STORE_BUFFER);
}

void
StoreBuffer::addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf, JS::GCSizes
*sizes)
{
    sizes->storeBufferVals       += bufferVal.sizeOfExcludingThis(mallocSizeOf);
    sizes->storeBufferCells      += bufferCell.sizeOfExcludingThis(mallocSizeOf);
    sizes->storeBufferSlots      += bufferSlot.sizeOfExcludingThis(mallocSizeOf);
    sizes->storeBufferWholeCells += bufferWholeCell.sizeOfExcludingThis(mallocSizeOf);
    sizes->storeBufferGenerics   += bufferGeneric.sizeOfExcludingThis(mallocSizeOf);
}

template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::ValueEdge>;
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::CellPtrEdge>;
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::SlotsEdge>;
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::WholeCellEdges>;
