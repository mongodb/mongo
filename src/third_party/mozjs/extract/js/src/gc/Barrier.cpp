/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Barrier.h"

#include "builtin/TypedObject.h"
#include "gc/Policy.h"
#include "gc/Zone.h"
#include "js/HashTable.h"
#include "js/Value.h"
#include "vm/EnvironmentObject.h"
#include "vm/JSCompartment.h"
#include "vm/JSObject.h"
#include "vm/SharedArrayObject.h"
#include "vm/SymbolType.h"
#include "wasm/WasmJS.h"

namespace js {

bool
RuntimeFromActiveCooperatingThreadIsHeapMajorCollecting(JS::shadow::Zone* shadowZone)
{
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(shadowZone->runtimeFromActiveCooperatingThread()));
    return JS::CurrentThreadIsHeapMajorCollecting();
}

#ifdef DEBUG

bool
IsMarkedBlack(JSObject* obj)
{
    return obj->isMarkedBlack();
}

bool
HeapSlot::preconditionForSet(NativeObject* owner, Kind kind, uint32_t slot) const
{
    if (kind == Slot)
        return &owner->getSlotRef(slot) == this;

    uint32_t numShifted = owner->getElementsHeader()->numShiftedElements();
    MOZ_ASSERT(slot >= numShifted);
    return &owner->getDenseElement(slot - numShifted) == (const Value*)this;
}

void
HeapSlot::assertPreconditionForWriteBarrierPost(NativeObject* obj, Kind kind, uint32_t slot,
                                                const Value& target) const
{
    if (kind == Slot) {
        MOZ_ASSERT(obj->getSlotAddressUnchecked(slot)->get() == target);
    } else {
        uint32_t numShifted = obj->getElementsHeader()->numShiftedElements();
        MOZ_ASSERT(slot >= numShifted);
        MOZ_ASSERT(static_cast<HeapSlot*>(obj->getDenseElements() + (slot - numShifted))->get() ==
                   target);
    }

    CheckTargetIsNotGray(obj);
}

bool
CurrentThreadIsIonCompiling()
{
    return TlsContext.get()->ionCompiling;
}

bool
CurrentThreadIsIonCompilingSafeForMinorGC()
{
    return TlsContext.get()->ionCompilingSafeForMinorGC;
}

bool
CurrentThreadIsGCSweeping()
{
    return TlsContext.get()->gcSweeping;
}

bool CurrentThreadIsTouchingGrayThings()
{
    return TlsContext.get()->isTouchingGrayThings;
}

AutoTouchingGrayThings::AutoTouchingGrayThings()
{
    TlsContext.get()->isTouchingGrayThings++;
}

AutoTouchingGrayThings::~AutoTouchingGrayThings()
{
    JSContext* cx = TlsContext.get();
    MOZ_ASSERT(cx->isTouchingGrayThings);
    cx->isTouchingGrayThings--;
}

#endif // DEBUG

template <typename S>
template <typename T>
void
ReadBarrierFunctor<S>::operator()(T* t)
{
    InternalBarrierMethods<T*>::readBarrier(t);
}

// All GC things may be held in a Value, either publicly or as a private GC
// thing.
#define JS_EXPAND_DEF(name, type, _) \
template void ReadBarrierFunctor<JS::Value>::operator()<type>(type*);
JS_FOR_EACH_TRACEKIND(JS_EXPAND_DEF);
#undef JS_EXPAND_DEF

template <typename S>
template <typename T>
void
PreBarrierFunctor<S>::operator()(T* t)
{
    InternalBarrierMethods<T*>::preBarrier(t);
}

// All GC things may be held in a Value, either publicly or as a private GC
// thing.
#define JS_EXPAND_DEF(name, type, _) \
template void PreBarrierFunctor<JS::Value>::operator()<type>(type*);
JS_FOR_EACH_TRACEKIND(JS_EXPAND_DEF);
#undef JS_EXPAND_DEF

template void PreBarrierFunctor<jsid>::operator()<JS::Symbol>(JS::Symbol*);
template void PreBarrierFunctor<jsid>::operator()<JSString>(JSString*);

template <typename T>
/* static */ bool
MovableCellHasher<T>::hasHash(const Lookup& l)
{
    if (!l)
        return true;

    return l->zoneFromAnyThread()->hasUniqueId(l);
}

template <typename T>
/* static */ bool
MovableCellHasher<T>::ensureHash(const Lookup& l)
{
    if (!l)
        return true;

    uint64_t unusedId;
    return l->zoneFromAnyThread()->getOrCreateUniqueId(l, &unusedId);
}

template <typename T>
/* static */ HashNumber
MovableCellHasher<T>::hash(const Lookup& l)
{
    if (!l)
        return 0;

    // We have to access the zone from-any-thread here: a worker thread may be
    // cloning a self-hosted object from the main runtime's self- hosting zone
    // into another runtime. The zone's uid lock will protect against multiple
    // workers doing this simultaneously.
    MOZ_ASSERT(CurrentThreadCanAccessZone(l->zoneFromAnyThread()) ||
               l->zoneFromAnyThread()->isSelfHostingZone() ||
               CurrentThreadIsPerformingGC());

    return l->zoneFromAnyThread()->getHashCodeInfallible(l);
}

template <typename T>
/* static */ bool
MovableCellHasher<T>::match(const Key& k, const Lookup& l)
{
    // Return true if both are null or false if only one is null.
    if (!k)
        return !l;
    if (!l)
        return false;

    MOZ_ASSERT(k);
    MOZ_ASSERT(l);
    MOZ_ASSERT(CurrentThreadCanAccessZone(l->zoneFromAnyThread()) ||
               l->zoneFromAnyThread()->isSelfHostingZone());

    Zone* zone = k->zoneFromAnyThread();
    if (zone != l->zoneFromAnyThread())
        return false;

#ifdef DEBUG
    // Incremental table sweeping means that existing table entries may no
    // longer have unique IDs. We fail the match in that case and the entry is
    // removed from the table later on.
    if (!zone->hasUniqueId(k)) {
        Key key = k;
        MOZ_ASSERT(IsAboutToBeFinalizedUnbarriered(&key));
    }
    MOZ_ASSERT(zone->hasUniqueId(l));
#endif

    uint64_t keyId;
    if (!zone->maybeGetUniqueId(k, &keyId)) {
        // Key is dead and cannot match lookup which must be live.
        return false;
    }

    return keyId == zone->getUniqueIdInfallible(l);
}

#ifdef JS_BROKEN_GCC_ATTRIBUTE_WARNING
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif // JS_BROKEN_GCC_ATTRIBUTE_WARNING

template struct JS_PUBLIC_API(MovableCellHasher<JSObject*>);
template struct JS_PUBLIC_API(MovableCellHasher<GlobalObject*>);
template struct JS_PUBLIC_API(MovableCellHasher<SavedFrame*>);
template struct JS_PUBLIC_API(MovableCellHasher<EnvironmentObject*>);
template struct JS_PUBLIC_API(MovableCellHasher<WasmInstanceObject*>);
template struct JS_PUBLIC_API(MovableCellHasher<JSScript*>);

#ifdef JS_BROKEN_GCC_ATTRIBUTE_WARNING
#pragma GCC diagnostic pop
#endif // JS_BROKEN_GCC_ATTRIBUTE_WARNING

} // namespace js

JS_PUBLIC_API(void)
JS::HeapObjectPostBarrier(JSObject** objp, JSObject* prev, JSObject* next)
{
    MOZ_ASSERT(objp);
    js::InternalBarrierMethods<JSObject*>::postBarrier(objp, prev, next);
}

JS_PUBLIC_API(void)
JS::HeapStringPostBarrier(JSString** strp, JSString* prev, JSString* next)
{
    MOZ_ASSERT(strp);
    js::InternalBarrierMethods<JSString*>::postBarrier(strp, prev, next);
}

JS_PUBLIC_API(void)
JS::HeapValuePostBarrier(JS::Value* valuep, const Value& prev, const Value& next)
{
    MOZ_ASSERT(valuep);
    js::InternalBarrierMethods<JS::Value>::postBarrier(valuep, prev, next);
}
