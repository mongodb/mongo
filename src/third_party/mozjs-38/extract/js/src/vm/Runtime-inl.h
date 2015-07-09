/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Runtime_inl_h
#define vm_Runtime_inl_h

#include "vm/Runtime.h"

#include "jscompartment.h"

#include "vm/Probes.h"

#include "jsgcinlines.h"

namespace js {

inline bool
NewObjectCache::lookupProto(const Class* clasp, JSObject* proto, gc::AllocKind kind, EntryIndex* pentry)
{
    MOZ_ASSERT(!proto->is<GlobalObject>());
    return lookup(clasp, proto, kind, pentry);
}

inline bool
NewObjectCache::lookupGlobal(const Class* clasp, js::GlobalObject* global, gc::AllocKind kind, EntryIndex* pentry)
{
    return lookup(clasp, global, kind, pentry);
}

inline void
NewObjectCache::fillGlobal(EntryIndex entry, const Class* clasp, js::GlobalObject* global,
                           gc::AllocKind kind, NativeObject* obj)
{
    //MOZ_ASSERT(global == obj->getGlobal());
    return fill(entry, clasp, global, kind, obj);
}

inline JSObject*
NewObjectCache::newObjectFromHit(JSContext* cx, EntryIndex entryIndex, js::gc::InitialHeap heap)
{
    // The new object cache does not account for metadata attached via callbacks.
    MOZ_ASSERT(!cx->compartment()->hasObjectMetadataCallback());

    MOZ_ASSERT(unsigned(entryIndex) < mozilla::ArrayLength(entries));
    Entry* entry = &entries[entryIndex];

    JSObject* templateObj = reinterpret_cast<JSObject*>(&entry->templateObject);

    // Do an end run around JSObject::group() to avoid doing AutoUnprotectCell
    // on the templateObj, which is not a GC thing and can't use runtimeFromAnyThread.
    ObjectGroup* group = templateObj->group_;

    if (group->shouldPreTenure())
        heap = gc::TenuredHeap;

    if (cx->runtime()->gc.upcomingZealousGC())
        return nullptr;

    JSObject* obj = js::gc::AllocateObjectForCacheHit<NoGC>(cx, entry->kind, heap, group->clasp());
    if (obj) {
        copyCachedToObject(obj, templateObj, entry->kind);
        probes::CreateObject(cx, obj);
        js::gc::TraceCreateObject(obj);
        return obj;
    }

    // Trigger an identical allocation to the one that notified us of OOM
    // so that we trigger the right kind of GC automatically.
    mozilla::DebugOnly<JSObject*> obj2 =
        js::gc::AllocateObjectForCacheHit<CanGC>(cx, entry->kind, heap, group->clasp());
    MOZ_ASSERT(!obj2);
    return nullptr;
}

}  /* namespace js */

#endif /* vm_Runtime_inl_h */
