/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_GCAPI_h
#define js_GCAPI_h

#include "mozilla/UniquePtr.h"
#include "mozilla/Vector.h"

#include "js/HeapAPI.h"

namespace js {
namespace gc {
class GCRuntime;
} // namespace gc
namespace gcstats {
struct Statistics;
} // namespace gcstats
} // namespace js

typedef enum JSGCMode {
    /** Perform only global GCs. */
    JSGC_MODE_GLOBAL = 0,

    /** Perform per-compartment GCs until too much garbage has accumulated. */
    JSGC_MODE_COMPARTMENT = 1,

    /**
     * Collect in short time slices rather than all at once. Implies
     * JSGC_MODE_COMPARTMENT.
     */
    JSGC_MODE_INCREMENTAL = 2
} JSGCMode;

/**
 * Kinds of js_GC invocation.
 */
typedef enum JSGCInvocationKind {
    /* Normal invocation. */
    GC_NORMAL = 0,

    /* Minimize GC triggers and release empty GC chunks right away. */
    GC_SHRINK = 1
} JSGCInvocationKind;

namespace JS {

using mozilla::UniquePtr;

#define GCREASONS(D)                            \
    /* Reasons internal to the JS engine */     \
    D(API)                                      \
    D(EAGER_ALLOC_TRIGGER)                      \
    D(DESTROY_RUNTIME)                          \
    D(DESTROY_CONTEXT)                          \
    D(LAST_DITCH)                               \
    D(TOO_MUCH_MALLOC)                          \
    D(ALLOC_TRIGGER)                            \
    D(DEBUG_GC)                                 \
    D(COMPARTMENT_REVIVED)                      \
    D(RESET)                                    \
    D(OUT_OF_NURSERY)                           \
    D(EVICT_NURSERY)                            \
    D(FULL_STORE_BUFFER)                        \
    D(SHARED_MEMORY_LIMIT)                      \
    D(PERIODIC_FULL_GC)                         \
    D(INCREMENTAL_TOO_SLOW)                     \
    D(ABORT_GC)                                 \
                                                \
    /* These are reserved for future use. */    \
    D(RESERVED0)                                \
    D(RESERVED1)                                \
    D(RESERVED2)                                \
    D(RESERVED3)                                \
    D(RESERVED4)                                \
    D(RESERVED5)                                \
    D(RESERVED6)                                \
    D(RESERVED7)                                \
    D(RESERVED8)                                \
    D(RESERVED9)                                \
    D(RESERVED10)                               \
    D(RESERVED11)                               \
    D(RESERVED12)                               \
    D(RESERVED13)                               \
    D(RESERVED14)                               \
    D(RESERVED15)                               \
                                                \
    /* Reasons from Firefox */                  \
    D(DOM_WINDOW_UTILS)                         \
    D(COMPONENT_UTILS)                          \
    D(MEM_PRESSURE)                             \
    D(CC_WAITING)                               \
    D(CC_FORCED)                                \
    D(LOAD_END)                                 \
    D(POST_COMPARTMENT)                         \
    D(PAGE_HIDE)                                \
    D(NSJSCONTEXT_DESTROY)                      \
    D(SET_NEW_DOCUMENT)                         \
    D(SET_DOC_SHELL)                            \
    D(DOM_UTILS)                                \
    D(DOM_IPC)                                  \
    D(DOM_WORKER)                               \
    D(INTER_SLICE_GC)                           \
    D(REFRESH_FRAME)                            \
    D(FULL_GC_TIMER)                            \
    D(SHUTDOWN_CC)                              \
    D(FINISH_LARGE_EVALUATE)                    \
    D(USER_INACTIVE)                            \
    D(XPCONNECT_SHUTDOWN)

namespace gcreason {

/* GCReasons will end up looking like JSGC_MAYBEGC */
enum Reason {
#define MAKE_REASON(name) name,
    GCREASONS(MAKE_REASON)
#undef MAKE_REASON
    NO_REASON,
    NUM_REASONS,

    /*
     * For telemetry, we want to keep a fixed max bucket size over time so we
     * don't have to switch histograms. 100 is conservative; as of this writing
     * there are 52. But the cost of extra buckets seems to be low while the
     * cost of switching histograms is high.
     */
    NUM_TELEMETRY_REASONS = 100
};

} /* namespace gcreason */

/*
 * Zone GC:
 *
 * SpiderMonkey's GC is capable of performing a collection on an arbitrary
 * subset of the zones in the system. This allows an embedding to minimize
 * collection time by only collecting zones that have run code recently,
 * ignoring the parts of the heap that are unlikely to have changed.
 *
 * When triggering a GC using one of the functions below, it is first necessary
 * to select the zones to be collected. To do this, you can call
 * PrepareZoneForGC on each zone, or you can call PrepareForFullGC to select
 * all zones. Failing to select any zone is an error.
 */

/**
 * Schedule the given zone to be collected as part of the next GC.
 */
extern JS_PUBLIC_API(void)
PrepareZoneForGC(Zone* zone);

/**
 * Schedule all zones to be collected in the next GC.
 */
extern JS_PUBLIC_API(void)
PrepareForFullGC(JSRuntime* rt);

/**
 * When performing an incremental GC, the zones that were selected for the
 * previous incremental slice must be selected in subsequent slices as well.
 * This function selects those slices automatically.
 */
extern JS_PUBLIC_API(void)
PrepareForIncrementalGC(JSRuntime* rt);

/**
 * Returns true if any zone in the system has been scheduled for GC with one of
 * the functions above or by the JS engine.
 */
extern JS_PUBLIC_API(bool)
IsGCScheduled(JSRuntime* rt);

/**
 * Undoes the effect of the Prepare methods above. The given zone will not be
 * collected in the next GC.
 */
extern JS_PUBLIC_API(void)
SkipZoneForGC(Zone* zone);

/*
 * Non-Incremental GC:
 *
 * The following functions perform a non-incremental GC.
 */

/**
 * Performs a non-incremental collection of all selected zones.
 *
 * If the gckind argument is GC_NORMAL, then some objects that are unreachable
 * from the program may still be alive afterwards because of internal
 * references; if GC_SHRINK is passed then caches and other temporary references
 * to objects will be cleared and all unreferenced objects will be removed from
 * the system.
 */
extern JS_PUBLIC_API(void)
GCForReason(JSRuntime* rt, JSGCInvocationKind gckind, gcreason::Reason reason);

/*
 * Incremental GC:
 *
 * Incremental GC divides the full mark-and-sweep collection into multiple
 * slices, allowing client JavaScript code to run between each slice. This
 * allows interactive apps to avoid long collection pauses. Incremental GC does
 * not make collection take less time, it merely spreads that time out so that
 * the pauses are less noticable.
 *
 * For a collection to be carried out incrementally the following conditions
 * must be met:
 *  - The collection must be run by calling JS::IncrementalGC() rather than
 *    JS_GC().
 *  - The GC mode must have been set to JSGC_MODE_INCREMENTAL with
 *    JS_SetGCParameter().
 *
 * Note: Even if incremental GC is enabled and working correctly,
 *       non-incremental collections can still happen when low on memory.
 */

/**
 * Begin an incremental collection and perform one slice worth of work. When
 * this function returns, the collection may not be complete.
 * IncrementalGCSlice() must be called repeatedly until
 * !IsIncrementalGCInProgress(rt).
 *
 * Note: SpiderMonkey's GC is not realtime. Slices in practice may be longer or
 *       shorter than the requested interval.
 */
extern JS_PUBLIC_API(void)
StartIncrementalGC(JSRuntime* rt, JSGCInvocationKind gckind, gcreason::Reason reason,
                   int64_t millis = 0);

/**
 * Perform a slice of an ongoing incremental collection. When this function
 * returns, the collection may not be complete. It must be called repeatedly
 * until !IsIncrementalGCInProgress(rt).
 *
 * Note: SpiderMonkey's GC is not realtime. Slices in practice may be longer or
 *       shorter than the requested interval.
 */
extern JS_PUBLIC_API(void)
IncrementalGCSlice(JSRuntime* rt, gcreason::Reason reason, int64_t millis = 0);

/**
 * If IsIncrementalGCInProgress(rt), this call finishes the ongoing collection
 * by performing an arbitrarily long slice. If !IsIncrementalGCInProgress(rt),
 * this is equivalent to GCForReason. When this function returns,
 * IsIncrementalGCInProgress(rt) will always be false.
 */
extern JS_PUBLIC_API(void)
FinishIncrementalGC(JSRuntime* rt, gcreason::Reason reason);

/**
 * If IsIncrementalGCInProgress(rt), this call aborts the ongoing collection and
 * performs whatever work needs to be done to return the collector to its idle
 * state. This may take an arbitrarily long time. When this function returns,
 * IsIncrementalGCInProgress(rt) will always be false.
 */
extern JS_PUBLIC_API(void)
AbortIncrementalGC(JSRuntime* rt);

namespace dbg {

// The `JS::dbg::GarbageCollectionEvent` class is essentially a view of the
// `js::gcstats::Statistics` data without the uber implementation-specific bits.
// It should generally be palatable for web developers.
class GarbageCollectionEvent
{
    // The major GC number of the GC cycle this data pertains to.
    uint64_t majorGCNumber_;

    // Reference to a non-owned, statically allocated C string. This is a very
    // short reason explaining why a GC was triggered.
    const char* reason;

    // Reference to a nullable, non-owned, statically allocated C string. If the
    // collection was forced to be non-incremental, this is a short reason of
    // why the GC could not perform an incremental collection.
    const char* nonincrementalReason;

    // Represents a single slice of a possibly multi-slice incremental garbage
    // collection.
    struct Collection {
        double startTimestamp;
        double endTimestamp;
    };

    // The set of garbage collection slices that made up this GC cycle.
    mozilla::Vector<Collection> collections;

    GarbageCollectionEvent(const GarbageCollectionEvent& rhs) = delete;
    GarbageCollectionEvent& operator=(const GarbageCollectionEvent& rhs) = delete;

  public:
    explicit GarbageCollectionEvent(uint64_t majorGCNum)
        : majorGCNumber_(majorGCNum)
        , reason(nullptr)
        , nonincrementalReason(nullptr)
        , collections()
    { }

    using Ptr = UniquePtr<GarbageCollectionEvent, DeletePolicy<GarbageCollectionEvent>>;
    static Ptr Create(JSRuntime* rt, ::js::gcstats::Statistics& stats, uint64_t majorGCNumber);

    JSObject* toJSObject(JSContext* cx) const;

    uint64_t majorGCNumber() const { return majorGCNumber_; }
};

} // namespace dbg

enum GCProgress {
    /*
     * During non-incremental GC, the GC is bracketed by JSGC_CYCLE_BEGIN/END
     * callbacks. During an incremental GC, the sequence of callbacks is as
     * follows:
     *   JSGC_CYCLE_BEGIN, JSGC_SLICE_END  (first slice)
     *   JSGC_SLICE_BEGIN, JSGC_SLICE_END  (second slice)
     *   ...
     *   JSGC_SLICE_BEGIN, JSGC_CYCLE_END  (last slice)
     */

    GC_CYCLE_BEGIN,
    GC_SLICE_BEGIN,
    GC_SLICE_END,
    GC_CYCLE_END
};

struct JS_PUBLIC_API(GCDescription) {
    bool isCompartment_;
    JSGCInvocationKind invocationKind_;
    gcreason::Reason reason_;

    GCDescription(bool isCompartment, JSGCInvocationKind kind, gcreason::Reason reason)
      : isCompartment_(isCompartment), invocationKind_(kind), reason_(reason) {}

    char16_t* formatSliceMessage(JSRuntime* rt) const;
    char16_t* formatSummaryMessage(JSRuntime* rt) const;
    char16_t* formatJSON(JSRuntime* rt, uint64_t timestamp) const;

    JS::dbg::GarbageCollectionEvent::Ptr toGCEvent(JSRuntime* rt) const;
};

typedef void
(* GCSliceCallback)(JSRuntime* rt, GCProgress progress, const GCDescription& desc);

/**
 * The GC slice callback is called at the beginning and end of each slice. This
 * callback may be used for GC notifications as well as to perform additional
 * marking.
 */
extern JS_PUBLIC_API(GCSliceCallback)
SetGCSliceCallback(JSRuntime* rt, GCSliceCallback callback);

/**
 * Incremental GC defaults to enabled, but may be disabled for testing or in
 * embeddings that have not yet implemented barriers on their native classes.
 * There is not currently a way to re-enable incremental GC once it has been
 * disabled on the runtime.
 */
extern JS_PUBLIC_API(void)
DisableIncrementalGC(JSRuntime* rt);

/**
 * Returns true if incremental GC is enabled. Simply having incremental GC
 * enabled is not sufficient to ensure incremental collections are happening.
 * See the comment "Incremental GC" above for reasons why incremental GC may be
 * suppressed. Inspection of the "nonincremental reason" field of the
 * GCDescription returned by GCSliceCallback may help narrow down the cause if
 * collections are not happening incrementally when expected.
 */
extern JS_PUBLIC_API(bool)
IsIncrementalGCEnabled(JSRuntime* rt);

/**
 * Returns true while an incremental GC is ongoing, both when actively
 * collecting and between slices.
 */
extern JS_PUBLIC_API(bool)
IsIncrementalGCInProgress(JSRuntime* rt);

/*
 * Returns true when writes to GC things must call an incremental (pre) barrier.
 * This is generally only true when running mutator code in-between GC slices.
 * At other times, the barrier may be elided for performance.
 */
extern JS_PUBLIC_API(bool)
IsIncrementalBarrierNeeded(JSRuntime* rt);

extern JS_PUBLIC_API(bool)
IsIncrementalBarrierNeeded(JSContext* cx);

/*
 * Notify the GC that a reference to a GC thing is about to be overwritten.
 * These methods must be called if IsIncrementalBarrierNeeded.
 */
extern JS_PUBLIC_API(void)
IncrementalReferenceBarrier(GCCellPtr thing);

extern JS_PUBLIC_API(void)
IncrementalValueBarrier(const Value& v);

extern JS_PUBLIC_API(void)
IncrementalObjectBarrier(JSObject* obj);

/**
 * Returns true if the most recent GC ran incrementally.
 */
extern JS_PUBLIC_API(bool)
WasIncrementalGC(JSRuntime* rt);

/*
 * Generational GC:
 *
 * Note: Generational GC is not yet enabled by default. The following class
 *       is non-functional unless SpiderMonkey was configured with
 *       --enable-gcgenerational.
 */

/** Ensure that generational GC is disabled within some scope. */
class JS_PUBLIC_API(AutoDisableGenerationalGC)
{
    js::gc::GCRuntime* gc;

  public:
    explicit AutoDisableGenerationalGC(JSRuntime* rt);
    ~AutoDisableGenerationalGC();
};

/**
 * Returns true if generational allocation and collection is currently enabled
 * on the given runtime.
 */
extern JS_PUBLIC_API(bool)
IsGenerationalGCEnabled(JSRuntime* rt);

/**
 * Returns the GC's "number". This does not correspond directly to the number
 * of GCs that have been run, but is guaranteed to be monotonically increasing
 * with GC activity.
 */
extern JS_PUBLIC_API(size_t)
GetGCNumber();

/**
 * The GC does not immediately return the unused memory freed by a collection
 * back to the system incase it is needed soon afterwards. This call forces the
 * GC to return this memory immediately.
 */
extern JS_PUBLIC_API(void)
ShrinkGCBuffers(JSRuntime* rt);

/**
 * Assert if a GC occurs while this class is live. This class does not disable
 * the static rooting hazard analysis.
 */
class JS_PUBLIC_API(AutoAssertOnGC)
{
#ifdef DEBUG
    js::gc::GCRuntime* gc;
    size_t gcNumber;

  public:
    AutoAssertOnGC();
    explicit AutoAssertOnGC(JSRuntime* rt);
    ~AutoAssertOnGC();

    static void VerifyIsSafeToGC(JSRuntime* rt);
#else
  public:
    AutoAssertOnGC() {}
    explicit AutoAssertOnGC(JSRuntime* rt) {}
    ~AutoAssertOnGC() {}

    static void VerifyIsSafeToGC(JSRuntime* rt) {}
#endif
};

/**
 * Assert if an allocation of a GC thing occurs while this class is live. This
 * class does not disable the static rooting hazard analysis.
 */
class JS_PUBLIC_API(AutoAssertNoAlloc)
{
#ifdef JS_DEBUG
    js::gc::GCRuntime* gc;

  public:
    AutoAssertNoAlloc() : gc(nullptr) {}
    explicit AutoAssertNoAlloc(JSRuntime* rt);
    void disallowAlloc(JSRuntime* rt);
    ~AutoAssertNoAlloc();
#else
  public:
    AutoAssertNoAlloc() {}
    explicit AutoAssertNoAlloc(JSRuntime* rt) {}
    void disallowAlloc(JSRuntime* rt) {}
#endif
};

/**
 * Disable the static rooting hazard analysis in the live region and assert if
 * any allocation that could potentially trigger a GC occurs while this guard
 * object is live. This is most useful to help the exact rooting hazard analysis
 * in complex regions, since it cannot understand dataflow.
 *
 * Note: GC behavior is unpredictable even when deterministic and is generally
 *       non-deterministic in practice. The fact that this guard has not
 *       asserted is not a guarantee that a GC cannot happen in the guarded
 *       region. As a rule, anyone performing a GC unsafe action should
 *       understand the GC properties of all code in that region and ensure
 *       that the hazard analysis is correct for that code, rather than relying
 *       on this class.
 */
class JS_PUBLIC_API(AutoSuppressGCAnalysis) : public AutoAssertNoAlloc
{
  public:
    AutoSuppressGCAnalysis() : AutoAssertNoAlloc() {}
    explicit AutoSuppressGCAnalysis(JSRuntime* rt) : AutoAssertNoAlloc(rt) {}
};

/**
 * Assert that code is only ever called from a GC callback, disable the static
 * rooting hazard analysis and assert if any allocation that could potentially
 * trigger a GC occurs while this guard object is live.
 *
 * This is useful to make the static analysis ignore code that runs in GC
 * callbacks.
 */
class JS_PUBLIC_API(AutoAssertGCCallback) : public AutoSuppressGCAnalysis
{
  public:
    explicit AutoAssertGCCallback(JSObject* obj);
};

/**
 * Place AutoCheckCannotGC in scopes that you believe can never GC. These
 * annotations will be verified both dynamically via AutoAssertOnGC, and
 * statically with the rooting hazard analysis (implemented by making the
 * analysis consider AutoCheckCannotGC to be a GC pointer, and therefore
 * complain if it is live across a GC call.) It is useful when dealing with
 * internal pointers to GC things where the GC thing itself may not be present
 * for the static analysis: e.g. acquiring inline chars from a JSString* on the
 * heap.
 */
class JS_PUBLIC_API(AutoCheckCannotGC) : public AutoAssertOnGC
{
  public:
    AutoCheckCannotGC() : AutoAssertOnGC() {}
    explicit AutoCheckCannotGC(JSRuntime* rt) : AutoAssertOnGC(rt) {}
};

/**
 * Unsets the gray bit for anything reachable from |thing|. |kind| should not be
 * JS::TraceKind::Shape. |thing| should be non-null.
 */
extern JS_FRIEND_API(bool)
UnmarkGrayGCThingRecursively(GCCellPtr thing);

} /* namespace JS */

namespace js {
namespace gc {

static MOZ_ALWAYS_INLINE void
ExposeGCThingToActiveJS(JS::GCCellPtr thing)
{
    MOZ_ASSERT(thing.kind() != JS::TraceKind::Shape);

    /*
     * GC things residing in the nursery cannot be gray: they have no mark bits.
     * All live objects in the nursery are moved to tenured at the beginning of
     * each GC slice, so the gray marker never sees nursery things.
     */
    if (IsInsideNursery(thing.asCell()))
        return;
    JS::shadow::Runtime* rt = detail::GetGCThingRuntime(thing.unsafeAsUIntPtr());
    if (IsIncrementalBarrierNeededOnTenuredGCThing(rt, thing))
        JS::IncrementalReferenceBarrier(thing);
    else if (JS::GCThingIsMarkedGray(thing))
        JS::UnmarkGrayGCThingRecursively(thing);
}

static MOZ_ALWAYS_INLINE void
MarkGCThingAsLive(JSRuntime* aRt, JS::GCCellPtr thing)
{
    JS::shadow::Runtime* rt = JS::shadow::Runtime::asShadowRuntime(aRt);
    /*
     * Any object in the nursery will not be freed during any GC running at that time.
     */
    if (IsInsideNursery(thing.asCell()))
        return;
    if (IsIncrementalBarrierNeededOnTenuredGCThing(rt, thing))
        JS::IncrementalReferenceBarrier(thing);
}

} /* namespace gc */
} /* namespace js */

namespace JS {

/*
 * This should be called when an object that is marked gray is exposed to the JS
 * engine (by handing it to running JS code or writing it into live JS
 * data). During incremental GC, since the gray bits haven't been computed yet,
 * we conservatively mark the object black.
 */
static MOZ_ALWAYS_INLINE void
ExposeObjectToActiveJS(JSObject* obj)
{
    js::gc::ExposeGCThingToActiveJS(GCCellPtr(obj));
}

static MOZ_ALWAYS_INLINE void
ExposeScriptToActiveJS(JSScript* script)
{
    js::gc::ExposeGCThingToActiveJS(GCCellPtr(script));
}

/*
 * If a GC is currently marking, mark the string black.
 */
static MOZ_ALWAYS_INLINE void
MarkStringAsLive(Zone* zone, JSString* string)
{
    JSRuntime* rt = JS::shadow::Zone::asShadowZone(zone)->runtimeFromMainThread();
    js::gc::MarkGCThingAsLive(rt, GCCellPtr(string));
}

/*
 * Internal to Firefox.
 *
 * Note: this is not related to the PokeGC in nsJSEnvironment.
 */
extern JS_FRIEND_API(void)
PokeGC(JSRuntime* rt);

/*
 * Internal to Firefox.
 */
extern JS_FRIEND_API(void)
NotifyDidPaint(JSRuntime* rt);

} /* namespace JS */

#endif /* js_GCAPI_h */
