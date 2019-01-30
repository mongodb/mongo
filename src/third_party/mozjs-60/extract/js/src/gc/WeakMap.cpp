/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/WeakMap.h"

#include <string.h>

#include "jsapi.h"
#include "jsfriendapi.h"

#include "gc/PublicIterators.h"
#include "js/Wrapper.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"

#include "vm/JSObject-inl.h"

using namespace js;
using namespace js::gc;

WeakMapBase::WeakMapBase(JSObject* memOf, Zone* zone)
  : memberOf(memOf),
    zone_(zone),
    marked(false)
{
    MOZ_ASSERT_IF(memberOf, memberOf->compartment()->zone() == zone);
}

WeakMapBase::~WeakMapBase()
{
    MOZ_ASSERT(CurrentThreadIsGCSweeping() || CurrentThreadCanAccessZone(zone_));
}

void
WeakMapBase::unmarkZone(JS::Zone* zone)
{
    for (WeakMapBase* m : zone->gcWeakMapList())
        m->marked = false;
}

void
WeakMapBase::traceZone(JS::Zone* zone, JSTracer* tracer)
{
    MOZ_ASSERT(tracer->weakMapAction() != DoNotTraceWeakMaps);
    for (WeakMapBase* m : zone->gcWeakMapList()) {
        m->trace(tracer);
        TraceNullableEdge(tracer, &m->memberOf, "memberOf");
    }
}

bool
WeakMapBase::markZoneIteratively(JS::Zone* zone, GCMarker* marker)
{
    bool markedAny = false;
    for (WeakMapBase* m : zone->gcWeakMapList()) {
        if (m->marked && m->markIteratively(marker))
            markedAny = true;
    }
    return markedAny;
}

bool
WeakMapBase::findInterZoneEdges(JS::Zone* zone)
{
    for (WeakMapBase* m : zone->gcWeakMapList()) {
        if (!m->findZoneEdges())
            return false;
    }
    return true;
}

void
WeakMapBase::sweepZone(JS::Zone* zone)
{
    for (WeakMapBase* m = zone->gcWeakMapList().getFirst(); m; ) {
        WeakMapBase* next = m->getNext();
        if (m->marked) {
            m->sweep();
        } else {
            /* Destroy the hash map now to catch any use after this point. */
            m->finish();
            m->removeFrom(zone->gcWeakMapList());
        }
        m = next;
    }

#ifdef DEBUG
    for (WeakMapBase* m : zone->gcWeakMapList())
        MOZ_ASSERT(m->isInList() && m->marked);
#endif
}

void
WeakMapBase::traceAllMappings(WeakMapTracer* tracer)
{
    JSRuntime* rt = tracer->runtime;
    for (ZonesIter zone(rt, SkipAtoms); !zone.done(); zone.next()) {
        for (WeakMapBase* m : zone->gcWeakMapList()) {
            // The WeakMapTracer callback is not allowed to GC.
            JS::AutoSuppressGCAnalysis nogc;
            m->traceMappings(tracer);
        }
    }
}

bool
WeakMapBase::saveZoneMarkedWeakMaps(JS::Zone* zone, WeakMapSet& markedWeakMaps)
{
    for (WeakMapBase* m : zone->gcWeakMapList()) {
        if (m->marked && !markedWeakMaps.put(m))
            return false;
    }
    return true;
}

void
WeakMapBase::restoreMarkedWeakMaps(WeakMapSet& markedWeakMaps)
{
    for (WeakMapSet::Range r = markedWeakMaps.all(); !r.empty(); r.popFront()) {
        WeakMapBase* map = r.front();
        MOZ_ASSERT(map->zone()->isGCMarking());
        MOZ_ASSERT(!map->marked);
        map->marked = true;
    }
}

bool
ObjectValueMap::findZoneEdges()
{
    /*
     * For unmarked weakmap keys with delegates in a different zone, add a zone
     * edge to ensure that the delegate zone finishes marking before the key
     * zone.
     */
    JS::AutoSuppressGCAnalysis nogc;
    for (Range r = all(); !r.empty(); r.popFront()) {
        JSObject* key = r.front().key();
        if (key->asTenured().isMarkedBlack())
            continue;
        JSObject* delegate = getDelegate(key);
        if (!delegate)
            continue;
        Zone* delegateZone = delegate->zone();
        if (delegateZone == zone() || !delegateZone->isGCMarking())
            continue;
        if (!delegateZone->gcSweepGroupEdges().put(key->zone()))
            return false;
    }
    return true;
}

ObjectWeakMap::ObjectWeakMap(JSContext* cx)
  : map(cx, nullptr)
{}

bool
ObjectWeakMap::init()
{
    return map.init();
}

JSObject*
ObjectWeakMap::lookup(const JSObject* obj)
{
    MOZ_ASSERT(map.initialized());
    if (ObjectValueMap::Ptr p = map.lookup(const_cast<JSObject*>(obj)))
        return &p->value().toObject();
    return nullptr;
}

bool
ObjectWeakMap::add(JSContext* cx, JSObject* obj, JSObject* target)
{
    MOZ_ASSERT(obj && target);
    MOZ_ASSERT(map.initialized());

    MOZ_ASSERT(!map.has(obj));
    if (!map.put(obj, ObjectValue(*target))) {
        ReportOutOfMemory(cx);
        return false;
    }

    return true;
}

void
ObjectWeakMap::clear()
{
    MOZ_ASSERT(map.initialized());
    map.clear();
}

void
ObjectWeakMap::trace(JSTracer* trc)
{
    map.trace(trc);
}

size_t
ObjectWeakMap::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf)
{
    MOZ_ASSERT(map.initialized());
    return map.sizeOfExcludingThis(mallocSizeOf);
}

#ifdef JSGC_HASH_TABLE_CHECKS
void
ObjectWeakMap::checkAfterMovingGC()
{
    MOZ_ASSERT(map.initialized());
    for (ObjectValueMap::Range r = map.all(); !r.empty(); r.popFront()) {
        CheckGCThingAfterMovingGC(r.front().key().get());
        CheckGCThingAfterMovingGC(&r.front().value().toObject());
    }
}
#endif // JSGC_HASH_TABLE_CHECKS
