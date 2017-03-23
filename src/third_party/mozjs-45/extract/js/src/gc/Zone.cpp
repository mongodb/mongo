/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Zone.h"

#include "jsgc.h"

#include "jit/BaselineJIT.h"
#include "jit/Ion.h"
#include "jit/JitCompartment.h"
#include "vm/Debugger.h"
#include "vm/Runtime.h"

#include "jscompartmentinlines.h"
#include "jsgcinlines.h"

using namespace js;
using namespace js::gc;

Zone * const Zone::NotOnList = reinterpret_cast<Zone*>(1);

JS::Zone::Zone(JSRuntime* rt)
  : JS::shadow::Zone(rt, &rt->gc.marker),
    debuggers(nullptr),
    arenas(rt),
    types(this),
    compartments(),
    gcGrayRoots(),
    gcMallocBytes(0),
    gcMallocGCTriggered(false),
    usage(&rt->gc.usage),
    gcDelayBytes(0),
    data(nullptr),
    isSystem(false),
    usedByExclusiveThread(false),
    active(false),
    jitZone_(nullptr),
    gcState_(NoGC),
    gcScheduled_(false),
    gcPreserveCode_(false),
    jitUsingBarriers_(false),
    listNext_(NotOnList)
{
    /* Ensure that there are no vtables to mess us up here. */
    MOZ_ASSERT(reinterpret_cast<JS::shadow::Zone*>(this) ==
               static_cast<JS::shadow::Zone*>(this));

    AutoLockGC lock(rt);
    threshold.updateAfterGC(8192, GC_NORMAL, rt->gc.tunables, rt->gc.schedulingState, lock);
    setGCMaxMallocBytes(rt->gc.maxMallocBytesAllocated() * 0.9);
}

Zone::~Zone()
{
    JSRuntime* rt = runtimeFromMainThread();
    if (this == rt->gc.systemZone)
        rt->gc.systemZone = nullptr;

    js_delete(debuggers);
    js_delete(jitZone_);
}

bool Zone::init(bool isSystemArg)
{
    isSystem = isSystemArg;
    return uniqueIds_.init() && gcZoneGroupEdges.init() && gcWeakKeys.init();
}

void
Zone::setNeedsIncrementalBarrier(bool needs, ShouldUpdateJit updateJit)
{
    if (updateJit == UpdateJit && needs != jitUsingBarriers_) {
        jit::ToggleBarriers(this, needs);
        jitUsingBarriers_ = needs;
    }

    MOZ_ASSERT_IF(needs && isAtomsZone(), !runtimeFromMainThread()->exclusiveThreadsPresent());
    MOZ_ASSERT_IF(needs, canCollect());
    needsIncrementalBarrier_ = needs;
}

void
Zone::resetGCMallocBytes()
{
    gcMallocBytes = ptrdiff_t(gcMaxMallocBytes);
    gcMallocGCTriggered = false;
}

void
Zone::setGCMaxMallocBytes(size_t value)
{
    /*
     * For compatibility treat any value that exceeds PTRDIFF_T_MAX to
     * mean that value.
     */
    gcMaxMallocBytes = (ptrdiff_t(value) >= 0) ? value : size_t(-1) >> 1;
    resetGCMallocBytes();
}

void
Zone::onTooMuchMalloc()
{
    if (!gcMallocGCTriggered) {
        GCRuntime& gc = runtimeFromAnyThread()->gc;
        gcMallocGCTriggered = gc.triggerZoneGC(this, JS::gcreason::TOO_MUCH_MALLOC);
    }
}

void
Zone::beginSweepTypes(FreeOp* fop, bool releaseTypes)
{
    // Periodically release observed types for all scripts. This is safe to
    // do when there are no frames for the zone on the stack.
    if (active)
        releaseTypes = false;

    AutoClearTypeInferenceStateOnOOM oom(this);
    types.beginSweep(fop, releaseTypes, oom);
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
Zone::logPromotionsToTenured()
{
    auto* dbgs = getDebuggers();
    if (MOZ_LIKELY(!dbgs))
        return;

    auto now = JS_GetCurrentEmbedderTime();
    JSRuntime* rt = runtimeFromAnyThread();

    for (auto** dbgp = dbgs->begin(); dbgp != dbgs->end(); dbgp++) {
        if (!(*dbgp)->isEnabled() || !(*dbgp)->isTrackingTenurePromotions())
            continue;

        for (auto range = awaitingTenureLogging.all(); !range.empty(); range.popFront()) {
            if ((*dbgp)->isDebuggeeUnbarriered(range.front()->compartment()))
                (*dbgp)->logTenurePromotion(rt, *range.front(), now);
        }
    }

    awaitingTenureLogging.clear();
}

void
Zone::sweepBreakpoints(FreeOp* fop)
{
    if (fop->runtime()->debuggerList.isEmpty())
        return;

    /*
     * Sweep all compartments in a zone at the same time, since there is no way
     * to iterate over the scripts belonging to a single compartment in a zone.
     */

    MOZ_ASSERT(isGCSweepingOrCompacting());
    for (ZoneCellIterUnderGC i(this, AllocKind::SCRIPT); !i.done(); i.next()) {
        JSScript* script = i.get<JSScript>();
        if (!script->hasAnyBreakpointsOrStepMode())
            continue;

        bool scriptGone = IsAboutToBeFinalizedUnbarriered(&script);
        MOZ_ASSERT(script == i.get<JSScript>());
        for (unsigned i = 0; i < script->length(); i++) {
            BreakpointSite* site = script->getBreakpointSite(script->offsetToPC(i));
            if (!site)
                continue;

            Breakpoint* nextbp;
            for (Breakpoint* bp = site->firstBreakpoint(); bp; bp = nextbp) {
                nextbp = bp->nextInSite();
                HeapPtrNativeObject& dbgobj = bp->debugger->toJSObjectRef();

                // If we are sweeping, then we expect the script and the
                // debugger object to be swept in the same zone group, except if
                // the breakpoint was added after we computed the zone
                // groups. In this case both script and debugger object must be
                // live.
                MOZ_ASSERT_IF(isGCSweeping() && dbgobj->zone()->isCollecting(),
                              dbgobj->zone()->isGCSweeping() ||
                              (!scriptGone && dbgobj->asTenured().isMarked()));

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
Zone::discardJitCode(FreeOp* fop)
{
    if (!jitZone())
        return;

    if (isPreservingCode()) {
        PurgeJITCaches(this);
    } else {

#ifdef DEBUG
        /* Assert no baseline scripts are marked as active. */
        for (ZoneCellIterUnderGC i(this, AllocKind::SCRIPT); !i.done(); i.next()) {
            JSScript* script = i.get<JSScript>();
            MOZ_ASSERT_IF(script->hasBaselineScript(), !script->baselineScript()->active());
        }
#endif

        /* Mark baseline scripts on the stack as active. */
        jit::MarkActiveBaselineScripts(this);

        /* Only mark OSI points if code is being discarded. */
        jit::InvalidateAll(fop, this);

        for (ZoneCellIterUnderGC i(this, AllocKind::SCRIPT); !i.done(); i.next()) {
            JSScript* script = i.get<JSScript>();
            jit::FinishInvalidation(fop, script);

            /*
             * Discard baseline script if it's not marked as active. Note that
             * this also resets the active flag.
             */
            jit::FinishDiscardBaselineScript(fop, script);

            /*
             * Warm-up counter for scripts are reset on GC. After discarding code we
             * need to let it warm back up to get information such as which
             * opcodes are setting array holes or accessing getter properties.
             */
            script->resetWarmUpCounter();
        }

        jitZone()->optimizedStubSpace()->free();
    }
}

#ifdef JSGC_HASH_TABLE_CHECKS
void
JS::Zone::checkUniqueIdTableAfterMovingGC()
{
    for (UniqueIdMap::Enum e(uniqueIds_); !e.empty(); e.popFront())
        js::gc::CheckGCThingAfterMovingGC(e.front().key());
}
#endif

uint64_t
Zone::gcNumber()
{
    // Zones in use by exclusive threads are not collected, and threads using
    // them cannot access the main runtime's gcNumber without racing.
    return usedByExclusiveThread ? 0 : runtimeFromMainThread()->gc.gcNumber();
}

js::jit::JitZone*
Zone::createJitZone(JSContext* cx)
{
    MOZ_ASSERT(!jitZone_);

    if (!cx->runtime()->getJitRuntime(cx))
        return nullptr;

    jitZone_ = cx->new_<js::jit::JitZone>();
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
    // Zones cannot be collected while in use by other threads.
    if (usedByExclusiveThread)
        return false;
    JSRuntime* rt = runtimeFromAnyThread();
    if (isAtomsZone() && rt->exclusiveThreadsPresent())
        return false;
    return true;
}

void
Zone::notifyObservingDebuggers()
{
    for (CompartmentsInZoneIter comps(this); !comps.done(); comps.next()) {
        JSRuntime* rt = runtimeFromAnyThread();
        RootedGlobalObject global(rt, comps->unsafeUnbarrieredMaybeGlobal());
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
js::ZonesIter::atAtomsZone(JSRuntime* rt)
{
    return rt->isAtomsZone(*it);
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

void
ZoneList::removeFront()
{
    MOZ_ASSERT(!isEmpty());
    check();

    Zone* front = head;
    head = head->listNext_;
    if (!head)
        tail = nullptr;

    front->listNext_ = Zone::NotOnList;
}

void
ZoneList::clear()
{
    while (!isEmpty())
        removeFront();
}
