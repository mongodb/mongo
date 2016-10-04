/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * This code implements an incremental mark-and-sweep garbage collector, with
 * most sweeping carried out in the background on a parallel thread.
 *
 * Full vs. zone GC
 * ----------------
 *
 * The collector can collect all zones at once, or a subset. These types of
 * collection are referred to as a full GC and a zone GC respectively.
 *
 * The atoms zone is only collected in a full GC since objects in any zone may
 * have pointers to atoms, and these are not recorded in the cross compartment
 * pointer map. Also, the atoms zone is not collected if any thread has an
 * AutoKeepAtoms instance on the stack, or there are any exclusive threads using
 * the runtime.
 *
 * It is possible for an incremental collection that started out as a full GC to
 * become a zone GC if new zones are created during the course of the
 * collection.
 *
 * Incremental collection
 * ----------------------
 *
 * For a collection to be carried out incrementally the following conditions
 * must be met:
 *  - the collection must be run by calling js::GCSlice() rather than js::GC()
 *  - the GC mode must have been set to JSGC_MODE_INCREMENTAL with
 *    JS_SetGCParameter()
 *  - no thread may have an AutoKeepAtoms instance on the stack
 *
 * The last condition is an engine-internal mechanism to ensure that incremental
 * collection is not carried out without the correct barriers being implemented.
 * For more information see 'Incremental marking' below.
 *
 * If the collection is not incremental, all foreground activity happens inside
 * a single call to GC() or GCSlice(). However the collection is not complete
 * until the background sweeping activity has finished.
 *
 * An incremental collection proceeds as a series of slices, interleaved with
 * mutator activity, i.e. running JavaScript code. Slices are limited by a time
 * budget. The slice finishes as soon as possible after the requested time has
 * passed.
 *
 * Collector states
 * ----------------
 *
 * The collector proceeds through the following states, the current state being
 * held in JSRuntime::gcIncrementalState:
 *
 *  - MARK_ROOTS - marks the stack and other roots
 *  - MARK       - incrementally marks reachable things
 *  - SWEEP      - sweeps zones in groups and continues marking unswept zones
 *
 * The MARK_ROOTS activity always takes place in the first slice. The next two
 * states can take place over one or more slices.
 *
 * In other words an incremental collection proceeds like this:
 *
 * Slice 1:   MARK_ROOTS: Roots pushed onto the mark stack.
 *            MARK:       The mark stack is processed by popping an element,
 *                        marking it, and pushing its children.
 *
 *          ... JS code runs ...
 *
 * Slice 2:   MARK:       More mark stack processing.
 *
 *          ... JS code runs ...
 *
 * Slice n-1: MARK:       More mark stack processing.
 *
 *          ... JS code runs ...
 *
 * Slice n:   MARK:       Mark stack is completely drained.
 *            SWEEP:      Select first group of zones to sweep and sweep them.
 *
 *          ... JS code runs ...
 *
 * Slice n+1: SWEEP:      Mark objects in unswept zones that were newly
 *                        identified as alive (see below). Then sweep more zone
 *                        groups.
 *
 *          ... JS code runs ...
 *
 * Slice n+2: SWEEP:      Mark objects in unswept zones that were newly
 *                        identified as alive. Then sweep more zone groups.
 *
 *          ... JS code runs ...
 *
 * Slice m:   SWEEP:      Sweeping is finished, and background sweeping
 *                        started on the helper thread.
 *
 *          ... JS code runs, remaining sweeping done on background thread ...
 *
 * When background sweeping finishes the GC is complete.
 *
 * Incremental marking
 * -------------------
 *
 * Incremental collection requires close collaboration with the mutator (i.e.,
 * JS code) to guarantee correctness.
 *
 *  - During an incremental GC, if a memory location (except a root) is written
 *    to, then the value it previously held must be marked. Write barriers
 *    ensure this.
 *
 *  - Any object that is allocated during incremental GC must start out marked.
 *
 *  - Roots are marked in the first slice and hence don't need write barriers.
 *    Roots are things like the C stack and the VM stack.
 *
 * The problem that write barriers solve is that between slices the mutator can
 * change the object graph. We must ensure that it cannot do this in such a way
 * that makes us fail to mark a reachable object (marking an unreachable object
 * is tolerable).
 *
 * We use a snapshot-at-the-beginning algorithm to do this. This means that we
 * promise to mark at least everything that is reachable at the beginning of
 * collection. To implement it we mark the old contents of every non-root memory
 * location written to by the mutator while the collection is in progress, using
 * write barriers. This is described in gc/Barrier.h.
 *
 * Incremental sweeping
 * --------------------
 *
 * Sweeping is difficult to do incrementally because object finalizers must be
 * run at the start of sweeping, before any mutator code runs. The reason is
 * that some objects use their finalizers to remove themselves from caches. If
 * mutator code was allowed to run after the start of sweeping, it could observe
 * the state of the cache and create a new reference to an object that was just
 * about to be destroyed.
 *
 * Sweeping all finalizable objects in one go would introduce long pauses, so
 * instead sweeping broken up into groups of zones. Zones which are not yet
 * being swept are still marked, so the issue above does not apply.
 *
 * The order of sweeping is restricted by cross compartment pointers - for
 * example say that object |a| from zone A points to object |b| in zone B and
 * neither object was marked when we transitioned to the SWEEP phase. Imagine we
 * sweep B first and then return to the mutator. It's possible that the mutator
 * could cause |a| to become alive through a read barrier (perhaps it was a
 * shape that was accessed via a shape table). Then we would need to mark |b|,
 * which |a| points to, but |b| has already been swept.
 *
 * So if there is such a pointer then marking of zone B must not finish before
 * marking of zone A.  Pointers which form a cycle between zones therefore
 * restrict those zones to being swept at the same time, and these are found
 * using Tarjan's algorithm for finding the strongly connected components of a
 * graph.
 *
 * GC things without finalizers, and things with finalizers that are able to run
 * in the background, are swept on the background thread. This accounts for most
 * of the sweeping work.
 *
 * Reset
 * -----
 *
 * During incremental collection it is possible, although unlikely, for
 * conditions to change such that incremental collection is no longer safe. In
 * this case, the collection is 'reset' by ResetIncrementalGC(). If we are in
 * the mark state, this just stops marking, but if we have started sweeping
 * already, we continue until we have swept the current zone group. Following a
 * reset, a new non-incremental collection is started.
 *
 * Compacting GC
 * -------------
 *
 * Compacting GC happens at the end of a major GC as part of the last slice.
 * There are three parts:
 *
 *  - Arenas are selected for compaction.
 *  - The contents of those arenas are moved to new arenas.
 *  - All references to moved things are updated.
 */

#include "jsgcinlines.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/MacroForEach.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Move.h"

#include <ctype.h>
#include <string.h>
#ifndef XP_WIN
# include <sys/mman.h>
# include <unistd.h>
#endif

#include "jsapi.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jscompartment.h"
#include "jsfriendapi.h"
#include "jsobj.h"
#include "jsprf.h"
#include "jsscript.h"
#include "jstypes.h"
#include "jsutil.h"
#include "jswatchpoint.h"
#include "jsweakmap.h"
#ifdef XP_WIN
# include "jswin.h"
#endif

#include "gc/FindSCCs.h"
#include "gc/GCInternals.h"
#include "gc/GCTrace.h"
#include "gc/Marking.h"
#include "gc/Memory.h"
#include "jit/BaselineJIT.h"
#include "jit/IonCode.h"
#include "jit/JitcodeMap.h"
#include "js/SliceBudget.h"
#include "proxy/DeadObjectProxy.h"
#include "vm/Debugger.h"
#include "vm/ProxyObject.h"
#include "vm/Shape.h"
#include "vm/String.h"
#include "vm/Symbol.h"
#include "vm/Time.h"
#include "vm/TraceLogging.h"
#include "vm/WrapperObject.h"

#include "jsobjinlines.h"
#include "jsscriptinlines.h"

#include "vm/Stack-inl.h"
#include "vm/String-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::ArrayLength;
using mozilla::Maybe;
using mozilla::Swap;

using JS::AutoGCRooter;

/* Perform a Full GC every 20 seconds if MaybeGC is called */
static const uint64_t GC_IDLE_FULL_SPAN = 20 * 1000 * 1000;

/* Increase the IGC marking slice time if we are in highFrequencyGC mode. */
static const int IGC_MARK_SLICE_MULTIPLIER = 2;

const AllocKind gc::slotsToThingKind[] = {
    /*  0 */ AllocKind::OBJECT0,  AllocKind::OBJECT2,  AllocKind::OBJECT2,  AllocKind::OBJECT4,
    /*  4 */ AllocKind::OBJECT4,  AllocKind::OBJECT8,  AllocKind::OBJECT8,  AllocKind::OBJECT8,
    /*  8 */ AllocKind::OBJECT8,  AllocKind::OBJECT12, AllocKind::OBJECT12, AllocKind::OBJECT12,
    /* 12 */ AllocKind::OBJECT12, AllocKind::OBJECT16, AllocKind::OBJECT16, AllocKind::OBJECT16,
    /* 16 */ AllocKind::OBJECT16
};

static_assert(JS_ARRAY_LENGTH(slotsToThingKind) == SLOTS_TO_THING_KIND_LIMIT,
              "We have defined a slot count for each kind.");

// Assert that SortedArenaList::MinThingSize is <= the real minimum thing size.
#define CHECK_MIN_THING_SIZE_INNER(x_)                                         \
    static_assert(x_ >= SortedArenaList::MinThingSize,                         \
    #x_ " is less than SortedArenaList::MinThingSize!");
#define CHECK_MIN_THING_SIZE(...) { __VA_ARGS__ }; /* Define the array. */     \
    MOZ_FOR_EACH(CHECK_MIN_THING_SIZE_INNER, (), (__VA_ARGS__ UINT32_MAX))

const uint32_t Arena::ThingSizes[] = CHECK_MIN_THING_SIZE(
    sizeof(JSFunction),         /* AllocKind::FUNCTION            */
    sizeof(FunctionExtended),   /* AllocKind::FUNCTION_EXTENDED   */
    sizeof(JSObject_Slots0),    /* AllocKind::OBJECT0             */
    sizeof(JSObject_Slots0),    /* AllocKind::OBJECT0_BACKGROUND  */
    sizeof(JSObject_Slots2),    /* AllocKind::OBJECT2             */
    sizeof(JSObject_Slots2),    /* AllocKind::OBJECT2_BACKGROUND  */
    sizeof(JSObject_Slots4),    /* AllocKind::OBJECT4             */
    sizeof(JSObject_Slots4),    /* AllocKind::OBJECT4_BACKGROUND  */
    sizeof(JSObject_Slots8),    /* AllocKind::OBJECT8             */
    sizeof(JSObject_Slots8),    /* AllocKind::OBJECT8_BACKGROUND  */
    sizeof(JSObject_Slots12),   /* AllocKind::OBJECT12            */
    sizeof(JSObject_Slots12),   /* AllocKind::OBJECT12_BACKGROUND */
    sizeof(JSObject_Slots16),   /* AllocKind::OBJECT16            */
    sizeof(JSObject_Slots16),   /* AllocKind::OBJECT16_BACKGROUND */
    sizeof(JSScript),           /* AllocKind::SCRIPT              */
    sizeof(LazyScript),         /* AllocKind::LAZY_SCRIPT         */
    sizeof(Shape),              /* AllocKind::SHAPE               */
    sizeof(AccessorShape),      /* AllocKind::ACCESSOR_SHAPE      */
    sizeof(BaseShape),          /* AllocKind::BASE_SHAPE          */
    sizeof(ObjectGroup),        /* AllocKind::OBJECT_GROUP        */
    sizeof(JSFatInlineString),  /* AllocKind::FAT_INLINE_STRING   */
    sizeof(JSString),           /* AllocKind::STRING              */
    sizeof(JSExternalString),   /* AllocKind::EXTERNAL_STRING     */
    sizeof(JS::Symbol),         /* AllocKind::SYMBOL              */
    sizeof(jit::JitCode),       /* AllocKind::JITCODE             */
);

#undef CHECK_MIN_THING_SIZE_INNER
#undef CHECK_MIN_THING_SIZE

#define OFFSET(type) uint32_t(sizeof(ArenaHeader) + (ArenaSize - sizeof(ArenaHeader)) % sizeof(type))

const uint32_t Arena::FirstThingOffsets[] = {
    OFFSET(JSFunction),         /* AllocKind::FUNCTION            */
    OFFSET(FunctionExtended),   /* AllocKind::FUNCTION_EXTENDED   */
    OFFSET(JSObject_Slots0),    /* AllocKind::OBJECT0             */
    OFFSET(JSObject_Slots0),    /* AllocKind::OBJECT0_BACKGROUND  */
    OFFSET(JSObject_Slots2),    /* AllocKind::OBJECT2             */
    OFFSET(JSObject_Slots2),    /* AllocKind::OBJECT2_BACKGROUND  */
    OFFSET(JSObject_Slots4),    /* AllocKind::OBJECT4             */
    OFFSET(JSObject_Slots4),    /* AllocKind::OBJECT4_BACKGROUND  */
    OFFSET(JSObject_Slots8),    /* AllocKind::OBJECT8             */
    OFFSET(JSObject_Slots8),    /* AllocKind::OBJECT8_BACKGROUND  */
    OFFSET(JSObject_Slots12),   /* AllocKind::OBJECT12            */
    OFFSET(JSObject_Slots12),   /* AllocKind::OBJECT12_BACKGROUND */
    OFFSET(JSObject_Slots16),   /* AllocKind::OBJECT16            */
    OFFSET(JSObject_Slots16),   /* AllocKind::OBJECT16_BACKGROUND */
    OFFSET(JSScript),           /* AllocKind::SCRIPT              */
    OFFSET(LazyScript),         /* AllocKind::LAZY_SCRIPT         */
    OFFSET(Shape),              /* AllocKind::SHAPE               */
    OFFSET(AccessorShape),      /* AllocKind::ACCESSOR_SHAPE      */
    OFFSET(BaseShape),          /* AllocKind::BASE_SHAPE          */
    OFFSET(ObjectGroup),        /* AllocKind::OBJECT_GROUP        */
    OFFSET(JSFatInlineString),  /* AllocKind::FAT_INLINE_STRING   */
    OFFSET(JSString),           /* AllocKind::STRING              */
    OFFSET(JSExternalString),   /* AllocKind::EXTERNAL_STRING     */
    OFFSET(JS::Symbol),         /* AllocKind::SYMBOL              */
    OFFSET(jit::JitCode),       /* AllocKind::JITCODE             */
};

#undef OFFSET

struct js::gc::FinalizePhase
{
    size_t length;
    const AllocKind* kinds;
    gcstats::Phase statsPhase;
};

#define PHASE(x, p) { ArrayLength(x), x, p }

/*
 * Finalization order for incrementally swept things.
 */

static const AllocKind IncrementalPhaseStrings[] = {
    AllocKind::EXTERNAL_STRING
};

static const AllocKind IncrementalPhaseScripts[] = {
    AllocKind::SCRIPT,
    AllocKind::LAZY_SCRIPT
};

static const AllocKind IncrementalPhaseJitCode[] = {
    AllocKind::JITCODE
};

static const FinalizePhase IncrementalFinalizePhases[] = {
    PHASE(IncrementalPhaseStrings, gcstats::PHASE_SWEEP_STRING),
    PHASE(IncrementalPhaseScripts, gcstats::PHASE_SWEEP_SCRIPT),
    PHASE(IncrementalPhaseJitCode, gcstats::PHASE_SWEEP_JITCODE)
};

/*
 * Finalization order for things swept in the background.
 */

static const AllocKind BackgroundPhaseObjects[] = {
    AllocKind::FUNCTION,
    AllocKind::FUNCTION_EXTENDED,
    AllocKind::OBJECT0_BACKGROUND,
    AllocKind::OBJECT2_BACKGROUND,
    AllocKind::OBJECT4_BACKGROUND,
    AllocKind::OBJECT8_BACKGROUND,
    AllocKind::OBJECT12_BACKGROUND,
    AllocKind::OBJECT16_BACKGROUND
};

static const AllocKind BackgroundPhaseStringsAndSymbols[] = {
    AllocKind::FAT_INLINE_STRING,
    AllocKind::STRING,
    AllocKind::SYMBOL
};

static const AllocKind BackgroundPhaseShapes[] = {
    AllocKind::SHAPE,
    AllocKind::ACCESSOR_SHAPE,
    AllocKind::BASE_SHAPE,
    AllocKind::OBJECT_GROUP
};

static const FinalizePhase BackgroundFinalizePhases[] = {
    PHASE(BackgroundPhaseObjects, gcstats::PHASE_SWEEP_OBJECT),
    PHASE(BackgroundPhaseStringsAndSymbols, gcstats::PHASE_SWEEP_STRING),
    PHASE(BackgroundPhaseShapes, gcstats::PHASE_SWEEP_SHAPE)
};

#undef PHASE

template<>
JSObject*
ArenaCellIterImpl::get<JSObject>() const
{
    MOZ_ASSERT(!done());
    return reinterpret_cast<JSObject*>(getCell());
}

#ifdef DEBUG
void
ArenaHeader::checkSynchronizedWithFreeList() const
{
    /*
     * Do not allow to access the free list when its real head is still stored
     * in FreeLists and is not synchronized with this one.
     */
    MOZ_ASSERT(allocated());

    /*
     * We can be called from the background finalization thread when the free
     * list in the zone can mutate at any moment. We cannot do any
     * checks in this case.
     */
    if (IsBackgroundFinalized(getAllocKind()) && zone->runtimeFromAnyThread()->gc.onBackgroundThread())
        return;

    FreeSpan firstSpan = firstFreeSpan.decompact(arenaAddress());
    if (firstSpan.isEmpty())
        return;
    const FreeList* freeList = zone->arenas.getFreeList(getAllocKind());
    if (freeList->isEmpty() || firstSpan.arenaAddress() != freeList->arenaAddress())
        return;

    /*
     * Here this arena has free things, FreeList::lists[thingKind] is not
     * empty and also points to this arena. Thus they must be the same.
     */
    MOZ_ASSERT(freeList->isSameNonEmptySpan(firstSpan));
}
#endif

void
ArenaHeader::unmarkAll()
{
    uintptr_t* word = chunk()->bitmap.arenaBits(this);
    memset(word, 0, ArenaBitmapWords * sizeof(uintptr_t));
}

/* static */ void
Arena::staticAsserts()
{
    static_assert(JS_ARRAY_LENGTH(ThingSizes) == size_t(AllocKind::LIMIT),
        "We haven't defined all thing sizes.");
    static_assert(JS_ARRAY_LENGTH(FirstThingOffsets) == size_t(AllocKind::LIMIT),
        "We haven't defined all offsets.");
}

void
Arena::setAsFullyUnused(AllocKind thingKind)
{
    FreeSpan fullSpan;
    size_t thingSize = Arena::thingSize(thingKind);
    fullSpan.initFinal(thingsStart(thingKind), thingsEnd() - thingSize, thingSize);
    aheader.setFirstFreeSpan(&fullSpan);
}

template<typename T>
inline size_t
Arena::finalize(FreeOp* fop, AllocKind thingKind, size_t thingSize)
{
    /* Enforce requirements on size of T. */
    MOZ_ASSERT(thingSize % CellSize == 0);
    MOZ_ASSERT(thingSize <= 255);

    MOZ_ASSERT(aheader.allocated());
    MOZ_ASSERT(thingKind == aheader.getAllocKind());
    MOZ_ASSERT(thingSize == aheader.getThingSize());
    MOZ_ASSERT(!aheader.hasDelayedMarking);
    MOZ_ASSERT(!aheader.markOverflow);
    MOZ_ASSERT(!aheader.allocatedDuringIncremental);

    uintptr_t firstThing = thingsStart(thingKind);
    uintptr_t firstThingOrSuccessorOfLastMarkedThing = firstThing;
    uintptr_t lastThing = thingsEnd() - thingSize;

    FreeSpan newListHead;
    FreeSpan* newListTail = &newListHead;
    size_t nmarked = 0;

    if (MOZ_UNLIKELY(MemProfiler::enabled())) {
        for (ArenaCellIterUnderFinalize i(&aheader); !i.done(); i.next()) {
            T* t = i.get<T>();
            if (t->asTenured().isMarked())
                MemProfiler::MarkTenured(reinterpret_cast<void*>(t));
        }
    }

    for (ArenaCellIterUnderFinalize i(&aheader); !i.done(); i.next()) {
        T* t = i.get<T>();
        if (t->asTenured().isMarked()) {
            uintptr_t thing = reinterpret_cast<uintptr_t>(t);
            if (thing != firstThingOrSuccessorOfLastMarkedThing) {
                // We just finished passing over one or more free things,
                // so record a new FreeSpan.
                newListTail->initBoundsUnchecked(firstThingOrSuccessorOfLastMarkedThing,
                                                 thing - thingSize);
                newListTail = newListTail->nextSpanUnchecked();
            }
            firstThingOrSuccessorOfLastMarkedThing = thing + thingSize;
            nmarked++;
        } else {
            t->finalize(fop);
            JS_POISON(t, JS_SWEPT_TENURED_PATTERN, thingSize);
            TraceTenuredFinalize(t);
        }
    }

    if (nmarked == 0) {
        // Do nothing. The caller will update the arena header appropriately.
        MOZ_ASSERT(newListTail == &newListHead);
        JS_EXTRA_POISON(data, JS_SWEPT_TENURED_PATTERN, sizeof(data));
        return nmarked;
    }

    MOZ_ASSERT(firstThingOrSuccessorOfLastMarkedThing != firstThing);
    uintptr_t lastMarkedThing = firstThingOrSuccessorOfLastMarkedThing - thingSize;
    if (lastThing == lastMarkedThing) {
        // If the last thing was marked, we will have already set the bounds of
        // the final span, and we just need to terminate the list.
        newListTail->initAsEmpty();
    } else {
        // Otherwise, end the list with a span that covers the final stretch of free things.
        newListTail->initFinal(firstThingOrSuccessorOfLastMarkedThing, lastThing, thingSize);
    }

#ifdef DEBUG
    size_t nfree = 0;
    for (const FreeSpan* span = &newListHead; !span->isEmpty(); span = span->nextSpan())
        nfree += span->length(thingSize);
    MOZ_ASSERT(nfree + nmarked == thingsPerArena(thingSize));
#endif
    aheader.setFirstFreeSpan(&newListHead);
    return nmarked;
}

// Finalize arenas from src list, releasing empty arenas if keepArenas wasn't
// specified and inserting the others into the appropriate destination size
// bins.
template<typename T>
static inline bool
FinalizeTypedArenas(FreeOp* fop,
                    ArenaHeader** src,
                    SortedArenaList& dest,
                    AllocKind thingKind,
                    SliceBudget& budget,
                    ArenaLists::KeepArenasEnum keepArenas)
{
    // When operating in the foreground, take the lock at the top.
    Maybe<AutoLockGC> maybeLock;
    if (!fop->onBackgroundThread())
        maybeLock.emplace(fop->runtime());

    // During background sweeping free arenas are released later on in
    // sweepBackgroundThings().
    MOZ_ASSERT_IF(fop->onBackgroundThread(), keepArenas == ArenaLists::KEEP_ARENAS);

    size_t thingSize = Arena::thingSize(thingKind);
    size_t thingsPerArena = Arena::thingsPerArena(thingSize);

    while (ArenaHeader* aheader = *src) {
        *src = aheader->next;
        size_t nmarked = aheader->getArena()->finalize<T>(fop, thingKind, thingSize);
        size_t nfree = thingsPerArena - nmarked;

        if (nmarked)
            dest.insertAt(aheader, nfree);
        else if (keepArenas == ArenaLists::KEEP_ARENAS)
            aheader->chunk()->recycleArena(aheader, dest, thingKind, thingsPerArena);
        else
            fop->runtime()->gc.releaseArena(aheader, maybeLock.ref());

        budget.step(thingsPerArena);
        if (budget.isOverBudget())
            return false;
    }

    return true;
}

/*
 * Finalize the list. On return, |al|'s cursor points to the first non-empty
 * arena in the list (which may be null if all arenas are full).
 */
static bool
FinalizeArenas(FreeOp* fop,
               ArenaHeader** src,
               SortedArenaList& dest,
               AllocKind thingKind,
               SliceBudget& budget,
               ArenaLists::KeepArenasEnum keepArenas)
{
    switch (thingKind) {
      case AllocKind::FUNCTION:
      case AllocKind::FUNCTION_EXTENDED:
      case AllocKind::OBJECT0:
      case AllocKind::OBJECT0_BACKGROUND:
      case AllocKind::OBJECT2:
      case AllocKind::OBJECT2_BACKGROUND:
      case AllocKind::OBJECT4:
      case AllocKind::OBJECT4_BACKGROUND:
      case AllocKind::OBJECT8:
      case AllocKind::OBJECT8_BACKGROUND:
      case AllocKind::OBJECT12:
      case AllocKind::OBJECT12_BACKGROUND:
      case AllocKind::OBJECT16:
      case AllocKind::OBJECT16_BACKGROUND:
        return FinalizeTypedArenas<JSObject>(fop, src, dest, thingKind, budget, keepArenas);
      case AllocKind::SCRIPT:
        return FinalizeTypedArenas<JSScript>(fop, src, dest, thingKind, budget, keepArenas);
      case AllocKind::LAZY_SCRIPT:
        return FinalizeTypedArenas<LazyScript>(fop, src, dest, thingKind, budget, keepArenas);
      case AllocKind::SHAPE:
        return FinalizeTypedArenas<Shape>(fop, src, dest, thingKind, budget, keepArenas);
      case AllocKind::ACCESSOR_SHAPE:
        return FinalizeTypedArenas<AccessorShape>(fop, src, dest, thingKind, budget, keepArenas);
      case AllocKind::BASE_SHAPE:
        return FinalizeTypedArenas<BaseShape>(fop, src, dest, thingKind, budget, keepArenas);
      case AllocKind::OBJECT_GROUP:
        return FinalizeTypedArenas<ObjectGroup>(fop, src, dest, thingKind, budget, keepArenas);
      case AllocKind::STRING:
        return FinalizeTypedArenas<JSString>(fop, src, dest, thingKind, budget, keepArenas);
      case AllocKind::FAT_INLINE_STRING:
        return FinalizeTypedArenas<JSFatInlineString>(fop, src, dest, thingKind, budget, keepArenas);
      case AllocKind::EXTERNAL_STRING:
        return FinalizeTypedArenas<JSExternalString>(fop, src, dest, thingKind, budget, keepArenas);
      case AllocKind::SYMBOL:
        return FinalizeTypedArenas<JS::Symbol>(fop, src, dest, thingKind, budget, keepArenas);
      case AllocKind::JITCODE:
        return FinalizeTypedArenas<jit::JitCode>(fop, src, dest, thingKind, budget, keepArenas);
      default:
        MOZ_CRASH("Invalid alloc kind");
    }
}

Chunk*
ChunkPool::pop()
{
    MOZ_ASSERT(bool(head_) == bool(count_));
    if (!count_)
        return nullptr;
    return remove(head_);
}

void
ChunkPool::push(Chunk* chunk)
{
    MOZ_ASSERT(!chunk->info.next);
    MOZ_ASSERT(!chunk->info.prev);

    chunk->info.age = 0;
    chunk->info.next = head_;
    if (head_)
        head_->info.prev = chunk;
    head_ = chunk;
    ++count_;

    MOZ_ASSERT(verify());
}

Chunk*
ChunkPool::remove(Chunk* chunk)
{
    MOZ_ASSERT(count_ > 0);
    MOZ_ASSERT(contains(chunk));

    if (head_ == chunk)
        head_ = chunk->info.next;
    if (chunk->info.prev)
        chunk->info.prev->info.next = chunk->info.next;
    if (chunk->info.next)
        chunk->info.next->info.prev = chunk->info.prev;
    chunk->info.next = chunk->info.prev = nullptr;
    --count_;

    MOZ_ASSERT(verify());
    return chunk;
}

#ifdef DEBUG
bool
ChunkPool::contains(Chunk* chunk) const
{
    verify();
    for (Chunk* cursor = head_; cursor; cursor = cursor->info.next) {
        if (cursor == chunk)
            return true;
    }
    return false;
}

bool
ChunkPool::verify() const
{
    MOZ_ASSERT(bool(head_) == bool(count_));
    uint32_t count = 0;
    for (Chunk* cursor = head_; cursor; cursor = cursor->info.next, ++count) {
        MOZ_ASSERT_IF(cursor->info.prev, cursor->info.prev->info.next == cursor);
        MOZ_ASSERT_IF(cursor->info.next, cursor->info.next->info.prev == cursor);
    }
    MOZ_ASSERT(count_ == count);
    return true;
}
#endif

void
ChunkPool::Iter::next()
{
    MOZ_ASSERT(!done());
    current_ = current_->info.next;
}

ChunkPool
GCRuntime::expireEmptyChunkPool(bool shrinkBuffers, const AutoLockGC& lock)
{
    /*
     * Return old empty chunks to the system while preserving the order of
     * other chunks in the list. This way, if the GC runs several times
     * without emptying the list, the older chunks will stay at the tail
     * and are more likely to reach the max age.
     */
    MOZ_ASSERT(emptyChunks(lock).verify());
    ChunkPool expired;
    unsigned freeChunkCount = 0;
    for (ChunkPool::Iter iter(emptyChunks(lock)); !iter.done();) {
        Chunk* chunk = iter.get();
        iter.next();

        MOZ_ASSERT(chunk->unused());
        MOZ_ASSERT(!fullChunks(lock).contains(chunk));
        MOZ_ASSERT(!availableChunks(lock).contains(chunk));
        if (freeChunkCount >= tunables.maxEmptyChunkCount() ||
            (freeChunkCount >= tunables.minEmptyChunkCount(lock) &&
             (shrinkBuffers || chunk->info.age == MAX_EMPTY_CHUNK_AGE)))
        {
            emptyChunks(lock).remove(chunk);
            prepareToFreeChunk(chunk->info);
            expired.push(chunk);
        } else {
            /* Keep the chunk but increase its age. */
            ++freeChunkCount;
            ++chunk->info.age;
        }
    }
    MOZ_ASSERT(expired.verify());
    MOZ_ASSERT(emptyChunks(lock).verify());
    MOZ_ASSERT(emptyChunks(lock).count() <= tunables.maxEmptyChunkCount());
    MOZ_ASSERT_IF(shrinkBuffers, emptyChunks(lock).count() <= tunables.minEmptyChunkCount(lock));
    return expired;
}

static void
FreeChunkPool(JSRuntime* rt, ChunkPool& pool)
{
    for (ChunkPool::Iter iter(pool); !iter.done();) {
        Chunk* chunk = iter.get();
        iter.next();
        pool.remove(chunk);
        MOZ_ASSERT(!chunk->info.numArenasFreeCommitted);
        UnmapPages(static_cast<void*>(chunk), ChunkSize);
    }
    MOZ_ASSERT(pool.count() == 0);
}

void
GCRuntime::freeEmptyChunks(JSRuntime* rt, const AutoLockGC& lock)
{
    FreeChunkPool(rt, emptyChunks(lock));
}

/* static */ Chunk*
Chunk::allocate(JSRuntime* rt)
{
    Chunk* chunk = static_cast<Chunk*>(MapAlignedPages(ChunkSize, ChunkSize));
    if (!chunk)
        return nullptr;
    chunk->init(rt);
    rt->gc.stats.count(gcstats::STAT_NEW_CHUNK);
    return chunk;
}

inline void
GCRuntime::prepareToFreeChunk(ChunkInfo& info)
{
    MOZ_ASSERT(numArenasFreeCommitted >= info.numArenasFreeCommitted);
    numArenasFreeCommitted -= info.numArenasFreeCommitted;
    stats.count(gcstats::STAT_DESTROY_CHUNK);
#ifdef DEBUG
    /*
     * Let FreeChunkPool detect a missing prepareToFreeChunk call before it
     * frees chunk.
     */
    info.numArenasFreeCommitted = 0;
#endif
}

void Chunk::decommitAllArenas(JSRuntime* rt)
{
    decommittedArenas.clear(true);
    MarkPagesUnused(&arenas[0], ArenasPerChunk * ArenaSize);

    info.freeArenasHead = nullptr;
    info.lastDecommittedArenaOffset = 0;
    info.numArenasFree = ArenasPerChunk;
    info.numArenasFreeCommitted = 0;
}

void
Chunk::init(JSRuntime* rt)
{
    JS_POISON(this, JS_FRESH_TENURED_PATTERN, ChunkSize);

    /*
     * We clear the bitmap to guard against JS::GCThingIsMarkedGray being called
     * on uninitialized data, which would happen before the first GC cycle.
     */
    bitmap.clear();

    /*
     * Decommit the arenas. We do this after poisoning so that if the OS does
     * not have to recycle the pages, we still get the benefit of poisoning.
     */
    decommitAllArenas(rt);

    /* Initialize the chunk info. */
    info.init();
    new (&info.trailer) ChunkTrailer(rt);

    /* The rest of info fields are initialized in pickChunk. */
}

/*
 * Search for and return the next decommitted Arena. Our goal is to keep
 * lastDecommittedArenaOffset "close" to a free arena. We do this by setting
 * it to the most recently freed arena when we free, and forcing it to
 * the last alloc + 1 when we allocate.
 */
uint32_t
Chunk::findDecommittedArenaOffset()
{
    /* Note: lastFreeArenaOffset can be past the end of the list. */
    for (unsigned i = info.lastDecommittedArenaOffset; i < ArenasPerChunk; i++)
        if (decommittedArenas.get(i))
            return i;
    for (unsigned i = 0; i < info.lastDecommittedArenaOffset; i++)
        if (decommittedArenas.get(i))
            return i;
    MOZ_CRASH("No decommitted arenas found.");
}

ArenaHeader*
Chunk::fetchNextDecommittedArena()
{
    MOZ_ASSERT(info.numArenasFreeCommitted == 0);
    MOZ_ASSERT(info.numArenasFree > 0);

    unsigned offset = findDecommittedArenaOffset();
    info.lastDecommittedArenaOffset = offset + 1;
    --info.numArenasFree;
    decommittedArenas.unset(offset);

    Arena* arena = &arenas[offset];
    MarkPagesInUse(arena, ArenaSize);
    arena->aheader.setAsNotAllocated();

    return &arena->aheader;
}

inline void
GCRuntime::updateOnFreeArenaAlloc(const ChunkInfo& info)
{
    MOZ_ASSERT(info.numArenasFreeCommitted <= numArenasFreeCommitted);
    --numArenasFreeCommitted;
}

inline ArenaHeader*
Chunk::fetchNextFreeArena(JSRuntime* rt)
{
    MOZ_ASSERT(info.numArenasFreeCommitted > 0);
    MOZ_ASSERT(info.numArenasFreeCommitted <= info.numArenasFree);

    ArenaHeader* aheader = info.freeArenasHead;
    info.freeArenasHead = aheader->next;
    --info.numArenasFreeCommitted;
    --info.numArenasFree;
    rt->gc.updateOnFreeArenaAlloc(info);

    return aheader;
}

ArenaHeader*
Chunk::allocateArena(JSRuntime* rt, Zone* zone, AllocKind thingKind, const AutoLockGC& lock)
{
    ArenaHeader* aheader = info.numArenasFreeCommitted > 0
                           ? fetchNextFreeArena(rt)
                           : fetchNextDecommittedArena();
    aheader->init(zone, thingKind);
    updateChunkListAfterAlloc(rt, lock);
    return aheader;
}

inline void
GCRuntime::updateOnArenaFree(const ChunkInfo& info)
{
    ++numArenasFreeCommitted;
}

void
Chunk::addArenaToFreeList(JSRuntime* rt, ArenaHeader* aheader)
{
    MOZ_ASSERT(!aheader->allocated());
    aheader->next = info.freeArenasHead;
    info.freeArenasHead = aheader;
    ++info.numArenasFreeCommitted;
    ++info.numArenasFree;
    rt->gc.updateOnArenaFree(info);
}

void
Chunk::addArenaToDecommittedList(JSRuntime* rt, const ArenaHeader* aheader)
{
    ++info.numArenasFree;
    decommittedArenas.set(Chunk::arenaIndex(aheader->arenaAddress()));
}

void
Chunk::recycleArena(ArenaHeader* aheader, SortedArenaList& dest, AllocKind thingKind,
                    size_t thingsPerArena)
{
    aheader->getArena()->setAsFullyUnused(thingKind);
    dest.insertAt(aheader, thingsPerArena);
}

void
Chunk::releaseArena(JSRuntime* rt, ArenaHeader* aheader, const AutoLockGC& lock)
{
    MOZ_ASSERT(aheader->allocated());
    MOZ_ASSERT(!aheader->hasDelayedMarking);

    aheader->setAsNotAllocated();
    addArenaToFreeList(rt, aheader);
    updateChunkListAfterFree(rt, lock);
}

bool
Chunk::decommitOneFreeArena(JSRuntime* rt, AutoLockGC& lock)
{
    MOZ_ASSERT(info.numArenasFreeCommitted > 0);
    ArenaHeader* aheader = fetchNextFreeArena(rt);
    updateChunkListAfterAlloc(rt, lock);

    bool ok;
    {
        AutoUnlockGC unlock(lock);
        ok = MarkPagesUnused(aheader->getArena(), ArenaSize);
    }

    if (ok)
        addArenaToDecommittedList(rt, aheader);
    else
        addArenaToFreeList(rt, aheader);
    updateChunkListAfterFree(rt, lock);

    return ok;
}

void
Chunk::decommitAllArenasWithoutUnlocking(const AutoLockGC& lock)
{
    for (size_t i = 0; i < ArenasPerChunk; ++i) {
        if (decommittedArenas.get(i) || arenas[i].aheader.allocated())
            continue;

        if (MarkPagesUnused(&arenas[i], ArenaSize)) {
            info.numArenasFreeCommitted--;
            decommittedArenas.set(i);
        }
    }
}

void
Chunk::updateChunkListAfterAlloc(JSRuntime* rt, const AutoLockGC& lock)
{
    if (MOZ_UNLIKELY(!hasAvailableArenas())) {
        rt->gc.availableChunks(lock).remove(this);
        rt->gc.fullChunks(lock).push(this);
    }
}

void
Chunk::updateChunkListAfterFree(JSRuntime* rt, const AutoLockGC& lock)
{
    if (info.numArenasFree == 1) {
        rt->gc.fullChunks(lock).remove(this);
        rt->gc.availableChunks(lock).push(this);
    } else if (!unused()) {
        MOZ_ASSERT(!rt->gc.fullChunks(lock).contains(this));
        MOZ_ASSERT(rt->gc.availableChunks(lock).contains(this));
        MOZ_ASSERT(!rt->gc.emptyChunks(lock).contains(this));
    } else {
        MOZ_ASSERT(unused());
        rt->gc.availableChunks(lock).remove(this);
        decommitAllArenas(rt);
        rt->gc.emptyChunks(lock).push(this);
    }
}

inline bool
GCRuntime::wantBackgroundAllocation(const AutoLockGC& lock) const
{
    // To minimize memory waste, we do not want to run the background chunk
    // allocation if we already have some empty chunks or when the runtime has
    // a small heap size (and therefore likely has a small growth rate).
    return allocTask.enabled() &&
           emptyChunks(lock).count() < tunables.minEmptyChunkCount(lock) &&
           (fullChunks(lock).count() + availableChunks(lock).count()) >= 4;
}

void
GCRuntime::startBackgroundAllocTaskIfIdle()
{
    AutoLockHelperThreadState helperLock;
    if (allocTask.isRunning())
        return;

    // Join the previous invocation of the task. This will return immediately
    // if the thread has never been started.
    allocTask.joinWithLockHeld();
    allocTask.startWithLockHeld();
}

Chunk*
GCRuntime::pickChunk(const AutoLockGC& lock,
                     AutoMaybeStartBackgroundAllocation& maybeStartBackgroundAllocation)
{
    if (availableChunks(lock).count())
        return availableChunks(lock).head();

    Chunk* chunk = emptyChunks(lock).pop();
    if (!chunk) {
        chunk = Chunk::allocate(rt);
        if (!chunk)
            return nullptr;
        MOZ_ASSERT(chunk->info.numArenasFreeCommitted == 0);
    }

    MOZ_ASSERT(chunk->unused());
    MOZ_ASSERT(!fullChunks(lock).contains(chunk));

    if (wantBackgroundAllocation(lock))
        maybeStartBackgroundAllocation.tryToStartBackgroundAllocation(rt);

    chunkAllocationSinceLastGC = true;

    availableChunks(lock).push(chunk);

    return chunk;
}

ArenaHeader*
GCRuntime::allocateArena(Chunk* chunk, Zone* zone, AllocKind thingKind, const AutoLockGC& lock)
{
    MOZ_ASSERT(chunk->hasAvailableArenas());

    // Fail the allocation if we are over our heap size limits.
    if (!rt->isHeapMinorCollecting() &&
        !isHeapCompacting() &&
        usage.gcBytes() >= tunables.gcMaxBytes())
    {
        return nullptr;
    }

    ArenaHeader* aheader = chunk->allocateArena(rt, zone, thingKind, lock);
    zone->usage.addGCArena();

    // Trigger an incremental slice if needed.
    if (!rt->isHeapMinorCollecting() && !isHeapCompacting())
        maybeAllocTriggerZoneGC(zone, lock);

    return aheader;
}

void
GCRuntime::releaseArena(ArenaHeader* aheader, const AutoLockGC& lock)
{
    aheader->zone->usage.removeGCArena();
    if (isBackgroundSweeping())
        aheader->zone->threshold.updateForRemovedArena(tunables);
    return aheader->chunk()->releaseArena(rt, aheader, lock);
}

GCRuntime::GCRuntime(JSRuntime* rt) :
    rt(rt),
    systemZone(nullptr),
    nursery(rt),
    storeBuffer(rt, nursery),
    stats(rt),
    marker(rt),
    usage(nullptr),
    mMemProfiler(rt),
    maxMallocBytes(0),
    nextCellUniqueId_(LargestTaggedNullCellPointer + 1), // Ensure disjoint from null tagged pointers.
    numArenasFreeCommitted(0),
    verifyPreData(nullptr),
    chunkAllocationSinceLastGC(false),
    nextFullGCTime(0),
    lastGCTime(PRMJ_Now()),
    mode(JSGC_MODE_INCREMENTAL),
    numActiveZoneIters(0),
    decommitThreshold(32 * 1024 * 1024),
    cleanUpEverything(false),
    grayBufferState(GCRuntime::GrayBufferState::Unused),
    grayBitsValid(false),
    majorGCTriggerReason(JS::gcreason::NO_REASON),
    minorGCTriggerReason(JS::gcreason::NO_REASON),
    fullGCForAtomsRequested_(false),
    minorGCNumber(0),
    majorGCNumber(0),
    jitReleaseNumber(0),
    number(0),
    startNumber(0),
    isFull(false),
#ifdef DEBUG
    disableStrictProxyCheckingCount(0),
#endif
    incrementalState(gc::NO_INCREMENTAL),
    lastMarkSlice(false),
    sweepOnBackgroundThread(false),
    foundBlackGrayEdges(false),
    freeLifoAlloc(JSRuntime::TEMP_LIFO_ALLOC_PRIMARY_CHUNK_SIZE),
    zoneGroupIndex(0),
    zoneGroups(nullptr),
    currentZoneGroup(nullptr),
    sweepZone(nullptr),
    sweepKindIndex(0),
    abortSweepAfterCurrentGroup(false),
    arenasAllocatedDuringSweep(nullptr),
    startedCompacting(false),
    relocatedArenasToRelease(nullptr),
#ifdef JS_GC_MARKING_VALIDATION
    markingValidator(nullptr),
#endif
    interFrameGC(false),
    defaultTimeBudget_(SliceBudget::UnlimitedTimeBudget),
    incrementalAllowed(true),
    generationalDisabled(0),
    compactingEnabled(true),
    compactingDisabledCount(0),
    manipulatingDeadZones(false),
    objectsMarkedInDeadZones(0),
    poked(false),
#ifdef JS_GC_ZEAL
    zealMode(0),
    zealFrequency(0),
    nextScheduled(0),
    deterministicOnly(false),
    incrementalLimit(0),
#endif
    validate(true),
    fullCompartmentChecks(false),
    mallocBytesUntilGC(0),
    mallocGCTriggered(false),
    alwaysPreserveCode(false),
#ifdef DEBUG
    inUnsafeRegion(0),
    noGCOrAllocationCheck(0),
#endif
    lock(nullptr),
    allocTask(rt, emptyChunks_),
    helperState(rt)
{
    setGCMode(JSGC_MODE_GLOBAL);
}

#ifdef JS_GC_ZEAL

void
GCRuntime::getZeal(uint8_t* zeal, uint32_t* frequency, uint32_t* scheduled)
{
    *zeal = zealMode;
    *frequency = zealFrequency;
    *scheduled = nextScheduled;
}

const char* gc::ZealModeHelpText =
    "  Specifies how zealous the garbage collector should be. Values for level:\n"
    "    0: Normal amount of collection\n"
    "    1: Collect when roots are added or removed\n"
    "    2: Collect when every N allocations (default: 100)\n"
    "    3: Collect when the window paints (browser only)\n"
    "    4: Verify pre write barriers between instructions\n"
    "    5: Verify pre write barriers between paints\n"
    "    6: Verify stack rooting\n"
    "    7: Collect the nursery every N nursery allocations\n"
    "    8: Incremental GC in two slices: 1) mark roots 2) finish collection\n"
    "    9: Incremental GC in two slices: 1) mark all 2) new marking and finish\n"
    "   10: Incremental GC in multiple slices\n"
    "   11: unused\n"
    "   12: unused\n"
    "   13: Check internal hashtables on minor GC\n"
    "   14: Perform a shrinking collection every N allocations\n";

void
GCRuntime::setZeal(uint8_t zeal, uint32_t frequency)
{
    if (verifyPreData)
        VerifyBarriers(rt, PreBarrierVerifier);

    if (zealMode == ZealGenerationalGCValue) {
        evictNursery(JS::gcreason::DEBUG_GC);
        nursery.leaveZealMode();
    }

    if (zeal == ZealGenerationalGCValue)
        nursery.enterZealMode();

    bool schedule = zeal >= js::gc::ZealAllocValue;
    zealMode = zeal;
    zealFrequency = frequency;
    nextScheduled = schedule ? frequency : 0;
}

void
GCRuntime::setNextScheduled(uint32_t count)
{
    nextScheduled = count;
}

bool
GCRuntime::parseAndSetZeal(const char* str)
{
    int zeal = -1;
    int frequency = -1;

    if (isdigit(str[0])) {
        zeal = atoi(str);

        const char* p = strchr(str, ',');
        if (!p)
            frequency = JS_DEFAULT_ZEAL_FREQ;
        else
            frequency = atoi(p + 1);
    }

    if (zeal < 0 || zeal > ZealLimit || frequency <= 0) {
        fprintf(stderr, "Format: JS_GC_ZEAL=level[,N]\n");
        fputs(ZealModeHelpText, stderr);
        return false;
    }

    setZeal(zeal, frequency);
    return true;
}

#endif

/*
 * Lifetime in number of major GCs for type sets attached to scripts containing
 * observed types.
 */
static const uint64_t JIT_SCRIPT_RELEASE_TYPES_PERIOD = 20;

bool
GCRuntime::init(uint32_t maxbytes, uint32_t maxNurseryBytes)
{
    InitMemorySubsystem();

    lock = PR_NewLock();
    if (!lock)
        return false;

    if (!rootsHash.init(256))
        return false;

    if (!helperState.init())
        return false;

    /*
     * Separate gcMaxMallocBytes from gcMaxBytes but initialize to maxbytes
     * for default backward API compatibility.
     */
    AutoLockGC lock(rt);
    tunables.setParameter(JSGC_MAX_BYTES, maxbytes, lock);
    setMaxMallocBytes(maxbytes);

    const char* size = getenv("JSGC_MARK_STACK_LIMIT");
    if (size)
        setMarkStackLimit(atoi(size), lock);

    jitReleaseNumber = majorGCNumber + JIT_SCRIPT_RELEASE_TYPES_PERIOD;

    if (!nursery.init(maxNurseryBytes))
        return false;

    if (!nursery.isEnabled()) {
        MOZ_ASSERT(nursery.nurserySize() == 0);
        ++rt->gc.generationalDisabled;
    } else {
        MOZ_ASSERT(nursery.nurserySize() > 0);
        if (!storeBuffer.enable())
            return false;
    }

#ifdef JS_GC_ZEAL
    const char* zealSpec = getenv("JS_GC_ZEAL");
    if (zealSpec && zealSpec[0] && !parseAndSetZeal(zealSpec))
        return false;
#endif

    if (!InitTrace(*this))
        return false;

    if (!marker.init(mode))
        return false;

    return true;
}

void
GCRuntime::finish()
{
    /* Wait for the nursery sweeping to end. */
    if (rt->gc.nursery.isEnabled())
        rt->gc.nursery.waitBackgroundFreeEnd();

    /*
     * Wait until the background finalization and allocation stops and the
     * helper thread shuts down before we forcefully release any remaining GC
     * memory.
     */
    helperState.finish();
    allocTask.cancel(GCParallelTask::CancelAndWait);

#ifdef JS_GC_ZEAL
    /* Free memory associated with GC verification. */
    finishVerifier();
#endif

    /* Delete all remaining zones. */
    if (rt->gcInitialized) {
        AutoSetThreadIsSweeping threadIsSweeping;
        for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
            for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next())
                js_delete(comp.get());
            js_delete(zone.get());
        }
    }

    zones.clear();

    FreeChunkPool(rt, fullChunks_);
    FreeChunkPool(rt, availableChunks_);
    FreeChunkPool(rt, emptyChunks_);

    if (lock) {
        PR_DestroyLock(lock);
        lock = nullptr;
    }

    FinishTrace();
}

template <typename T>
static void
FinishPersistentRootedChain(mozilla::LinkedList<PersistentRooted<T>>& list)
{
    while (!list.isEmpty())
        list.getFirst()->reset();
}

void
js::gc::FinishPersistentRootedChains(RootLists& roots)
{
    FinishPersistentRootedChain(roots.getPersistentRootedList<JSObject*>());
    FinishPersistentRootedChain(roots.getPersistentRootedList<JSScript*>());
    FinishPersistentRootedChain(roots.getPersistentRootedList<JSString*>());
    FinishPersistentRootedChain(roots.getPersistentRootedList<jsid>());
    FinishPersistentRootedChain(roots.getPersistentRootedList<Value>());
    FinishPersistentRootedChain(roots.heapRoots_[THING_ROOT_TRACEABLE]);
}

void
GCRuntime::finishRoots()
{
    if (rootsHash.initialized())
        rootsHash.clear();

    FinishPersistentRootedChains(rt->mainThread.roots);
}

void
GCRuntime::setParameter(JSGCParamKey key, uint32_t value, AutoLockGC& lock)
{
    switch (key) {
      case JSGC_MAX_MALLOC_BYTES:
        setMaxMallocBytes(value);
        for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next())
            zone->setGCMaxMallocBytes(maxMallocBytesAllocated() * 0.9);
        break;
      case JSGC_SLICE_TIME_BUDGET:
        defaultTimeBudget_ = value ? value : SliceBudget::UnlimitedTimeBudget;
        break;
      case JSGC_MARK_STACK_LIMIT:
        setMarkStackLimit(value, lock);
        break;
      case JSGC_DECOMMIT_THRESHOLD:
        decommitThreshold = value * 1024 * 1024;
        break;
      case JSGC_MODE:
        mode = JSGCMode(value);
        MOZ_ASSERT(mode == JSGC_MODE_GLOBAL ||
                   mode == JSGC_MODE_COMPARTMENT ||
                   mode == JSGC_MODE_INCREMENTAL);
        break;
      case JSGC_COMPACTING_ENABLED:
        compactingEnabled = value != 0;
        break;
      default:
        tunables.setParameter(key, value, lock);
        for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
            zone->threshold.updateAfterGC(zone->usage.gcBytes(), GC_NORMAL, tunables,
                                          schedulingState, lock);
        }
    }
}

void
GCSchedulingTunables::setParameter(JSGCParamKey key, uint32_t value, const AutoLockGC& lock)
{
    switch(key) {
      case JSGC_MAX_BYTES:
        gcMaxBytes_ = value;
        break;
      case JSGC_HIGH_FREQUENCY_TIME_LIMIT:
        highFrequencyThresholdUsec_ = value * PRMJ_USEC_PER_MSEC;
        break;
      case JSGC_HIGH_FREQUENCY_LOW_LIMIT:
        highFrequencyLowLimitBytes_ = value * 1024 * 1024;
        if (highFrequencyLowLimitBytes_ >= highFrequencyHighLimitBytes_)
            highFrequencyHighLimitBytes_ = highFrequencyLowLimitBytes_ + 1;
        MOZ_ASSERT(highFrequencyHighLimitBytes_ > highFrequencyLowLimitBytes_);
        break;
      case JSGC_HIGH_FREQUENCY_HIGH_LIMIT:
        MOZ_ASSERT(value > 0);
        highFrequencyHighLimitBytes_ = value * 1024 * 1024;
        if (highFrequencyHighLimitBytes_ <= highFrequencyLowLimitBytes_)
            highFrequencyLowLimitBytes_ = highFrequencyHighLimitBytes_ - 1;
        MOZ_ASSERT(highFrequencyHighLimitBytes_ > highFrequencyLowLimitBytes_);
        break;
      case JSGC_HIGH_FREQUENCY_HEAP_GROWTH_MAX:
        highFrequencyHeapGrowthMax_ = value / 100.0;
        MOZ_ASSERT(highFrequencyHeapGrowthMax_ / 0.85 > 1.0);
        break;
      case JSGC_HIGH_FREQUENCY_HEAP_GROWTH_MIN:
        highFrequencyHeapGrowthMin_ = value / 100.0;
        MOZ_ASSERT(highFrequencyHeapGrowthMin_ / 0.85 > 1.0);
        break;
      case JSGC_LOW_FREQUENCY_HEAP_GROWTH:
        lowFrequencyHeapGrowth_ = value / 100.0;
        MOZ_ASSERT(lowFrequencyHeapGrowth_ / 0.9 > 1.0);
        break;
      case JSGC_DYNAMIC_HEAP_GROWTH:
        dynamicHeapGrowthEnabled_ = value != 0;
        break;
      case JSGC_DYNAMIC_MARK_SLICE:
        dynamicMarkSliceEnabled_ = value != 0;
        break;
      case JSGC_ALLOCATION_THRESHOLD:
        gcZoneAllocThresholdBase_ = value * 1024 * 1024;
        break;
      case JSGC_MIN_EMPTY_CHUNK_COUNT:
        minEmptyChunkCount_ = value;
        if (minEmptyChunkCount_ > maxEmptyChunkCount_)
            maxEmptyChunkCount_ = minEmptyChunkCount_;
        MOZ_ASSERT(maxEmptyChunkCount_ >= minEmptyChunkCount_);
        break;
      case JSGC_MAX_EMPTY_CHUNK_COUNT:
        maxEmptyChunkCount_ = value;
        if (minEmptyChunkCount_ > maxEmptyChunkCount_)
            minEmptyChunkCount_ = maxEmptyChunkCount_;
        MOZ_ASSERT(maxEmptyChunkCount_ >= minEmptyChunkCount_);
        break;
      default:
        MOZ_CRASH("Unknown GC parameter.");
    }
}

uint32_t
GCRuntime::getParameter(JSGCParamKey key, const AutoLockGC& lock)
{
    switch (key) {
      case JSGC_MAX_BYTES:
        return uint32_t(tunables.gcMaxBytes());
      case JSGC_MAX_MALLOC_BYTES:
        return maxMallocBytes;
      case JSGC_BYTES:
        return uint32_t(usage.gcBytes());
      case JSGC_MODE:
        return uint32_t(mode);
      case JSGC_UNUSED_CHUNKS:
        return uint32_t(emptyChunks(lock).count());
      case JSGC_TOTAL_CHUNKS:
        return uint32_t(fullChunks(lock).count() +
                        availableChunks(lock).count() +
                        emptyChunks(lock).count());
      case JSGC_SLICE_TIME_BUDGET:
        if (defaultTimeBudget_ == SliceBudget::UnlimitedTimeBudget) {
            return 0;
        } else {
            MOZ_RELEASE_ASSERT(defaultTimeBudget_ >= 0);
            MOZ_RELEASE_ASSERT(defaultTimeBudget_ < UINT32_MAX);
            return uint32_t(defaultTimeBudget_);
        }
      case JSGC_MARK_STACK_LIMIT:
        return marker.maxCapacity();
      case JSGC_HIGH_FREQUENCY_TIME_LIMIT:
        return tunables.highFrequencyThresholdUsec();
      case JSGC_HIGH_FREQUENCY_LOW_LIMIT:
        return tunables.highFrequencyLowLimitBytes() / 1024 / 1024;
      case JSGC_HIGH_FREQUENCY_HIGH_LIMIT:
        return tunables.highFrequencyHighLimitBytes() / 1024 / 1024;
      case JSGC_HIGH_FREQUENCY_HEAP_GROWTH_MAX:
        return uint32_t(tunables.highFrequencyHeapGrowthMax() * 100);
      case JSGC_HIGH_FREQUENCY_HEAP_GROWTH_MIN:
        return uint32_t(tunables.highFrequencyHeapGrowthMin() * 100);
      case JSGC_LOW_FREQUENCY_HEAP_GROWTH:
        return uint32_t(tunables.lowFrequencyHeapGrowth() * 100);
      case JSGC_DYNAMIC_HEAP_GROWTH:
        return tunables.isDynamicHeapGrowthEnabled();
      case JSGC_DYNAMIC_MARK_SLICE:
        return tunables.isDynamicMarkSliceEnabled();
      case JSGC_ALLOCATION_THRESHOLD:
        return tunables.gcZoneAllocThresholdBase() / 1024 / 1024;
      case JSGC_MIN_EMPTY_CHUNK_COUNT:
        return tunables.minEmptyChunkCount(lock);
      case JSGC_MAX_EMPTY_CHUNK_COUNT:
        return tunables.maxEmptyChunkCount();
      case JSGC_COMPACTING_ENABLED:
        return compactingEnabled;
      default:
        MOZ_ASSERT(key == JSGC_NUMBER);
        return uint32_t(number);
    }
}

void
GCRuntime::setMarkStackLimit(size_t limit, AutoLockGC& lock)
{
    MOZ_ASSERT(!rt->isHeapBusy());
    AutoUnlockGC unlock(lock);
    AutoStopVerifyingBarriers pauseVerification(rt, false);
    marker.setMaxCapacity(limit);
}

bool
GCRuntime::addBlackRootsTracer(JSTraceDataOp traceOp, void* data)
{
    AssertHeapIsIdle(rt);
    return !!blackRootTracers.append(Callback<JSTraceDataOp>(traceOp, data));
}

void
GCRuntime::removeBlackRootsTracer(JSTraceDataOp traceOp, void* data)
{
    // Can be called from finalizers
    for (size_t i = 0; i < blackRootTracers.length(); i++) {
        Callback<JSTraceDataOp>* e = &blackRootTracers[i];
        if (e->op == traceOp && e->data == data) {
            blackRootTracers.erase(e);
        }
    }
}

void
GCRuntime::setGrayRootsTracer(JSTraceDataOp traceOp, void* data)
{
    AssertHeapIsIdle(rt);
    grayRootTracer.op = traceOp;
    grayRootTracer.data = data;
}

void
GCRuntime::setGCCallback(JSGCCallback callback, void* data)
{
    gcCallback.op = callback;
    gcCallback.data = data;
}

void
GCRuntime::callGCCallback(JSGCStatus status) const
{
    if (gcCallback.op)
        gcCallback.op(rt, status, gcCallback.data);
}

namespace {

class AutoNotifyGCActivity {
  public:
    explicit AutoNotifyGCActivity(GCRuntime& gc) : gc_(gc) {
        if (!gc_.isIncrementalGCInProgress()) {
            gcstats::AutoPhase ap(gc_.stats, gcstats::PHASE_GC_BEGIN);
            gc_.callGCCallback(JSGC_BEGIN);
        }
    }
    ~AutoNotifyGCActivity() {
        if (!gc_.isIncrementalGCInProgress()) {
            gcstats::AutoPhase ap(gc_.stats, gcstats::PHASE_GC_END);
            gc_.callGCCallback(JSGC_END);
        }
    }

  private:
    GCRuntime& gc_;
};

} // (anon)

bool
GCRuntime::addFinalizeCallback(JSFinalizeCallback callback, void* data)
{
    return finalizeCallbacks.append(Callback<JSFinalizeCallback>(callback, data));
}

void
GCRuntime::removeFinalizeCallback(JSFinalizeCallback callback)
{
    for (Callback<JSFinalizeCallback>* p = finalizeCallbacks.begin();
         p < finalizeCallbacks.end(); p++)
    {
        if (p->op == callback) {
            finalizeCallbacks.erase(p);
            break;
        }
    }
}

void
GCRuntime::callFinalizeCallbacks(FreeOp* fop, JSFinalizeStatus status) const
{
    for (auto& p : finalizeCallbacks) {
        p.op(fop, status, !isFull, p.data);
    }
}

bool
GCRuntime::addWeakPointerZoneGroupCallback(JSWeakPointerZoneGroupCallback callback, void* data)
{
    return updateWeakPointerZoneGroupCallbacks.append(
            Callback<JSWeakPointerZoneGroupCallback>(callback, data));
}

void
GCRuntime::removeWeakPointerZoneGroupCallback(JSWeakPointerZoneGroupCallback callback)
{
    for (auto& p : updateWeakPointerZoneGroupCallbacks) {
        if (p.op == callback) {
            updateWeakPointerZoneGroupCallbacks.erase(&p);
            break;
        }
    }
}

void
GCRuntime::callWeakPointerZoneGroupCallbacks() const
{
    for (auto const& p : updateWeakPointerZoneGroupCallbacks) {
        p.op(rt, p.data);
    }
}

bool
GCRuntime::addWeakPointerCompartmentCallback(JSWeakPointerCompartmentCallback callback, void* data)
{
    return updateWeakPointerCompartmentCallbacks.append(
            Callback<JSWeakPointerCompartmentCallback>(callback, data));
}

void
GCRuntime::removeWeakPointerCompartmentCallback(JSWeakPointerCompartmentCallback callback)
{
    for (auto& p : updateWeakPointerCompartmentCallbacks) {
        if (p.op == callback) {
            updateWeakPointerCompartmentCallbacks.erase(&p);
            break;
        }
    }
}

void
GCRuntime::callWeakPointerCompartmentCallbacks(JSCompartment* comp) const
{
    for (auto const& p : updateWeakPointerCompartmentCallbacks) {
        p.op(rt, comp, p.data);
    }
}

JS::GCSliceCallback
GCRuntime::setSliceCallback(JS::GCSliceCallback callback) {
    return stats.setSliceCallback(callback);
}

bool
GCRuntime::addRoot(Value* vp, const char* name)
{
    /*
     * Sometimes Firefox will hold weak references to objects and then convert
     * them to strong references by calling AddRoot (e.g., via PreserveWrapper,
     * or ModifyBusyCount in workers). We need a read barrier to cover these
     * cases.
     */
    if (isIncrementalGCInProgress())
        HeapValue::writeBarrierPre(*vp);

    return rootsHash.put(vp, name);
}

void
GCRuntime::removeRoot(Value* vp)
{
    rootsHash.remove(vp);
    poke();
}

extern JS_FRIEND_API(bool)
js::AddRawValueRoot(JSContext* cx, Value* vp, const char* name)
{
    MOZ_ASSERT(vp);
    MOZ_ASSERT(name);
    bool ok = cx->runtime()->gc.addRoot(vp, name);
    if (!ok)
        JS_ReportOutOfMemory(cx);
    return ok;
}

extern JS_FRIEND_API(void)
js::RemoveRawValueRoot(JSContext* cx, Value* vp)
{
    cx->runtime()->gc.removeRoot(vp);
}

void
GCRuntime::setMaxMallocBytes(size_t value)
{
    /*
     * For compatibility treat any value that exceeds PTRDIFF_T_MAX to
     * mean that value.
     */
    maxMallocBytes = (ptrdiff_t(value) >= 0) ? value : size_t(-1) >> 1;
    resetMallocBytes();
    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next())
        zone->setGCMaxMallocBytes(value);
}

void
GCRuntime::resetMallocBytes()
{
    mallocBytesUntilGC = ptrdiff_t(maxMallocBytes);
    mallocGCTriggered = false;
}

void
GCRuntime::updateMallocCounter(JS::Zone* zone, size_t nbytes)
{
    mallocBytesUntilGC -= ptrdiff_t(nbytes);
    if (MOZ_UNLIKELY(isTooMuchMalloc()))
        onTooMuchMalloc();
    else if (zone)
        zone->updateMallocCounter(nbytes);
}

void
GCRuntime::onTooMuchMalloc()
{
    if (!mallocGCTriggered)
        mallocGCTriggered = triggerGC(JS::gcreason::TOO_MUCH_MALLOC);
}

double
ZoneHeapThreshold::allocTrigger(bool highFrequencyGC) const
{
    return (highFrequencyGC ? 0.85 : 0.9) * gcTriggerBytes();
}

/* static */ double
ZoneHeapThreshold::computeZoneHeapGrowthFactorForHeapSize(size_t lastBytes,
                                                          const GCSchedulingTunables& tunables,
                                                          const GCSchedulingState& state)
{
    if (!tunables.isDynamicHeapGrowthEnabled())
        return 3.0;

    // For small zones, our collection heuristics do not matter much: favor
    // something simple in this case.
    if (lastBytes < 1 * 1024 * 1024)
        return tunables.lowFrequencyHeapGrowth();

    // If GC's are not triggering in rapid succession, use a lower threshold so
    // that we will collect garbage sooner.
    if (!state.inHighFrequencyGCMode())
        return tunables.lowFrequencyHeapGrowth();

    // The heap growth factor depends on the heap size after a GC and the GC
    // frequency. For low frequency GCs (more than 1sec between GCs) we let
    // the heap grow to 150%. For high frequency GCs we let the heap grow
    // depending on the heap size:
    //   lastBytes < highFrequencyLowLimit: 300%
    //   lastBytes > highFrequencyHighLimit: 150%
    //   otherwise: linear interpolation between 300% and 150% based on lastBytes

    // Use shorter names to make the operation comprehensible.
    double minRatio = tunables.highFrequencyHeapGrowthMin();
    double maxRatio = tunables.highFrequencyHeapGrowthMax();
    double lowLimit = tunables.highFrequencyLowLimitBytes();
    double highLimit = tunables.highFrequencyHighLimitBytes();

    if (lastBytes <= lowLimit)
        return maxRatio;

    if (lastBytes >= highLimit)
        return minRatio;

    double factor = maxRatio - ((maxRatio - minRatio) * ((lastBytes - lowLimit) /
                                                         (highLimit - lowLimit)));
    MOZ_ASSERT(factor >= minRatio);
    MOZ_ASSERT(factor <= maxRatio);
    return factor;
}

/* static */ size_t
ZoneHeapThreshold::computeZoneTriggerBytes(double growthFactor, size_t lastBytes,
                                           JSGCInvocationKind gckind,
                                           const GCSchedulingTunables& tunables,
                                           const AutoLockGC& lock)
{
    size_t base = gckind == GC_SHRINK
                ? Max(lastBytes, tunables.minEmptyChunkCount(lock) * ChunkSize)
                : Max(lastBytes, tunables.gcZoneAllocThresholdBase());
    double trigger = double(base) * growthFactor;
    return size_t(Min(double(tunables.gcMaxBytes()), trigger));
}

void
ZoneHeapThreshold::updateAfterGC(size_t lastBytes, JSGCInvocationKind gckind,
                                 const GCSchedulingTunables& tunables,
                                 const GCSchedulingState& state, const AutoLockGC& lock)
{
    gcHeapGrowthFactor_ = computeZoneHeapGrowthFactorForHeapSize(lastBytes, tunables, state);
    gcTriggerBytes_ = computeZoneTriggerBytes(gcHeapGrowthFactor_, lastBytes, gckind, tunables,
                                              lock);
}

void
ZoneHeapThreshold::updateForRemovedArena(const GCSchedulingTunables& tunables)
{
    size_t amount = ArenaSize * gcHeapGrowthFactor_;

    MOZ_ASSERT(amount > 0);
    MOZ_ASSERT(gcTriggerBytes_ >= amount);

    if (gcTriggerBytes_ - amount < tunables.gcZoneAllocThresholdBase() * gcHeapGrowthFactor_)
        return;

    gcTriggerBytes_ -= amount;
}

void
GCMarker::delayMarkingArena(ArenaHeader* aheader)
{
    if (aheader->hasDelayedMarking) {
        /* Arena already scheduled to be marked later */
        return;
    }
    aheader->setNextDelayedMarking(unmarkedArenaStackTop);
    unmarkedArenaStackTop = aheader;
    markLaterArenas++;
}

void
GCMarker::delayMarkingChildren(const void* thing)
{
    const TenuredCell* cell = TenuredCell::fromPointer(thing);
    cell->arenaHeader()->markOverflow = 1;
    delayMarkingArena(cell->arenaHeader());
}

inline void
ArenaLists::prepareForIncrementalGC(JSRuntime* rt)
{
    for (auto i : AllAllocKinds()) {
        FreeList* freeList = &freeLists[i];
        if (!freeList->isEmpty()) {
            ArenaHeader* aheader = freeList->arenaHeader();
            aheader->allocatedDuringIncremental = true;
            rt->gc.marker.delayMarkingArena(aheader);
        }
    }
}

/* Compacting GC */

bool
GCRuntime::shouldCompact()
{
    // Compact on shrinking GC if enabled, but skip compacting in incremental
    // GCs if we are currently animating.
    return invocationKind == GC_SHRINK && isCompactingGCEnabled() &&
        (!isIncremental || rt->lastAnimationTime + PRMJ_USEC_PER_SEC < PRMJ_Now());
}

void
GCRuntime::disableCompactingGC()
{
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
    ++compactingDisabledCount;
}

void
GCRuntime::enableCompactingGC()
{
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
    MOZ_ASSERT(compactingDisabledCount > 0);
    --compactingDisabledCount;
}

bool
GCRuntime::isCompactingGCEnabled() const
{
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
    return compactingEnabled && compactingDisabledCount == 0;
}

AutoDisableCompactingGC::AutoDisableCompactingGC(JSRuntime* rt)
  : gc(rt->gc)
{
    gc.disableCompactingGC();
}

AutoDisableCompactingGC::~AutoDisableCompactingGC()
{
    gc.enableCompactingGC();
}

static bool
CanRelocateZone(Zone* zone)
{
    return !zone->isAtomsZone() && !zone->isSelfHostingZone();
}

static bool
CanRelocateAllocKind(AllocKind kind)
{
    return IsObjectAllocKind(kind);
}

size_t ArenaHeader::countFreeCells()
{
    size_t count = 0;
    size_t thingSize = getThingSize();
    FreeSpan firstSpan(getFirstFreeSpan());
    for (const FreeSpan* span = &firstSpan; !span->isEmpty(); span = span->nextSpan())
        count += span->length(thingSize);
    return count;
}

size_t ArenaHeader::countUsedCells()
{
    return Arena::thingsPerArena(getThingSize()) - countFreeCells();
}

ArenaHeader*
ArenaList::removeRemainingArenas(ArenaHeader** arenap)
{
    // This is only ever called to remove arenas that are after the cursor, so
    // we don't need to update it.
#ifdef DEBUG
    for (ArenaHeader* arena = *arenap; arena; arena = arena->next)
        MOZ_ASSERT(cursorp_ != &arena->next);
#endif
    ArenaHeader* remainingArenas = *arenap;
    *arenap = nullptr;
    check();
    return remainingArenas;
}

static bool
ShouldRelocateAllArenas(JS::gcreason::Reason reason)
{
    return reason == JS::gcreason::DEBUG_GC;
}

/*
 * Choose which arenas to relocate all cells from. Return an arena cursor that
 * can be passed to removeRemainingArenas().
 */
ArenaHeader**
ArenaList::pickArenasToRelocate(size_t& arenaTotalOut, size_t& relocTotalOut)
{
    // Relocate the greatest number of arenas such that the number of used cells
    // in relocated arenas is less than or equal to the number of free cells in
    // unrelocated arenas. In other words we only relocate cells we can move
    // into existing arenas, and we choose the least full areans to relocate.
    //
    // This is made easier by the fact that the arena list has been sorted in
    // descending order of number of used cells, so we will always relocate a
    // tail of the arena list. All we need to do is find the point at which to
    // start relocating.

    check();

    if (isCursorAtEnd())
        return nullptr;

    ArenaHeader** arenap = cursorp_;  // Next arena to consider for relocation.
    size_t previousFreeCells = 0;     // Count of free cells before arenap.
    size_t followingUsedCells = 0;    // Count of used cells after arenap.
    size_t fullArenaCount = 0;        // Number of full arenas (not relocated).
    size_t nonFullArenaCount = 0;     // Number of non-full arenas (considered for relocation).
    size_t arenaIndex = 0;            // Index of the next arena to consider.

    for (ArenaHeader* arena = head_; arena != *cursorp_; arena = arena->next)
        fullArenaCount++;

    for (ArenaHeader* arena = *cursorp_; arena; arena = arena->next) {
        followingUsedCells += arena->countUsedCells();
        nonFullArenaCount++;
    }

    mozilla::DebugOnly<size_t> lastFreeCells(0);
    size_t cellsPerArena = Arena::thingsPerArena((*arenap)->getThingSize());

    while (*arenap) {
        ArenaHeader* arena = *arenap;
        if (followingUsedCells <= previousFreeCells)
            break;

        size_t freeCells = arena->countFreeCells();
        size_t usedCells = cellsPerArena - freeCells;
        followingUsedCells -= usedCells;
#ifdef DEBUG
        MOZ_ASSERT(freeCells >= lastFreeCells);
        lastFreeCells = freeCells;
#endif
        previousFreeCells += freeCells;
        arenap = &arena->next;
        arenaIndex++;
    }

    size_t relocCount = nonFullArenaCount - arenaIndex;
    MOZ_ASSERT(relocCount < nonFullArenaCount);
    MOZ_ASSERT((relocCount == 0) == (!*arenap));
    arenaTotalOut += fullArenaCount + nonFullArenaCount;
    relocTotalOut += relocCount;

    return arenap;
}

#ifdef DEBUG
inline bool
PtrIsInRange(const void* ptr, const void* start, size_t length)
{
    return uintptr_t(ptr) - uintptr_t(start) < length;
}
#endif

static TenuredCell*
AllocRelocatedCell(Zone* zone, AllocKind thingKind, size_t thingSize)
{
    AutoEnterOOMUnsafeRegion oomUnsafe;
    void* dstAlloc = zone->arenas.allocateFromFreeList(thingKind, thingSize);
    if (!dstAlloc)
        dstAlloc = GCRuntime::refillFreeListInGC(zone, thingKind);
    if (!dstAlloc) {
        // This can only happen in zeal mode or debug builds as we don't
        // otherwise relocate more cells than we have existing free space
        // for.
        oomUnsafe.crash("Could not allocate new arena while compacting");
    }
    return TenuredCell::fromPointer(dstAlloc);
}

static void
RelocateCell(Zone* zone, TenuredCell* src, AllocKind thingKind, size_t thingSize)
{
    JS::AutoSuppressGCAnalysis nogc(zone->runtimeFromMainThread());

    // Allocate a new cell.
    MOZ_ASSERT(zone == src->zone());
    TenuredCell* dst = AllocRelocatedCell(zone, thingKind, thingSize);

    // Copy source cell contents to destination.
    memcpy(dst, src, thingSize);

    // Move any uid attached to the object.
    src->zone()->transferUniqueId(dst, src);

    if (IsObjectAllocKind(thingKind)) {
        JSObject* srcObj = static_cast<JSObject*>(static_cast<Cell*>(src));
        JSObject* dstObj = static_cast<JSObject*>(static_cast<Cell*>(dst));

        if (srcObj->isNative()) {
            NativeObject* srcNative = &srcObj->as<NativeObject>();
            NativeObject* dstNative = &dstObj->as<NativeObject>();

            // Fixup the pointer to inline object elements if necessary.
            if (srcNative->hasFixedElements())
                dstNative->setFixedElements();

            // For copy-on-write objects that own their elements, fix up the
            // owner pointer to point to the relocated object.
            if (srcNative->denseElementsAreCopyOnWrite()) {
                HeapPtrNativeObject& owner = dstNative->getElementsHeader()->ownerObject();
                if (owner == srcNative)
                    owner = dstNative;
            }
        }

        // Call object moved hook if present.
        if (JSObjectMovedOp op = srcObj->getClass()->ext.objectMovedOp)
            op(dstObj, srcObj);

        MOZ_ASSERT_IF(dstObj->isNative(),
                      !PtrIsInRange((const Value*)dstObj->as<NativeObject>().getDenseElements(),
                                    src, thingSize));
    }

    // Copy the mark bits.
    dst->copyMarkBitsFrom(src);

    // Mark source cell as forwarded and leave a pointer to the destination.
    RelocationOverlay* overlay = RelocationOverlay::fromCell(src);
    overlay->forwardTo(dst);
}

static void
RelocateArena(ArenaHeader* aheader, SliceBudget& sliceBudget)
{
    MOZ_ASSERT(aheader->allocated());
    MOZ_ASSERT(!aheader->hasDelayedMarking);
    MOZ_ASSERT(!aheader->markOverflow);
    MOZ_ASSERT(!aheader->allocatedDuringIncremental);

    Zone* zone = aheader->zone;

    AllocKind thingKind = aheader->getAllocKind();
    size_t thingSize = aheader->getThingSize();

    for (ArenaCellIterUnderFinalize i(aheader); !i.done(); i.next()) {
        RelocateCell(zone, i.getCell(), thingKind, thingSize);
        sliceBudget.step();
    }

#ifdef DEBUG
    for (ArenaCellIterUnderFinalize i(aheader); !i.done(); i.next()) {
        TenuredCell* src = i.getCell();
        MOZ_ASSERT(RelocationOverlay::isCellForwarded(src));
        TenuredCell* dest = Forwarded(src);
        MOZ_ASSERT(src->isMarked(BLACK) == dest->isMarked(BLACK));
        MOZ_ASSERT(src->isMarked(GRAY) == dest->isMarked(GRAY));
    }
#endif
}

static inline bool
ShouldProtectRelocatedArenas(JS::gcreason::Reason reason)
{
    // For zeal mode collections we don't release the relocated arenas
    // immediately. Instead we protect them and keep them around until the next
    // collection so we can catch any stray accesses to them.
#ifdef DEBUG
    return reason == JS::gcreason::DEBUG_GC;
#else
    return false;
#endif
}

/*
 * Relocate all arenas identified by pickArenasToRelocate: for each arena,
 * relocate each cell within it, then add it to a list of relocated arenas.
 */
ArenaHeader*
ArenaList::relocateArenas(ArenaHeader* toRelocate, ArenaHeader* relocated, SliceBudget& sliceBudget,
                          gcstats::Statistics& stats)
{
    check();

    while (ArenaHeader* arena = toRelocate) {
        toRelocate = arena->next;
        RelocateArena(arena, sliceBudget);
        // Prepend to list of relocated arenas
        arena->next = relocated;
        relocated = arena;
        stats.count(gcstats::STAT_ARENA_RELOCATED);
    }

    check();

    return relocated;
}

// Skip compacting zones unless we can free a certain proportion of their GC
// heap memory.
static const double MIN_ZONE_RECLAIM_PERCENT = 2.0;

static bool
IsOOMReason(JS::gcreason::Reason reason)
{
    return reason == JS::gcreason::LAST_DITCH ||
           reason == JS::gcreason::MEM_PRESSURE;
}

static bool
ShouldRelocateZone(size_t arenaCount, size_t relocCount, JS::gcreason::Reason reason)
{
    if (relocCount == 0)
        return false;

    if (IsOOMReason(reason))
        return true;

    return (relocCount * 100.0) / arenaCount >= MIN_ZONE_RECLAIM_PERCENT;
}

bool
ArenaLists::relocateArenas(Zone* zone, ArenaHeader*& relocatedListOut, JS::gcreason::Reason reason,
                           SliceBudget& sliceBudget, gcstats::Statistics& stats)
{
    // This is only called from the main thread while we are doing a GC, so
    // there is no need to lock.
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime_));
    MOZ_ASSERT(runtime_->gc.isHeapCompacting());
    MOZ_ASSERT(!runtime_->gc.isBackgroundSweeping());

    // Flush all the freeLists back into the arena headers
    purge();
    checkEmptyFreeLists();

    if (ShouldRelocateAllArenas(reason)) {
        zone->prepareForCompacting();
        for (auto i : AllAllocKinds()) {
            if (CanRelocateAllocKind(i)) {
                ArenaList& al = arenaLists[i];
                ArenaHeader* allArenas = al.head();
                al.clear();
                relocatedListOut = al.relocateArenas(allArenas, relocatedListOut, sliceBudget, stats);
            }
        }
    } else {
        size_t arenaCount = 0;
        size_t relocCount = 0;
        AllAllocKindArray<ArenaHeader**> toRelocate;

        for (auto i : AllAllocKinds()) {
            toRelocate[i] = nullptr;
            if (CanRelocateAllocKind(i))
                toRelocate[i] = arenaLists[i].pickArenasToRelocate(arenaCount, relocCount);
        }

        if (!ShouldRelocateZone(arenaCount, relocCount, reason))
            return false;

        zone->prepareForCompacting();
        for (auto i : AllAllocKinds()) {
            if (toRelocate[i]) {
                ArenaList& al = arenaLists[i];
                ArenaHeader* arenas = al.removeRemainingArenas(toRelocate[i]);
                relocatedListOut = al.relocateArenas(arenas, relocatedListOut, sliceBudget, stats);
            }
        }
    }

    // When we allocate new locations for cells, we use
    // allocateFromFreeList(). Reset the free list again so that
    // AutoCopyFreeListToArenasForGC doesn't complain that the free lists are
    // different now.
    purge();
    checkEmptyFreeLists();

    return true;
}

bool
GCRuntime::relocateArenas(Zone* zone, JS::gcreason::Reason reason, ArenaHeader*& relocatedListOut,
                          SliceBudget& sliceBudget)
{
    gcstats::AutoPhase ap(stats, gcstats::PHASE_COMPACT_MOVE);

    MOZ_ASSERT(!zone->isPreservingCode());
    MOZ_ASSERT(CanRelocateZone(zone));

    jit::StopAllOffThreadCompilations(zone);

    if (!zone->arenas.relocateArenas(zone, relocatedListOut, reason, sliceBudget, stats))
        return false;

#ifdef DEBUG
    // Check that we did as much compaction as we should have. There
    // should always be less than one arena's worth of free cells.
    for (auto i : AllAllocKinds()) {
        size_t thingsPerArena = Arena::thingsPerArena(Arena::thingSize(i));
        if (CanRelocateAllocKind(i)) {
            ArenaList& al = zone->arenas.arenaLists[i];
            size_t freeCells = 0;
            for (ArenaHeader* arena = al.arenaAfterCursor(); arena; arena = arena->next)
                freeCells += arena->countFreeCells();
            MOZ_ASSERT(freeCells < thingsPerArena);
        }
    }
#endif

    return true;
}

void
MovingTracer::onObjectEdge(JSObject** objp)
{
    if (IsForwarded(*objp))
        *objp = Forwarded(*objp);
}

void
Zone::prepareForCompacting()
{
    FreeOp* fop = runtimeFromMainThread()->defaultFreeOp();
    discardJitCode(fop);
}

void
GCRuntime::sweepTypesAfterCompacting(Zone* zone)
{
    FreeOp* fop = rt->defaultFreeOp();
    zone->beginSweepTypes(fop, rt->gc.releaseObservedTypes && !zone->isPreservingCode());

    AutoClearTypeInferenceStateOnOOM oom(zone);

    for (ZoneCellIterUnderGC i(zone, AllocKind::SCRIPT); !i.done(); i.next()) {
        JSScript* script = i.get<JSScript>();
        script->maybeSweepTypes(&oom);
    }

    for (ZoneCellIterUnderGC i(zone, AllocKind::OBJECT_GROUP); !i.done(); i.next()) {
        ObjectGroup* group = i.get<ObjectGroup>();
        group->maybeSweep(&oom);
    }

    zone->types.endSweep(rt);
}

void
GCRuntime::sweepZoneAfterCompacting(Zone* zone)
{
    MOZ_ASSERT(zone->isCollecting());
    FreeOp* fop = rt->defaultFreeOp();
    sweepTypesAfterCompacting(zone);
    zone->sweepBreakpoints(fop);
    zone->sweepWeakMaps();

    for (CompartmentsInZoneIter c(zone); !c.done(); c.next()) {
        c->sweepInnerViews();
        c->sweepBaseShapeTable();
        c->sweepInitialShapeTable();
        c->objectGroups.sweep(fop);
        c->sweepRegExps();
        c->sweepSavedStacks();
        c->sweepGlobalObject(fop);
        c->sweepObjectPendingMetadata();
        c->sweepSelfHostingScriptSource();
        c->sweepDebugScopes();
        c->sweepJitCompartment(fop);
        c->sweepNativeIterators();
        c->sweepTemplateObjects();
    }
}

template <typename T>
static void
UpdateCellPointersTyped(MovingTracer* trc, ArenaHeader* arena, JS::TraceKind traceKind)
{
    for (ArenaCellIterUnderGC i(arena); !i.done(); i.next()) {
        T* cell = reinterpret_cast<T*>(i.getCell());
        cell->fixupAfterMovingGC();
        TraceChildren(trc, cell, traceKind);
    }
}

/*
 * Update the interal pointers for all cells in an arena.
 */
static void
UpdateCellPointers(MovingTracer* trc, ArenaHeader* arena)
{
    AllocKind kind = arena->getAllocKind();
    JS::TraceKind traceKind = MapAllocToTraceKind(kind);

    switch (kind) {
      case AllocKind::FUNCTION:
      case AllocKind::FUNCTION_EXTENDED:
      case AllocKind::OBJECT0:
      case AllocKind::OBJECT0_BACKGROUND:
      case AllocKind::OBJECT2:
      case AllocKind::OBJECT2_BACKGROUND:
      case AllocKind::OBJECT4:
      case AllocKind::OBJECT4_BACKGROUND:
      case AllocKind::OBJECT8:
      case AllocKind::OBJECT8_BACKGROUND:
      case AllocKind::OBJECT12:
      case AllocKind::OBJECT12_BACKGROUND:
      case AllocKind::OBJECT16:
      case AllocKind::OBJECT16_BACKGROUND:
        UpdateCellPointersTyped<JSObject>(trc, arena, traceKind);
        return;
      case AllocKind::SCRIPT:
        UpdateCellPointersTyped<JSScript>(trc, arena, traceKind);
        return;
      case AllocKind::LAZY_SCRIPT:
        UpdateCellPointersTyped<LazyScript>(trc, arena, traceKind);
        return;
      case AllocKind::SHAPE:
        UpdateCellPointersTyped<Shape>(trc, arena, traceKind);
        return;
      case AllocKind::ACCESSOR_SHAPE:
        UpdateCellPointersTyped<AccessorShape>(trc, arena, traceKind);
        return;
      case AllocKind::BASE_SHAPE:
        UpdateCellPointersTyped<BaseShape>(trc, arena, traceKind);
        return;
      case AllocKind::OBJECT_GROUP:
        UpdateCellPointersTyped<ObjectGroup>(trc, arena, traceKind);
        return;
      case AllocKind::JITCODE:
        UpdateCellPointersTyped<jit::JitCode>(trc, arena, traceKind);
        return;
      default:
        MOZ_CRASH("Invalid alloc kind for UpdateCellPointers");
    }
}

namespace js {
namespace gc {

struct ArenasToUpdate
{
    enum KindsToUpdate {
        FOREGROUND = 1,
        BACKGROUND = 2,
        ALL = FOREGROUND | BACKGROUND
    };
    ArenasToUpdate(Zone* zone, KindsToUpdate kinds);
    bool done() { return kind == AllocKind::LIMIT; }
    ArenaHeader* getArenasToUpdate(AutoLockHelperThreadState& lock, unsigned max);

  private:
    KindsToUpdate kinds; // Selects which thing kinds to iterate
    Zone* zone;          // Zone to process
    AllocKind kind;      // Current alloc kind to process
    ArenaHeader* arena;  // Next arena to process

    AllocKind nextAllocKind(AllocKind i) { return AllocKind(uint8_t(i) + 1); }
    bool shouldProcessKind(AllocKind kind);
    ArenaHeader* next(AutoLockHelperThreadState& lock);
};

bool ArenasToUpdate::shouldProcessKind(AllocKind kind)
{
    MOZ_ASSERT(IsValidAllocKind(kind));

    // GC things that do not contain JSObject pointers don't need updating.
    if (kind == AllocKind::FAT_INLINE_STRING ||
        kind == AllocKind::STRING ||
        kind == AllocKind::EXTERNAL_STRING ||
        kind == AllocKind::SYMBOL)
    {
        return false;
    }

    // We try to update as many GC things in parallel as we can, but there are
    // kinds for which this might not be safe:
    //  - we assume JSObjects that are foreground finalized are not safe to
    //    update in parallel
    //  - updating a shape touches child shapes in fixupShapeTreeAfterMovingGC()
    if (js::gc::IsBackgroundFinalized(kind) &&
        kind != AllocKind::SHAPE &&
        kind != AllocKind::ACCESSOR_SHAPE)
    {
        return (kinds & BACKGROUND) != 0;
    } else {
        return (kinds & FOREGROUND) != 0;
    }
}

ArenasToUpdate::ArenasToUpdate(Zone* zone, KindsToUpdate kinds)
  : kinds(kinds), zone(zone), kind(AllocKind::FIRST), arena(nullptr)
{
    MOZ_ASSERT(zone->isGCCompacting());
    MOZ_ASSERT(kinds && !(kinds & ~ALL));
}

ArenaHeader*
ArenasToUpdate::next(AutoLockHelperThreadState& lock)
{
    // Find the next arena to update.
    //
    // This iterates through the GC thing kinds filtered by shouldProcessKind(),
    // and then through thea arenas of that kind.  All state is held in the
    // object and we just return when we find an arena.

    for (; kind < AllocKind::LIMIT; kind = nextAllocKind(kind)) {
        if (shouldProcessKind(kind)) {
            if (!arena)
                arena = zone->arenas.getFirstArena(kind);
            else
                arena = arena->next;
            if (arena)
                return arena;
        }
    }

    MOZ_ASSERT(!arena);
    MOZ_ASSERT(done());
    return nullptr;
}

ArenaHeader*
ArenasToUpdate::getArenasToUpdate(AutoLockHelperThreadState& lock, unsigned count)
{
    if (done())
        return nullptr;

    ArenaHeader* head = nullptr;
    ArenaHeader* tail = nullptr;

    for (unsigned i = 0; i < count; ++i) {
        ArenaHeader* arena = next(lock);
        if (!arena)
            break;

        if (tail)
            tail->setNextArenaToUpdate(arena);
        else
            head = arena;
        tail = arena;
    }

    return head;
}

struct UpdateCellPointersTask : public GCParallelTask
{
    // Number of arenas to update in one block.
#ifdef DEBUG
    static const unsigned ArenasToProcess = 16;
#else
    static const unsigned ArenasToProcess = 256;
#endif

    UpdateCellPointersTask() : rt_(nullptr), source_(nullptr), arenaList_(nullptr) {}
    void init(JSRuntime* rt, ArenasToUpdate* source, AutoLockHelperThreadState& lock);
    ~UpdateCellPointersTask() override { join(); }

  private:
    JSRuntime* rt_;
    ArenasToUpdate* source_;
    ArenaHeader* arenaList_;

    virtual void run() override;
    void getArenasToUpdate(AutoLockHelperThreadState& lock);
    void updateArenas();
};

void
UpdateCellPointersTask::init(JSRuntime* rt, ArenasToUpdate* source, AutoLockHelperThreadState& lock)
{
    rt_ = rt;
    source_ = source;
    getArenasToUpdate(lock);
}

void
UpdateCellPointersTask::getArenasToUpdate(AutoLockHelperThreadState& lock)
{
    arenaList_ = source_->getArenasToUpdate(lock, ArenasToProcess);
}

void
UpdateCellPointersTask::updateArenas()
{
    MovingTracer trc(rt_);
    for (ArenaHeader* arena = arenaList_;
         arena;
         arena = arena->getNextArenaToUpdateAndUnlink())
    {
        UpdateCellPointers(&trc, arena);
    }
    arenaList_ = nullptr;
}

/* virtual */ void
UpdateCellPointersTask::run()
{
    MOZ_ASSERT(!HelperThreadState().isLocked());
    while (arenaList_) {
        updateArenas();
        {
            AutoLockHelperThreadState lock;
            getArenasToUpdate(lock);
        }
    }
}

} // namespace gc
} // namespace js

void
GCRuntime::updateAllCellPointersParallel(MovingTracer* trc, Zone* zone)
{
    AutoDisableProxyCheck noProxyCheck(rt); // These checks assert when run in parallel.

    const size_t minTasks = 2;
    const size_t maxTasks = 8;
    size_t targetTaskCount = HelperThreadState().cpuCount / 2;
    size_t taskCount = Min(Max(targetTaskCount, minTasks), maxTasks);
    UpdateCellPointersTask bgTasks[maxTasks];
    UpdateCellPointersTask fgTask;

    ArenasToUpdate fgArenas(zone, ArenasToUpdate::FOREGROUND);
    ArenasToUpdate bgArenas(zone, ArenasToUpdate::BACKGROUND);

    unsigned tasksStarted = 0;
    {
        AutoLockHelperThreadState lock;
        unsigned i;
        for (i = 0; i < taskCount && !bgArenas.done(); ++i) {
            bgTasks[i].init(rt, &bgArenas, lock);
            startTask(bgTasks[i], gcstats::PHASE_COMPACT_UPDATE_CELLS);
        }
        tasksStarted = i;

        fgTask.init(rt, &fgArenas, lock);
    }

    fgTask.runFromMainThread(rt);

    {
        AutoLockHelperThreadState lock;
        for (unsigned i = 0; i < tasksStarted; ++i)
            joinTask(bgTasks[i], gcstats::PHASE_COMPACT_UPDATE_CELLS);
    }
}

void
GCRuntime::updateAllCellPointersSerial(MovingTracer* trc, Zone* zone)
{
    UpdateCellPointersTask task;
    {
        AutoLockHelperThreadState lock;
        ArenasToUpdate allArenas(zone, ArenasToUpdate::ALL);
        task.init(rt, &allArenas, lock);
    }
    task.runFromMainThread(rt);
}

/*
 * Update pointers to relocated cells by doing a full heap traversal and sweep.
 *
 * The latter is necessary to update weak references which are not marked as
 * part of the traversal.
 */
void
GCRuntime::updatePointersToRelocatedCells(Zone* zone)
{
    MOZ_ASSERT(zone->isGCCompacting());
    MOZ_ASSERT(rt->currentThreadHasExclusiveAccess());

    gcstats::AutoPhase ap(stats, gcstats::PHASE_COMPACT_UPDATE);
    MovingTracer trc(rt);

    // Fixup compartment global pointers as these get accessed during marking.
    for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next())
        comp->fixupAfterMovingGC();
    JSCompartment::fixupCrossCompartmentWrappersAfterMovingGC(&trc);

    // Mark roots to update them.
    {
        markRuntime(&trc, MarkRuntime);

        gcstats::AutoPhase ap(stats, gcstats::PHASE_MARK_ROOTS);
        Debugger::markAll(&trc);
        Debugger::markIncomingCrossCompartmentEdges(&trc);

        WeakMapBase::markAll(zone, &trc);
        for (CompartmentsInZoneIter c(zone); !c.done(); c.next()) {
            c->trace(&trc);
            if (c->watchpointMap)
                c->watchpointMap->markAll(&trc);
        }

        // Mark all gray roots, making sure we call the trace callback to get the
        // current set.
        if (JSTraceDataOp op = grayRootTracer.op)
            (*op)(&trc, grayRootTracer.data);
    }

    // Sweep everything to fix up weak pointers
    WatchpointMap::sweepAll(rt);
    Debugger::sweepAll(rt->defaultFreeOp());
    jit::JitRuntime::SweepJitcodeGlobalTable(rt);
    rt->gc.sweepZoneAfterCompacting(zone);

    // Type inference may put more blocks here to free.
    freeLifoAlloc.freeAll();

    // Clear runtime caches that can contain cell pointers.
    // TODO: Should possibly just call purgeRuntime() here.
    rt->newObjectCache.purge();
    rt->nativeIterCache.purge();

    // Call callbacks to get the rest of the system to fixup other untraced pointers.
    callWeakPointerZoneGroupCallbacks();
    for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next())
        callWeakPointerCompartmentCallbacks(comp);

    // Finally, iterate through all cells that can contain JSObject pointers to
    // update them. Since updating each cell is independent we try to
    // parallelize this as much as possible.
    if (CanUseExtraThreads())
        updateAllCellPointersParallel(&trc, zone);
    else
        updateAllCellPointersSerial(&trc, zone);
}

void
GCRuntime::protectAndHoldArenas(ArenaHeader* arenaList)
{
    for (ArenaHeader* arena = arenaList; arena; ) {
        MOZ_ASSERT(arena->allocated());
        ArenaHeader* next = arena->next;
        if (!next) {
            // Prepend to hold list before we protect the memory.
            arena->next = relocatedArenasToRelease;
            relocatedArenasToRelease = arenaList;
        }
        ProtectPages(arena, ArenaSize);
        arena = next;
    }
}

void
GCRuntime::unprotectHeldRelocatedArenas()
{
    for (ArenaHeader* arena = relocatedArenasToRelease; arena; arena = arena->next) {
        UnprotectPages(arena, ArenaSize);
        MOZ_ASSERT(arena->allocated());
    }
}

void
GCRuntime::releaseRelocatedArenas(ArenaHeader* arenaList)
{
    AutoLockGC lock(rt);
    releaseRelocatedArenasWithoutUnlocking(arenaList, lock);
    expireChunksAndArenas(true, lock);
}

void
GCRuntime::releaseRelocatedArenasWithoutUnlocking(ArenaHeader* arenaList, const AutoLockGC& lock)
{
    // Release the relocated arenas, now containing only forwarding pointers
    unsigned count = 0;
    while (arenaList) {
        ArenaHeader* aheader = arenaList;
        arenaList = arenaList->next;

        // Clear the mark bits
        aheader->unmarkAll();

        // Mark arena as empty
        AllocKind thingKind = aheader->getAllocKind();
        size_t thingSize = aheader->getThingSize();
        Arena* arena = aheader->getArena();
        FreeSpan fullSpan;
        fullSpan.initFinal(arena->thingsStart(thingKind), arena->thingsEnd() - thingSize, thingSize);
        aheader->setFirstFreeSpan(&fullSpan);

#if defined(JS_CRASH_DIAGNOSTICS) || defined(JS_GC_ZEAL)
        JS_POISON(reinterpret_cast<void*>(arena->thingsStart(thingKind)),
                  JS_MOVED_TENURED_PATTERN, Arena::thingsSpan(thingSize));
#endif

        releaseArena(aheader, lock);
        ++count;
    }
}

// In debug mode we don't always release relocated arenas straight away.
// Sometimes protect them instead and hold onto them until the next GC sweep
// phase to catch any pointers to them that didn't get forwarded.

void
GCRuntime::releaseHeldRelocatedArenas()
{
#ifdef DEBUG
    unprotectHeldRelocatedArenas();
    releaseRelocatedArenas(relocatedArenasToRelease);
    relocatedArenasToRelease = nullptr;
#endif
}

void
GCRuntime::releaseHeldRelocatedArenasWithoutUnlocking(const AutoLockGC& lock)
{
#ifdef DEBUG
    unprotectHeldRelocatedArenas();
    releaseRelocatedArenasWithoutUnlocking(relocatedArenasToRelease, lock);
    relocatedArenasToRelease = nullptr;
#endif
}

void
ReleaseArenaList(JSRuntime* rt, ArenaHeader* aheader, const AutoLockGC& lock)
{
    ArenaHeader* next;
    for (; aheader; aheader = next) {
        next = aheader->next;
        rt->gc.releaseArena(aheader, lock);
    }
}

ArenaLists::~ArenaLists()
{
    AutoLockGC lock(runtime_);

    for (auto i : AllAllocKinds()) {
        /*
         * We can only call this during the shutdown after the last GC when
         * the background finalization is disabled.
         */
        MOZ_ASSERT(backgroundFinalizeState[i] == BFS_DONE);
        ReleaseArenaList(runtime_, arenaLists[i].head(), lock);
    }
    ReleaseArenaList(runtime_, incrementalSweptArenas.head(), lock);

    for (auto i : ObjectAllocKinds())
        ReleaseArenaList(runtime_, savedObjectArenas[i].head(), lock);
    ReleaseArenaList(runtime_, savedEmptyObjectArenas, lock);
}

void
ArenaLists::finalizeNow(FreeOp* fop, const FinalizePhase& phase)
{
    gcstats::AutoPhase ap(fop->runtime()->gc.stats, phase.statsPhase);
    for (unsigned i = 0; i < phase.length; ++i)
        finalizeNow(fop, phase.kinds[i], RELEASE_ARENAS, nullptr);
}

void
ArenaLists::finalizeNow(FreeOp* fop, AllocKind thingKind, KeepArenasEnum keepArenas, ArenaHeader** empty)
{
    MOZ_ASSERT(!IsBackgroundFinalized(thingKind));
    forceFinalizeNow(fop, thingKind, keepArenas, empty);
}

void
ArenaLists::forceFinalizeNow(FreeOp* fop, AllocKind thingKind, KeepArenasEnum keepArenas, ArenaHeader** empty)
{
    MOZ_ASSERT(backgroundFinalizeState[thingKind] == BFS_DONE);

    ArenaHeader* arenas = arenaLists[thingKind].head();
    if (!arenas)
        return;
    arenaLists[thingKind].clear();

    size_t thingsPerArena = Arena::thingsPerArena(Arena::thingSize(thingKind));
    SortedArenaList finalizedSorted(thingsPerArena);

    auto unlimited = SliceBudget::unlimited();
    FinalizeArenas(fop, &arenas, finalizedSorted, thingKind, unlimited, keepArenas);
    MOZ_ASSERT(!arenas);

    if (empty) {
        MOZ_ASSERT(keepArenas == KEEP_ARENAS);
        finalizedSorted.extractEmpty(empty);
    }

    arenaLists[thingKind] = finalizedSorted.toArenaList();
}

void
ArenaLists::queueForForegroundSweep(FreeOp* fop, const FinalizePhase& phase)
{
    gcstats::AutoPhase ap(fop->runtime()->gc.stats, phase.statsPhase);
    for (unsigned i = 0; i < phase.length; ++i)
        queueForForegroundSweep(fop, phase.kinds[i]);
}

void
ArenaLists::queueForForegroundSweep(FreeOp* fop, AllocKind thingKind)
{
    MOZ_ASSERT(!IsBackgroundFinalized(thingKind));
    MOZ_ASSERT(backgroundFinalizeState[thingKind] == BFS_DONE);
    MOZ_ASSERT(!arenaListsToSweep[thingKind]);

    arenaListsToSweep[thingKind] = arenaLists[thingKind].head();
    arenaLists[thingKind].clear();
}

void
ArenaLists::queueForBackgroundSweep(FreeOp* fop, const FinalizePhase& phase)
{
    gcstats::AutoPhase ap(fop->runtime()->gc.stats, phase.statsPhase);
    for (unsigned i = 0; i < phase.length; ++i)
        queueForBackgroundSweep(fop, phase.kinds[i]);
}

inline void
ArenaLists::queueForBackgroundSweep(FreeOp* fop, AllocKind thingKind)
{
    MOZ_ASSERT(IsBackgroundFinalized(thingKind));

    ArenaList* al = &arenaLists[thingKind];
    if (al->isEmpty()) {
        MOZ_ASSERT(backgroundFinalizeState[thingKind] == BFS_DONE);
        return;
    }

    MOZ_ASSERT(backgroundFinalizeState[thingKind] == BFS_DONE);

    arenaListsToSweep[thingKind] = al->head();
    al->clear();
    backgroundFinalizeState[thingKind] = BFS_RUN;
}

/*static*/ void
ArenaLists::backgroundFinalize(FreeOp* fop, ArenaHeader* listHead, ArenaHeader** empty)
{
    MOZ_ASSERT(listHead);
    MOZ_ASSERT(empty);

    AllocKind thingKind = listHead->getAllocKind();
    Zone* zone = listHead->zone;

    size_t thingsPerArena = Arena::thingsPerArena(Arena::thingSize(thingKind));
    SortedArenaList finalizedSorted(thingsPerArena);

    auto unlimited = SliceBudget::unlimited();
    FinalizeArenas(fop, &listHead, finalizedSorted, thingKind, unlimited, KEEP_ARENAS);
    MOZ_ASSERT(!listHead);

    finalizedSorted.extractEmpty(empty);

    // When arenas are queued for background finalization, all arenas are moved
    // to arenaListsToSweep[], leaving the arenaLists[] empty. However, new
    // arenas may be allocated before background finalization finishes; now that
    // finalization is complete, we want to merge these lists back together.
    ArenaLists* lists = &zone->arenas;
    ArenaList* al = &lists->arenaLists[thingKind];

    // Flatten |finalizedSorted| into a regular ArenaList.
    ArenaList finalized = finalizedSorted.toArenaList();

    // We must take the GC lock to be able to safely modify the ArenaList;
    // however, this does not by itself make the changes visible to all threads,
    // as not all threads take the GC lock to read the ArenaLists.
    // That safety is provided by the ReleaseAcquire memory ordering of the
    // background finalize state, which we explicitly set as the final step.
    {
        AutoLockGC lock(fop->runtime());
        MOZ_ASSERT(lists->backgroundFinalizeState[thingKind] == BFS_RUN);

        // Join |al| and |finalized| into a single list.
        *al = finalized.insertListWithCursorAtEnd(*al);

        lists->arenaListsToSweep[thingKind] = nullptr;
    }

    lists->backgroundFinalizeState[thingKind] = BFS_DONE;
}

void
ArenaLists::queueForegroundObjectsForSweep(FreeOp* fop)
{
    gcstats::AutoPhase ap(fop->runtime()->gc.stats, gcstats::PHASE_SWEEP_OBJECT);

#ifdef DEBUG
    for (auto i : ObjectAllocKinds()) { // Braces needed to appease MSVC 2013.
        MOZ_ASSERT(savedObjectArenas[i].isEmpty());
    }
    MOZ_ASSERT(savedEmptyObjectArenas == nullptr);
#endif

    // Foreground finalized objects must be finalized at the beginning of the
    // sweep phase, before control can return to the mutator. Otherwise,
    // mutator behavior can resurrect certain objects whose references would
    // otherwise have been erased by the finalizer.
    finalizeNow(fop, AllocKind::OBJECT0, KEEP_ARENAS, &savedEmptyObjectArenas);
    finalizeNow(fop, AllocKind::OBJECT2, KEEP_ARENAS, &savedEmptyObjectArenas);
    finalizeNow(fop, AllocKind::OBJECT4, KEEP_ARENAS, &savedEmptyObjectArenas);
    finalizeNow(fop, AllocKind::OBJECT8, KEEP_ARENAS, &savedEmptyObjectArenas);
    finalizeNow(fop, AllocKind::OBJECT12, KEEP_ARENAS, &savedEmptyObjectArenas);
    finalizeNow(fop, AllocKind::OBJECT16, KEEP_ARENAS, &savedEmptyObjectArenas);

    // Prevent the arenas from having new objects allocated into them. We need
    // to know which objects are marked while we incrementally sweep dead
    // references from type information.
    savedObjectArenas[AllocKind::OBJECT0] = arenaLists[AllocKind::OBJECT0].copyAndClear();
    savedObjectArenas[AllocKind::OBJECT2] = arenaLists[AllocKind::OBJECT2].copyAndClear();
    savedObjectArenas[AllocKind::OBJECT4] = arenaLists[AllocKind::OBJECT4].copyAndClear();
    savedObjectArenas[AllocKind::OBJECT8] = arenaLists[AllocKind::OBJECT8].copyAndClear();
    savedObjectArenas[AllocKind::OBJECT12] = arenaLists[AllocKind::OBJECT12].copyAndClear();
    savedObjectArenas[AllocKind::OBJECT16] = arenaLists[AllocKind::OBJECT16].copyAndClear();
}

void
ArenaLists::mergeForegroundSweptObjectArenas()
{
    AutoLockGC lock(runtime_);
    ReleaseArenaList(runtime_, savedEmptyObjectArenas, lock);
    savedEmptyObjectArenas = nullptr;

    mergeSweptArenas(AllocKind::OBJECT0);
    mergeSweptArenas(AllocKind::OBJECT2);
    mergeSweptArenas(AllocKind::OBJECT4);
    mergeSweptArenas(AllocKind::OBJECT8);
    mergeSweptArenas(AllocKind::OBJECT12);
    mergeSweptArenas(AllocKind::OBJECT16);
}

inline void
ArenaLists::mergeSweptArenas(AllocKind thingKind)
{
    ArenaList* al = &arenaLists[thingKind];
    ArenaList* saved = &savedObjectArenas[thingKind];

    *al = saved->insertListWithCursorAtEnd(*al);
    saved->clear();
}

void
ArenaLists::queueForegroundThingsForSweep(FreeOp* fop)
{
    gcShapeArenasToUpdate = arenaListsToSweep[AllocKind::SHAPE];
    gcAccessorShapeArenasToUpdate = arenaListsToSweep[AllocKind::ACCESSOR_SHAPE];
    gcObjectGroupArenasToUpdate = arenaListsToSweep[AllocKind::OBJECT_GROUP];
    gcScriptArenasToUpdate = arenaListsToSweep[AllocKind::SCRIPT];
}

/* static */ void*
GCRuntime::refillFreeListInGC(Zone* zone, AllocKind thingKind)
{
    /*
     * Called by compacting GC to refill a free list while we are in a GC.
     */

    MOZ_ASSERT(zone->arenas.freeLists[thingKind].isEmpty());
    mozilla::DebugOnly<JSRuntime*> rt = zone->runtimeFromMainThread();
    MOZ_ASSERT(rt->isHeapMajorCollecting());
    MOZ_ASSERT(!rt->gc.isBackgroundSweeping());

    AutoMaybeStartBackgroundAllocation maybeStartBackgroundAllocation;
    return zone->arenas.allocateFromArena(zone, thingKind, maybeStartBackgroundAllocation);
}

SliceBudget::SliceBudget()
  : timeBudget(UnlimitedTimeBudget), workBudget(UnlimitedWorkBudget)
{
    makeUnlimited();
}

SliceBudget::SliceBudget(TimeBudget time)
  : timeBudget(time), workBudget(UnlimitedWorkBudget)
{
    if (time.budget < 0) {
        makeUnlimited();
    } else {
        // Note: TimeBudget(0) is equivalent to WorkBudget(CounterReset).
        deadline = PRMJ_Now() + time.budget * PRMJ_USEC_PER_MSEC;
        counter = CounterReset;
    }
}

SliceBudget::SliceBudget(WorkBudget work)
  : timeBudget(UnlimitedTimeBudget), workBudget(work)
{
    if (work.budget < 0) {
        makeUnlimited();
    } else {
        deadline = 0;
        counter = work.budget;
    }
}

int
SliceBudget::describe(char* buffer, size_t maxlen) const
{
    if (isUnlimited())
        return JS_snprintf(buffer, maxlen, "unlimited");
    else if (isWorkBudget())
        return JS_snprintf(buffer, maxlen, "work(%lld)", workBudget.budget);
    else
        return JS_snprintf(buffer, maxlen, "%lldms", timeBudget.budget);
}

bool
SliceBudget::checkOverBudget()
{
    bool over = PRMJ_Now() >= deadline;
    if (!over)
        counter = CounterReset;
    return over;
}

void
js::MarkCompartmentActive(InterpreterFrame* fp)
{
    fp->script()->compartment()->zone()->active = true;
}

void
GCRuntime::requestMajorGC(JS::gcreason::Reason reason)
{
    if (majorGCRequested())
        return;

    majorGCTriggerReason = reason;
    rt->requestInterrupt(JSRuntime::RequestInterruptUrgent);
}

void
GCRuntime::requestMinorGC(JS::gcreason::Reason reason)
{
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
    if (minorGCRequested())
        return;

    minorGCTriggerReason = reason;
    rt->requestInterrupt(JSRuntime::RequestInterruptUrgent);
}

bool
GCRuntime::triggerGC(JS::gcreason::Reason reason)
{
    /*
     * Don't trigger GCs if this is being called off the main thread from
     * onTooMuchMalloc().
     */
    if (!CurrentThreadCanAccessRuntime(rt))
        return false;

    /* GC is already running. */
    if (rt->isHeapCollecting())
        return false;

    JS::PrepareForFullGC(rt);
    requestMajorGC(reason);
    return true;
}

void
GCRuntime::maybeAllocTriggerZoneGC(Zone* zone, const AutoLockGC& lock)
{
    size_t usedBytes = zone->usage.gcBytes();
    size_t thresholdBytes = zone->threshold.gcTriggerBytes();
    size_t igcThresholdBytes = thresholdBytes * tunables.zoneAllocThresholdFactor();

    if (usedBytes >= thresholdBytes) {
        // The threshold has been surpassed, immediately trigger a GC,
        // which will be done non-incrementally.
        triggerZoneGC(zone, JS::gcreason::ALLOC_TRIGGER);
    } else if (usedBytes >= igcThresholdBytes) {
        // Reduce the delay to the start of the next incremental slice.
        if (zone->gcDelayBytes < ArenaSize)
            zone->gcDelayBytes = 0;
        else
            zone->gcDelayBytes -= ArenaSize;

        if (!zone->gcDelayBytes) {
            // Start or continue an in progress incremental GC. We do this
            // to try to avoid performing non-incremental GCs on zones
            // which allocate a lot of data, even when incremental slices
            // can't be triggered via scheduling in the event loop.
            triggerZoneGC(zone, JS::gcreason::ALLOC_TRIGGER);

            // Delay the next slice until a certain amount of allocation
            // has been performed.
            zone->gcDelayBytes = tunables.zoneAllocDelayBytes();
        }
    }
}

bool
GCRuntime::triggerZoneGC(Zone* zone, JS::gcreason::Reason reason)
{
    /* Zones in use by a thread with an exclusive context can't be collected. */
    if (!CurrentThreadCanAccessRuntime(rt)) {
        MOZ_ASSERT(zone->usedByExclusiveThread || zone->isAtomsZone());
        return false;
    }

    /* GC is already running. */
    if (rt->isHeapCollecting())
        return false;

#ifdef JS_GC_ZEAL
    if (zealMode == ZealAllocValue) {
        triggerGC(reason);
        return true;
    }
#endif

    if (zone->isAtomsZone()) {
        /* We can't do a zone GC of the atoms compartment. */
        if (rt->keepAtoms()) {
            /* Skip GC and retrigger later, since atoms zone won't be collected
             * if keepAtoms is true. */
            fullGCForAtomsRequested_ = true;
            return false;
        }
        triggerGC(reason);
        return true;
    }

    PrepareZoneForGC(zone);
    requestMajorGC(reason);
    return true;
}

bool
GCRuntime::maybeGC(Zone* zone)
{
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));

#ifdef JS_GC_ZEAL
    if (zealMode == ZealAllocValue || zealMode == ZealPokeValue) {
        JS::PrepareForFullGC(rt);
        gc(GC_NORMAL, JS::gcreason::DEBUG_GC);
        return true;
    }
#endif

    if (gcIfRequested())
        return true;

    if (zone->usage.gcBytes() > 1024 * 1024 &&
        zone->usage.gcBytes() >= zone->threshold.allocTrigger(schedulingState.inHighFrequencyGCMode()) &&
        !isIncrementalGCInProgress() &&
        !isBackgroundSweeping())
    {
        PrepareZoneForGC(zone);
        startGC(GC_NORMAL, JS::gcreason::EAGER_ALLOC_TRIGGER);
        return true;
    }

    return false;
}

void
GCRuntime::maybePeriodicFullGC()
{
    /*
     * Trigger a periodic full GC.
     *
     * This is a source of non-determinism, but is not called from the shell.
     *
     * Access to the counters and, on 32 bit, setting gcNextFullGCTime below
     * is not atomic and a race condition could trigger or suppress the GC. We
     * tolerate this.
     */
#ifndef JS_MORE_DETERMINISTIC
    int64_t now = PRMJ_Now();
    if (nextFullGCTime && nextFullGCTime <= now && !isIncrementalGCInProgress()) {
        if (chunkAllocationSinceLastGC ||
            numArenasFreeCommitted > decommitThreshold)
        {
            JS::PrepareForFullGC(rt);
            startGC(GC_SHRINK, JS::gcreason::PERIODIC_FULL_GC);
        } else {
            nextFullGCTime = now + GC_IDLE_FULL_SPAN;
        }
    }
#endif
}

// Do all possible decommit immediately from the current thread without
// releasing the GC lock or allocating any memory.
void
GCRuntime::decommitAllWithoutUnlocking(const AutoLockGC& lock)
{
    MOZ_ASSERT(emptyChunks(lock).count() == 0);
    for (ChunkPool::Iter chunk(availableChunks(lock)); !chunk.done(); chunk.next())
        chunk->decommitAllArenasWithoutUnlocking(lock);
    MOZ_ASSERT(availableChunks(lock).verify());
}

void
GCRuntime::decommitArenas(AutoLockGC& lock)
{
    // Verify that all entries in the empty chunks pool are decommitted.
    for (ChunkPool::Iter chunk(emptyChunks(lock)); !chunk.done(); chunk.next())
        MOZ_ASSERT(!chunk->info.numArenasFreeCommitted);

    // Build a Vector of all current available Chunks. Since we release the
    // gc lock while doing the decommit syscall, it is dangerous to iterate
    // the available list directly, as concurrent operations can modify it.
    mozilla::Vector<Chunk*> toDecommit;
    MOZ_ASSERT(availableChunks(lock).verify());
    for (ChunkPool::Iter iter(availableChunks(lock)); !iter.done(); iter.next()) {
        if (!toDecommit.append(iter.get())) {
            // The OOM handler does a full, immediate decommit, so there is
            // nothing more to do here in any case.
            return onOutOfMallocMemory(lock);
        }
    }

    // Start at the tail and stop before the first chunk: we allocate from the
    // head and don't want to thrash with the mutator.
    for (size_t i = toDecommit.length(); i > 1; --i) {
        Chunk* chunk = toDecommit[i - 1];
        MOZ_ASSERT(chunk);

        // The arena list is not doubly-linked, so we have to work in the free
        // list order and not in the natural order.
        while (chunk->info.numArenasFreeCommitted) {
            bool ok = chunk->decommitOneFreeArena(rt, lock);

            // FIXME Bug 1095620: add cancellation support when this becomes
            // a ParallelTask.
            if (/* cancel_ || */ !ok)
                return;
        }
    }
    MOZ_ASSERT(availableChunks(lock).verify());
}

void
GCRuntime::expireChunksAndArenas(bool shouldShrink, AutoLockGC& lock)
{
    ChunkPool toFree = expireEmptyChunkPool(shouldShrink, lock);
    if (toFree.count()) {
        AutoUnlockGC unlock(lock);
        FreeChunkPool(rt, toFree);
    }

    if (shouldShrink)
        decommitArenas(lock);
}

void
GCRuntime::sweepBackgroundThings(ZoneList& zones, LifoAlloc& freeBlocks, ThreadType threadType)
{
    freeBlocks.freeAll();

    if (zones.isEmpty())
        return;

    // We must finalize thing kinds in the order specified by BackgroundFinalizePhases.
    ArenaHeader* emptyArenas = nullptr;
    FreeOp fop(rt, threadType);
    for (unsigned phase = 0 ; phase < ArrayLength(BackgroundFinalizePhases) ; ++phase) {
        for (Zone* zone = zones.front(); zone; zone = zone->nextZone()) {
            for (unsigned index = 0 ; index < BackgroundFinalizePhases[phase].length ; ++index) {
                AllocKind kind = BackgroundFinalizePhases[phase].kinds[index];
                ArenaHeader* arenas = zone->arenas.arenaListsToSweep[kind];
                if (arenas)
                    ArenaLists::backgroundFinalize(&fop, arenas, &emptyArenas);
            }
        }
    }

    AutoLockGC lock(rt);
    ReleaseArenaList(rt, emptyArenas, lock);
    while (!zones.isEmpty())
        zones.removeFront();
}

void
GCRuntime::assertBackgroundSweepingFinished()
{
#ifdef DEBUG
    MOZ_ASSERT(backgroundSweepZones.isEmpty());
    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        for (auto i : AllAllocKinds()) {
            MOZ_ASSERT(!zone->arenas.arenaListsToSweep[i]);
            MOZ_ASSERT(zone->arenas.doneBackgroundFinalize(i));
        }
    }
    MOZ_ASSERT(freeLifoAlloc.computedSizeOfExcludingThis() == 0);
#endif
}

unsigned
js::GetCPUCount()
{
    static unsigned ncpus = 0;
    if (ncpus == 0) {
# ifdef XP_WIN
        SYSTEM_INFO sysinfo;
        GetSystemInfo(&sysinfo);
        ncpus = unsigned(sysinfo.dwNumberOfProcessors);
# else
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        ncpus = (n > 0) ? unsigned(n) : 1;
# endif
    }
    return ncpus;
}

bool
GCHelperState::init()
{
    if (!(done = PR_NewCondVar(rt->gc.lock)))
        return false;

    return true;
}

void
GCHelperState::finish()
{
    if (!rt->gc.lock) {
        MOZ_ASSERT(state_ == IDLE);
        return;
    }

    // Wait for any lingering background sweeping to finish.
    waitBackgroundSweepEnd();

    if (done)
        PR_DestroyCondVar(done);
}

GCHelperState::State
GCHelperState::state()
{
    MOZ_ASSERT(rt->gc.currentThreadOwnsGCLock());
    return state_;
}

void
GCHelperState::setState(State state)
{
    MOZ_ASSERT(rt->gc.currentThreadOwnsGCLock());
    state_ = state;
}

void
GCHelperState::startBackgroundThread(State newState)
{
    MOZ_ASSERT(!thread && state() == IDLE && newState != IDLE);
    setState(newState);

    {
        AutoEnterOOMUnsafeRegion noOOM;
        if (!HelperThreadState().gcHelperWorklist().append(this))
            noOOM.crash("Could not add to pending GC helpers list");
    }

    HelperThreadState().notifyAll(GlobalHelperThreadState::PRODUCER);
}

void
GCHelperState::waitForBackgroundThread()
{
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));

#ifdef DEBUG
    rt->gc.lockOwner.value = nullptr;
#endif
    PR_WaitCondVar(done, PR_INTERVAL_NO_TIMEOUT);
#ifdef DEBUG
    rt->gc.lockOwner.value = PR_GetCurrentThread();
#endif
}

void
GCHelperState::work()
{
    MOZ_ASSERT(CanUseExtraThreads());

    AutoLockGC lock(rt);

    MOZ_ASSERT(!thread);
    thread = PR_GetCurrentThread();

    TraceLoggerThread* logger = TraceLoggerForCurrentThread();

    switch (state()) {

      case IDLE:
        MOZ_CRASH("GC helper triggered on idle state");
        break;

      case SWEEPING: {
        AutoTraceLog logSweeping(logger, TraceLogger_GCSweeping);
        doSweep(lock);
        MOZ_ASSERT(state() == SWEEPING);
        break;
      }

    }

    setState(IDLE);
    thread = nullptr;

    PR_NotifyAllCondVar(done);
}

BackgroundAllocTask::BackgroundAllocTask(JSRuntime* rt, ChunkPool& pool)
  : runtime(rt),
    chunkPool_(pool),
    enabled_(CanUseExtraThreads() && GetCPUCount() >= 2)
{
}

/* virtual */ void
BackgroundAllocTask::run()
{
    TraceLoggerThread* logger = TraceLoggerForCurrentThread();
    AutoTraceLog logAllocation(logger, TraceLogger_GCAllocation);

    AutoLockGC lock(runtime);
    while (!cancel_ && runtime->gc.wantBackgroundAllocation(lock)) {
        Chunk* chunk;
        {
            AutoUnlockGC unlock(lock);
            chunk = Chunk::allocate(runtime);
            if (!chunk)
                break;
        }
        chunkPool_.push(chunk);
    }
}

void
GCRuntime::queueZonesForBackgroundSweep(ZoneList& zones)
{
    AutoLockHelperThreadState helperLock;
    AutoLockGC lock(rt);
    backgroundSweepZones.transferFrom(zones);
    helperState.maybeStartBackgroundSweep(lock);
}

void
GCRuntime::freeUnusedLifoBlocksAfterSweeping(LifoAlloc* lifo)
{
    MOZ_ASSERT(rt->isHeapBusy());
    AutoLockGC lock(rt);
    freeLifoAlloc.transferUnusedFrom(lifo);
}

void
GCRuntime::freeAllLifoBlocksAfterSweeping(LifoAlloc* lifo)
{
    MOZ_ASSERT(rt->isHeapBusy());
    AutoLockGC lock(rt);
    freeLifoAlloc.transferFrom(lifo);
}

void
GCHelperState::maybeStartBackgroundSweep(const AutoLockGC& lock)
{
    MOZ_ASSERT(CanUseExtraThreads());

    if (state() == IDLE)
        startBackgroundThread(SWEEPING);
}

void
GCHelperState::startBackgroundShrink(const AutoLockGC& lock)
{
    MOZ_ASSERT(CanUseExtraThreads());
    switch (state()) {
      case IDLE:
        shrinkFlag = true;
        startBackgroundThread(SWEEPING);
        break;
      case SWEEPING:
        shrinkFlag = true;
        break;
      default:
        MOZ_CRASH("Invalid GC helper thread state.");
    }
}

void
GCHelperState::waitBackgroundSweepEnd()
{
    AutoLockGC lock(rt);
    while (state() == SWEEPING)
        waitForBackgroundThread();
    if (!rt->gc.isIncrementalGCInProgress())
        rt->gc.assertBackgroundSweepingFinished();
}

void
GCHelperState::doSweep(AutoLockGC& lock)
{
    // The main thread may call queueZonesForBackgroundSweep() or
    // ShrinkGCBuffers() while this is running so we must check there is no more
    // work to do before exiting.

    do {
        while (!rt->gc.backgroundSweepZones.isEmpty()) {
            AutoSetThreadIsSweeping threadIsSweeping;

            ZoneList zones;
            zones.transferFrom(rt->gc.backgroundSweepZones);
            LifoAlloc freeLifoAlloc(JSRuntime::TEMP_LIFO_ALLOC_PRIMARY_CHUNK_SIZE);
            freeLifoAlloc.transferFrom(&rt->gc.freeLifoAlloc);

            AutoUnlockGC unlock(lock);
            rt->gc.sweepBackgroundThings(zones, freeLifoAlloc, BackgroundThread);
        }

        bool shrinking = shrinkFlag;
        shrinkFlag = false;
        rt->gc.expireChunksAndArenas(shrinking, lock);
    } while (!rt->gc.backgroundSweepZones.isEmpty() || shrinkFlag);
}

bool
GCHelperState::onBackgroundThread()
{
    return PR_GetCurrentThread() == thread;
}

bool
GCRuntime::shouldReleaseObservedTypes()
{
    bool releaseTypes = false;

#ifdef JS_GC_ZEAL
    if (zealMode != 0)
        releaseTypes = true;
#endif

    /* We may miss the exact target GC due to resets. */
    if (majorGCNumber >= jitReleaseNumber)
        releaseTypes = true;

    if (releaseTypes)
        jitReleaseNumber = majorGCNumber + JIT_SCRIPT_RELEASE_TYPES_PERIOD;

    return releaseTypes;
}

struct IsAboutToBeFinalizedFunctor {
    template <typename T> bool operator()(Cell** t) {
        mozilla::DebugOnly<const Cell*> prior = *t;
        bool result = IsAboutToBeFinalizedUnbarriered(reinterpret_cast<T**>(t));
        // Sweep should not have to deal with moved pointers, since moving GC
        // handles updating the UID table manually.
        MOZ_ASSERT(*t == prior);
        return result;
    }
};

/* static */ bool
UniqueIdGCPolicy::needsSweep(Cell** cell, uint64_t*)
{
    return DispatchTraceKindTyped(IsAboutToBeFinalizedFunctor(), (*cell)->getTraceKind(), cell);
}

void
JS::Zone::sweepUniqueIds(js::FreeOp* fop)
{
    uniqueIds_.sweep();
}

/*
 * It's simpler if we preserve the invariant that every zone has at least one
 * compartment. If we know we're deleting the entire zone, then
 * SweepCompartments is allowed to delete all compartments. In this case,
 * |keepAtleastOne| is false. If some objects remain in the zone so that it
 * cannot be deleted, then we set |keepAtleastOne| to true, which prohibits
 * SweepCompartments from deleting every compartment. Instead, it preserves an
 * arbitrary compartment in the zone.
 */
void
Zone::sweepCompartments(FreeOp* fop, bool keepAtleastOne, bool destroyingRuntime)
{
    JSRuntime* rt = runtimeFromMainThread();
    JSDestroyCompartmentCallback callback = rt->destroyCompartmentCallback;

    JSCompartment** read = compartments.begin();
    JSCompartment** end = compartments.end();
    JSCompartment** write = read;
    bool foundOne = false;
    while (read < end) {
        JSCompartment* comp = *read++;
        MOZ_ASSERT(!rt->isAtomsCompartment(comp));

        /*
         * Don't delete the last compartment if all the ones before it were
         * deleted and keepAtleastOne is true.
         */
        bool dontDelete = read == end && !foundOne && keepAtleastOne;
        if ((!comp->marked && !dontDelete) || destroyingRuntime) {
            if (callback)
                callback(fop, comp);
            if (comp->principals())
                JS_DropPrincipals(rt, comp->principals());
            js_delete(comp);
        } else {
            *write++ = comp;
            foundOne = true;
        }
    }
    compartments.resize(write - compartments.begin());
    MOZ_ASSERT_IF(keepAtleastOne, !compartments.empty());
}

void
GCRuntime::sweepZones(FreeOp* fop, bool destroyingRuntime)
{
    MOZ_ASSERT_IF(destroyingRuntime, rt->gc.numActiveZoneIters == 0);
    if (rt->gc.numActiveZoneIters)
        return;

    AutoLockGC lock(rt); // Avoid race with background sweeping.

    JSZoneCallback callback = rt->destroyZoneCallback;

    /* Skip the atomsCompartment zone. */
    Zone** read = zones.begin() + 1;
    Zone** end = zones.end();
    Zone** write = read;
    MOZ_ASSERT(zones.length() >= 1);
    MOZ_ASSERT(zones[0]->isAtomsZone());

    while (read < end) {
        Zone* zone = *read++;

        if (zone->wasGCStarted()) {
            if ((!zone->isQueuedForBackgroundSweep() &&
                 zone->arenas.arenaListsAreEmpty() &&
                 !zone->hasMarkedCompartments()) || destroyingRuntime)
            {
                zone->arenas.checkEmptyFreeLists();
                AutoUnlockGC unlock(lock);

                if (callback)
                    callback(zone);
                zone->sweepCompartments(fop, false, destroyingRuntime);
                MOZ_ASSERT(zone->compartments.empty());
                fop->delete_(zone);
                continue;
            }
            zone->sweepCompartments(fop, true, destroyingRuntime);
        }
        *write++ = zone;
    }
    zones.resize(write - zones.begin());
}

void
GCRuntime::purgeRuntime()
{
    for (GCCompartmentsIter comp(rt); !comp.done(); comp.next())
        comp->purge();

    freeUnusedLifoBlocksAfterSweeping(&rt->tempLifoAlloc);

    rt->interpreterStack().purge(rt);
    rt->gsnCache.purge();
    rt->scopeCoordinateNameCache.purge();
    rt->newObjectCache.purge();
    rt->nativeIterCache.purge();
    rt->uncompressedSourceCache.purge();
    rt->evalCache.clear();

    if (!rt->hasActiveCompilations())
        rt->parseMapPool().purgeAll();
}

bool
GCRuntime::shouldPreserveJITCode(JSCompartment* comp, int64_t currentTime,
                                 JS::gcreason::Reason reason)
{
    if (cleanUpEverything)
        return false;

    if (alwaysPreserveCode)
        return true;
    if (comp->preserveJitCode())
        return true;
    if (comp->lastAnimationTime + PRMJ_USEC_PER_SEC >= currentTime)
        return true;
    if (reason == JS::gcreason::DEBUG_GC)
        return true;

    return false;
}

#ifdef DEBUG
class CompartmentCheckTracer : public JS::CallbackTracer
{
    void onChild(const JS::GCCellPtr& thing) override;

  public:
    explicit CompartmentCheckTracer(JSRuntime* rt)
      : JS::CallbackTracer(rt), src(nullptr), zone(nullptr), compartment(nullptr)
    {}

    Cell* src;
    JS::TraceKind srcKind;
    Zone* zone;
    JSCompartment* compartment;
};

static bool
InCrossCompartmentMap(JSObject* src, Cell* dst, JS::TraceKind dstKind)
{
    JSCompartment* srccomp = src->compartment();

    if (dstKind == JS::TraceKind::Object) {
        Value key = ObjectValue(*static_cast<JSObject*>(dst));
        if (WrapperMap::Ptr p = srccomp->lookupWrapper(key)) {
            if (*p->value().unsafeGet() == ObjectValue(*src))
                return true;
        }
    }

    /*
     * If the cross-compartment edge is caused by the debugger, then we don't
     * know the right hashtable key, so we have to iterate.
     */
    for (JSCompartment::WrapperEnum e(srccomp); !e.empty(); e.popFront()) {
        if (e.front().key().wrapped == dst && ToMarkable(e.front().value()) == src)
            return true;
    }

    return false;
}

struct MaybeCompartmentFunctor {
    template <typename T> JSCompartment* operator()(T* t) { return t->maybeCompartment(); }
};

void
CompartmentCheckTracer::onChild(const JS::GCCellPtr& thing)
{
    TenuredCell* tenured = TenuredCell::fromPointer(thing.asCell());

    JSCompartment* comp = DispatchTyped(MaybeCompartmentFunctor(), thing);
    if (comp && compartment) {
        MOZ_ASSERT(comp == compartment || runtime()->isAtomsCompartment(comp) ||
                   (srcKind == JS::TraceKind::Object &&
                    InCrossCompartmentMap(static_cast<JSObject*>(src), tenured, thing.kind())));
    } else {
        MOZ_ASSERT(tenured->zone() == zone || tenured->zone()->isAtomsZone());
    }
}

void
GCRuntime::checkForCompartmentMismatches()
{
    if (disableStrictProxyCheckingCount)
        return;

    CompartmentCheckTracer trc(rt);
    for (ZonesIter zone(rt, SkipAtoms); !zone.done(); zone.next()) {
        trc.zone = zone;
        for (auto thingKind : AllAllocKinds()) {
            for (ZoneCellIterUnderGC i(zone, thingKind); !i.done(); i.next()) {
                trc.src = i.getCell();
                trc.srcKind = MapAllocToTraceKind(thingKind);
                trc.compartment = DispatchTraceKindTyped(MaybeCompartmentFunctor(),
                                                         trc.src, trc.srcKind);
                js::TraceChildren(&trc, trc.src, trc.srcKind);
            }
        }
    }
}
#endif

static void
RelazifyFunctions(Zone* zone, AllocKind kind)
{
    MOZ_ASSERT(kind == AllocKind::FUNCTION ||
               kind == AllocKind::FUNCTION_EXTENDED);

    JSRuntime* rt = zone->runtimeFromMainThread();

    for (ZoneCellIterUnderGC i(zone, kind); !i.done(); i.next()) {
        JSFunction* fun = &i.get<JSObject>()->as<JSFunction>();
        if (fun->hasScript())
            fun->maybeRelazify(rt);
    }
}

bool
GCRuntime::beginMarkPhase(JS::gcreason::Reason reason)
{
    int64_t currentTime = PRMJ_Now();

#ifdef DEBUG
    if (fullCompartmentChecks)
        checkForCompartmentMismatches();
#endif

    isFull = true;
    bool any = false;

    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        /* Assert that zone state is as we expect */
        MOZ_ASSERT(!zone->isCollecting());
        MOZ_ASSERT(!zone->compartments.empty());
#ifdef DEBUG
        for (auto i : AllAllocKinds()) { // Braces needed to appease MSVC 2013.
            MOZ_ASSERT(!zone->arenas.arenaListsToSweep[i]);
        }
#endif

        /* Set up which zones will be collected. */
        if (zone->isGCScheduled()) {
            if (!zone->isAtomsZone()) {
                any = true;
                zone->setGCState(Zone::Mark);
            }
        } else {
            isFull = false;
        }

        zone->setPreservingCode(false);
    }

    for (CompartmentsIter c(rt, WithAtoms); !c.done(); c.next()) {
        c->marked = false;
        c->scheduledForDestruction = false;
        c->maybeAlive = false;
        if (shouldPreserveJITCode(c, currentTime, reason))
            c->zone()->setPreservingCode(true);
    }

    if (!rt->gc.cleanUpEverything) {
        if (JSCompartment* comp = jit::TopmostIonActivationCompartment(rt))
            comp->zone()->setPreservingCode(true);
    }

    /*
     * Atoms are not in the cross-compartment map. So if there are any
     * zones that are not being collected, we are not allowed to collect
     * atoms. Otherwise, the non-collected zones could contain pointers
     * to atoms that we would miss.
     *
     * keepAtoms() will only change on the main thread, which we are currently
     * on. If the value of keepAtoms() changes between GC slices, then we'll
     * cancel the incremental GC. See IsIncrementalGCSafe.
     */
    if (isFull && !rt->keepAtoms()) {
        Zone* atomsZone = rt->atomsCompartment()->zone();
        if (atomsZone->isGCScheduled()) {
            MOZ_ASSERT(!atomsZone->isCollecting());
            atomsZone->setGCState(Zone::Mark);
            any = true;
        }
    }

    /* Check that at least one zone is scheduled for collection. */
    if (!any)
        return false;

    /*
     * At the end of each incremental slice, we call prepareForIncrementalGC,
     * which marks objects in all arenas that we're currently allocating
     * into. This can cause leaks if unreachable objects are in these
     * arenas. This purge call ensures that we only mark arenas that have had
     * allocations after the incremental GC started.
     */
    if (isIncremental) {
        for (GCZonesIter zone(rt); !zone.done(); zone.next())
            zone->arenas.purge();
    }

    MemProfiler::MarkTenuredStart(rt);
    marker.start();
    GCMarker* gcmarker = &marker;

    /* For non-incremental GC the following sweep discards the jit code. */
    if (isIncremental) {
        for (GCZonesIter zone(rt); !zone.done(); zone.next()) {
            gcstats::AutoPhase ap(stats, gcstats::PHASE_MARK_DISCARD_CODE);
            zone->discardJitCode(rt->defaultFreeOp());
        }
    }

    /*
     * Relazify functions after discarding JIT code (we can't relazify
     * functions with JIT code) and before the actual mark phase, so that
     * the current GC can collect the JSScripts we're unlinking here.
     */
    for (GCZonesIter zone(rt); !zone.done(); zone.next()) {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_RELAZIFY_FUNCTIONS);
        RelazifyFunctions(zone, AllocKind::FUNCTION);
        RelazifyFunctions(zone, AllocKind::FUNCTION_EXTENDED);
    }

    startNumber = number;

    /*
     * We must purge the runtime at the beginning of an incremental GC. The
     * danger if we purge later is that the snapshot invariant of incremental
     * GC will be broken, as follows. If some object is reachable only through
     * some cache (say the dtoaCache) then it will not be part of the snapshot.
     * If we purge after root marking, then the mutator could obtain a pointer
     * to the object and start using it. This object might never be marked, so
     * a GC hazard would exist.
     */
    {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_PURGE);
        purgeRuntime();
    }

    /*
     * Mark phase.
     */
    gcstats::AutoPhase ap1(stats, gcstats::PHASE_MARK);

    {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_UNMARK);

        for (GCZonesIter zone(rt); !zone.done(); zone.next()) {
            /* Unmark everything in the zones being collected. */
            zone->arenas.unmarkAll();
        }

        for (GCZonesIter zone(rt); !zone.done(); zone.next()) {
            /* Unmark all weak maps in the zones being collected. */
            WeakMapBase::unmarkZone(zone);
        }

        if (isFull)
            UnmarkScriptData(rt);
    }

    markRuntime(gcmarker, MarkRuntime);

    gcstats::AutoPhase ap2(stats, gcstats::PHASE_MARK_ROOTS);

    if (isIncremental) {
        gcstats::AutoPhase ap3(stats, gcstats::PHASE_BUFFER_GRAY_ROOTS);
        bufferGrayRoots();
    }

    markCompartments();

    foundBlackGrayEdges = false;

    return true;
}

void
GCRuntime::markCompartments()
{
    gcstats::AutoPhase ap(stats, gcstats::PHASE_MARK_COMPARTMENTS);

    /*
     * This code ensures that if a compartment is "dead", then it will be
     * collected in this GC. A compartment is considered dead if its maybeAlive
     * flag is false. The maybeAlive flag is set if:
     *   (1) the compartment has incoming cross-compartment edges, or
     *   (2) an object in the compartment was marked during root marking, either
     *       as a black root or a gray root.
     * If the maybeAlive is false, then we set the scheduledForDestruction flag.
     * At the end of the GC, we look for compartments where
     * scheduledForDestruction is true. These are compartments that were somehow
     * "revived" during the incremental GC. If any are found, we do a special,
     * non-incremental GC of those compartments to try to collect them.
     *
     * Compartments can be revived for a variety of reasons. On reason is bug
     * 811587, where a reflector that was dead can be revived by DOM code that
     * still refers to the underlying DOM node.
     *
     * Read barriers and allocations can also cause revival. This might happen
     * during a function like JS_TransplantObject, which iterates over all
     * compartments, live or dead, and operates on their objects. See bug 803376
     * for details on this problem. To avoid the problem, we try to avoid
     * allocation and read barriers during JS_TransplantObject and the like.
     */

    /* Set the maybeAlive flag based on cross-compartment edges. */
    for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next()) {
        for (JSCompartment::WrapperEnum e(c); !e.empty(); e.popFront()) {
            const CrossCompartmentKey& key = e.front().key();
            JSCompartment* dest;
            switch (key.kind) {
              case CrossCompartmentKey::ObjectWrapper:
              case CrossCompartmentKey::DebuggerObject:
              case CrossCompartmentKey::DebuggerSource:
              case CrossCompartmentKey::DebuggerEnvironment:
                dest = static_cast<JSObject*>(key.wrapped)->compartment();
                break;
              case CrossCompartmentKey::DebuggerScript:
                dest = static_cast<JSScript*>(key.wrapped)->compartment();
                break;
              default:
                dest = nullptr;
                break;
            }
            if (dest)
                dest->maybeAlive = true;
        }
    }

    /*
     * For black roots, code in gc/Marking.cpp will already have set maybeAlive
     * during MarkRuntime.
     */

    for (GCCompartmentsIter c(rt); !c.done(); c.next()) {
        if (!c->maybeAlive && !rt->isAtomsCompartment(c))
            c->scheduledForDestruction = true;
    }
}

template <class ZoneIterT>
void
GCRuntime::markWeakReferences(gcstats::Phase phase)
{
    MOZ_ASSERT(marker.isDrained());

    gcstats::AutoPhase ap1(stats, phase);

    marker.enterWeakMarkingMode();

    // TODO bug 1167452: Make weak marking incremental
    SliceBudget budget = SliceBudget::unlimited();
    marker.drainMarkStack(budget);

    for (;;) {
        bool markedAny = false;
        if (!marker.isWeakMarkingTracer()) {
            for (ZoneIterT zone(rt); !zone.done(); zone.next())
                markedAny |= WeakMapBase::markZoneIteratively(zone, &marker);
        }
        for (CompartmentsIterT<ZoneIterT> c(rt); !c.done(); c.next()) {
            if (c->watchpointMap)
                markedAny |= c->watchpointMap->markIteratively(&marker);
        }
        markedAny |= Debugger::markAllIteratively(&marker);
        markedAny |= jit::JitRuntime::MarkJitcodeGlobalTableIteratively(&marker);

        if (!markedAny)
            break;

        auto unlimited = SliceBudget::unlimited();
        marker.drainMarkStack(unlimited);
    }
    MOZ_ASSERT(marker.isDrained());

    marker.leaveWeakMarkingMode();
}

void
GCRuntime::markWeakReferencesInCurrentGroup(gcstats::Phase phase)
{
    markWeakReferences<GCZoneGroupIter>(phase);
}

template <class ZoneIterT, class CompartmentIterT>
void
GCRuntime::markGrayReferences(gcstats::Phase phase)
{
    gcstats::AutoPhase ap(stats, phase);
    if (hasBufferedGrayRoots()) {
        for (ZoneIterT zone(rt); !zone.done(); zone.next())
            markBufferedGrayRoots(zone);
    } else {
        MOZ_ASSERT(!isIncremental);
        if (JSTraceDataOp op = grayRootTracer.op)
            (*op)(&marker, grayRootTracer.data);
    }
    auto unlimited = SliceBudget::unlimited();
    marker.drainMarkStack(unlimited);
}

void
GCRuntime::markGrayReferencesInCurrentGroup(gcstats::Phase phase)
{
    markGrayReferences<GCZoneGroupIter, GCCompartmentGroupIter>(phase);
}

void
GCRuntime::markAllWeakReferences(gcstats::Phase phase)
{
    markWeakReferences<GCZonesIter>(phase);
}

void
GCRuntime::markAllGrayReferences(gcstats::Phase phase)
{
    markGrayReferences<GCZonesIter, GCCompartmentsIter>(phase);
}

#ifdef DEBUG

struct GCChunkHasher {
    typedef gc::Chunk* Lookup;

    /*
     * Strip zeros for better distribution after multiplying by the golden
     * ratio.
     */
    static HashNumber hash(gc::Chunk* chunk) {
        MOZ_ASSERT(!(uintptr_t(chunk) & gc::ChunkMask));
        return HashNumber(uintptr_t(chunk) >> gc::ChunkShift);
    }

    static bool match(gc::Chunk* k, gc::Chunk* l) {
        MOZ_ASSERT(!(uintptr_t(k) & gc::ChunkMask));
        MOZ_ASSERT(!(uintptr_t(l) & gc::ChunkMask));
        return k == l;
    }
};

class js::gc::MarkingValidator
{
  public:
    explicit MarkingValidator(GCRuntime* gc);
    ~MarkingValidator();
    void nonIncrementalMark();
    void validate();

  private:
    GCRuntime* gc;
    bool initialized;

    typedef HashMap<Chunk*, ChunkBitmap*, GCChunkHasher, SystemAllocPolicy> BitmapMap;
    BitmapMap map;
};

#endif // DEBUG

#ifdef JS_GC_MARKING_VALIDATION

js::gc::MarkingValidator::MarkingValidator(GCRuntime* gc)
  : gc(gc),
    initialized(false)
{}

js::gc::MarkingValidator::~MarkingValidator()
{
    if (!map.initialized())
        return;

    for (BitmapMap::Range r(map.all()); !r.empty(); r.popFront())
        js_delete(r.front().value());
}

void
js::gc::MarkingValidator::nonIncrementalMark()
{
    /*
     * Perform a non-incremental mark for all collecting zones and record
     * the results for later comparison.
     *
     * Currently this does not validate gray marking.
     */

    if (!map.init())
        return;

    JSRuntime* runtime = gc->rt;
    GCMarker* gcmarker = &gc->marker;

    gc->waitBackgroundSweepEnd();

    /* Save existing mark bits. */
    for (auto chunk = gc->allNonEmptyChunks(); !chunk.done(); chunk.next()) {
        ChunkBitmap* bitmap = &chunk->bitmap;
	ChunkBitmap* entry = js_new<ChunkBitmap>();
        if (!entry)
            return;

        memcpy((void*)entry->bitmap, (void*)bitmap->bitmap, sizeof(bitmap->bitmap));
        if (!map.putNew(chunk, entry))
            return;
    }

    /*
     * Temporarily clear the weakmaps' mark flags for the compartments we are
     * collecting.
     */

    WeakMapSet markedWeakMaps;
    if (!markedWeakMaps.init())
        return;

    /*
     * For saving, smush all of the keys into one big table and split them back
     * up into per-zone tables when restoring.
     */
    gc::WeakKeyTable savedWeakKeys;
    if (!savedWeakKeys.init())
        return;

    for (GCZonesIter zone(runtime); !zone.done(); zone.next()) {
        if (!WeakMapBase::saveZoneMarkedWeakMaps(zone, markedWeakMaps))
            return;

        for (gc::WeakKeyTable::Range r = zone->gcWeakKeys.all(); !r.empty(); r.popFront()) {
            AutoEnterOOMUnsafeRegion oomUnsafe;
            if (!savedWeakKeys.put(Move(r.front().key), Move(r.front().value)))
                oomUnsafe.crash("saving weak keys table for validator");
        }

        zone->gcWeakKeys.clear();
    }

    /*
     * After this point, the function should run to completion, so we shouldn't
     * do anything fallible.
     */
    initialized = true;

    /* Re-do all the marking, but non-incrementally. */
    js::gc::State state = gc->incrementalState;
    gc->incrementalState = MARK_ROOTS;

    {
        gcstats::AutoPhase ap(gc->stats, gcstats::PHASE_MARK);

        {
            gcstats::AutoPhase ap(gc->stats, gcstats::PHASE_UNMARK);

            for (GCZonesIter zone(runtime); !zone.done(); zone.next())
                WeakMapBase::unmarkZone(zone);

            MOZ_ASSERT(gcmarker->isDrained());
            gcmarker->reset();

            for (auto chunk = gc->allNonEmptyChunks(); !chunk.done(); chunk.next())
                chunk->bitmap.clear();
        }

        gc->markRuntime(gcmarker, GCRuntime::MarkRuntime);

        auto unlimited = SliceBudget::unlimited();
        gc->incrementalState = MARK;
        gc->marker.drainMarkStack(unlimited);
    }

    gc->incrementalState = SWEEP;
    {
        gcstats::AutoPhase ap1(gc->stats, gcstats::PHASE_SWEEP);
        gcstats::AutoPhase ap2(gc->stats, gcstats::PHASE_SWEEP_MARK);

        gc->markAllWeakReferences(gcstats::PHASE_SWEEP_MARK_WEAK);

        /* Update zone state for gray marking. */
        for (GCZonesIter zone(runtime); !zone.done(); zone.next()) {
            MOZ_ASSERT(zone->isGCMarkingBlack());
            zone->setGCState(Zone::MarkGray);
        }
        gc->marker.setMarkColorGray();

        gc->markAllGrayReferences(gcstats::PHASE_SWEEP_MARK_GRAY);
        gc->markAllWeakReferences(gcstats::PHASE_SWEEP_MARK_GRAY_WEAK);

        /* Restore zone state. */
        for (GCZonesIter zone(runtime); !zone.done(); zone.next()) {
            MOZ_ASSERT(zone->isGCMarkingGray());
            zone->setGCState(Zone::Mark);
        }
        MOZ_ASSERT(gc->marker.isDrained());
        gc->marker.setMarkColorBlack();
    }

    /* Take a copy of the non-incremental mark state and restore the original. */
    for (auto chunk = gc->allNonEmptyChunks(); !chunk.done(); chunk.next()) {
        ChunkBitmap* bitmap = &chunk->bitmap;
        ChunkBitmap* entry = map.lookup(chunk)->value();
        Swap(*entry, *bitmap);
    }

    for (GCZonesIter zone(runtime); !zone.done(); zone.next()) {
        WeakMapBase::unmarkZone(zone);
        zone->gcWeakKeys.clear();
    }

    WeakMapBase::restoreMarkedWeakMaps(markedWeakMaps);

    for (gc::WeakKeyTable::Range r = savedWeakKeys.all(); !r.empty(); r.popFront()) {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        Zone* zone = gc::TenuredCell::fromPointer(r.front().key.asCell())->zone();
        if (!zone->gcWeakKeys.put(Move(r.front().key), Move(r.front().value)))
            oomUnsafe.crash("restoring weak keys table for validator");
    }

    gc->incrementalState = state;
}

void
js::gc::MarkingValidator::validate()
{
    /*
     * Validates the incremental marking for a single compartment by comparing
     * the mark bits to those previously recorded for a non-incremental mark.
     */

    if (!initialized)
        return;

    gc->waitBackgroundSweepEnd();

    for (auto chunk = gc->allNonEmptyChunks(); !chunk.done(); chunk.next()) {
        BitmapMap::Ptr ptr = map.lookup(chunk);
        if (!ptr)
            continue;  /* Allocated after we did the non-incremental mark. */

        ChunkBitmap* bitmap = ptr->value();
        ChunkBitmap* incBitmap = &chunk->bitmap;

        for (size_t i = 0; i < ArenasPerChunk; i++) {
            if (chunk->decommittedArenas.get(i))
                continue;
            Arena* arena = &chunk->arenas[i];
            if (!arena->aheader.allocated())
                continue;
            if (!arena->aheader.zone->isGCSweeping())
                continue;
            if (arena->aheader.allocatedDuringIncremental)
                continue;

            AllocKind kind = arena->aheader.getAllocKind();
            uintptr_t thing = arena->thingsStart(kind);
            uintptr_t end = arena->thingsEnd();
            while (thing < end) {
                Cell* cell = (Cell*)thing;

                /*
                 * If a non-incremental GC wouldn't have collected a cell, then
                 * an incremental GC won't collect it.
                 */
                MOZ_ASSERT_IF(bitmap->isMarked(cell, BLACK), incBitmap->isMarked(cell, BLACK));

                /*
                 * If the cycle collector isn't allowed to collect an object
                 * after a non-incremental GC has run, then it isn't allowed to
                 * collected it after an incremental GC.
                 */
                MOZ_ASSERT_IF(!bitmap->isMarked(cell, GRAY), !incBitmap->isMarked(cell, GRAY));

                thing += Arena::thingSize(kind);
            }
        }
    }
}

#endif // JS_GC_MARKING_VALIDATION

void
GCRuntime::computeNonIncrementalMarkingForValidation()
{
#ifdef JS_GC_MARKING_VALIDATION
    MOZ_ASSERT(!markingValidator);
    if (isIncremental && validate)
        markingValidator = js_new<MarkingValidator>(this);
    if (markingValidator)
        markingValidator->nonIncrementalMark();
#endif
}

void
GCRuntime::validateIncrementalMarking()
{
#ifdef JS_GC_MARKING_VALIDATION
    if (markingValidator)
        markingValidator->validate();
#endif
}

void
GCRuntime::finishMarkingValidation()
{
#ifdef JS_GC_MARKING_VALIDATION
    js_delete(markingValidator);
    markingValidator = nullptr;
#endif
}

static void
DropStringWrappers(JSRuntime* rt)
{
    /*
     * String "wrappers" are dropped on GC because their presence would require
     * us to sweep the wrappers in all compartments every time we sweep a
     * compartment group.
     */
    for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next()) {
        for (JSCompartment::WrapperEnum e(c); !e.empty(); e.popFront()) {
            if (e.front().key().kind == CrossCompartmentKey::StringWrapper)
                e.removeFront();
        }
    }
}

/*
 * Group zones that must be swept at the same time.
 *
 * If compartment A has an edge to an unmarked object in compartment B, then we
 * must not sweep A in a later slice than we sweep B. That's because a write
 * barrier in A that could lead to the unmarked object in B becoming
 * marked. However, if we had already swept that object, we would be in trouble.
 *
 * If we consider these dependencies as a graph, then all the compartments in
 * any strongly-connected component of this graph must be swept in the same
 * slice.
 *
 * Tarjan's algorithm is used to calculate the components.
 */

void
JSCompartment::findOutgoingEdges(ComponentFinder<JS::Zone>& finder)
{
    for (js::WrapperMap::Enum e(crossCompartmentWrappers); !e.empty(); e.popFront()) {
        CrossCompartmentKey::Kind kind = e.front().key().kind;
        MOZ_ASSERT(kind != CrossCompartmentKey::StringWrapper);
        TenuredCell& other = e.front().key().wrapped->asTenured();
        if (kind == CrossCompartmentKey::ObjectWrapper) {
            /*
             * Add edge to wrapped object compartment if wrapped object is not
             * marked black to indicate that wrapper compartment not be swept
             * after wrapped compartment.
             */
            if (!other.isMarked(BLACK) || other.isMarked(GRAY)) {
                JS::Zone* w = other.zone();
                if (w->isGCMarking())
                    finder.addEdgeTo(w);
            }
        } else {
            MOZ_ASSERT(kind == CrossCompartmentKey::DebuggerScript ||
                       kind == CrossCompartmentKey::DebuggerSource ||
                       kind == CrossCompartmentKey::DebuggerObject ||
                       kind == CrossCompartmentKey::DebuggerEnvironment);
            /*
             * Add edge for debugger object wrappers, to ensure (in conjuction
             * with call to Debugger::findCompartmentEdges below) that debugger
             * and debuggee objects are always swept in the same group.
             */
            JS::Zone* w = other.zone();
            if (w->isGCMarking())
                finder.addEdgeTo(w);
        }
    }
}

void
Zone::findOutgoingEdges(ComponentFinder<JS::Zone>& finder)
{
    /*
     * Any compartment may have a pointer to an atom in the atoms
     * compartment, and these aren't in the cross compartment map.
     */
    JSRuntime* rt = runtimeFromMainThread();
    if (rt->atomsCompartment()->zone()->isGCMarking())
        finder.addEdgeTo(rt->atomsCompartment()->zone());

    for (CompartmentsInZoneIter comp(this); !comp.done(); comp.next())
        comp->findOutgoingEdges(finder);

    for (ZoneSet::Range r = gcZoneGroupEdges.all(); !r.empty(); r.popFront()) {
        if (r.front()->isGCMarking())
            finder.addEdgeTo(r.front());
    }
    gcZoneGroupEdges.clear();

    Debugger::findZoneEdges(this, finder);
}

bool
GCRuntime::findZoneEdgesForWeakMaps()
{
    /*
     * Weakmaps which have keys with delegates in a different zone introduce the
     * need for zone edges from the delegate's zone to the weakmap zone.
     *
     * Since the edges point into and not away from the zone the weakmap is in
     * we must find these edges in advance and store them in a set on the Zone.
     * If we run out of memory, we fall back to sweeping everything in one
     * group.
     */

    for (GCZonesIter zone(rt); !zone.done(); zone.next()) {
        if (!WeakMapBase::findInterZoneEdges(zone))
            return false;
    }

    return true;
}

void
GCRuntime::findZoneGroups()
{
    ComponentFinder<Zone> finder(rt->mainThread.nativeStackLimit[StackForSystemCode]);
    if (!isIncremental || !findZoneEdgesForWeakMaps())
        finder.useOneComponent();

    for (GCZonesIter zone(rt); !zone.done(); zone.next()) {
        MOZ_ASSERT(zone->isGCMarking());
        finder.addNode(zone);
    }
    zoneGroups = finder.getResultsList();
    currentZoneGroup = zoneGroups;
    zoneGroupIndex = 0;

    for (Zone* head = currentZoneGroup; head; head = head->nextGroup()) {
        for (Zone* zone = head; zone; zone = zone->nextNodeInGroup())
            MOZ_ASSERT(zone->isGCMarking());
    }

    MOZ_ASSERT_IF(!isIncremental, !currentZoneGroup->nextGroup());
}

static void
ResetGrayList(JSCompartment* comp);

void
GCRuntime::getNextZoneGroup()
{
    currentZoneGroup = currentZoneGroup->nextGroup();
    ++zoneGroupIndex;
    if (!currentZoneGroup) {
        abortSweepAfterCurrentGroup = false;
        return;
    }

    for (Zone* zone = currentZoneGroup; zone; zone = zone->nextNodeInGroup()) {
        MOZ_ASSERT(zone->isGCMarking());
        MOZ_ASSERT(!zone->isQueuedForBackgroundSweep());
    }

    if (!isIncremental)
        ComponentFinder<Zone>::mergeGroups(currentZoneGroup);

    if (abortSweepAfterCurrentGroup) {
        MOZ_ASSERT(!isIncremental);
        for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
            MOZ_ASSERT(!zone->gcNextGraphComponent);
            MOZ_ASSERT(zone->isGCMarking());
            zone->setNeedsIncrementalBarrier(false, Zone::UpdateJit);
            zone->setGCState(Zone::NoGC);
            zone->gcGrayRoots.clearAndFree();
        }

        for (GCCompartmentGroupIter comp(rt); !comp.done(); comp.next())
            ResetGrayList(comp);

        abortSweepAfterCurrentGroup = false;
        currentZoneGroup = nullptr;
    }
}

/*
 * Gray marking:
 *
 * At the end of collection, anything reachable from a gray root that has not
 * otherwise been marked black must be marked gray.
 *
 * This means that when marking things gray we must not allow marking to leave
 * the current compartment group, as that could result in things being marked
 * grey when they might subsequently be marked black.  To achieve this, when we
 * find a cross compartment pointer we don't mark the referent but add it to a
 * singly-linked list of incoming gray pointers that is stored with each
 * compartment.
 *
 * The list head is stored in JSCompartment::gcIncomingGrayPointers and contains
 * cross compartment wrapper objects. The next pointer is stored in the second
 * extra slot of the cross compartment wrapper.
 *
 * The list is created during gray marking when one of the
 * MarkCrossCompartmentXXX functions is called for a pointer that leaves the
 * current compartent group.  This calls DelayCrossCompartmentGrayMarking to
 * push the referring object onto the list.
 *
 * The list is traversed and then unlinked in
 * MarkIncomingCrossCompartmentPointers.
 */

static bool
IsGrayListObject(JSObject* obj)
{
    MOZ_ASSERT(obj);
    return obj->is<CrossCompartmentWrapperObject>() && !IsDeadProxyObject(obj);
}

/* static */ unsigned
ProxyObject::grayLinkExtraSlot(JSObject* obj)
{
    MOZ_ASSERT(IsGrayListObject(obj));
    return 1;
}

#ifdef DEBUG
static void
AssertNotOnGrayList(JSObject* obj)
{
    MOZ_ASSERT_IF(IsGrayListObject(obj),
                  GetProxyExtra(obj, ProxyObject::grayLinkExtraSlot(obj)).isUndefined());
}
#endif

static JSObject*
CrossCompartmentPointerReferent(JSObject* obj)
{
    MOZ_ASSERT(IsGrayListObject(obj));
    return &obj->as<ProxyObject>().private_().toObject();
}

static JSObject*
NextIncomingCrossCompartmentPointer(JSObject* prev, bool unlink)
{
    unsigned slot = ProxyObject::grayLinkExtraSlot(prev);
    JSObject* next = GetProxyExtra(prev, slot).toObjectOrNull();
    MOZ_ASSERT_IF(next, IsGrayListObject(next));

    if (unlink)
        SetProxyExtra(prev, slot, UndefinedValue());

    return next;
}

void
js::DelayCrossCompartmentGrayMarking(JSObject* src)
{
    MOZ_ASSERT(IsGrayListObject(src));

    /* Called from MarkCrossCompartmentXXX functions. */
    unsigned slot = ProxyObject::grayLinkExtraSlot(src);
    JSObject* dest = CrossCompartmentPointerReferent(src);
    JSCompartment* comp = dest->compartment();

    if (GetProxyExtra(src, slot).isUndefined()) {
        SetProxyExtra(src, slot, ObjectOrNullValue(comp->gcIncomingGrayPointers));
        comp->gcIncomingGrayPointers = src;
    } else {
        MOZ_ASSERT(GetProxyExtra(src, slot).isObjectOrNull());
    }

#ifdef DEBUG
    /*
     * Assert that the object is in our list, also walking the list to check its
     * integrity.
     */
    JSObject* obj = comp->gcIncomingGrayPointers;
    bool found = false;
    while (obj) {
        if (obj == src)
            found = true;
        obj = NextIncomingCrossCompartmentPointer(obj, false);
    }
    MOZ_ASSERT(found);
#endif
}

static void
MarkIncomingCrossCompartmentPointers(JSRuntime* rt, const uint32_t color)
{
    MOZ_ASSERT(color == BLACK || color == GRAY);

    static const gcstats::Phase statsPhases[] = {
        gcstats::PHASE_SWEEP_MARK_INCOMING_BLACK,
        gcstats::PHASE_SWEEP_MARK_INCOMING_GRAY
    };
    gcstats::AutoPhase ap1(rt->gc.stats, statsPhases[color]);

    bool unlinkList = color == GRAY;

    for (GCCompartmentGroupIter c(rt); !c.done(); c.next()) {
        MOZ_ASSERT_IF(color == GRAY, c->zone()->isGCMarkingGray());
        MOZ_ASSERT_IF(color == BLACK, c->zone()->isGCMarkingBlack());
        MOZ_ASSERT_IF(c->gcIncomingGrayPointers, IsGrayListObject(c->gcIncomingGrayPointers));

        for (JSObject* src = c->gcIncomingGrayPointers;
             src;
             src = NextIncomingCrossCompartmentPointer(src, unlinkList))
        {
            JSObject* dst = CrossCompartmentPointerReferent(src);
            MOZ_ASSERT(dst->compartment() == c);

            if (color == GRAY) {
                if (IsMarkedUnbarriered(&src) && src->asTenured().isMarked(GRAY))
                    TraceManuallyBarrieredEdge(&rt->gc.marker, &dst,
                                               "cross-compartment gray pointer");
            } else {
                if (IsMarkedUnbarriered(&src) && !src->asTenured().isMarked(GRAY))
                    TraceManuallyBarrieredEdge(&rt->gc.marker, &dst,
                                               "cross-compartment black pointer");
            }
        }

        if (unlinkList)
            c->gcIncomingGrayPointers = nullptr;
    }

    auto unlimited = SliceBudget::unlimited();
    rt->gc.marker.drainMarkStack(unlimited);
}

static bool
RemoveFromGrayList(JSObject* wrapper)
{
    if (!IsGrayListObject(wrapper))
        return false;

    unsigned slot = ProxyObject::grayLinkExtraSlot(wrapper);
    if (GetProxyExtra(wrapper, slot).isUndefined())
        return false;  /* Not on our list. */

    JSObject* tail = GetProxyExtra(wrapper, slot).toObjectOrNull();
    SetProxyExtra(wrapper, slot, UndefinedValue());

    JSCompartment* comp = CrossCompartmentPointerReferent(wrapper)->compartment();
    JSObject* obj = comp->gcIncomingGrayPointers;
    if (obj == wrapper) {
        comp->gcIncomingGrayPointers = tail;
        return true;
    }

    while (obj) {
        unsigned slot = ProxyObject::grayLinkExtraSlot(obj);
        JSObject* next = GetProxyExtra(obj, slot).toObjectOrNull();
        if (next == wrapper) {
            SetProxyExtra(obj, slot, ObjectOrNullValue(tail));
            return true;
        }
        obj = next;
    }

    MOZ_CRASH("object not found in gray link list");
}

static void
ResetGrayList(JSCompartment* comp)
{
    JSObject* src = comp->gcIncomingGrayPointers;
    while (src)
        src = NextIncomingCrossCompartmentPointer(src, true);
    comp->gcIncomingGrayPointers = nullptr;
}

void
js::NotifyGCNukeWrapper(JSObject* obj)
{
    /*
     * References to target of wrapper are being removed, we no longer have to
     * remember to mark it.
     */
    RemoveFromGrayList(obj);
}

enum {
    JS_GC_SWAP_OBJECT_A_REMOVED = 1 << 0,
    JS_GC_SWAP_OBJECT_B_REMOVED = 1 << 1
};

unsigned
js::NotifyGCPreSwap(JSObject* a, JSObject* b)
{
    /*
     * Two objects in the same compartment are about to have had their contents
     * swapped.  If either of them are in our gray pointer list, then we remove
     * them from the lists, returning a bitset indicating what happened.
     */
    return (RemoveFromGrayList(a) ? JS_GC_SWAP_OBJECT_A_REMOVED : 0) |
           (RemoveFromGrayList(b) ? JS_GC_SWAP_OBJECT_B_REMOVED : 0);
}

void
js::NotifyGCPostSwap(JSObject* a, JSObject* b, unsigned removedFlags)
{
    /*
     * Two objects in the same compartment have had their contents swapped.  If
     * either of them were in our gray pointer list, we re-add them again.
     */
    if (removedFlags & JS_GC_SWAP_OBJECT_A_REMOVED)
        DelayCrossCompartmentGrayMarking(b);
    if (removedFlags & JS_GC_SWAP_OBJECT_B_REMOVED)
        DelayCrossCompartmentGrayMarking(a);
}

void
GCRuntime::endMarkingZoneGroup()
{
    gcstats::AutoPhase ap(stats, gcstats::PHASE_SWEEP_MARK);

    /*
     * Mark any incoming black pointers from previously swept compartments
     * whose referents are not marked. This can occur when gray cells become
     * black by the action of UnmarkGray.
     */
    MarkIncomingCrossCompartmentPointers(rt, BLACK);
    markWeakReferencesInCurrentGroup(gcstats::PHASE_SWEEP_MARK_WEAK);

    /*
     * Change state of current group to MarkGray to restrict marking to this
     * group.  Note that there may be pointers to the atoms compartment, and
     * these will be marked through, as they are not marked with
     * MarkCrossCompartmentXXX.
     */
    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        MOZ_ASSERT(zone->isGCMarkingBlack());
        zone->setGCState(Zone::MarkGray);
    }
    marker.setMarkColorGray();

    /* Mark incoming gray pointers from previously swept compartments. */
    MarkIncomingCrossCompartmentPointers(rt, GRAY);

    /* Mark gray roots and mark transitively inside the current compartment group. */
    markGrayReferencesInCurrentGroup(gcstats::PHASE_SWEEP_MARK_GRAY);
    markWeakReferencesInCurrentGroup(gcstats::PHASE_SWEEP_MARK_GRAY_WEAK);

    /* Restore marking state. */
    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        MOZ_ASSERT(zone->isGCMarkingGray());
        zone->setGCState(Zone::Mark);
    }
    MOZ_ASSERT(marker.isDrained());
    marker.setMarkColorBlack();
}

class GCSweepTask : public GCParallelTask
{
    virtual void runFromHelperThread() override {
        AutoSetThreadIsSweeping threadIsSweeping;
        GCParallelTask::runFromHelperThread();
    }
  protected:
    JSRuntime* runtime;
  public:
    explicit GCSweepTask(JSRuntime* rt) : runtime(rt) {}
};

#define MAKE_GC_SWEEP_TASK(name)                                              \
    class name : public GCSweepTask {                                         \
        virtual void run() override;                                          \
      public:                                                                 \
        explicit name (JSRuntime* rt) : GCSweepTask(rt) {}                    \
    }
MAKE_GC_SWEEP_TASK(SweepAtomsTask);
MAKE_GC_SWEEP_TASK(SweepInnerViewsTask);
MAKE_GC_SWEEP_TASK(SweepCCWrappersTask);
MAKE_GC_SWEEP_TASK(SweepBaseShapesTask);
MAKE_GC_SWEEP_TASK(SweepInitialShapesTask);
MAKE_GC_SWEEP_TASK(SweepObjectGroupsTask);
MAKE_GC_SWEEP_TASK(SweepRegExpsTask);
MAKE_GC_SWEEP_TASK(SweepMiscTask);
#undef MAKE_GC_SWEEP_TASK

/* virtual */ void
SweepAtomsTask::run()
{
    runtime->sweepAtoms();
}

/* virtual */ void
SweepInnerViewsTask::run()
{
    for (GCCompartmentGroupIter c(runtime); !c.done(); c.next())
        c->sweepInnerViews();
}

/* virtual */ void
SweepCCWrappersTask::run()
{
    for (GCCompartmentGroupIter c(runtime); !c.done(); c.next())
        c->sweepCrossCompartmentWrappers();
}

/* virtual */ void
SweepBaseShapesTask::run()
{
    for (GCCompartmentGroupIter c(runtime); !c.done(); c.next())
        c->sweepBaseShapeTable();
}

/* virtual */ void
SweepInitialShapesTask::run()
{
    for (GCCompartmentGroupIter c(runtime); !c.done(); c.next())
        c->sweepInitialShapeTable();
}

/* virtual */ void
SweepObjectGroupsTask::run()
{
    for (GCCompartmentGroupIter c(runtime); !c.done(); c.next())
        c->objectGroups.sweep(runtime->defaultFreeOp());
}

/* virtual */ void
SweepRegExpsTask::run()
{
    for (GCCompartmentGroupIter c(runtime); !c.done(); c.next())
        c->sweepRegExps();
}

/* virtual */ void
SweepMiscTask::run()
{
    for (GCCompartmentGroupIter c(runtime); !c.done(); c.next()) {
        c->sweepSavedStacks();
        c->sweepSelfHostingScriptSource();
        c->sweepNativeIterators();
    }
}

void
GCRuntime::startTask(GCParallelTask& task, gcstats::Phase phase)
{
    MOZ_ASSERT(HelperThreadState().isLocked());
    if (!task.startWithLockHeld()) {
        AutoUnlockHelperThreadState unlock;
        gcstats::AutoPhase ap(stats, phase);
        task.runFromMainThread(rt);
    }
}

void
GCRuntime::joinTask(GCParallelTask& task, gcstats::Phase phase)
{
    gcstats::AutoPhase ap(stats, task, phase);
    task.joinWithLockHeld();
}

void
GCRuntime::beginSweepingZoneGroup()
{
    /*
     * Begin sweeping the group of zones in gcCurrentZoneGroup,
     * performing actions that must be done before yielding to caller.
     */

    bool sweepingAtoms = false;
    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        /* Set the GC state to sweeping. */
        MOZ_ASSERT(zone->isGCMarking());
        zone->setGCState(Zone::Sweep);

        /* Purge the ArenaLists before sweeping. */
        zone->arenas.purge();

        if (zone->isAtomsZone())
            sweepingAtoms = true;

        if (rt->sweepZoneCallback)
            rt->sweepZoneCallback(zone);

        zone->gcLastZoneGroupIndex = zoneGroupIndex;
    }

    validateIncrementalMarking();

    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        /* Clear all weakrefs that point to unmarked things. */
        for (auto edge : zone->gcWeakRefs) {
            /* Edges may be present multiple times, so may already be nulled. */
            if (*edge && IsAboutToBeFinalizedDuringSweep(**edge))
                *edge = nullptr;
        }
        zone->gcWeakRefs.clear();

        /* No need to look up any more weakmap keys from this zone group. */
        zone->gcWeakKeys.clear();
    }

    FreeOp fop(rt);
    SweepAtomsTask sweepAtomsTask(rt);
    SweepInnerViewsTask sweepInnerViewsTask(rt);
    SweepCCWrappersTask sweepCCWrappersTask(rt);
    SweepBaseShapesTask sweepBaseShapesTask(rt);
    SweepInitialShapesTask sweepInitialShapesTask(rt);
    SweepObjectGroupsTask sweepObjectGroupsTask(rt);
    SweepRegExpsTask sweepRegExpsTask(rt);
    SweepMiscTask sweepMiscTask(rt);

    {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_FINALIZE_START);
        callFinalizeCallbacks(&fop, JSFINALIZE_GROUP_START);
        {
            gcstats::AutoPhase ap2(stats, gcstats::PHASE_WEAK_ZONEGROUP_CALLBACK);
            callWeakPointerZoneGroupCallbacks();
        }
        {
            gcstats::AutoPhase ap2(stats, gcstats::PHASE_WEAK_COMPARTMENT_CALLBACK);
            for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
                for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next())
                    callWeakPointerCompartmentCallbacks(comp);
            }
        }
    }

    if (sweepingAtoms) {
        AutoLockHelperThreadState helperLock;
        startTask(sweepAtomsTask, gcstats::PHASE_SWEEP_ATOMS);
    }

    {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_SWEEP_COMPARTMENTS);
        gcstats::AutoSCC scc(stats, zoneGroupIndex);

        {
            AutoLockHelperThreadState helperLock;
            startTask(sweepInnerViewsTask, gcstats::PHASE_SWEEP_INNER_VIEWS);
            startTask(sweepCCWrappersTask, gcstats::PHASE_SWEEP_CC_WRAPPER);
            startTask(sweepBaseShapesTask, gcstats::PHASE_SWEEP_BASE_SHAPE);
            startTask(sweepInitialShapesTask, gcstats::PHASE_SWEEP_INITIAL_SHAPE);
            startTask(sweepObjectGroupsTask, gcstats::PHASE_SWEEP_TYPE_OBJECT);
            startTask(sweepRegExpsTask, gcstats::PHASE_SWEEP_REGEXP);
            startTask(sweepMiscTask, gcstats::PHASE_SWEEP_MISC);
        }

        // The remainder of the of the tasks run in parallel on the main
        // thread until we join, below.
        {
            gcstats::AutoPhase ap(stats, gcstats::PHASE_SWEEP_MISC);

            for (GCCompartmentGroupIter c(rt); !c.done(); c.next()) {
                c->sweepGlobalObject(&fop);
                c->sweepObjectPendingMetadata();
                c->sweepDebugScopes();
                c->sweepJitCompartment(&fop);
                c->sweepTemplateObjects();
            }

            for (GCZoneGroupIter zone(rt); !zone.done(); zone.next())
                zone->sweepWeakMaps();

            // Bug 1071218: the following two methods have not yet been
            // refactored to work on a single zone-group at once.

            // Collect watch points associated with unreachable objects.
            WatchpointMap::sweepAll(rt);

            // Detach unreachable debuggers and global objects from each other.
            Debugger::sweepAll(&fop);

            // Sweep entries containing about-to-be-finalized JitCode and
            // update relocated TypeSet::Types inside the JitcodeGlobalTable.
            jit::JitRuntime::SweepJitcodeGlobalTable(rt);
        }

        {
            gcstats::AutoPhase apdc(stats, gcstats::PHASE_SWEEP_DISCARD_CODE);
            for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
                zone->discardJitCode(&fop);
            }
        }

        {
            gcstats::AutoPhase ap1(stats, gcstats::PHASE_SWEEP_TYPES);
            gcstats::AutoPhase ap2(stats, gcstats::PHASE_SWEEP_TYPES_BEGIN);
            for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
                zone->beginSweepTypes(&fop, releaseObservedTypes && !zone->isPreservingCode());
            }
        }

        {
            gcstats::AutoPhase ap(stats, gcstats::PHASE_SWEEP_BREAKPOINT);
            for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
                zone->sweepBreakpoints(&fop);
            }
        }

        {
            gcstats::AutoPhase ap(stats, gcstats::PHASE_SWEEP_BREAKPOINT);
            for (GCZoneGroupIter zone(rt); !zone.done(); zone.next())
                zone->sweepUniqueIds(&fop);
        }
    }

    if (sweepingAtoms) {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_SWEEP_SYMBOL_REGISTRY);
        rt->symbolRegistry().sweep();
    }

    // Rejoin our off-main-thread tasks.
    if (sweepingAtoms) {
        AutoLockHelperThreadState helperLock;
        joinTask(sweepAtomsTask, gcstats::PHASE_SWEEP_ATOMS);
    }

    {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_SWEEP_COMPARTMENTS);
        gcstats::AutoSCC scc(stats, zoneGroupIndex);

        AutoLockHelperThreadState helperLock;
        joinTask(sweepInnerViewsTask, gcstats::PHASE_SWEEP_INNER_VIEWS);
        joinTask(sweepCCWrappersTask, gcstats::PHASE_SWEEP_CC_WRAPPER);
        joinTask(sweepBaseShapesTask, gcstats::PHASE_SWEEP_BASE_SHAPE);
        joinTask(sweepInitialShapesTask, gcstats::PHASE_SWEEP_INITIAL_SHAPE);
        joinTask(sweepObjectGroupsTask, gcstats::PHASE_SWEEP_TYPE_OBJECT);
        joinTask(sweepRegExpsTask, gcstats::PHASE_SWEEP_REGEXP);
        joinTask(sweepMiscTask, gcstats::PHASE_SWEEP_MISC);
    }

    /*
     * Queue all GC things in all zones for sweeping, either in the
     * foreground or on the background thread.
     *
     * Note that order is important here for the background case.
     *
     * Objects are finalized immediately but this may change in the future.
     */

    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        gcstats::AutoSCC scc(stats, zoneGroupIndex);
        zone->arenas.queueForegroundObjectsForSweep(&fop);
    }
    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        gcstats::AutoSCC scc(stats, zoneGroupIndex);
        for (unsigned i = 0; i < ArrayLength(IncrementalFinalizePhases); ++i)
            zone->arenas.queueForForegroundSweep(&fop, IncrementalFinalizePhases[i]);
    }
    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        gcstats::AutoSCC scc(stats, zoneGroupIndex);
        for (unsigned i = 0; i < ArrayLength(BackgroundFinalizePhases); ++i)
            zone->arenas.queueForBackgroundSweep(&fop, BackgroundFinalizePhases[i]);
    }
    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        gcstats::AutoSCC scc(stats, zoneGroupIndex);
        zone->arenas.queueForegroundThingsForSweep(&fop);
    }

    sweepingTypes = true;

    finalizePhase = 0;
    sweepZone = currentZoneGroup;
    sweepKindIndex = 0;

    {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_FINALIZE_END);
        callFinalizeCallbacks(&fop, JSFINALIZE_GROUP_END);
    }
}

void
GCRuntime::endSweepingZoneGroup()
{
    /* Update the GC state for zones we have swept. */
    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        MOZ_ASSERT(zone->isGCSweeping());
        AutoLockGC lock(rt);
        zone->setGCState(Zone::Finished);
        zone->threshold.updateAfterGC(zone->usage.gcBytes(), invocationKind, tunables,
                                      schedulingState, lock);
    }

    /* Start background thread to sweep zones if required. */
    ZoneList zones;
    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next())
        zones.append(zone);
    if (sweepOnBackgroundThread)
        queueZonesForBackgroundSweep(zones);
    else
        sweepBackgroundThings(zones, freeLifoAlloc, MainThread);

    /* Reset the list of arenas marked as being allocated during sweep phase. */
    while (ArenaHeader* arena = arenasAllocatedDuringSweep) {
        arenasAllocatedDuringSweep = arena->getNextAllocDuringSweep();
        arena->unsetAllocDuringSweep();
    }
}

void
GCRuntime::beginSweepPhase(bool destroyingRuntime)
{
    /*
     * Sweep phase.
     *
     * Finalize as we sweep, outside of lock but with rt->isHeapBusy()
     * true so that any attempt to allocate a GC-thing from a finalizer will
     * fail, rather than nest badly and leave the unmarked newborn to be swept.
     */

    MOZ_ASSERT(!abortSweepAfterCurrentGroup);

    AutoSetThreadIsSweeping threadIsSweeping;

    releaseHeldRelocatedArenas();

    computeNonIncrementalMarkingForValidation();

    gcstats::AutoPhase ap(stats, gcstats::PHASE_SWEEP);

    sweepOnBackgroundThread =
        !destroyingRuntime && !TraceEnabled() && CanUseExtraThreads();

    releaseObservedTypes = shouldReleaseObservedTypes();

#ifdef DEBUG
    for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next()) {
        MOZ_ASSERT(!c->gcIncomingGrayPointers);
        for (JSCompartment::WrapperEnum e(c); !e.empty(); e.popFront()) {
            if (e.front().key().kind != CrossCompartmentKey::StringWrapper)
                AssertNotOnGrayList(&e.front().value().unbarrieredGet().toObject());
        }
    }
#endif

    DropStringWrappers(rt);

    findZoneGroups();
    endMarkingZoneGroup();
    beginSweepingZoneGroup();
}

bool
ArenaLists::foregroundFinalize(FreeOp* fop, AllocKind thingKind, SliceBudget& sliceBudget,
                               SortedArenaList& sweepList)
{
    if (!arenaListsToSweep[thingKind] && incrementalSweptArenas.isEmpty())
        return true;

    if (!FinalizeArenas(fop, &arenaListsToSweep[thingKind], sweepList,
                        thingKind, sliceBudget, RELEASE_ARENAS))
    {
        incrementalSweptArenaKind = thingKind;
        incrementalSweptArenas = sweepList.toArenaList();
        return false;
    }

    // Clear any previous incremental sweep state we may have saved.
    incrementalSweptArenas.clear();

    // Join |arenaLists[thingKind]| and |sweepList| into a single list.
    ArenaList finalized = sweepList.toArenaList();
    arenaLists[thingKind] =
        finalized.insertListWithCursorAtEnd(arenaLists[thingKind]);

    return true;
}

GCRuntime::IncrementalProgress
GCRuntime::drainMarkStack(SliceBudget& sliceBudget, gcstats::Phase phase)
{
    /* Run a marking slice and return whether the stack is now empty. */
    gcstats::AutoPhase ap(stats, phase);
    return marker.drainMarkStack(sliceBudget) ? Finished : NotFinished;
}

static void
SweepThing(Shape* shape)
{
    if (!shape->isMarked())
        shape->sweep();
}

static void
SweepThing(JSScript* script, AutoClearTypeInferenceStateOnOOM* oom)
{
    script->maybeSweepTypes(oom);
}

static void
SweepThing(ObjectGroup* group, AutoClearTypeInferenceStateOnOOM* oom)
{
    group->maybeSweep(oom);
}

template <typename T, typename... Args>
static bool
SweepArenaList(ArenaHeader** arenasToSweep, SliceBudget& sliceBudget, Args... args)
{
    while (ArenaHeader* arena = *arenasToSweep) {
        for (ArenaCellIterUnderGC i(arena); !i.done(); i.next())
            SweepThing(i.get<T>(), args...);

        *arenasToSweep = (*arenasToSweep)->next;
        AllocKind kind = MapTypeToFinalizeKind<T>::kind;
        sliceBudget.step(Arena::thingsPerArena(Arena::thingSize(kind)));
        if (sliceBudget.isOverBudget())
            return false;
    }

    return true;
}

GCRuntime::IncrementalProgress
GCRuntime::sweepPhase(SliceBudget& sliceBudget)
{
    AutoSetThreadIsSweeping threadIsSweeping;

    gcstats::AutoPhase ap(stats, gcstats::PHASE_SWEEP);
    FreeOp fop(rt);

    if (drainMarkStack(sliceBudget, gcstats::PHASE_SWEEP_MARK) == NotFinished)
        return NotFinished;


    for (;;) {
        // Sweep dead type information stored in scripts and object groups, but
        // don't finalize them yet. We have to sweep dead information from both
        // live and dead scripts and object groups, so that no dead references
        // remain in them. Type inference can end up crawling these zones
        // again, such as for TypeCompartment::markSetsUnknown, and if this
        // happens after sweeping for the zone group finishes we won't be able
        // to determine which things in the zone are live.
        if (sweepingTypes) {
            gcstats::AutoPhase ap1(stats, gcstats::PHASE_SWEEP_COMPARTMENTS);
            gcstats::AutoPhase ap2(stats, gcstats::PHASE_SWEEP_TYPES);

            for (; sweepZone; sweepZone = sweepZone->nextNodeInGroup()) {
                ArenaLists& al = sweepZone->arenas;

                AutoClearTypeInferenceStateOnOOM oom(sweepZone);

                if (!SweepArenaList<JSScript>(&al.gcScriptArenasToUpdate, sliceBudget, &oom))
                    return NotFinished;

                if (!SweepArenaList<ObjectGroup>(
                        &al.gcObjectGroupArenasToUpdate, sliceBudget, &oom))
                {
                    return NotFinished;
                }

                // Finish sweeping type information in the zone.
                {
                    gcstats::AutoPhase ap(stats, gcstats::PHASE_SWEEP_TYPES_END);
                    sweepZone->types.endSweep(rt);
                }

                // Foreground finalized objects have already been finalized,
                // and now their arenas can be reclaimed by freeing empty ones
                // and making non-empty ones available for allocation.
                al.mergeForegroundSweptObjectArenas();
            }

            sweepZone = currentZoneGroup;
            sweepingTypes = false;
        }

        /* Finalize foreground finalized things. */
        for (; finalizePhase < ArrayLength(IncrementalFinalizePhases) ; ++finalizePhase) {
            gcstats::AutoPhase ap(stats, IncrementalFinalizePhases[finalizePhase].statsPhase);

            for (; sweepZone; sweepZone = sweepZone->nextNodeInGroup()) {
                Zone* zone = sweepZone;

                while (sweepKindIndex < IncrementalFinalizePhases[finalizePhase].length) {
                    AllocKind kind = IncrementalFinalizePhases[finalizePhase].kinds[sweepKindIndex];

                    /* Set the number of things per arena for this AllocKind. */
                    size_t thingsPerArena = Arena::thingsPerArena(Arena::thingSize(kind));
                    incrementalSweepList.setThingsPerArena(thingsPerArena);

                    if (!zone->arenas.foregroundFinalize(&fop, kind, sliceBudget,
                                                         incrementalSweepList))
                        return NotFinished;

                    /* Reset the slots of the sweep list that we used. */
                    incrementalSweepList.reset(thingsPerArena);

                    ++sweepKindIndex;
                }
                sweepKindIndex = 0;
            }
            sweepZone = currentZoneGroup;
        }

        /* Remove dead shapes from the shape tree, but don't finalize them yet. */
        {
            gcstats::AutoPhase ap(stats, gcstats::PHASE_SWEEP_SHAPE);

            for (; sweepZone; sweepZone = sweepZone->nextNodeInGroup()) {
                ArenaLists& al = sweepZone->arenas;

                if (!SweepArenaList<Shape>(&al.gcShapeArenasToUpdate, sliceBudget))
                    return NotFinished;

                if (!SweepArenaList<AccessorShape>(&al.gcAccessorShapeArenasToUpdate, sliceBudget))
                    return NotFinished;
            }
        }

        endSweepingZoneGroup();
        getNextZoneGroup();
        if (!currentZoneGroup)
            return Finished;

        endMarkingZoneGroup();
        beginSweepingZoneGroup();
    }
}

void
GCRuntime::endSweepPhase(bool destroyingRuntime)
{
    AutoSetThreadIsSweeping threadIsSweeping;

    gcstats::AutoPhase ap(stats, gcstats::PHASE_SWEEP);
    FreeOp fop(rt);

    MOZ_ASSERT_IF(destroyingRuntime, !sweepOnBackgroundThread);

    /*
     * Recalculate whether GC was full or not as this may have changed due to
     * newly created zones.  Can only change from full to not full.
     */
    if (isFull) {
        for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
            if (!zone->isCollecting()) {
                isFull = false;
                break;
            }
        }
    }

    /*
     * If we found any black->gray edges during marking, we completely clear the
     * mark bits of all uncollected zones, or if a reset has occured, zones that
     * will no longer be collected. This is safe, although it may
     * prevent the cycle collector from collecting some dead objects.
     */
    if (foundBlackGrayEdges) {
        for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
            if (!zone->isCollecting())
                zone->arenas.unmarkAll();
        }
    }

    {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_DESTROY);

        /*
         * Sweep script filenames after sweeping functions in the generic loop
         * above. In this way when a scripted function's finalizer destroys the
         * script and calls rt->destroyScriptHook, the hook can still access the
         * script's filename. See bug 323267.
         */
        if (isFull)
            SweepScriptData(rt);

        /* Clear out any small pools that we're hanging on to. */
        if (jit::JitRuntime* jitRuntime = rt->jitRuntime())
            jitRuntime->execAlloc().purge();

        /*
         * This removes compartments from rt->compartment, so we do it last to make
         * sure we don't miss sweeping any compartments.
         */
        if (!destroyingRuntime)
            sweepZones(&fop, destroyingRuntime);
    }

    {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_FINALIZE_END);
        callFinalizeCallbacks(&fop, JSFINALIZE_COLLECTION_END);

        /* If we finished a full GC, then the gray bits are correct. */
        if (isFull)
            grayBitsValid = true;
    }

    /* If not sweeping on background thread then we must do it here. */
    if (!sweepOnBackgroundThread) {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_DESTROY);

        assertBackgroundSweepingFinished();

        /*
         * Destroy arenas after we finished the sweeping so finalizers can
         * safely use IsAboutToBeFinalized(). This is done on the
         * GCHelperState if possible. We acquire the lock only because
         * Expire needs to unlock it for other callers.
         */
        {
            AutoLockGC lock(rt);
            expireChunksAndArenas(invocationKind == GC_SHRINK, lock);
        }

        /* Ensure the compartments get swept if it's the last GC. */
        if (destroyingRuntime)
            sweepZones(&fop, destroyingRuntime);
    }

    finishMarkingValidation();

#ifdef DEBUG
    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        for (auto i : AllAllocKinds()) {
            MOZ_ASSERT_IF(!IsBackgroundFinalized(i) ||
                          !sweepOnBackgroundThread,
                          !zone->arenas.arenaListsToSweep[i]);
        }
    }

    for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next()) {
        MOZ_ASSERT(!c->gcIncomingGrayPointers);

        for (JSCompartment::WrapperEnum e(c); !e.empty(); e.popFront()) {
            if (e.front().key().kind != CrossCompartmentKey::StringWrapper)
                AssertNotOnGrayList(&e.front().value().unbarrieredGet().toObject());
        }
    }
#endif
}

GCRuntime::IncrementalProgress
GCRuntime::beginCompactPhase()
{
    gcstats::AutoPhase ap(stats, gcstats::PHASE_COMPACT);

    if (isIncremental) {
        // Poll for end of background sweeping
        AutoLockGC lock(rt);
        if (isBackgroundSweeping())
            return NotFinished;
    } else {
        waitBackgroundSweepEnd();
    }

    MOZ_ASSERT(zonesToMaybeCompact.isEmpty());
    for (GCZonesIter zone(rt); !zone.done(); zone.next()) {
        if (CanRelocateZone(zone))
            zonesToMaybeCompact.append(zone);
    }

    MOZ_ASSERT(!relocatedArenasToRelease);
    startedCompacting = true;
    return Finished;
}

GCRuntime::IncrementalProgress
GCRuntime::compactPhase(JS::gcreason::Reason reason, SliceBudget& sliceBudget)
{
    MOZ_ASSERT(rt->gc.nursery.isEmpty());
    assertBackgroundSweepingFinished();
    MOZ_ASSERT(startedCompacting);

    gcstats::AutoPhase ap(stats, gcstats::PHASE_COMPACT);

    while (!zonesToMaybeCompact.isEmpty()) {
        Zone* zone = zonesToMaybeCompact.front();
        MOZ_ASSERT(zone->isGCFinished());
        ArenaHeader* relocatedArenas = nullptr;
        if (relocateArenas(zone, reason, relocatedArenas, sliceBudget)) {
            zone->setGCState(Zone::Compact);
            updatePointersToRelocatedCells(zone);
            zone->setGCState(Zone::Finished);
        }
        if (ShouldProtectRelocatedArenas(reason))
            protectAndHoldArenas(relocatedArenas);
        else
            releaseRelocatedArenas(relocatedArenas);
        zonesToMaybeCompact.removeFront();
        if (sliceBudget.isOverBudget())
            break;
    }

#ifdef DEBUG
    CheckHashTablesAfterMovingGC(rt);
#endif

    return zonesToMaybeCompact.isEmpty() ? Finished : NotFinished;
}

void
GCRuntime::endCompactPhase(JS::gcreason::Reason reason)
{
    startedCompacting = false;
}

void
GCRuntime::finishCollection(JS::gcreason::Reason reason)
{
    MOZ_ASSERT(marker.isDrained());
    marker.stop();
    clearBufferedGrayRoots();
    MemProfiler::SweepTenured(rt);

    uint64_t currentTime = PRMJ_Now();
    schedulingState.updateHighFrequencyMode(lastGCTime, currentTime, tunables);

    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        if (zone->isCollecting()) {
            MOZ_ASSERT(zone->isGCFinished());
            zone->setGCState(Zone::NoGC);
            zone->active = false;
        }

        MOZ_ASSERT(!zone->isCollecting());
        MOZ_ASSERT(!zone->wasGCStarted());
    }

    MOZ_ASSERT(zonesToMaybeCompact.isEmpty());

    if (invocationKind == GC_SHRINK) {
        // Ensure excess chunks are returns to the system and free arenas
        // decommitted.
        shrinkBuffers();
    }

    lastGCTime = currentTime;

    // If this is an OOM GC reason, wait on the background sweeping thread
    // before returning to ensure that we free as much as possible. If this is
    // a zeal-triggered GC, we want to ensure that the mutator can continue
    // allocating on the same pages to reduce fragmentation.
    if (IsOOMReason(reason) || reason == JS::gcreason::DEBUG_GC) {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_WAIT_BACKGROUND_THREAD);
        rt->gc.waitBackgroundSweepOrAllocEnd();
    }
}

static const char*
HeapStateToLabel(JS::HeapState heapState)
{
    switch (heapState) {
      case JS::HeapState::MinorCollecting:
        return "js::Nursery::collect";
      case JS::HeapState::MajorCollecting:
        return "js::GCRuntime::collect";
      case JS::HeapState::Tracing:
        return "JS_IterateCompartments";
      case JS::HeapState::Idle:
        MOZ_CRASH("Should never have an Idle heap state when pushing GC pseudo frames!");
    }
    MOZ_ASSERT_UNREACHABLE("Should have exhausted every JS::HeapState variant!");
    return nullptr;
}

/* Start a new heap session. */
AutoTraceSession::AutoTraceSession(JSRuntime* rt, JS::HeapState heapState)
  : lock(rt),
    runtime(rt),
    prevState(rt->heapState_),
    pseudoFrame(rt, HeapStateToLabel(heapState), ProfileEntry::Category::GC)
{
    MOZ_ASSERT(rt->heapState_ == JS::HeapState::Idle);
    MOZ_ASSERT(heapState != JS::HeapState::Idle);
    MOZ_ASSERT_IF(heapState == JS::HeapState::MajorCollecting, rt->gc.nursery.isEmpty());

    // Threads with an exclusive context can hit refillFreeList while holding
    // the exclusive access lock. To avoid deadlocking when we try to acquire
    // this lock during GC and the other thread is waiting, make sure we hold
    // the exclusive access lock during GC sessions.
    MOZ_ASSERT(rt->currentThreadHasExclusiveAccess());

    if (rt->exclusiveThreadsPresent()) {
        // Lock the helper thread state when changing the heap state in the
        // presence of exclusive threads, to avoid racing with refillFreeList.
        AutoLockHelperThreadState lock;
        rt->heapState_ = heapState;
    } else {
        rt->heapState_ = heapState;
    }
}

AutoTraceSession::~AutoTraceSession()
{
    MOZ_ASSERT(runtime->isHeapBusy());

    if (runtime->exclusiveThreadsPresent()) {
        AutoLockHelperThreadState lock;
        runtime->heapState_ = prevState;

        // Notify any helper threads waiting for the trace session to end.
        HelperThreadState().notifyAll(GlobalHelperThreadState::PRODUCER);
    } else {
        runtime->heapState_ = prevState;
    }
}

AutoCopyFreeListToArenas::AutoCopyFreeListToArenas(JSRuntime* rt, ZoneSelector selector)
  : runtime(rt),
    selector(selector)
{
    for (ZonesIter zone(rt, selector); !zone.done(); zone.next())
        zone->arenas.copyFreeListsToArenas();
}

AutoCopyFreeListToArenas::~AutoCopyFreeListToArenas()
{
    for (ZonesIter zone(runtime, selector); !zone.done(); zone.next())
        zone->arenas.clearFreeListsInArenas();
}

class AutoCopyFreeListToArenasForGC
{
    JSRuntime* runtime;

  public:
    explicit AutoCopyFreeListToArenasForGC(JSRuntime* rt) : runtime(rt) {
        MOZ_ASSERT(rt->currentThreadHasExclusiveAccess());
        for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next())
            zone->arenas.copyFreeListsToArenas();
    }
    ~AutoCopyFreeListToArenasForGC() {
        for (ZonesIter zone(runtime, WithAtoms); !zone.done(); zone.next())
            zone->arenas.clearFreeListsInArenas();
    }
};

void
GCRuntime::resetIncrementalGC(const char* reason)
{
    switch (incrementalState) {
      case NO_INCREMENTAL:
        return;

      case MARK: {
        /* Cancel any ongoing marking. */
        AutoCopyFreeListToArenasForGC copy(rt);

        marker.reset();
        marker.stop();
        clearBufferedGrayRoots();

        for (GCCompartmentsIter c(rt); !c.done(); c.next())
            ResetGrayList(c);

        for (GCZonesIter zone(rt); !zone.done(); zone.next()) {
            MOZ_ASSERT(zone->isGCMarking());
            zone->setNeedsIncrementalBarrier(false, Zone::UpdateJit);
            zone->setGCState(Zone::NoGC);
        }

        freeLifoAlloc.freeAll();

        incrementalState = NO_INCREMENTAL;

        MOZ_ASSERT(!marker.shouldCheckCompartments());

        break;
      }

      case SWEEP: {
        marker.reset();

        for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next())
            c->scheduledForDestruction = false;

        /* Finish sweeping the current zone group, then abort. */
        abortSweepAfterCurrentGroup = true;

        /* Don't perform any compaction after sweeping. */
        bool wasCompacting = isCompacting;
        isCompacting = false;

        auto unlimited = SliceBudget::unlimited();
        incrementalCollectSlice(unlimited, JS::gcreason::RESET);

        isCompacting = wasCompacting;

        {
            gcstats::AutoPhase ap(stats, gcstats::PHASE_WAIT_BACKGROUND_THREAD);
            rt->gc.waitBackgroundSweepOrAllocEnd();
        }
        break;
      }

      case COMPACT: {
        {
            gcstats::AutoPhase ap(stats, gcstats::PHASE_WAIT_BACKGROUND_THREAD);
            rt->gc.waitBackgroundSweepOrAllocEnd();
        }

        bool wasCompacting = isCompacting;

        isCompacting = true;
        startedCompacting = true;
        zonesToMaybeCompact.clear();

        auto unlimited = SliceBudget::unlimited();
        incrementalCollectSlice(unlimited, JS::gcreason::RESET);

        isCompacting = wasCompacting;
        break;
      }

      default:
        MOZ_CRASH("Invalid incremental GC state");
    }

    stats.reset(reason);

#ifdef DEBUG
    assertBackgroundSweepingFinished();
    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        MOZ_ASSERT(!zone->isCollecting());
        MOZ_ASSERT(!zone->needsIncrementalBarrier());
        MOZ_ASSERT(!zone->isOnList());
    }
    MOZ_ASSERT(zonesToMaybeCompact.isEmpty());
    MOZ_ASSERT(incrementalState == NO_INCREMENTAL);
#endif
}

namespace {

class AutoGCSlice {
  public:
    explicit AutoGCSlice(JSRuntime* rt);
    ~AutoGCSlice();

  private:
    JSRuntime* runtime;
};

} /* anonymous namespace */

AutoGCSlice::AutoGCSlice(JSRuntime* rt)
  : runtime(rt)
{
    /*
     * During incremental GC, the compartment's active flag determines whether
     * there are stack frames active for any of its scripts. Normally this flag
     * is set at the beginning of the mark phase. During incremental GC, we also
     * set it at the start of every phase.
     */
    for (ActivationIterator iter(rt); !iter.done(); ++iter)
        iter->compartment()->zone()->active = true;

    for (GCZonesIter zone(rt); !zone.done(); zone.next()) {
        /*
         * Clear needsIncrementalBarrier early so we don't do any write
         * barriers during GC. We don't need to update the Ion barriers (which
         * is expensive) because Ion code doesn't run during GC. If need be,
         * we'll update the Ion barriers in ~AutoGCSlice.
         */
        if (zone->isGCMarking()) {
            MOZ_ASSERT(zone->needsIncrementalBarrier());
            zone->setNeedsIncrementalBarrier(false, Zone::DontUpdateJit);
        } else {
            MOZ_ASSERT(!zone->needsIncrementalBarrier());
        }
    }
}

AutoGCSlice::~AutoGCSlice()
{
    /* We can't use GCZonesIter if this is the end of the last slice. */
    for (ZonesIter zone(runtime, WithAtoms); !zone.done(); zone.next()) {
        if (zone->isGCMarking()) {
            zone->setNeedsIncrementalBarrier(true, Zone::UpdateJit);
            zone->arenas.prepareForIncrementalGC(runtime);
        } else {
            zone->setNeedsIncrementalBarrier(false, Zone::UpdateJit);
        }
    }
}

void
GCRuntime::pushZealSelectedObjects()
{
#ifdef JS_GC_ZEAL
    /* Push selected objects onto the mark stack and clear the list. */
    for (JSObject** obj = selectedForMarking.begin(); obj != selectedForMarking.end(); obj++)
        TraceManuallyBarrieredEdge(&marker, obj, "selected obj");
#endif
}

static bool
IsShutdownGC(JS::gcreason::Reason reason)
{
    return reason == JS::gcreason::SHUTDOWN_CC || reason == JS::gcreason::DESTROY_RUNTIME;
}

static bool
ShouldCleanUpEverything(JS::gcreason::Reason reason, JSGCInvocationKind gckind)
{
    // During shutdown, we must clean everything up, for the sake of leak
    // detection. When a runtime has no contexts, or we're doing a GC before a
    // shutdown CC, those are strong indications that we're shutting down.
    return IsShutdownGC(reason) || gckind == GC_SHRINK;
}

void
GCRuntime::incrementalCollectSlice(SliceBudget& budget, JS::gcreason::Reason reason)
{
    MOZ_ASSERT(rt->currentThreadHasExclusiveAccess());

    AutoCopyFreeListToArenasForGC copy(rt);
    AutoGCSlice slice(rt);

    bool destroyingRuntime = (reason == JS::gcreason::DESTROY_RUNTIME);

    gc::State initialState = incrementalState;

    int zeal = 0;
#ifdef JS_GC_ZEAL
    if (reason == JS::gcreason::DEBUG_GC && !budget.isUnlimited()) {
        /*
         * Do the incremental collection type specified by zeal mode if the
         * collection was triggered by runDebugGC() and incremental GC has not
         * been cancelled by resetIncrementalGC().
         */
        zeal = zealMode;
    }
#endif

    MOZ_ASSERT_IF(isIncrementalGCInProgress(), isIncremental);
    isIncremental = !budget.isUnlimited();

    if (zeal == ZealIncrementalRootsThenFinish || zeal == ZealIncrementalMarkAllThenFinish) {
        /*
         * Yields between slices occurs at predetermined points in these modes;
         * the budget is not used.
         */
        budget.makeUnlimited();
    }

    switch (incrementalState) {
      case NO_INCREMENTAL:
        initialReason = reason;
        cleanUpEverything = ShouldCleanUpEverything(reason, invocationKind);
        isCompacting = shouldCompact();
        lastMarkSlice = false;

        incrementalState = MARK_ROOTS;
        /* fall through */

      case MARK_ROOTS:
        if (!beginMarkPhase(reason)) {
            incrementalState = NO_INCREMENTAL;
            return;
        }

        if (!destroyingRuntime)
            pushZealSelectedObjects();

        incrementalState = MARK;

        if (isIncremental && zeal == ZealIncrementalRootsThenFinish)
            break;

        /* fall through */

      case MARK:
        AutoGCRooter::traceAllWrappers(&marker);

        /* If we needed delayed marking for gray roots, then collect until done. */
        if (!hasBufferedGrayRoots()) {
            budget.makeUnlimited();
            isIncremental = false;
        }

        if (drainMarkStack(budget, gcstats::PHASE_MARK) == NotFinished)
            break;

        MOZ_ASSERT(marker.isDrained());

        if (!lastMarkSlice && isIncremental &&
            ((initialState == MARK && zeal != ZealIncrementalRootsThenFinish) ||
             zeal == ZealIncrementalMarkAllThenFinish))
        {
            /*
             * Yield with the aim of starting the sweep in the next
             * slice.  We will need to mark anything new on the stack
             * when we resume, so we stay in MARK state.
             */
            lastMarkSlice = true;
            break;
        }

        incrementalState = SWEEP;

        /*
         * This runs to completion, but we don't continue if the budget is
         * now exhasted.
         */
        beginSweepPhase(destroyingRuntime);
        if (budget.isOverBudget())
            break;

        /*
         * Always yield here when running in incremental multi-slice zeal
         * mode, so RunDebugGC can reset the slice buget.
         */
        if (isIncremental && zeal == ZealIncrementalMultipleSlices)
            break;

        /* fall through */

      case SWEEP:
        if (sweepPhase(budget) == NotFinished)
            break;

        endSweepPhase(destroyingRuntime);

        incrementalState = COMPACT;
        MOZ_ASSERT(!startedCompacting);

        /* Yield before compacting since it is not incremental. */
        if (isCompacting && isIncremental)
            break;

      case COMPACT:
        if (isCompacting) {
            if (!startedCompacting && beginCompactPhase() == NotFinished)
                break;

            if (compactPhase(reason, budget) == NotFinished)
                break;

            endCompactPhase(reason);
        }

        finishCollection(reason);

        incrementalState = NO_INCREMENTAL;
        break;

      default:
        MOZ_ASSERT(false);
    }
}

IncrementalSafety
gc::IsIncrementalGCSafe(JSRuntime* rt)
{
    MOZ_ASSERT(!rt->mainThread.suppressGC);

    if (rt->keepAtoms())
        return IncrementalSafety::Unsafe("keepAtoms set");

    if (!rt->gc.isIncrementalGCAllowed())
        return IncrementalSafety::Unsafe("incremental permanently disabled");

    return IncrementalSafety::Safe();
}

void
GCRuntime::budgetIncrementalGC(SliceBudget& budget)
{
    IncrementalSafety safe = IsIncrementalGCSafe(rt);
    if (!safe) {
        resetIncrementalGC(safe.reason());
        budget.makeUnlimited();
        stats.nonincremental(safe.reason());
        return;
    }

    if (mode != JSGC_MODE_INCREMENTAL) {
        resetIncrementalGC("GC mode change");
        budget.makeUnlimited();
        stats.nonincremental("GC mode");
        return;
    }

    if (isTooMuchMalloc()) {
        budget.makeUnlimited();
        stats.nonincremental("malloc bytes trigger");
    }

    bool reset = false;
    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        if (zone->usage.gcBytes() >= zone->threshold.gcTriggerBytes()) {
            budget.makeUnlimited();
            stats.nonincremental("allocation trigger");
        }

        if (isIncrementalGCInProgress() && zone->isGCScheduled() != zone->wasGCStarted())
            reset = true;

        if (zone->isTooMuchMalloc()) {
            budget.makeUnlimited();
            stats.nonincremental("malloc bytes trigger");
        }
    }

    if (reset)
        resetIncrementalGC("zone change");
}

namespace {

class AutoScheduleZonesForGC
{
    JSRuntime* rt_;

  public:
    explicit AutoScheduleZonesForGC(JSRuntime* rt) : rt_(rt) {
        for (ZonesIter zone(rt_, WithAtoms); !zone.done(); zone.next()) {
            if (rt->gc.gcMode() == JSGC_MODE_GLOBAL)
                zone->scheduleGC();

            /* This is a heuristic to avoid resets. */
            if (rt->gc.isIncrementalGCInProgress() && zone->needsIncrementalBarrier())
                zone->scheduleGC();

            /* This is a heuristic to reduce the total number of collections. */
            if (zone->usage.gcBytes() >=
                zone->threshold.allocTrigger(rt->gc.schedulingState.inHighFrequencyGCMode()))
            {
                zone->scheduleGC();
            }
        }
    }

    ~AutoScheduleZonesForGC() {
        for (ZonesIter zone(rt_, WithAtoms); !zone.done(); zone.next())
            zone->unscheduleGC();
    }
};

} /* anonymous namespace */

/*
 * Run one GC "cycle" (either a slice of incremental GC or an entire
 * non-incremental GC. We disable inlining to ensure that the bottom of the
 * stack with possible GC roots recorded in MarkRuntime excludes any pointers we
 * use during the marking implementation.
 *
 * Returns true if we "reset" an existing incremental GC, which would force us
 * to run another cycle.
 */
MOZ_NEVER_INLINE bool
GCRuntime::gcCycle(bool nonincrementalByAPI, SliceBudget& budget, JS::gcreason::Reason reason)
{
    AutoNotifyGCActivity notify(*this);

    evictNursery(reason);

    AutoTraceSession session(rt, JS::HeapState::MajorCollecting);

    majorGCTriggerReason = JS::gcreason::NO_REASON;
    interFrameGC = true;

    number++;
    if (!isIncrementalGCInProgress())
        incMajorGcNumber();

    // It's ok if threads other than the main thread have suppressGC set, as
    // they are operating on zones which will not be collected from here.
    MOZ_ASSERT(!rt->mainThread.suppressGC);

    // Assert if this is a GC unsafe region.
    JS::AutoAssertOnGC::VerifyIsSafeToGC(rt);

    {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_WAIT_BACKGROUND_THREAD);

        // As we are about to clear the mark bits, wait for background
        // finalization to finish. We only need to wait on the first slice.
        if (!isIncrementalGCInProgress())
            waitBackgroundSweepEnd();

        // We must also wait for background allocation to finish so we can
        // avoid taking the GC lock when manipulating the chunks during the GC.
        // The background alloc task can run between slices, so we must wait
        // for it at the start of every slice.
        allocTask.cancel(GCParallelTask::CancelAndWait);
    }

    State prevState = incrementalState;

    if (nonincrementalByAPI) {
        // Reset any in progress incremental GC if this was triggered via the
        // API. This isn't required for correctness, but sometimes during tests
        // the caller expects this GC to collect certain objects, and we need
        // to make sure to collect everything possible.
        if (reason != JS::gcreason::ALLOC_TRIGGER)
            resetIncrementalGC("requested");

        stats.nonincremental("requested");
        budget.makeUnlimited();
    } else {
        budgetIncrementalGC(budget);
    }

    /* The GC was reset, so we need a do-over. */
    if (prevState != NO_INCREMENTAL && !isIncrementalGCInProgress())
        return true;

    TraceMajorGCStart();

    incrementalCollectSlice(budget, reason);

#ifndef JS_MORE_DETERMINISTIC
    nextFullGCTime = PRMJ_Now() + GC_IDLE_FULL_SPAN;
#endif

    chunkAllocationSinceLastGC = false;

#ifdef JS_GC_ZEAL
    /* Keeping these around after a GC is dangerous. */
    clearSelectedForMarking();
#endif

    /* Clear gcMallocBytes for all zones. */
    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next())
        zone->resetGCMallocBytes();

    resetMallocBytes();

    TraceMajorGCEnd();

    return false;
}

#ifdef JS_GC_ZEAL
static bool
IsDeterministicGCReason(JS::gcreason::Reason reason)
{
    if (reason > JS::gcreason::DEBUG_GC &&
        reason != JS::gcreason::CC_FORCED && reason != JS::gcreason::SHUTDOWN_CC)
    {
        return false;
    }

    if (reason == JS::gcreason::EAGER_ALLOC_TRIGGER)
        return false;

    return true;
}
#endif

gcstats::ZoneGCStats
GCRuntime::scanZonesBeforeGC()
{
    gcstats::ZoneGCStats zoneStats;
    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        zoneStats.zoneCount++;
        if (zone->isGCScheduled()) {
            zoneStats.collectedZoneCount++;
            zoneStats.collectedCompartmentCount += zone->compartments.length();
        }
    }

    for (CompartmentsIter c(rt, WithAtoms); !c.done(); c.next())
        zoneStats.compartmentCount++;

    return zoneStats;
}

void
GCRuntime::checkCanCallAPI()
{
    JS_AbortIfWrongThread(rt);

    /* If we attempt to invoke the GC while we are running in the GC, assert. */
    MOZ_RELEASE_ASSERT(!rt->isHeapBusy());

    /* The engine never locks across anything that could GC. */
    MOZ_ASSERT(!rt->currentThreadHasExclusiveAccess());

    MOZ_ASSERT(isAllocAllowed());
}

bool
GCRuntime::checkIfGCAllowedInCurrentState(JS::gcreason::Reason reason)
{
    if (rt->mainThread.suppressGC)
        return false;

#ifdef JS_GC_ZEAL
    if (deterministicOnly && !IsDeterministicGCReason(reason))
        return false;
#endif

    return true;
}

void
GCRuntime::collect(bool nonincrementalByAPI, SliceBudget budget, JS::gcreason::Reason reason)
{
    // Checks run for each request, even if we do not actually GC.
    checkCanCallAPI();

    // Check if we are allowed to GC at this time before proceeding.
    if (!checkIfGCAllowedInCurrentState(reason))
        return;

    AutoTraceLog logGC(TraceLoggerForMainThread(rt), TraceLogger_GC);
    AutoStopVerifyingBarriers av(rt, IsShutdownGC(reason));
    AutoEnqueuePendingParseTasksAfterGC aept(*this);
    AutoScheduleZonesForGC asz(rt);
    gcstats::AutoGCSlice agc(stats, scanZonesBeforeGC(), invocationKind, budget, reason);

    bool repeat = false;
    do {
        poked = false;
        bool wasReset = gcCycle(nonincrementalByAPI, budget, reason);

        /* Need to re-schedule all zones for GC. */
        if (poked && cleanUpEverything)
            JS::PrepareForFullGC(rt);

        /*
         * This code makes an extra effort to collect compartments that we
         * thought were dead at the start of the GC. See the large comment in
         * beginMarkPhase.
         */
        bool repeatForDeadZone = false;
        if (!nonincrementalByAPI && !isIncrementalGCInProgress()) {
            for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next()) {
                if (c->scheduledForDestruction) {
                    nonincrementalByAPI = true;
                    repeatForDeadZone = true;
                    reason = JS::gcreason::COMPARTMENT_REVIVED;
                    c->zone()->scheduleGC();
                }
            }
        }

        /*
         * If we reset an existing GC, we need to start a new one. Also, we
         * repeat GCs that happen during shutdown (the gcShouldCleanUpEverything
         * case) until we can be sure that no additional garbage is created
         * (which typically happens if roots are dropped during finalizers).
         */
        repeat = (poked && cleanUpEverything) || wasReset || repeatForDeadZone;
    } while (repeat);
}

js::AutoEnqueuePendingParseTasksAfterGC::~AutoEnqueuePendingParseTasksAfterGC()
{
    if (!gc_.isIncrementalGCInProgress())
        EnqueuePendingParseTasksAfterGC(gc_.rt);
}

SliceBudget
GCRuntime::defaultBudget(JS::gcreason::Reason reason, int64_t millis)
{
    if (millis == 0) {
        if (reason == JS::gcreason::ALLOC_TRIGGER)
            millis = defaultSliceBudget();
        else if (schedulingState.inHighFrequencyGCMode() && tunables.isDynamicMarkSliceEnabled())
            millis = defaultSliceBudget() * IGC_MARK_SLICE_MULTIPLIER;
        else
            millis = defaultSliceBudget();
    }

    return SliceBudget(TimeBudget(millis));
}

void
GCRuntime::gc(JSGCInvocationKind gckind, JS::gcreason::Reason reason)
{
    invocationKind = gckind;
    collect(true, SliceBudget::unlimited(), reason);
}

void
GCRuntime::startGC(JSGCInvocationKind gckind, JS::gcreason::Reason reason, int64_t millis)
{
    MOZ_ASSERT(!isIncrementalGCInProgress());
    invocationKind = gckind;
    collect(false, defaultBudget(reason, millis), reason);
}

void
GCRuntime::gcSlice(JS::gcreason::Reason reason, int64_t millis)
{
    MOZ_ASSERT(isIncrementalGCInProgress());
    collect(false, defaultBudget(reason, millis), reason);
}

void
GCRuntime::finishGC(JS::gcreason::Reason reason)
{
    MOZ_ASSERT(isIncrementalGCInProgress());

    // If we're not collecting because we're out of memory then skip the
    // compacting phase if we need to finish an ongoing incremental GC
    // non-incrementally to avoid janking the browser.
    if (!IsOOMReason(initialReason)) {
        if (incrementalState == COMPACT) {
            abortGC();
            return;
        }

        isCompacting = false;
    }

    collect(false, SliceBudget::unlimited(), reason);
}

void
GCRuntime::abortGC()
{
    checkCanCallAPI();
    MOZ_ASSERT(!rt->mainThread.suppressGC);

    AutoStopVerifyingBarriers av(rt, false);

    gcstats::AutoGCSlice agc(stats, scanZonesBeforeGC(), invocationKind,
                             SliceBudget::unlimited(), JS::gcreason::ABORT_GC);

    evictNursery(JS::gcreason::ABORT_GC);
    AutoTraceSession session(rt, JS::HeapState::MajorCollecting);

    number++;
    resetIncrementalGC("abort");
}

void
GCRuntime::notifyDidPaint()
{
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));

#ifdef JS_GC_ZEAL
    if (zealMode == ZealFrameVerifierPreValue) {
        verifyPreBarriers();
        return;
    }

    if (zealMode == ZealFrameGCValue) {
        JS::PrepareForFullGC(rt);
        gc(GC_NORMAL, JS::gcreason::REFRESH_FRAME);
        return;
    }
#endif

    if (isIncrementalGCInProgress() && !interFrameGC) {
        JS::PrepareForIncrementalGC(rt);
        gcSlice(JS::gcreason::REFRESH_FRAME);
    }

    interFrameGC = false;
}

static bool
ZonesSelected(JSRuntime* rt)
{
    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        if (zone->isGCScheduled())
            return true;
    }
    return false;
}

void
GCRuntime::startDebugGC(JSGCInvocationKind gckind, SliceBudget& budget)
{
    MOZ_ASSERT(!isIncrementalGCInProgress());
    if (!ZonesSelected(rt))
        JS::PrepareForFullGC(rt);
    invocationKind = gckind;
    collect(false, budget, JS::gcreason::DEBUG_GC);
}

void
GCRuntime::debugGCSlice(SliceBudget& budget)
{
    MOZ_ASSERT(isIncrementalGCInProgress());
    if (!ZonesSelected(rt))
        JS::PrepareForIncrementalGC(rt);
    collect(false, budget, JS::gcreason::DEBUG_GC);
}

/* Schedule a full GC unless a zone will already be collected. */
void
js::PrepareForDebugGC(JSRuntime* rt)
{
    if (!ZonesSelected(rt))
        JS::PrepareForFullGC(rt);
}

JS_PUBLIC_API(void)
JS::ShrinkGCBuffers(JSRuntime* rt)
{
    MOZ_ASSERT(!rt->isHeapBusy());
    rt->gc.shrinkBuffers();
}

void
GCRuntime::shrinkBuffers()
{
    AutoLockHelperThreadState helperLock;
    AutoLockGC lock(rt);

    if (CanUseExtraThreads())
        helperState.startBackgroundShrink(lock);
    else
        expireChunksAndArenas(true, lock);
}

void
GCRuntime::onOutOfMallocMemory()
{
    // Stop allocating new chunks.
    allocTask.cancel(GCParallelTask::CancelAndWait);

    // Wait for background free of nursery huge slots to finish.
    nursery.waitBackgroundFreeEnd();

    AutoLockGC lock(rt);
    onOutOfMallocMemory(lock);
}

void
GCRuntime::onOutOfMallocMemory(const AutoLockGC& lock)
{
    // Release any relocated arenas we may be holding on to, without releasing
    // the GC lock.
    releaseHeldRelocatedArenasWithoutUnlocking(lock);

    // Throw away any excess chunks we have lying around.
    freeEmptyChunks(rt, lock);

    // Immediately decommit as many arenas as possible in the hopes that this
    // might let the OS scrape together enough pages to satisfy the failing
    // malloc request.
    decommitAllWithoutUnlocking(lock);
}

void
GCRuntime::minorGCImpl(JS::gcreason::Reason reason, Nursery::ObjectGroupList* pretenureGroups)
{
    minorGCTriggerReason = JS::gcreason::NO_REASON;
    TraceLoggerThread* logger = TraceLoggerForMainThread(rt);
    AutoTraceLog logMinorGC(logger, TraceLogger_MinorGC);
    nursery.collect(rt, reason, pretenureGroups);
    MOZ_ASSERT_IF(!rt->mainThread.suppressGC, nursery.isEmpty());
}

// Alternate to the runtime-taking form that allows marking object groups as
// needing pretenuring.
void
GCRuntime::minorGC(JSContext* cx, JS::gcreason::Reason reason)
{
    gcstats::AutoPhase ap(stats, gcstats::PHASE_MINOR_GC);

    Nursery::ObjectGroupList pretenureGroups;
    minorGCImpl(reason, &pretenureGroups);
    for (size_t i = 0; i < pretenureGroups.length(); i++) {
        if (pretenureGroups[i]->canPreTenure())
            pretenureGroups[i]->setShouldPreTenure(cx);
    }
}

void
GCRuntime::clearPostBarrierCallbacks()
{
    if (storeBuffer.hasPostBarrierCallbacks())
        evictNursery();
}

void
GCRuntime::disableGenerationalGC()
{
    if (isGenerationalGCEnabled()) {
        minorGC(JS::gcreason::API);
        nursery.disable();
        storeBuffer.disable();
    }
    ++rt->gc.generationalDisabled;
}

void
GCRuntime::enableGenerationalGC()
{
    MOZ_ASSERT(generationalDisabled > 0);
    --generationalDisabled;
    if (generationalDisabled == 0) {
        nursery.enable();
        storeBuffer.enable();
    }
}

bool
GCRuntime::gcIfRequested(JSContext* cx /* = nullptr */)
{
    // This method returns whether a major GC was performed.

    if (minorGCRequested()) {
        if (cx)
            minorGC(cx, minorGCTriggerReason);
        else
            minorGC(minorGCTriggerReason);
    }

    if (majorGCRequested()) {
        if (!isIncrementalGCInProgress())
            startGC(GC_NORMAL, majorGCTriggerReason);
        else
            gcSlice(majorGCTriggerReason);
        return true;
    }

    return false;
}

AutoFinishGC::AutoFinishGC(JSRuntime* rt)
{
    if (JS::IsIncrementalGCInProgress(rt)) {
        JS::PrepareForIncrementalGC(rt);
        JS::FinishIncrementalGC(rt, JS::gcreason::API);
    }

    rt->gc.waitBackgroundSweepEnd();
    rt->gc.nursery.waitBackgroundFreeEnd();
}

AutoPrepareForTracing::AutoPrepareForTracing(JSRuntime* rt, ZoneSelector selector)
  : finish(rt),
    session(rt),
    copy(rt, selector)
{
}

JSCompartment*
js::NewCompartment(JSContext* cx, Zone* zone, JSPrincipals* principals,
                   const JS::CompartmentOptions& options)
{
    JSRuntime* rt = cx->runtime();
    JS_AbortIfWrongThread(rt);

    ScopedJSDeletePtr<Zone> zoneHolder;
    if (!zone) {
        zone = cx->new_<Zone>(rt);
        if (!zone)
            return nullptr;

        zoneHolder.reset(zone);

        const JSPrincipals* trusted = rt->trustedPrincipals();
        bool isSystem = principals && principals == trusted;
        if (!zone->init(isSystem)) {
            ReportOutOfMemory(cx);
            return nullptr;
        }
    }

    ScopedJSDeletePtr<JSCompartment> compartment(cx->new_<JSCompartment>(zone, options));
    if (!compartment || !compartment->init(cx))
        return nullptr;

    // Set up the principals.
    JS_SetCompartmentPrincipals(compartment, principals);

    AutoLockGC lock(rt);

    if (!zone->compartments.append(compartment.get())) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    if (zoneHolder && !rt->gc.zones.append(zone)) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    zoneHolder.forget();
    return compartment.forget();
}

void
gc::MergeCompartments(JSCompartment* source, JSCompartment* target)
{
    // The source compartment must be specifically flagged as mergable.  This
    // also implies that the compartment is not visible to the debugger.
    MOZ_ASSERT(source->options_.mergeable());

    MOZ_ASSERT(source->addonId == target->addonId);

    JSRuntime* rt = source->runtimeFromMainThread();

    AutoPrepareForTracing prepare(rt, SkipAtoms);

    // Cleanup tables and other state in the source compartment that will be
    // meaningless after merging into the target compartment.

    source->clearTables();
    source->unsetIsDebuggee();

    // The delazification flag indicates the presence of LazyScripts in a
    // compartment for the Debugger API, so if the source compartment created
    // LazyScripts, the flag must be propagated to the target compartment.
    if (source->needsDelazificationForDebugger())
        target->scheduleDelazificationForDebugger();

    // Release any relocated arenas which we may be holding on to as they might
    // be in the source zone
    rt->gc.releaseHeldRelocatedArenas();

    // Fixup compartment pointers in source to refer to target, and make sure
    // type information generations are in sync.

    // Get the static global lexical scope of the target compartment. Static
    // scopes need to be fixed up below.
    RootedObject targetStaticGlobalLexicalScope(rt);
    targetStaticGlobalLexicalScope = &target->maybeGlobal()->lexicalScope().staticBlock();

    for (ZoneCellIter iter(source->zone(), AllocKind::SCRIPT); !iter.done(); iter.next()) {
        JSScript* script = iter.get<JSScript>();
        MOZ_ASSERT(script->compartment() == source);
        script->compartment_ = target;
        script->setTypesGeneration(target->zone()->types.generation);

        // See warning in handleParseWorkload. If we start optimizing global
        // lexicals, we would need to merge the contents of the static global
        // lexical scope.
        if (JSObject* enclosing = script->enclosingStaticScope()) {
            if (IsStaticGlobalLexicalScope(enclosing))
                script->fixEnclosingStaticGlobalLexicalScope();
        }

        if (script->hasBlockScopes()) {
            BlockScopeArray* scopes = script->blockScopes();
            for (uint32_t i = 0; i < scopes->length; i++) {
                uint32_t scopeIndex = scopes->vector[i].index;
                if (scopeIndex != BlockScopeNote::NoBlockScopeIndex) {
                    ScopeObject* scope = &script->getObject(scopeIndex)->as<ScopeObject>();
                    MOZ_ASSERT(!IsStaticGlobalLexicalScope(scope));
                    JSObject* enclosing = &scope->enclosingScope();
                    if (IsStaticGlobalLexicalScope(enclosing))
                        scope->setEnclosingScope(targetStaticGlobalLexicalScope);
                }
            }
        }
    }

    for (ZoneCellIter iter(source->zone(), AllocKind::BASE_SHAPE); !iter.done(); iter.next()) {
        BaseShape* base = iter.get<BaseShape>();
        MOZ_ASSERT(base->compartment() == source);
        base->compartment_ = target;
    }

    for (ZoneCellIter iter(source->zone(), AllocKind::OBJECT_GROUP); !iter.done(); iter.next()) {
        ObjectGroup* group = iter.get<ObjectGroup>();
        group->setGeneration(target->zone()->types.generation);
        group->compartment_ = target;

        // Remove any unboxed layouts from the list in the off thread
        // compartment. These do not need to be reinserted in the target
        // compartment's list, as the list is not required to be complete.
        if (UnboxedLayout* layout = group->maybeUnboxedLayoutDontCheckGeneration())
            layout->detachFromCompartment();
    }

    // Fixup zone pointers in source's zone to refer to target's zone.

    for (auto thingKind : AllAllocKinds()) {
        for (ArenaIter aiter(source->zone(), thingKind); !aiter.done(); aiter.next()) {
            ArenaHeader* aheader = aiter.get();
            aheader->zone = target->zone();
        }
    }

    // After fixing JSFunctions' compartments, we can fix LazyScripts'
    // enclosing scopes.
    for (ZoneCellIter iter(source->zone(), AllocKind::LAZY_SCRIPT); !iter.done(); iter.next()) {
        LazyScript* lazy = iter.get<LazyScript>();
        MOZ_ASSERT(lazy->functionNonDelazifying()->compartment() == target);

        // See warning in handleParseWorkload. If we start optimizing global
        // lexicals, we would need to merge the contents of the static global
        // lexical scope.
        if (JSObject* enclosing = lazy->enclosingScope()) {
            if (IsStaticGlobalLexicalScope(enclosing))
                lazy->fixEnclosingStaticGlobalLexicalScope();
        }
    }

    // The source should be the only compartment in its zone.
    for (CompartmentsInZoneIter c(source->zone()); !c.done(); c.next())
        MOZ_ASSERT(c.get() == source);

    // Merge the allocator in source's zone into target's zone.
    target->zone()->arenas.adoptArenas(rt, &source->zone()->arenas);
    target->zone()->usage.adopt(source->zone()->usage);

    // Merge other info in source's zone into target's zone.
    target->zone()->types.typeLifoAlloc.transferFrom(&source->zone()->types.typeLifoAlloc);

    // Ensure that we did not create any UIDs when running off-thread.
    source->zone()->assertNoUniqueIdsInZone();
}

void
GCRuntime::runDebugGC()
{
#ifdef JS_GC_ZEAL
    int type = zealMode;

    if (rt->mainThread.suppressGC)
        return;

    if (type == js::gc::ZealGenerationalGCValue)
        return minorGC(JS::gcreason::DEBUG_GC);

    PrepareForDebugGC(rt);

    auto budget = SliceBudget::unlimited();
    if (type == ZealIncrementalRootsThenFinish ||
        type == ZealIncrementalMarkAllThenFinish ||
        type == ZealIncrementalMultipleSlices)
    {
        js::gc::State initialState = incrementalState;
        if (type == ZealIncrementalMultipleSlices) {
            /*
             * Start with a small slice limit and double it every slice. This
             * ensure that we get multiple slices, and collection runs to
             * completion.
             */
            if (!isIncrementalGCInProgress())
                incrementalLimit = zealFrequency / 2;
            else
                incrementalLimit *= 2;
            budget = SliceBudget(WorkBudget(incrementalLimit));
        } else {
            // This triggers incremental GC but is actually ignored by IncrementalMarkSlice.
            budget = SliceBudget(WorkBudget(1));
        }

        if (!isIncrementalGCInProgress())
            invocationKind = GC_SHRINK;
        collect(false, budget, JS::gcreason::DEBUG_GC);

        /*
         * For multi-slice zeal, reset the slice size when we get to the sweep
         * or compact phases.
         */
        if (type == ZealIncrementalMultipleSlices) {
            if ((initialState == MARK && incrementalState == SWEEP) ||
                (initialState == SWEEP && incrementalState == COMPACT))
            {
                incrementalLimit = zealFrequency / 2;
            }
        }
    } else if (type == ZealCompactValue) {
        gc(GC_SHRINK, JS::gcreason::DEBUG_GC);
    } else {
        gc(GC_NORMAL, JS::gcreason::DEBUG_GC);
    }

#endif
}

void
GCRuntime::setValidate(bool enabled)
{
    MOZ_ASSERT(!rt->isHeapMajorCollecting());
    validate = enabled;
}

void
GCRuntime::setFullCompartmentChecks(bool enabled)
{
    MOZ_ASSERT(!rt->isHeapMajorCollecting());
    fullCompartmentChecks = enabled;
}

#ifdef JS_GC_ZEAL
bool
GCRuntime::selectForMarking(JSObject* object)
{
    MOZ_ASSERT(!rt->isHeapMajorCollecting());
    return selectedForMarking.append(object);
}

void
GCRuntime::clearSelectedForMarking()
{
    selectedForMarking.clearAndFree();
}

void
GCRuntime::setDeterministic(bool enabled)
{
    MOZ_ASSERT(!rt->isHeapMajorCollecting());
    deterministicOnly = enabled;
}
#endif

#ifdef DEBUG

/* Should only be called manually under gdb */
void PreventGCDuringInteractiveDebug()
{
    TlsPerThreadData.get()->suppressGC++;
}

#endif

void
js::ReleaseAllJITCode(FreeOp* fop)
{
    /*
     * Scripts can entrain nursery things, inserting references to the script
     * into the store buffer. Clear the store buffer before discarding scripts.
     */
    fop->runtime()->gc.evictNursery();

    for (ZonesIter zone(fop->runtime(), SkipAtoms); !zone.done(); zone.next()) {
        if (!zone->jitZone())
            continue;

#ifdef DEBUG
        /* Assert no baseline scripts are marked as active. */
        for (ZoneCellIter i(zone, AllocKind::SCRIPT); !i.done(); i.next()) {
            JSScript* script = i.get<JSScript>();
            MOZ_ASSERT_IF(script->hasBaselineScript(), !script->baselineScript()->active());
        }
#endif

        /* Mark baseline scripts on the stack as active. */
        jit::MarkActiveBaselineScripts(zone);

        jit::InvalidateAll(fop, zone);

        for (ZoneCellIter i(zone, AllocKind::SCRIPT); !i.done(); i.next()) {
            JSScript* script = i.get<JSScript>();
            jit::FinishInvalidation(fop, script);

            /*
             * Discard baseline script if it's not marked as active. Note that
             * this also resets the active flag.
             */
            jit::FinishDiscardBaselineScript(fop, script);
        }

        zone->jitZone()->optimizedStubSpace()->free();
    }
}

void
js::PurgeJITCaches(Zone* zone)
{
    for (ZoneCellIterUnderGC i(zone, AllocKind::SCRIPT); !i.done(); i.next()) {
        JSScript* script = i.get<JSScript>();

        /* Discard Ion caches. */
        jit::PurgeCaches(script);
    }
}

void
ArenaLists::normalizeBackgroundFinalizeState(AllocKind thingKind)
{
    ArenaLists::BackgroundFinalizeState* bfs = &backgroundFinalizeState[thingKind];
    switch (*bfs) {
      case BFS_DONE:
        break;
      default:
        MOZ_ASSERT(!"Background finalization in progress, but it should not be.");
        break;
    }
}

void
ArenaLists::adoptArenas(JSRuntime* rt, ArenaLists* fromArenaLists)
{
    // GC should be inactive, but still take the lock as a kind of read fence.
    AutoLockGC lock(rt);

    fromArenaLists->purge();

    for (auto thingKind : AllAllocKinds()) {
        // When we enter a parallel section, we join the background
        // thread, and we do not run GC while in the parallel section,
        // so no finalizer should be active!
        normalizeBackgroundFinalizeState(thingKind);
        fromArenaLists->normalizeBackgroundFinalizeState(thingKind);

        ArenaList* fromList = &fromArenaLists->arenaLists[thingKind];
        ArenaList* toList = &arenaLists[thingKind];
        fromList->check();
        toList->check();
        ArenaHeader* next;
        for (ArenaHeader* fromHeader = fromList->head(); fromHeader; fromHeader = next) {
            // Copy fromHeader->next before releasing/reinserting.
            next = fromHeader->next;

            MOZ_ASSERT(!fromHeader->isEmpty());
            toList->insertAtCursor(fromHeader);
        }
        fromList->clear();
        toList->check();
    }
}

bool
ArenaLists::containsArena(JSRuntime* rt, ArenaHeader* needle)
{
    AutoLockGC lock(rt);
    ArenaList& list = arenaLists[needle->getAllocKind()];
    for (ArenaHeader* aheader = list.head(); aheader; aheader = aheader->next) {
        if (aheader == needle)
            return true;
    }
    return false;
}


AutoSuppressGC::AutoSuppressGC(ExclusiveContext* cx)
  : suppressGC_(cx->perThreadData->suppressGC)
{
    suppressGC_++;
}

AutoSuppressGC::AutoSuppressGC(JSCompartment* comp)
  : suppressGC_(comp->runtimeFromMainThread()->mainThread.suppressGC)
{
    suppressGC_++;
}

AutoSuppressGC::AutoSuppressGC(JSRuntime* rt)
  : suppressGC_(rt->mainThread.suppressGC)
{
    suppressGC_++;
}

bool
js::UninlinedIsInsideNursery(const gc::Cell* cell)
{
    return IsInsideNursery(cell);
}

#ifdef DEBUG
AutoDisableProxyCheck::AutoDisableProxyCheck(JSRuntime* rt)
  : gc(rt->gc)
{
    gc.disableStrictProxyChecking();
}

AutoDisableProxyCheck::~AutoDisableProxyCheck()
{
    gc.enableStrictProxyChecking();
}

JS_FRIEND_API(void)
JS::AssertGCThingMustBeTenured(JSObject* obj)
{
    MOZ_ASSERT(obj->isTenured() &&
               (!IsNurseryAllocable(obj->asTenured().getAllocKind()) || obj->getClass()->finalize));
}

JS_FRIEND_API(void)
JS::AssertGCThingIsNotAnObjectSubclass(Cell* cell)
{
    MOZ_ASSERT(cell);
    MOZ_ASSERT(cell->getTraceKind() != JS::TraceKind::Object);
}

JS_FRIEND_API(void)
js::gc::AssertGCThingHasType(js::gc::Cell* cell, JS::TraceKind kind)
{
    if (!cell)
        MOZ_ASSERT(kind == JS::TraceKind::Null);
    else if (IsInsideNursery(cell))
        MOZ_ASSERT(kind == JS::TraceKind::Object);
    else
        MOZ_ASSERT(MapAllocToTraceKind(cell->asTenured().getAllocKind()) == kind);
}

JS_PUBLIC_API(size_t)
JS::GetGCNumber()
{
    JSRuntime* rt = js::TlsPerThreadData.get()->runtimeFromMainThread();
    if (!rt)
        return 0;
    return rt->gc.gcNumber();
}
#endif

#ifdef DEBUG
JS::AutoAssertOnGC::AutoAssertOnGC()
  : gc(nullptr), gcNumber(0)
{
    js::PerThreadData* data = js::TlsPerThreadData.get();
    if (data) {
        /*
         * GC's from off-thread will always assert, so off-thread is implicitly
         * AutoAssertOnGC. We still need to allow AutoAssertOnGC to be used in
         * code that works from both threads, however. We also use this to
         * annotate the off thread run loops.
         */
        JSRuntime* runtime = data->runtimeIfOnOwnerThread();
        if (runtime) {
            gc = &runtime->gc;
            gcNumber = gc->gcNumber();
            gc->enterUnsafeRegion();
        }
    }
}

JS::AutoAssertOnGC::AutoAssertOnGC(JSRuntime* rt)
  : gc(&rt->gc), gcNumber(rt->gc.gcNumber())
{
    gc->enterUnsafeRegion();
}

JS::AutoAssertOnGC::~AutoAssertOnGC()
{
    if (gc) {
        gc->leaveUnsafeRegion();

        /*
         * The following backstop assertion should never fire: if we bumped the
         * gcNumber, we should have asserted because inUnsafeRegion was true.
         */
        MOZ_ASSERT(gcNumber == gc->gcNumber(), "GC ran inside an AutoAssertOnGC scope.");
    }
}

/* static */ void
JS::AutoAssertOnGC::VerifyIsSafeToGC(JSRuntime* rt)
{
    if (rt->gc.isInsideUnsafeRegion())
        MOZ_CRASH("[AutoAssertOnGC] possible GC in GC-unsafe region");
}

JS::AutoAssertNoAlloc::AutoAssertNoAlloc(JSRuntime* rt)
  : gc(nullptr)
{
    disallowAlloc(rt);
}

void JS::AutoAssertNoAlloc::disallowAlloc(JSRuntime* rt)
{
    MOZ_ASSERT(!gc);
    gc = &rt->gc;
    gc->disallowAlloc();
}

JS::AutoAssertNoAlloc::~AutoAssertNoAlloc()
{
    if (gc)
        gc->allowAlloc();
}
#endif

JS::AutoAssertGCCallback::AutoAssertGCCallback(JSObject* obj)
  : AutoSuppressGCAnalysis()
{
    MOZ_ASSERT(obj->runtimeFromMainThread()->isHeapCollecting());
}

JS_FRIEND_API(const char*)
JS::GCTraceKindToAscii(JS::TraceKind kind)
{
    switch(kind) {
#define MAP_NAME(name, _0, _1) case JS::TraceKind::name: return #name;
JS_FOR_EACH_TRACEKIND(MAP_NAME);
#undef MAP_NAME
      default: return "Invalid";
    }
}

JS::GCCellPtr::GCCellPtr(const Value& v)
  : ptr(0)
{
    if (v.isString())
        ptr = checkedCast(v.toString(), JS::TraceKind::String);
    else if (v.isObject())
        ptr = checkedCast(&v.toObject(), JS::TraceKind::Object);
    else if (v.isSymbol())
        ptr = checkedCast(v.toSymbol(), JS::TraceKind::Symbol);
    else
        ptr = checkedCast(nullptr, JS::TraceKind::Null);
}

JS::TraceKind
JS::GCCellPtr::outOfLineKind() const
{
    MOZ_ASSERT((ptr & OutOfLineTraceKindMask) == OutOfLineTraceKindMask);
    MOZ_ASSERT(asCell()->isTenured());
    return MapAllocToTraceKind(asCell()->asTenured().getAllocKind());
}

bool
JS::GCCellPtr::mayBeOwnedByOtherRuntime() const
{
    return (is<JSString>() && as<JSString>().isPermanentAtom()) ||
           (is<Symbol>() && as<Symbol>().isWellKnownSymbol());
}

#ifdef JSGC_HASH_TABLE_CHECKS
void
js::gc::CheckHashTablesAfterMovingGC(JSRuntime* rt)
{
    /*
     * Check that internal hash tables no longer have any pointers to things
     * that have been moved.
     */
    for (ZonesIter zone(rt, SkipAtoms); !zone.done(); zone.next()) {
        zone->checkUniqueIdTableAfterMovingGC();
    }
    for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next()) {
        c->objectGroups.checkTablesAfterMovingGC();
        c->checkInitialShapesTableAfterMovingGC();
        c->checkWrapperMapAfterMovingGC();
        c->checkBaseShapeTableAfterMovingGC();
        if (c->debugScopes)
            c->debugScopes->checkHashTablesAfterMovingGC(rt);
    }
}
#endif

JS_PUBLIC_API(void)
JS::PrepareZoneForGC(Zone* zone)
{
    zone->scheduleGC();
}

JS_PUBLIC_API(void)
JS::PrepareForFullGC(JSRuntime* rt)
{
    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next())
        zone->scheduleGC();
}

JS_PUBLIC_API(void)
JS::PrepareForIncrementalGC(JSRuntime* rt)
{
    if (!JS::IsIncrementalGCInProgress(rt))
        return;

    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        if (zone->wasGCStarted())
            PrepareZoneForGC(zone);
    }
}

JS_PUBLIC_API(bool)
JS::IsGCScheduled(JSRuntime* rt)
{
    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        if (zone->isGCScheduled())
            return true;
    }

    return false;
}

JS_PUBLIC_API(void)
JS::SkipZoneForGC(Zone* zone)
{
    zone->unscheduleGC();
}

JS_PUBLIC_API(void)
JS::GCForReason(JSRuntime* rt, JSGCInvocationKind gckind, gcreason::Reason reason)
{
    MOZ_ASSERT(gckind == GC_NORMAL || gckind == GC_SHRINK);
    rt->gc.gc(gckind, reason);
}

JS_PUBLIC_API(void)
JS::StartIncrementalGC(JSRuntime* rt, JSGCInvocationKind gckind, gcreason::Reason reason, int64_t millis)
{
    MOZ_ASSERT(gckind == GC_NORMAL || gckind == GC_SHRINK);
    rt->gc.startGC(gckind, reason, millis);
}

JS_PUBLIC_API(void)
JS::IncrementalGCSlice(JSRuntime* rt, gcreason::Reason reason, int64_t millis)
{
    rt->gc.gcSlice(reason, millis);
}

JS_PUBLIC_API(void)
JS::FinishIncrementalGC(JSRuntime* rt, gcreason::Reason reason)
{
    rt->gc.finishGC(reason);
}

JS_PUBLIC_API(void)
JS::AbortIncrementalGC(JSRuntime* rt)
{
    rt->gc.abortGC();
}

char16_t*
JS::GCDescription::formatSliceMessage(JSRuntime* rt) const
{
    UniqueChars cstr = rt->gc.stats.formatCompactSliceMessage();

    size_t nchars = strlen(cstr.get());
    UniquePtr<char16_t, JS::FreePolicy> out(js_pod_malloc<char16_t>(nchars + 1));
    if (!out)
        return nullptr;
    out.get()[nchars] = 0;

    CopyAndInflateChars(out.get(), cstr.get(), nchars);
    return out.release();
}

char16_t*
JS::GCDescription::formatSummaryMessage(JSRuntime* rt) const
{
    UniqueChars cstr = rt->gc.stats.formatCompactSummaryMessage();

    size_t nchars = strlen(cstr.get());
    UniquePtr<char16_t, JS::FreePolicy> out(js_pod_malloc<char16_t>(nchars + 1));
    if (!out)
        return nullptr;
    out.get()[nchars] = 0;

    CopyAndInflateChars(out.get(), cstr.get(), nchars);
    return out.release();
}

JS::dbg::GarbageCollectionEvent::Ptr
JS::GCDescription::toGCEvent(JSRuntime* rt) const
{
    return JS::dbg::GarbageCollectionEvent::Create(rt, rt->gc.stats, rt->gc.majorGCCount());
}

char16_t*
JS::GCDescription::formatJSON(JSRuntime* rt, uint64_t timestamp) const
{
    UniqueChars cstr = rt->gc.stats.formatJsonMessage(timestamp);

    size_t nchars = strlen(cstr.get());
    UniquePtr<char16_t, JS::FreePolicy> out(js_pod_malloc<char16_t>(nchars + 1));
    if (!out)
        return nullptr;
    out.get()[nchars] = 0;

    CopyAndInflateChars(out.get(), cstr.get(), nchars);
    return out.release();
}

JS_PUBLIC_API(JS::GCSliceCallback)
JS::SetGCSliceCallback(JSRuntime* rt, GCSliceCallback callback)
{
    return rt->gc.setSliceCallback(callback);
}

JS_PUBLIC_API(void)
JS::DisableIncrementalGC(JSRuntime* rt)
{
    rt->gc.disallowIncrementalGC();
}

JS_PUBLIC_API(bool)
JS::IsIncrementalGCEnabled(JSRuntime* rt)
{
    return rt->gc.isIncrementalGCEnabled();
}

JS_PUBLIC_API(bool)
JS::IsIncrementalGCInProgress(JSRuntime* rt)
{
    return rt->gc.isIncrementalGCInProgress() && !rt->gc.isVerifyPreBarriersEnabled();
}

JS_PUBLIC_API(bool)
JS::IsIncrementalBarrierNeeded(JSRuntime* rt)
{
    return rt->gc.state() == gc::MARK && !rt->isHeapBusy();
}

JS_PUBLIC_API(bool)
JS::IsIncrementalBarrierNeeded(JSContext* cx)
{
    return IsIncrementalBarrierNeeded(cx->runtime());
}

struct IncrementalReferenceBarrierFunctor {
    template <typename T> void operator()(T* t) { T::writeBarrierPre(t); }
};

JS_PUBLIC_API(void)
JS::IncrementalReferenceBarrier(GCCellPtr thing)
{
    if (!thing)
        return;

    DispatchTyped(IncrementalReferenceBarrierFunctor(), thing);
}

JS_PUBLIC_API(void)
JS::IncrementalValueBarrier(const Value& v)
{
    js::HeapValue::writeBarrierPre(v);
}

JS_PUBLIC_API(void)
JS::IncrementalObjectBarrier(JSObject* obj)
{
    if (!obj)
        return;

    MOZ_ASSERT(!obj->zone()->runtimeFromMainThread()->isHeapMajorCollecting());

    JSObject::writeBarrierPre(obj);
}

JS_PUBLIC_API(bool)
JS::WasIncrementalGC(JSRuntime* rt)
{
    return rt->gc.isIncrementalGc();
}

JS::AutoDisableGenerationalGC::AutoDisableGenerationalGC(JSRuntime* rt)
  : gc(&rt->gc)
{
    gc->disableGenerationalGC();
}

JS::AutoDisableGenerationalGC::~AutoDisableGenerationalGC()
{
    gc->enableGenerationalGC();
}

JS_PUBLIC_API(bool)
JS::IsGenerationalGCEnabled(JSRuntime* rt)
{
    return rt->gc.isGenerationalGCEnabled();
}

uint64_t
js::gc::NextCellUniqueId(JSRuntime* rt)
{
    return rt->gc.nextCellUniqueId();
}

namespace js {
namespace gc {
namespace MemInfo {

static bool
GCBytesGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setNumber(double(cx->runtime()->gc.usage.gcBytes()));
    return true;
}

static bool
GCMaxBytesGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setNumber(double(cx->runtime()->gc.tunables.gcMaxBytes()));
    return true;
}

static bool
MallocBytesGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setNumber(double(cx->runtime()->gc.getMallocBytes()));
    return true;
}

static bool
MaxMallocGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setNumber(double(cx->runtime()->gc.maxMallocBytesAllocated()));
    return true;
}

static bool
GCHighFreqGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setBoolean(cx->runtime()->gc.schedulingState.inHighFrequencyGCMode());
    return true;
}

static bool
GCNumberGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setNumber(double(cx->runtime()->gc.gcNumber()));
    return true;
}

static bool
MajorGCCountGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setNumber(double(cx->runtime()->gc.majorGCCount()));
    return true;
}

static bool
MinorGCCountGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setNumber(double(cx->runtime()->gc.minorGCCount()));
    return true;
}

static bool
ZoneGCBytesGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setNumber(double(cx->zone()->usage.gcBytes()));
    return true;
}

static bool
ZoneGCTriggerBytesGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setNumber(double(cx->zone()->threshold.gcTriggerBytes()));
    return true;
}

static bool
ZoneGCAllocTriggerGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setNumber(double(cx->zone()->threshold.allocTrigger(cx->runtime()->gc.schedulingState.inHighFrequencyGCMode())));
    return true;
}

static bool
ZoneMallocBytesGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setNumber(double(cx->zone()->gcMallocBytes));
    return true;
}

static bool
ZoneMaxMallocGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setNumber(double(cx->zone()->gcMaxMallocBytes));
    return true;
}

static bool
ZoneGCDelayBytesGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setNumber(double(cx->zone()->gcDelayBytes));
    return true;
}

static bool
ZoneGCHeapGrowthFactorGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setNumber(cx->zone()->threshold.gcHeapGrowthFactor());
    return true;
}

static bool
ZoneGCNumberGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setNumber(double(cx->zone()->gcNumber()));
    return true;
}

#ifdef JS_MORE_DETERMINISTIC
static bool
DummyGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setUndefined();
    return true;
}
#endif

} /* namespace MemInfo */

JSObject*
NewMemoryInfoObject(JSContext* cx)
{
    RootedObject obj(cx, JS_NewObject(cx, nullptr));

    using namespace MemInfo;
    struct NamedGetter {
        const char* name;
        JSNative getter;
    } getters[] = {
        { "gcBytes", GCBytesGetter },
        { "gcMaxBytes", GCMaxBytesGetter },
        { "mallocBytesRemaining", MallocBytesGetter },
        { "maxMalloc", MaxMallocGetter },
        { "gcIsHighFrequencyMode", GCHighFreqGetter },
        { "gcNumber", GCNumberGetter },
        { "majorGCCount", MajorGCCountGetter },
        { "minorGCCount", MinorGCCountGetter }
    };

    for (auto pair : getters) {
#ifdef JS_MORE_DETERMINISTIC
        JSNative getter = DummyGetter;
#else
        JSNative getter = pair.getter;
#endif
        if (!JS_DefineProperty(cx, obj, pair.name, UndefinedHandleValue,
                               JSPROP_ENUMERATE | JSPROP_SHARED,
                               getter, nullptr))
        {
            return nullptr;
        }
    }

    RootedObject zoneObj(cx, JS_NewObject(cx, nullptr));
    if (!zoneObj)
        return nullptr;

    if (!JS_DefineProperty(cx, obj, "zone", zoneObj, JSPROP_ENUMERATE))
        return nullptr;

    struct NamedZoneGetter {
        const char* name;
        JSNative getter;
    } zoneGetters[] = {
        { "gcBytes", ZoneGCBytesGetter },
        { "gcTriggerBytes", ZoneGCTriggerBytesGetter },
        { "gcAllocTrigger", ZoneGCAllocTriggerGetter },
        { "mallocBytesRemaining", ZoneMallocBytesGetter },
        { "maxMalloc", ZoneMaxMallocGetter },
        { "delayBytes", ZoneGCDelayBytesGetter },
        { "heapGrowthFactor", ZoneGCHeapGrowthFactorGetter },
        { "gcNumber", ZoneGCNumberGetter }
    };

    for (auto pair : zoneGetters) {
 #ifdef JS_MORE_DETERMINISTIC
        JSNative getter = DummyGetter;
#else
        JSNative getter = pair.getter;
#endif
        if (!JS_DefineProperty(cx, zoneObj, pair.name, UndefinedHandleValue,
                               JSPROP_ENUMERATE | JSPROP_SHARED,
                               getter, nullptr))
        {
            return nullptr;
        }
    }

    return obj;
}

} /* namespace gc */
} /* namespace js */
