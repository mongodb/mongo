/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Barrier.h"

#include "jscompartment.h"
#include "jsobj.h"

#include "gc/Zone.h"
#include "js/HashTable.h"
#include "js/Value.h"
#include "vm/ScopeObject.h"
#include "vm/SharedArrayObject.h"
#include "vm/Symbol.h"

namespace js {

#ifdef DEBUG

template <typename T>
void
BarrieredBase<T>::assertTypeConstraints() const
{
    static_assert(mozilla::IsBaseOf<gc::Cell, typename mozilla::RemovePointer<T>::Type>::value ||
                  mozilla::IsSame<JS::Value, T>::value ||
                  mozilla::IsSame<jsid, T>::value ||
                  mozilla::IsSame<TaggedProto, T>::value,
                  "ensure only supported types are instantiated with barriers");
}
#define INSTANTIATE_ALL_VALID_TYPES(type) \
    template void BarrieredBase<type>::assertTypeConstraints() const;
FOR_EACH_GC_POINTER_TYPE(INSTANTIATE_ALL_VALID_TYPES)
#undef INSTANTIATE_ALL_VALID_TYPES

bool
HeapSlot::preconditionForSet(NativeObject* owner, Kind kind, uint32_t slot)
{
    return kind == Slot
         ? &owner->getSlotRef(slot) == this
         : &owner->getDenseElement(slot) == (const Value*)this;
}

bool
HeapSlot::preconditionForWriteBarrierPost(NativeObject* obj, Kind kind, uint32_t slot,
                                              Value target) const
{
    return kind == Slot
         ? obj->getSlotAddressUnchecked(slot)->get() == target
         : static_cast<HeapSlot*>(obj->getDenseElements() + slot)->get() == target;
}

bool
RuntimeFromMainThreadIsHeapMajorCollecting(JS::shadow::Zone* shadowZone)
{
    return shadowZone->runtimeFromMainThread()->isHeapMajorCollecting();
}

bool
CurrentThreadIsIonCompiling()
{
    return TlsPerThreadData.get()->ionCompiling;
}

bool
CurrentThreadIsIonCompilingSafeForMinorGC()
{
    return TlsPerThreadData.get()->ionCompilingSafeForMinorGC;
}

bool
CurrentThreadIsGCSweeping()
{
    return TlsPerThreadData.get()->gcSweeping;
}

bool
CurrentThreadIsHandlingInitFailure()
{
    JSRuntime* rt = TlsPerThreadData.get()->runtimeIfOnOwnerThread();
    return rt && rt->handlingInitFailure;
}

#endif // DEBUG

template <typename S>
template <typename T>
void
ReadBarrierFunctor<S>::operator()(T* t)
{
    InternalGCMethods<T*>::readBarrier(t);
}
template void ReadBarrierFunctor<JS::Value>::operator()<JS::Symbol>(JS::Symbol*);
template void ReadBarrierFunctor<JS::Value>::operator()<JSObject>(JSObject*);
template void ReadBarrierFunctor<JS::Value>::operator()<JSString>(JSString*);

template <typename S>
template <typename T>
void
PreBarrierFunctor<S>::operator()(T* t)
{
    InternalGCMethods<T*>::preBarrier(t);
}
template void PreBarrierFunctor<JS::Value>::operator()<JS::Symbol>(JS::Symbol*);
template void PreBarrierFunctor<JS::Value>::operator()<JSObject>(JSObject*);
template void PreBarrierFunctor<JS::Value>::operator()<JSString>(JSString*);
template void PreBarrierFunctor<jsid>::operator()<JS::Symbol>(JS::Symbol*);
template void PreBarrierFunctor<jsid>::operator()<JSString>(JSString*);

template <typename T>
/* static */ HashNumber
MovableCellHasher<T>::hash(const Lookup& l)
{
    if (!l)
        return 0;

    // We have to access the zone from-any-thread here: a worker thread may be
    // cloning a self-hosted object from the main-thread-runtime-owned self-
    // hosting zone into the off-main-thread runtime. The zone's uid lock will
    // protect against multiple workers doing this simultaneously.
    MOZ_ASSERT(CurrentThreadCanAccessZone(l->zoneFromAnyThread()) ||
               l->zoneFromAnyThread()->isSelfHostingZone());

    HashNumber hn;
    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (!l->zoneFromAnyThread()->getHashCode(l, &hn))
        oomUnsafe.crash("failed to get a stable hash code");
    return hn;
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
    MOZ_ASSERT(zone->hasUniqueId(k));
    MOZ_ASSERT(zone->hasUniqueId(l));

    // Since both already have a uid (from hash), the get is infallible.
    uint64_t uidK, uidL;
    MOZ_ALWAYS_TRUE(zone->getUniqueId(k, &uidK));
    MOZ_ALWAYS_TRUE(zone->getUniqueId(l, &uidL));
    return uidK == uidL;
}

template struct MovableCellHasher<JSObject*>;
template struct MovableCellHasher<GlobalObject*>;
template struct MovableCellHasher<SavedFrame*>;
template struct MovableCellHasher<ScopeObject*>;
template struct MovableCellHasher<JSScript*>;

} // namespace js

JS_PUBLIC_API(void)
JS::HeapObjectPostBarrier(JSObject** objp, JSObject* prev, JSObject* next)
{
    MOZ_ASSERT(objp);
    js::InternalGCMethods<JSObject*>::postBarrier(objp, prev, next);
}

JS_PUBLIC_API(void)
JS::HeapValuePostBarrier(JS::Value* valuep, const Value& prev, const Value& next)
{
    MOZ_ASSERT(valuep);
    js::InternalGCMethods<JS::Value>::postBarrier(valuep, prev, next);
}
