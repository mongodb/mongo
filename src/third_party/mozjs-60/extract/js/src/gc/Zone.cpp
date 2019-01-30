/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Zone.h"

#include "gc/Policy.h"
#include "gc/PublicIterators.h"
#include "jit/BaselineJIT.h"
#include "jit/Ion.h"
#include "jit/JitCompartment.h"
#include "vm/Debugger.h"
#include "vm/Runtime.h"

#include "gc/GC-inl.h"
#include "gc/Marking-inl.h"
#include "vm/JSCompartment-inl.h"

using namespace js;
using namespace js::gc;

Zone * const Zone::NotOnList = reinterpret_cast<Zone*>(1);

JS::Zone::Zone(JSRuntime* rt, ZoneGroup* group)
  : JS::shadow::Zone(rt, &rt->gc.marker),
    group_(group),
    debuggers(group, nullptr),
    uniqueIds_(group),
    suppressAllocationMetadataBuilder(group, false),
    arenas(rt, group),
    types(this),
    gcWeakMapList_(group),
    compartments_(),
    gcGrayRoots_(group),
    gcWeakRefs_(group),
    weakCaches_(group),
    gcWeakKeys_(group, SystemAllocPolicy(), rt->randomHashCodeScrambler()),
    typeDescrObjects_(group, this),
    regExps(this),
    markedAtoms_(group),
    atomCache_(group),
    externalStringCache_(group),
    functionToStringCache_(group),
    usage(&rt->gc.usage),
    threshold(),
    gcDelayBytes(0),
    tenuredStrings(group, 0),
    allocNurseryStrings(group, true),
    propertyTree_(group, this),
    baseShapes_(group, this),
    initialShapes_(group, this),
    nurseryShapes_(group),
    data(group, nullptr),
    isSystem(group, false),
#ifdef DEBUG
    gcLastSweepGroupIndex(group, 0),
#endif
    jitZone_(group, nullptr),
    gcScheduled_(false),
    gcScheduledSaved_(false),
    gcPreserveCode_(group, false),
    keepShapeTables_(group, false),
    listNext_(NotOnList)
{
    /* Ensure that there are no vtables to mess us up here. */
    MOZ_ASSERT(reinterpret_cast<JS::shadow::Zone*>(this) ==
               static_cast<JS::shadow::Zone*>(this));

    AutoLockGC lock(rt);
    threshold.updateAfterGC(8192, GC_NORMAL, rt->gc.tunables, rt->gc.schedulingState, lock);
    setGCMaxMallocBytes(rt->gc.tunables.maxMallocBytes(), lock);
    jitCodeCounter.setMax(jit::MaxCodeBytesPerProcess * 0.8, lock);
}

Zone::~Zone()
{
    JSRuntime* rt = runtimeFromAnyThread();
    if (this == rt->gc.systemZone)
        rt->gc.systemZone = nullptr;

    js_delete(debuggers.ref());
    js_delete(jitZone_.ref());

#ifdef DEBUG
    // Avoid assertions failures warning that not everything has been destroyed
    // if the embedding leaked GC things.
    if (!rt->gc.shutdownCollectedEverything()) {
        gcWeakMapList().clear();
        regExps.clear();
    }
#endif
}

bool
Zone::init(bool isSystemArg)
{
    isSystem = isSystemArg;
    return uniqueIds().init() &&
           gcSweepGroupEdges().init() &&
           gcWeakKeys().init() &&
           typeDescrObjects().init() &&
           markedAtoms().init() &&
           atomCache().init() &&
           regExps.init();
}

void
Zone::setNeedsIncrementalBarrier(bool needs)
{
    MOZ_ASSERT_IF(needs, canCollect());
    needsIncrementalBarrier_ = needs;
}

void
Zone::beginSweepTypes(bool releaseTypes)
{
    AutoClearTypeInferenceStateOnOOM oom(this);
    types.beginSweep(releaseTypes, oom);
}

Zone::DebuggerVector*
Zone::getOrCreateDebuggers(JSContext* cx)
{
    if (debuggers)
        return debuggers;

    debuggers = js_new<DebuggerVector>();
    if (!debuggers)
        ReportOutOfMemory(cx);
    return debuggers;
}

void
Zone::sweepBreakpoints(FreeOp* fop)
{
    if (!group() || group()->debuggerList().isEmpty())
        return;

    /*
     * Sweep all compartments in a zone at the same time, since there is no way
     * to iterate over the scripts belonging to a single compartment in a zone.
     */

    MOZ_ASSERT(isGCSweepingOrCompacting());
    for (auto iter = cellIter<JSScript>(); !iter.done(); iter.next()) {
        JSScript* script = iter;
        if (!script->hasAnyBreakpointsOrStepMode())
            continue;

        bool scriptGone = IsAboutToBeFinalizedUnbarriered(&script);
        MOZ_ASSERT(script == iter);
        for (unsigned i = 0; i < script->length(); i++) {
            BreakpointSite* site = script->getBreakpointSite(script->offsetToPC(i));
            if (!site)
                continue;

            Breakpoint* nextbp;
            for (Breakpoint* bp = site->firstBreakpoint(); bp; bp = nextbp) {
                nextbp = bp->nextInSite();
                GCPtrNativeObject& dbgobj = bp->debugger->toJSObjectRef();

                // If we are sweeping, then we expect the script and the
                // debugger object to be swept in the same sweep group, except
                // if the breakpoint was added after we computed the sweep
                // groups. In this case both script and debugger object must be
                // live.
                MOZ_ASSERT_IF(isGCSweeping() && dbgobj->zone()->isCollecting(),
                              dbgobj->zone()->isGCSweeping() ||
                              (!scriptGone && dbgobj->asTenured().isMarkedAny()));

                bool dying = scriptGone || IsAboutToBeFinalized(&dbgobj);
                MOZ_ASSERT_IF(!dying, !IsAboutToBeFinalized(&bp->getHandlerRef()));
                if (dying)
                    bp->destroy(fop);
            }
        }
    }
}

void
Zone::sweepWeakMaps()
{
    /* Finalize unreachable (key,value) pairs in all weak maps. */
    WeakMapBase::sweepZone(this);
}

void
Zone::discardJitCode(FreeOp* fop, bool discardBaselineCode)
{
    if (!jitZone())
        return;

    if (isPreservingCode())
        return;

    if (discardBaselineCode) {
#ifdef DEBUG
        /* Assert no baseline scripts are marked as active. */
        for (auto script = cellIter<JSScript>(); !script.done(); script.next())
            MOZ_ASSERT_IF(script->hasBaselineScript(), !script->baselineScript()->active());
#endif

        /* Mark baseline scripts on the stack as active. */
        jit::MarkActiveBaselineScripts(this);
    }

    /* Only mark OSI points if code is being discarded. */
    jit::InvalidateAll(fop, this);

    for (auto script = cellIter<JSScript>(); !script.done(); script.next())  {
        jit::FinishInvalidation(fop, script);

        /*
         * Discard baseline script if it's not marked as active. Note that
         * this also resets the active flag.
         */
        if (discardBaselineCode)
            jit::FinishDiscardBaselineScript(fop, script);

        /*
         * Warm-up counter for scripts are reset on GC. After discarding code we
         * need to let it warm back up to get information such as which
         * opcodes are setting array holes or accessing getter properties.
         */
        script->resetWarmUpCounter();

        /*
         * Make it impossible to use the control flow graphs cached on the
         * BaselineScript. They get deleted.
         */
        if (script->hasBaselineScript())
            script->baselineScript()->setControlFlowGraph(nullptr);
    }

    /*
     * When scripts contains pointers to nursery things, the store buffer
     * can contain entries that point into the optimized stub space. Since
     * this method can be called outside the context of a GC, this situation
     * could result in us trying to mark invalid store buffer entries.
     *
     * Defer freeing any allocated blocks until after the next minor GC.
     */
    if (discardBaselineCode) {
        jitZone()->optimizedStubSpace()->freeAllAfterMinorGC(this);
        jitZone()->purgeIonCacheIRStubInfo();
    }

    /*
     * Free all control flow graphs that are cached on BaselineScripts.
     * Assuming this happens on the active thread and all control flow
     * graph reads happen on the active thread, this is safe.
     */
    jitZone()->cfgSpace()->lifoAlloc().freeAll();
}

#ifdef JSGC_HASH_TABLE_CHECKS
void
JS::Zone::checkUniqueIdTableAfterMovingGC()
{
    for (auto r = uniqueIds().all(); !r.empty(); r.popFront())
        js::gc::CheckGCThingAfterMovingGC(r.front().key());
}
#endif

uint64_t
Zone::gcNumber()
{
    // Zones in use by exclusive threads are not collected, and threads using
    // them cannot access the main runtime's gcNumber without racing.
    return usedByHelperThread() ? 0 : runtimeFromActiveCooperatingThread()->gc.gcNumber();
}

js::jit::JitZone*
Zone::createJitZone(JSContext* cx)
{
    MOZ_ASSERT(!jitZone_);

    if (!cx->runtime()->getJitRuntime(cx))
        return nullptr;

    UniquePtr<jit::JitZone> jitZone(cx->new_<js::jit::JitZone>());
    if (!jitZone || !jitZone->init(cx))
        return nullptr;

    jitZone_ = jitZone.release();
    return jitZone_;
}

bool
Zone::hasMarkedCompartments()
{
    for (CompartmentsInZoneIter comp(this); !comp.done(); comp.next()) {
        if (comp->marked)
            return true;
    }
    return false;
}

bool
Zone::canCollect()
{
    // The atoms zone cannot be collected while off-thread parsing is taking
    // place.
    if (isAtomsZone())
        return !runtimeFromAnyThread()->hasHelperThreadZones();

    // Zones that will be or are currently used by other threads cannot be
    // collected.
    return !group()->createdForHelperThread();
}

void
Zone::notifyObservingDebuggers()
{
    for (CompartmentsInZoneIter comps(this); !comps.done(); comps.next()) {
        JSRuntime* rt = runtimeFromAnyThread();
        RootedGlobalObject global(TlsContext.get(), comps->unsafeUnbarrieredMaybeGlobal());
        if (!global)
            continue;

        GlobalObject::DebuggerVector* dbgs = global->getDebuggers();
        if (!dbgs)
            continue;

        for (GlobalObject::DebuggerVector::Range r = dbgs->all(); !r.empty(); r.popFront()) {
            if (!r.front()->debuggeeIsBeingCollected(rt->gc.majorGCCount())) {
#ifdef DEBUG
                fprintf(stderr,
                        "OOM while notifying observing Debuggers of a GC: The onGarbageCollection\n"
                        "hook will not be fired for this GC for some Debuggers!\n");
#endif
                return;
            }
        }
    }
}

bool
Zone::isOnList() const
{
    return listNext_ != NotOnList;
}

Zone*
Zone::nextZone() const
{
    MOZ_ASSERT(isOnList());
    return listNext_;
}

void
Zone::clearTables()
{
    MOZ_ASSERT(regExps.empty());

    if (baseShapes().initialized())
        baseShapes().clear();
    if (initialShapes().initialized())
        initialShapes().clear();
}

void
Zone::fixupAfterMovingGC()
{
    fixupInitialShapeTable();
}

bool
Zone::addTypeDescrObject(JSContext* cx, HandleObject obj)
{
    // Type descriptor objects are always tenured so we don't need post barriers
    // on the set.
    MOZ_ASSERT(!IsInsideNursery(obj));

    if (!typeDescrObjects().put(obj)) {
        ReportOutOfMemory(cx);
        return false;
    }

    return true;
}

void
Zone::deleteEmptyCompartment(JSCompartment* comp)
{
    MOZ_ASSERT(comp->zone() == this);
    MOZ_ASSERT(arenas.checkEmptyArenaLists());
    for (auto& i : compartments()) {
        if (i == comp) {
            compartments().erase(&i);
            comp->destroy(runtimeFromActiveCooperatingThread()->defaultFreeOp());
            return;
        }
    }
    MOZ_CRASH("Compartment not found");
}

ZoneList::ZoneList()
  : head(nullptr), tail(nullptr)
{}

ZoneList::ZoneList(Zone* zone)
  : head(zone), tail(zone)
{
    MOZ_RELEASE_ASSERT(!zone->isOnList());
    zone->listNext_ = nullptr;
}

ZoneList::~ZoneList()
{
    MOZ_ASSERT(isEmpty());
}

void
ZoneList::check() const
{
#ifdef DEBUG
    MOZ_ASSERT((head == nullptr) == (tail == nullptr));
    if (!head)
        return;

    Zone* zone = head;
    for (;;) {
        MOZ_ASSERT(zone && zone->isOnList());
        if  (zone == tail)
            break;
        zone = zone->listNext_;
    }
    MOZ_ASSERT(!zone->listNext_);
#endif
}

bool
ZoneList::isEmpty() const
{
    return head == nullptr;
}

Zone*
ZoneList::front() const
{
    MOZ_ASSERT(!isEmpty());
    MOZ_ASSERT(head->isOnList());
    return head;
}

void
ZoneList::append(Zone* zone)
{
    ZoneList singleZone(zone);
    transferFrom(singleZone);
}

void
ZoneList::transferFrom(ZoneList& other)
{
    check();
    other.check();
    MOZ_ASSERT(tail != other.tail);

    if (tail)
        tail->listNext_ = other.head;
    else
        head = other.head;
    tail = other.tail;

    other.head = nullptr;
    other.tail = nullptr;
}

Zone*
ZoneList::removeFront()
{
    MOZ_ASSERT(!isEmpty());
    check();

    Zone* front = head;
    head = head->listNext_;
    if (!head)
        tail = nullptr;

    front->listNext_ = Zone::NotOnList;

    return front;
}

void
ZoneList::clear()
{
    while (!isEmpty())
        removeFront();
}

JS_PUBLIC_API(void)
JS::shadow::RegisterWeakCache(JS::Zone* zone, detail::WeakCacheBase* cachep)
{
    zone->registerWeakCache(cachep);
}
