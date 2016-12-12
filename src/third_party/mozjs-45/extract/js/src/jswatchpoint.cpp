/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jswatchpoint.h"

#include "jsatom.h"
#include "jscompartment.h"
#include "jsfriendapi.h"

#include "gc/Marking.h"

#include "jsgcinlines.h"

using namespace js;
using namespace js::gc;

inline HashNumber
WatchKeyHasher::hash(const Lookup& key)
{
    return MovableCellHasher<PreBarrieredObject>::hash(key.object) ^ HashId(key.id);
}

namespace {

class AutoEntryHolder {
    typedef WatchpointMap::Map Map;
    Generation gen;
    Map& map;
    Map::Ptr p;
    RootedObject obj;
    RootedId id;

  public:
    AutoEntryHolder(JSContext* cx, Map& map, Map::Ptr p)
      : gen(map.generation()), map(map), p(p), obj(cx, p->key().object), id(cx, p->key().id)
    {
        MOZ_ASSERT(!p->value().held);
        p->value().held = true;
    }

    ~AutoEntryHolder() {
        if (gen != map.generation())
            p = map.lookup(WatchKey(obj, id));
        if (p)
            p->value().held = false;
    }
};

} /* anonymous namespace */

bool
WatchpointMap::init()
{
    return map.init();
}

bool
WatchpointMap::watch(JSContext* cx, HandleObject obj, HandleId id,
                     JSWatchPointHandler handler, HandleObject closure)
{
    MOZ_ASSERT(JSID_IS_STRING(id) || JSID_IS_INT(id) || JSID_IS_SYMBOL(id));

    if (!obj->setWatched(cx))
        return false;

    Watchpoint w(handler, closure, false);
    if (!map.put(WatchKey(obj, id), w)) {
        ReportOutOfMemory(cx);
        return false;
    }
    /*
     * For generational GC, we don't need to post-barrier writes to the
     * hashtable here because we mark all watchpoints as part of root marking in
     * markAll().
     */
    return true;
}

void
WatchpointMap::unwatch(JSObject* obj, jsid id,
                       JSWatchPointHandler* handlerp, JSObject** closurep)
{
    if (Map::Ptr p = map.lookup(WatchKey(obj, id))) {
        if (handlerp)
            *handlerp = p->value().handler;
        if (closurep) {
            // Read barrier to prevent an incorrectly gray closure from escaping the
            // watchpoint. See the comment before UnmarkGrayChildren in gc/Marking.cpp
            JS::ExposeObjectToActiveJS(p->value().closure);
            *closurep = p->value().closure;
        }
        map.remove(p);
    }
}

void
WatchpointMap::unwatchObject(JSObject* obj)
{
    for (Map::Enum e(map); !e.empty(); e.popFront()) {
        Map::Entry& entry = e.front();
        if (entry.key().object == obj)
            e.removeFront();
    }
}

void
WatchpointMap::clear()
{
    map.clear();
}

bool
WatchpointMap::triggerWatchpoint(JSContext* cx, HandleObject obj, HandleId id, MutableHandleValue vp)
{
    Map::Ptr p = map.lookup(WatchKey(obj, id));
    if (!p || p->value().held)
        return true;

    AutoEntryHolder holder(cx, map, p);

    /* Copy the entry, since GC would invalidate p. */
    JSWatchPointHandler handler = p->value().handler;
    RootedObject closure(cx, p->value().closure);

    /* Determine the property's old value. */
    Value old;
    old.setUndefined();
    if (obj->isNative()) {
        NativeObject* nobj = &obj->as<NativeObject>();
        if (Shape* shape = nobj->lookup(cx, id)) {
            if (shape->hasSlot())
                old = nobj->getSlot(shape->slot());
        }
    }

    // Read barrier to prevent an incorrectly gray closure from escaping the
    // watchpoint. See the comment before UnmarkGrayChildren in gc/Marking.cpp
    JS::ExposeObjectToActiveJS(closure);

    /* Call the handler. */
    return handler(cx, obj, id, old, vp.address(), closure);
}

bool
WatchpointMap::markIteratively(JSTracer* trc)
{
    bool marked = false;
    for (Map::Enum e(map); !e.empty(); e.popFront()) {
        Map::Entry& entry = e.front();
        JSObject* priorKeyObj = entry.key().object;
        jsid priorKeyId(entry.key().id.get());
        bool objectIsLive =
            IsMarked(const_cast<PreBarrieredObject*>(&entry.key().object));
        if (objectIsLive || entry.value().held) {
            if (!objectIsLive) {
                TraceEdge(trc, const_cast<PreBarrieredObject*>(&entry.key().object),
                           "held Watchpoint object");
                marked = true;
            }

            MOZ_ASSERT(JSID_IS_STRING(priorKeyId) ||
                       JSID_IS_INT(priorKeyId) ||
                       JSID_IS_SYMBOL(priorKeyId));
            TraceEdge(trc, const_cast<PreBarrieredId*>(&entry.key().id), "WatchKey::id");

            if (entry.value().closure && !IsMarked(&entry.value().closure)) {
                TraceEdge(trc, &entry.value().closure, "Watchpoint::closure");
                marked = true;
            }

            /* We will sweep this entry in sweepAll if !objectIsLive. */
            if (priorKeyObj != entry.key().object || priorKeyId != entry.key().id)
                e.rekeyFront(WatchKey(entry.key().object, entry.key().id));
        }
    }
    return marked;
}

void
WatchpointMap::markAll(JSTracer* trc)
{
    for (Map::Enum e(map); !e.empty(); e.popFront()) {
        Map::Entry& entry = e.front();
        WatchKey key = entry.key();
        WatchKey prior = key;
        MOZ_ASSERT(JSID_IS_STRING(prior.id) || JSID_IS_INT(prior.id) || JSID_IS_SYMBOL(prior.id));

        TraceEdge(trc, const_cast<PreBarrieredObject*>(&key.object),
                   "held Watchpoint object");
        TraceEdge(trc, const_cast<PreBarrieredId*>(&key.id), "WatchKey::id");
        TraceEdge(trc, &entry.value().closure, "Watchpoint::closure");

        if (prior.object != key.object || prior.id != key.id)
            e.rekeyFront(key);
    }
}

void
WatchpointMap::sweepAll(JSRuntime* rt)
{
    for (GCCompartmentsIter c(rt); !c.done(); c.next()) {
        if (WatchpointMap* wpmap = c->watchpointMap)
            wpmap->sweep();
    }
}

void
WatchpointMap::sweep()
{
    for (Map::Enum e(map); !e.empty(); e.popFront()) {
        Map::Entry& entry = e.front();
        JSObject* obj(entry.key().object);
        if (IsAboutToBeFinalizedUnbarriered(&obj)) {
            MOZ_ASSERT(!entry.value().held);
            e.removeFront();
        } else if (obj != entry.key().object) {
            e.rekeyFront(WatchKey(obj, entry.key().id));
        }
    }
}

void
WatchpointMap::traceAll(WeakMapTracer* trc)
{
    JSRuntime* rt = trc->runtime;
    for (CompartmentsIter comp(rt, SkipAtoms); !comp.done(); comp.next()) {
        if (WatchpointMap* wpmap = comp->watchpointMap)
            wpmap->trace(trc);
    }
}

void
WatchpointMap::trace(WeakMapTracer* trc)
{
    for (Map::Range r = map.all(); !r.empty(); r.popFront()) {
        Map::Entry& entry = r.front();
        trc->trace(nullptr,
                   JS::GCCellPtr(entry.key().object.get()),
                   JS::GCCellPtr(entry.value().closure.get()));
    }
}
