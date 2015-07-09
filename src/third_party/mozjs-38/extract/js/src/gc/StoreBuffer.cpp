/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/StoreBuffer.h"

#include "mozilla/Assertions.h"

#include "gc/Statistics.h"
#include "vm/ArgumentsObject.h"

#include "jsgcinlines.h"

using namespace js;
using namespace js::gc;
using mozilla::ReentrancyGuard;

/*** Edges ***/

void
StoreBuffer::SlotsEdge::mark(JSTracer* trc) const
{
    NativeObject* obj = object();

    // Beware JSObject::swap exchanging a native object for a non-native one.
    if (!obj->isNative())
        return;

    if (IsInsideNursery(obj))
        return;

    if (kind() == ElementKind) {
        int32_t initLen = obj->getDenseInitializedLength();
        int32_t clampedStart = Min(start_, initLen);
        int32_t clampedEnd = Min(start_ + count_, initLen);
        gc::MarkArraySlots(trc, clampedEnd - clampedStart,
                           obj->getDenseElements() + clampedStart, "element");
    } else {
        int32_t start = Min(uint32_t(start_), obj->slotSpan());
        int32_t end = Min(uint32_t(start_) + count_, obj->slotSpan());
        MOZ_ASSERT(end >= start);
        MarkObjectSlots(trc, obj, start, end - start);
    }
}

void
StoreBuffer::WholeCellEdges::mark(JSTracer* trc) const
{
    MOZ_ASSERT(edge->isTenured());
    JSGCTraceKind kind = GetGCThingTraceKind(edge);
    if (kind <= JSTRACE_OBJECT) {
        JSObject* object = static_cast<JSObject*>(edge);
        if (object->is<ArgumentsObject>())
            ArgumentsObject::trace(trc, object);
        MarkChildren(trc, object);
        return;
    }
    MOZ_ASSERT(kind == JSTRACE_JITCODE);
    static_cast<jit::JitCode*>(edge)->trace(trc);
}

void
StoreBuffer::CellPtrEdge::mark(JSTracer* trc) const
{
    if (!*edge)
        return;

    MOZ_ASSERT(GetGCThingTraceKind(*edge) == JSTRACE_OBJECT);
    MarkObjectRoot(trc, reinterpret_cast<JSObject**>(edge), "store buffer edge");
}

void
StoreBuffer::ValueEdge::mark(JSTracer* trc) const
{
    if (!deref())
        return;

    MarkValueRoot(trc, edge, "store buffer edge");
}

/*** MonoTypeBuffer ***/

template <typename T>
void
StoreBuffer::MonoTypeBuffer<T>::mark(StoreBuffer* owner, JSTracer* trc)
{
    ReentrancyGuard g(*owner);
    MOZ_ASSERT(owner->isEnabled());
    MOZ_ASSERT(stores_.initialized());
    sinkStores(owner);
    for (typename StoreSet::Range r = stores_.all(); !r.empty(); r.popFront())
        r.front().mark(trc);
}

/*** GenericBuffer ***/

void
StoreBuffer::GenericBuffer::mark(StoreBuffer* owner, JSTracer* trc)
{
    ReentrancyGuard g(*owner);
    MOZ_ASSERT(owner->isEnabled());
    if (!storage_)
        return;

    for (LifoAlloc::Enum e(*storage_); !e.empty();) {
        unsigned size = *e.get<unsigned>();
        e.popFront<unsigned>();
        BufferableRef* edge = e.get<BufferableRef>(size);
        edge->mark(trc);
        e.popFront(size);
    }
}

/*** StoreBuffer ***/

bool
StoreBuffer::enable()
{
    if (enabled_)
        return true;

    if (!bufferVal.init() ||
        !bufferCell.init() ||
        !bufferSlot.init() ||
        !bufferWholeCell.init() ||
        !bufferRelocVal.init() ||
        !bufferRelocCell.init() ||
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

    bufferVal.clear();
    bufferCell.clear();
    bufferSlot.clear();
    bufferWholeCell.clear();
    bufferRelocVal.clear();
    bufferRelocCell.clear();
    bufferGeneric.clear();

    return true;
}

void
StoreBuffer::markAll(JSTracer* trc)
{
    bufferVal.mark(this, trc);
    bufferCell.mark(this, trc);
    bufferSlot.mark(this, trc);
    bufferWholeCell.mark(this, trc);
    bufferRelocVal.mark(this, trc);
    bufferRelocCell.mark(this, trc);
    bufferGeneric.mark(this, trc);
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
    sizes->storeBufferRelocVals  += bufferRelocVal.sizeOfExcludingThis(mallocSizeOf);
    sizes->storeBufferRelocCells += bufferRelocCell.sizeOfExcludingThis(mallocSizeOf);
    sizes->storeBufferGenerics   += bufferGeneric.sizeOfExcludingThis(mallocSizeOf);
}

JS_PUBLIC_API(void)
JS::HeapCellPostBarrier(js::gc::Cell** cellp)
{
    MOZ_ASSERT(cellp);
    MOZ_ASSERT(*cellp);
    StoreBuffer* storeBuffer = (*cellp)->storeBuffer();
    if (storeBuffer)
        storeBuffer->putRelocatableCellFromAnyThread(cellp);
}

JS_PUBLIC_API(void)
JS::HeapCellRelocate(js::gc::Cell** cellp)
{
    /* Called with old contents of *cellp before overwriting. */
    MOZ_ASSERT(cellp);
    MOZ_ASSERT(*cellp);
    JSRuntime* runtime = (*cellp)->runtimeFromMainThread();
    runtime->gc.storeBuffer.removeRelocatableCellFromAnyThread(cellp);
}

JS_PUBLIC_API(void)
JS::HeapValuePostBarrier(JS::Value* valuep)
{
    MOZ_ASSERT(valuep);
    MOZ_ASSERT(valuep->isMarkable());
    if (valuep->isObject()) {
        StoreBuffer* storeBuffer = valuep->toObject().storeBuffer();
        if (storeBuffer)
            storeBuffer->putRelocatableValueFromAnyThread(valuep);
    }
}

JS_PUBLIC_API(void)
JS::HeapValueRelocate(JS::Value* valuep)
{
    /* Called with old contents of *valuep before overwriting. */
    MOZ_ASSERT(valuep);
    MOZ_ASSERT(valuep->isMarkable());
    if (valuep->isString() && valuep->toString()->isPermanentAtom())
        return;
    JSRuntime* runtime = static_cast<js::gc::Cell*>(valuep->toGCThing())->runtimeFromMainThread();
    runtime->gc.storeBuffer.removeRelocatableValueFromAnyThread(valuep);
}

template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::ValueEdge>;
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::CellPtrEdge>;
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::SlotsEdge>;
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::WholeCellEdges>;
