/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2016 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmTable.h"

#include "mozilla/CheckedInt.h"

#include "vm/JSCompartment.h"
#include "vm/JSContext.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmJS.h"

using namespace js;
using namespace js::wasm;
using mozilla::CheckedInt;

Table::Table(JSContext* cx, const TableDesc& desc, HandleWasmTableObject maybeObject,
             UniqueByteArray array)
  : maybeObject_(maybeObject),
    observers_(cx->zone()),
    array_(Move(array)),
    kind_(desc.kind),
    length_(desc.limits.initial),
    maximum_(desc.limits.maximum),
    external_(desc.external)
{}

/* static */ SharedTable
Table::create(JSContext* cx, const TableDesc& desc, HandleWasmTableObject maybeObject)
{
    // The raw element type of a Table depends on whether it is external: an
    // external table can contain functions from multiple instances and thus
    // must store an additional instance pointer in each element.
    UniqueByteArray array;
    if (desc.external)
        array.reset((uint8_t*)cx->pod_calloc<ExternalTableElem>(desc.limits.initial));
    else
        array.reset((uint8_t*)cx->pod_calloc<void*>(desc.limits.initial));
    if (!array)
        return nullptr;

    return SharedTable(cx->new_<Table>(cx, desc, maybeObject, Move(array)));
}

void
Table::tracePrivate(JSTracer* trc)
{
    // If this table has a WasmTableObject, then this method is only called by
    // WasmTableObject's trace hook so maybeObject_ must already be marked.
    // TraceEdge is called so that the pointer can be updated during a moving
    // GC. TraceWeakEdge may sound better, but it is less efficient given that
    // we know object_ is already marked.
    if (maybeObject_) {
        MOZ_ASSERT(!gc::IsAboutToBeFinalized(&maybeObject_));
        TraceEdge(trc, &maybeObject_, "wasm table object");
    }

    if (external_) {
        ExternalTableElem* array = externalArray();
        for (uint32_t i = 0; i < length_; i++) {
            if (array[i].tls)
                array[i].tls->instance->trace(trc);
            else
                MOZ_ASSERT(!array[i].code);
        }
    }
}

void
Table::trace(JSTracer* trc)
{
    // The trace hook of WasmTableObject will call Table::tracePrivate at
    // which point we can mark the rest of the children. If there is no
    // WasmTableObject, call Table::tracePrivate directly. Redirecting through
    // the WasmTableObject avoids marking the entire Table on each incoming
    // edge (once per dependent Instance).
    if (maybeObject_)
        TraceEdge(trc, &maybeObject_, "wasm table object");
    else
        tracePrivate(trc);
}

void**
Table::internalArray() const
{
    MOZ_ASSERT(!external_);
    return (void**)array_.get();
}

ExternalTableElem*
Table::externalArray() const
{
    MOZ_ASSERT(external_);
    return (ExternalTableElem*)array_.get();
}

void
Table::set(uint32_t index, void* code, Instance& instance)
{
    if (external_) {
        ExternalTableElem& elem = externalArray()[index];
        if (elem.tls)
            JSObject::writeBarrierPre(elem.tls->instance->objectUnbarriered());

        elem.code = code;
        elem.tls = instance.tlsData();

        MOZ_ASSERT(elem.tls->instance->objectUnbarriered()->isTenured(), "no writeBarrierPost");
    } else {
        internalArray()[index] = code;
    }
}

void
Table::setNull(uint32_t index)
{
    // Only external tables can set elements to null after initialization.
    ExternalTableElem& elem = externalArray()[index];
    if (elem.tls)
        JSObject::writeBarrierPre(elem.tls->instance->objectUnbarriered());

    elem.code = nullptr;
    elem.tls = nullptr;
}

uint32_t
Table::grow(uint32_t delta, JSContext* cx)
{
    // This isn't just an optimization: movingGrowable() assumes that
    // onMovingGrowTable does not fire when length == maximum.
    if (!delta)
        return length_;

    uint32_t oldLength = length_;

    CheckedInt<uint32_t> newLength = oldLength;
    newLength += delta;
    if (!newLength.isValid())
        return -1;

    if (maximum_ && newLength.value() > maximum_.value())
        return -1;

    MOZ_ASSERT(movingGrowable());

    JSRuntime* rt = cx->runtime();  // Use JSRuntime's MallocProvider to avoid throwing.

    // Note that realloc does not release array_'s pointee (which is returned by
    // externalArray()) on failure which is exactly what we need here.
    ExternalTableElem* newArray = rt->pod_realloc(externalArray(), length_, newLength.value());
    if (!newArray)
        return -1;
    Unused << array_.release();
    array_.reset((uint8_t*)newArray);

    // Realloc does not zero the delta for us.
    PodZero(newArray + length_, delta);
    length_ = newLength.value();

    if (observers_.initialized()) {
        for (InstanceSet::Range r = observers_.all(); !r.empty(); r.popFront())
            r.front()->instance().onMovingGrowTable();
    }

    return oldLength;
}

bool
Table::movingGrowable() const
{
    return !maximum_ || length_ < maximum_.value();
}

bool
Table::addMovingGrowObserver(JSContext* cx, WasmInstanceObject* instance)
{
    MOZ_ASSERT(movingGrowable());

    if (!observers_.initialized() && !observers_.init()) {
        ReportOutOfMemory(cx);
        return false;
    }

    if (!observers_.putNew(instance)) {
        ReportOutOfMemory(cx);
        return false;
    }

    return true;
}

size_t
Table::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const
{
    return mallocSizeOf(array_.get());
}
