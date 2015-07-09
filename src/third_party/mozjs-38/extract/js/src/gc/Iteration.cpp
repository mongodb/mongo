/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jscompartment.h"
#include "jsgc.h"

#include "gc/GCInternals.h"
#include "js/HashTable.h"
#include "vm/Runtime.h"

#include "jscntxtinlines.h"
#include "jsgcinlines.h"

using namespace js;
using namespace js::gc;

void
js::TraceRuntime(JSTracer* trc)
{
    MOZ_ASSERT(!IS_GC_MARKING_TRACER(trc));

    JSRuntime* rt = trc->runtime();
    rt->gc.evictNursery();
    AutoPrepareForTracing prep(rt, WithAtoms);
    gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_TRACE_HEAP);
    rt->gc.markRuntime(trc);
}

static void
IterateCompartmentsArenasCells(JSRuntime* rt, Zone* zone, void* data,
                               JSIterateCompartmentCallback compartmentCallback,
                               IterateArenaCallback arenaCallback,
                               IterateCellCallback cellCallback)
{
    for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next())
        (*compartmentCallback)(rt, data, comp);

    for (size_t thingKind = 0; thingKind != FINALIZE_LIMIT; thingKind++) {
        JSGCTraceKind traceKind = MapAllocToTraceKind(AllocKind(thingKind));
        size_t thingSize = Arena::thingSize(AllocKind(thingKind));

        for (ArenaIter aiter(zone, AllocKind(thingKind)); !aiter.done(); aiter.next()) {
            ArenaHeader* aheader = aiter.get();
            (*arenaCallback)(rt, data, aheader->getArena(), traceKind, thingSize);
            for (ArenaCellIterUnderGC iter(aheader); !iter.done(); iter.next())
                (*cellCallback)(rt, data, iter.getCell(), traceKind, thingSize);
        }
    }
}

void
js::IterateZonesCompartmentsArenasCells(JSRuntime* rt, void* data,
                                        IterateZoneCallback zoneCallback,
                                        JSIterateCompartmentCallback compartmentCallback,
                                        IterateArenaCallback arenaCallback,
                                        IterateCellCallback cellCallback)
{
    AutoPrepareForTracing prop(rt, WithAtoms);

    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        (*zoneCallback)(rt, data, zone);
        IterateCompartmentsArenasCells(rt, zone, data,
                                       compartmentCallback, arenaCallback, cellCallback);
    }
}

void
js::IterateZoneCompartmentsArenasCells(JSRuntime* rt, Zone* zone, void* data,
                                       IterateZoneCallback zoneCallback,
                                       JSIterateCompartmentCallback compartmentCallback,
                                       IterateArenaCallback arenaCallback,
                                       IterateCellCallback cellCallback)
{
    AutoPrepareForTracing prop(rt, WithAtoms);

    (*zoneCallback)(rt, data, zone);
    IterateCompartmentsArenasCells(rt, zone, data,
                                   compartmentCallback, arenaCallback, cellCallback);
}

void
js::IterateChunks(JSRuntime* rt, void* data, IterateChunkCallback chunkCallback)
{
    AutoPrepareForTracing prep(rt, SkipAtoms);

    for (auto chunk = rt->gc.allNonEmptyChunks(); !chunk.done(); chunk.next())
        chunkCallback(rt, data, chunk);
}

void
js::IterateScripts(JSRuntime* rt, JSCompartment* compartment,
                   void* data, IterateScriptCallback scriptCallback)
{
    rt->gc.evictNursery();
    AutoPrepareForTracing prep(rt, SkipAtoms);

    if (compartment) {
        for (ZoneCellIterUnderGC i(compartment->zone(), gc::FINALIZE_SCRIPT); !i.done(); i.next()) {
            JSScript* script = i.get<JSScript>();
            if (script->compartment() == compartment)
                scriptCallback(rt, data, script);
        }
    } else {
        for (ZonesIter zone(rt, SkipAtoms); !zone.done(); zone.next()) {
            for (ZoneCellIterUnderGC i(zone, gc::FINALIZE_SCRIPT); !i.done(); i.next())
                scriptCallback(rt, data, i.get<JSScript>());
        }
    }
}

void
js::IterateGrayObjects(Zone* zone, GCThingCallback cellCallback, void* data)
{
    zone->runtimeFromMainThread()->gc.evictNursery();
    AutoPrepareForTracing prep(zone->runtimeFromMainThread(), SkipAtoms);

    for (size_t finalizeKind = 0; finalizeKind <= FINALIZE_OBJECT_LAST; finalizeKind++) {
        for (ZoneCellIterUnderGC i(zone, AllocKind(finalizeKind)); !i.done(); i.next()) {
            JSObject* obj = i.get<JSObject>();
            if (obj->asTenured().isMarked(GRAY))
                cellCallback(data, JS::GCCellPtr(obj));
        }
    }
}

JS_PUBLIC_API(void)
JS_IterateCompartments(JSRuntime* rt, void* data,
                       JSIterateCompartmentCallback compartmentCallback)
{
    MOZ_ASSERT(!rt->isHeapBusy());

    AutoTraceSession session(rt);

    for (CompartmentsIter c(rt, WithAtoms); !c.done(); c.next())
        (*compartmentCallback)(rt, data, c);
}
