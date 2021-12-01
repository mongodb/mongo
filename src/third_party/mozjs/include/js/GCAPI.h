/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * High-level interface to the JS garbage collector.
 */

#ifndef js_GCAPI_h
#define js_GCAPI_h

#include "mozilla/TimeStamp.h"
#include "mozilla/Vector.h"

#include "js/GCAnnotations.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"

struct JSFreeOp;

#ifdef JS_BROKEN_GCC_ATTRIBUTE_WARNING
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif // JS_BROKEN_GCC_ATTRIBUTE_WARNING

class JS_PUBLIC_API(JSTracer);

#ifdef JS_BROKEN_GCC_ATTRIBUTE_WARNING
#pragma GCC diagnostic pop
#endif // JS_BROKEN_GCC_ATTRIBUTE_WARNING

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

    /** Perform per-zone GCs until too much garbage has accumulated. */
    JSGC_MODE_ZONE = 1,

    /**
     * Collect in short time slices rather than all at once. Implies
     * JSGC_MODE_ZONE.
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

typedef enum JSGCParamKey {
    /**
     * Maximum nominal heap before last ditch GC.
     *
     * Soft limit on the number of bytes we are allowed to allocate in the GC
     * heap. Attempts to allocate gcthings over this limit will return null and
     * subsequently invoke the standard OOM machinery, independent of available
     * physical memory.
     *
     * Pref: javascript.options.mem.max
     * Default: 0xffffffff
     */
    JSGC_MAX_BYTES          = 0,

    /**
     * Initial value for the malloc bytes threshold.
     *
     * Pref: javascript.options.mem.high_water_mark
     * Default: TuningDefaults::MaxMallocBytes
     */
    JSGC_MAX_MALLOC_BYTES   = 1,

    /**
     * Maximum size of the generational GC nurseries.
     *
     * Pref: javascript.options.mem.nursery.max_kb
     * Default: JS::DefaultNurseryBytes
     */
    JSGC_MAX_NURSERY_BYTES  = 2,

    /** Amount of bytes allocated by the GC. */
    JSGC_BYTES = 3,

    /** Number of times GC has been invoked. Includes both major and minor GC. */
    JSGC_NUMBER = 4,

    /**
     * Select GC mode.
     *
     * See: JSGCMode in GCAPI.h
     * prefs: javascript.options.mem.gc_per_zone and
     *   javascript.options.mem.gc_incremental.
     * Default: JSGC_MODE_INCREMENTAL
     */
    JSGC_MODE = 6,

    /** Number of cached empty GC chunks. */
    JSGC_UNUSED_CHUNKS = 7,

    /** Total number of allocated GC chunks. */
    JSGC_TOTAL_CHUNKS = 8,

    /**
     * Max milliseconds to spend in an incremental GC slice.
     *
     * Pref: javascript.options.mem.gc_incremental_slice_ms
     * Default: DefaultTimeBudget.
     */
    JSGC_SLICE_TIME_BUDGET = 9,

    /**
     * Maximum size the GC mark stack can grow to.
     *
     * Pref: none
     * Default: MarkStack::DefaultCapacity
     */
    JSGC_MARK_STACK_LIMIT = 10,

    /**
     * GCs less than this far apart in time will be considered 'high-frequency
     * GCs'.
     *
     * See setGCLastBytes in jsgc.cpp.
     *
     * Pref: javascript.options.mem.gc_high_frequency_time_limit_ms
     * Default: HighFrequencyThresholdUsec
     */
    JSGC_HIGH_FREQUENCY_TIME_LIMIT = 11,

    /**
     * Start of dynamic heap growth.
     *
     * Pref: javascript.options.mem.gc_high_frequency_low_limit_mb
     * Default: HighFrequencyLowLimitBytes
     */
    JSGC_HIGH_FREQUENCY_LOW_LIMIT = 12,

    /**
     * End of dynamic heap growth.
     *
     * Pref: javascript.options.mem.gc_high_frequency_high_limit_mb
     * Default: HighFrequencyHighLimitBytes
     */
    JSGC_HIGH_FREQUENCY_HIGH_LIMIT = 13,

    /**
     * Upper bound of heap growth.
     *
     * Pref: javascript.options.mem.gc_high_frequency_heap_growth_max
     * Default: HighFrequencyHeapGrowthMax
     */
    JSGC_HIGH_FREQUENCY_HEAP_GROWTH_MAX = 14,

    /**
     * Lower bound of heap growth.
     *
     * Pref: javascript.options.mem.gc_high_frequency_heap_growth_min
     * Default: HighFrequencyHeapGrowthMin
     */
    JSGC_HIGH_FREQUENCY_HEAP_GROWTH_MIN = 15,

    /**
     * Heap growth for low frequency GCs.
     *
     * Pref: javascript.options.mem.gc_low_frequency_heap_growth
     * Default: LowFrequencyHeapGrowth
     */
    JSGC_LOW_FREQUENCY_HEAP_GROWTH = 16,

    /**
     * If false, the heap growth factor is fixed at 3. If true, it is determined
     * based on whether GCs are high- or low- frequency.
     *
     * Pref: javascript.options.mem.gc_dynamic_heap_growth
     * Default: DynamicHeapGrowthEnabled
     */
    JSGC_DYNAMIC_HEAP_GROWTH = 17,

    /**
     * If true, high-frequency GCs will use a longer mark slice.
     *
     * Pref: javascript.options.mem.gc_dynamic_mark_slice
     * Default: DynamicMarkSliceEnabled
     */
    JSGC_DYNAMIC_MARK_SLICE = 18,

    /**
     * Lower limit after which we limit the heap growth.
     *
     * The base value used to compute zone->threshold.gcTriggerBytes(). When
     * usage.gcBytes() surpasses threshold.gcTriggerBytes() for a zone, the
     * zone may be scheduled for a GC, depending on the exact circumstances.
     *
     * Pref: javascript.options.mem.gc_allocation_threshold_mb
     * Default GCZoneAllocThresholdBase
     */
    JSGC_ALLOCATION_THRESHOLD = 19,

    /**
     * We try to keep at least this many unused chunks in the free chunk pool at
     * all times, even after a shrinking GC.
     *
     * Pref: javascript.options.mem.gc_min_empty_chunk_count
     * Default: MinEmptyChunkCount
     */
    JSGC_MIN_EMPTY_CHUNK_COUNT = 21,

    /**
     * We never keep more than this many unused chunks in the free chunk
     * pool.
     *
     * Pref: javascript.options.mem.gc_min_empty_chunk_count
     * Default: MinEmptyChunkCount
     */
    JSGC_MAX_EMPTY_CHUNK_COUNT = 22,

    /**
     * Whether compacting GC is enabled.
     *
     * Pref: javascript.options.mem.gc_compacting
     * Default: CompactingEnabled
     */
    JSGC_COMPACTING_ENABLED = 23,

    /**
     * Factor for triggering a GC based on JSGC_ALLOCATION_THRESHOLD
     *
     * Default: ZoneAllocThresholdFactorDefault
     * Pref: None
     */
    JSGC_ALLOCATION_THRESHOLD_FACTOR = 25,

    /**
     * Factor for triggering a GC based on JSGC_ALLOCATION_THRESHOLD.
     * Used if another GC (in different zones) is already running.
     *
     * Default: ZoneAllocThresholdFactorAvoidInterruptDefault
     * Pref: None
     */
    JSGC_ALLOCATION_THRESHOLD_FACTOR_AVOID_INTERRUPT = 26,
} JSGCParamKey;

/*
 * Generic trace operation that calls JS::TraceEdge on each traceable thing's
 * location reachable from data.
 */
typedef void
(* JSTraceDataOp)(JSTracer* trc, void* data);

typedef enum JSGCStatus {
    JSGC_BEGIN,
    JSGC_END
} JSGCStatus;

typedef void
(* JSGCCallback)(JSContext* cx, JSGCStatus status, void* data);

typedef void
(* JSObjectsTenuredCallback)(JSContext* cx, void* data);

typedef enum JSFinalizeStatus {
    /**
     * Called when preparing to sweep a group of zones, before anything has been
     * swept.  The collector will not yield to the mutator before calling the
     * callback with JSFINALIZE_GROUP_START status.
     */
    JSFINALIZE_GROUP_PREPARE,

    /**
     * Called after preparing to sweep a group of zones. Weak references to
     * unmarked things have been removed at this point, but no GC things have
     * been swept. The collector may yield to the mutator after this point.
     */
    JSFINALIZE_GROUP_START,

    /**
     * Called after sweeping a group of zones. All dead GC things have been
     * swept at this point.
     */
    JSFINALIZE_GROUP_END,

    /**
     * Called at the end of collection when everything has been swept.
     */
    JSFINALIZE_COLLECTION_END
} JSFinalizeStatus;

typedef void
(* JSFinalizeCallback)(JSFreeOp* fop, JSFinalizeStatus status, void* data);

typedef void
(* JSWeakPointerZonesCallback)(JSContext* cx, void* data);

typedef void
(* JSWeakPointerCompartmentCallback)(JSContext* cx, JSCompartment* comp, void* data);

/**
 * Finalizes external strings created by JS_NewExternalString. The finalizer
 * can be called off the main thread.
 */
struct JSStringFinalizer {
    void (*finalize)(const JSStringFinalizer* fin, char16_t* chars);
};

namespace JS {

#define GCREASONS(D)                            \
    /* Reasons internal to the JS engine */     \
    D(API)                                      \
    D(EAGER_ALLOC_TRIGGER)                      \
    D(DESTROY_RUNTIME)                          \
    D(ROOTS_REMOVED)                            \
    D(LAST_DITCH)                               \
    D(TOO_MUCH_MALLOC)                          \
    D(ALLOC_TRIGGER)                            \
    D(DEBUG_GC)                                 \
    D(COMPARTMENT_REVIVED)                      \
    D(RESET)                                    \
    D(OUT_OF_NURSERY)                           \
    D(EVICT_NURSERY)                            \
    D(DELAYED_ATOMS_GC)                         \
    D(SHARED_MEMORY_LIMIT)                      \
    D(IDLE_TIME_COLLECTION)                     \
    D(INCREMENTAL_TOO_SLOW)                     \
    D(ABORT_GC)                                 \
    D(FULL_WHOLE_CELL_BUFFER)                   \
    D(FULL_GENERIC_BUFFER)                      \
    D(FULL_VALUE_BUFFER)                        \
    D(FULL_CELL_PTR_BUFFER)                     \
    D(FULL_SLOT_BUFFER)                         \
    D(FULL_SHAPE_BUFFER)                        \
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
    D(UNUSED1)                                  \
    D(FULL_GC_TIMER)                            \
    D(SHUTDOWN_CC)                              \
    D(UNUSED2)                                  \
    D(USER_INACTIVE)                            \
    D(XPCONNECT_SHUTDOWN)                       \
    D(DOCSHELL)                                 \
    D(HTML_PARSER)

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

/**
 * Get a statically allocated C string explaining the given GC reason.
 */
extern JS_PUBLIC_API(const char*)
ExplainReason(JS::gcreason::Reason reason);

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
PrepareForFullGC(JSContext* cx);

/**
 * When performing an incremental GC, the zones that were selected for the
 * previous incremental slice must be selected in subsequent slices as well.
 * This function selects those slices automatically.
 */
extern JS_PUBLIC_API(void)
PrepareForIncrementalGC(JSContext* cx);

/**
 * Returns true if any zone in the system has been scheduled for GC with one of
 * the functions above or by the JS engine.
 */
extern JS_PUBLIC_API(bool)
IsGCScheduled(JSContext* cx);

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
GCForReason(JSContext* cx, JSGCInvocationKind gckind, gcreason::Reason reason);

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
 * !IsIncrementalGCInProgress(cx).
 *
 * Note: SpiderMonkey's GC is not realtime. Slices in practice may be longer or
 *       shorter than the requested interval.
 */
extern JS_PUBLIC_API(void)
StartIncrementalGC(JSContext* cx, JSGCInvocationKind gckind, gcreason::Reason reason,
                   int64_t millis = 0);

/**
 * Perform a slice of an ongoing incremental collection. When this function
 * returns, the collection may not be complete. It must be called repeatedly
 * until !IsIncrementalGCInProgress(cx).
 *
 * Note: SpiderMonkey's GC is not realtime. Slices in practice may be longer or
 *       shorter than the requested interval.
 */
extern JS_PUBLIC_API(void)
IncrementalGCSlice(JSContext* cx, gcreason::Reason reason, int64_t millis = 0);

/**
 * If IsIncrementalGCInProgress(cx), this call finishes the ongoing collection
 * by performing an arbitrarily long slice. If !IsIncrementalGCInProgress(cx),
 * this is equivalent to GCForReason. When this function returns,
 * IsIncrementalGCInProgress(cx) will always be false.
 */
extern JS_PUBLIC_API(void)
FinishIncrementalGC(JSContext* cx, gcreason::Reason reason);

/**
 * If IsIncrementalGCInProgress(cx), this call aborts the ongoing collection and
 * performs whatever work needs to be done to return the collector to its idle
 * state. This may take an arbitrarily long time. When this function returns,
 * IsIncrementalGCInProgress(cx) will always be false.
 */
extern JS_PUBLIC_API(void)
AbortIncrementalGC(JSContext* cx);

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
        mozilla::TimeStamp startTimestamp;
        mozilla::TimeStamp endTimestamp;
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

    using Ptr = js::UniquePtr<GarbageCollectionEvent>;
    static Ptr Create(JSRuntime* rt, ::js::gcstats::Statistics& stats, uint64_t majorGCNumber);

    JSObject* toJSObject(JSContext* cx) const;

    uint64_t majorGCNumber() const { return majorGCNumber_; }
};

} // namespace dbg

enum GCProgress {
    /*
     * During GC, the GC is bracketed by GC_CYCLE_BEGIN/END callbacks. Each
     * slice between those (whether an incremental or the sole non-incremental
     * slice) is bracketed by GC_SLICE_BEGIN/GC_SLICE_END.
     */

    GC_CYCLE_BEGIN,
    GC_SLICE_BEGIN,
    GC_SLICE_END,
    GC_CYCLE_END
};

struct JS_PUBLIC_API(GCDescription) {
    bool isZone_;
    bool isComplete_;
    JSGCInvocationKind invocationKind_;
    gcreason::Reason reason_;

    GCDescription(bool isZone, bool isComplete, JSGCInvocationKind kind, gcreason::Reason reason)
      : isZone_(isZone), isComplete_(isComplete), invocationKind_(kind), reason_(reason) {}

    char16_t* formatSliceMessage(JSContext* cx) const;
    char16_t* formatSummaryMessage(JSContext* cx) const;
    char16_t* formatJSON(JSContext* cx, uint64_t timestamp) const;

    mozilla::TimeStamp startTime(JSContext* cx) const;
    mozilla::TimeStamp endTime(JSContext* cx) const;
    mozilla::TimeStamp lastSliceStart(JSContext* cx) const;
    mozilla::TimeStamp lastSliceEnd(JSContext* cx) const;

    JS::UniqueChars sliceToJSON(JSContext* cx) const;
    JS::UniqueChars summaryToJSON(JSContext* cx) const;

    JS::dbg::GarbageCollectionEvent::Ptr toGCEvent(JSContext* cx) const;
};

extern JS_PUBLIC_API(UniqueChars)
MinorGcToJSON(JSContext* cx);

typedef void
(* GCSliceCallback)(JSContext* cx, GCProgress progress, const GCDescription& desc);

/**
 * The GC slice callback is called at the beginning and end of each slice. This
 * callback may be used for GC notifications as well as to perform additional
 * marking.
 */
extern JS_PUBLIC_API(GCSliceCallback)
SetGCSliceCallback(JSContext* cx, GCSliceCallback callback);

/**
 * Describes the progress of an observed nursery collection.
 */
enum class GCNurseryProgress {
    /**
     * The nursery collection is starting.
     */
    GC_NURSERY_COLLECTION_START,
    /**
     * The nursery collection is ending.
     */
    GC_NURSERY_COLLECTION_END
};

/**
 * A nursery collection callback receives the progress of the nursery collection
 * and the reason for the collection.
 */
using GCNurseryCollectionCallback = void(*)(JSContext* cx, GCNurseryProgress progress,
                                            gcreason::Reason reason);

/**
 * Set the nursery collection callback for the given runtime. When set, it will
 * be called at the start and end of every nursery collection.
 */
extern JS_PUBLIC_API(GCNurseryCollectionCallback)
SetGCNurseryCollectionCallback(JSContext* cx, GCNurseryCollectionCallback callback);

typedef void
(* DoCycleCollectionCallback)(JSContext* cx);

/**
 * The purge gray callback is called after any COMPARTMENT_REVIVED GC in which
 * the majority of compartments have been marked gray.
 */
extern JS_PUBLIC_API(DoCycleCollectionCallback)
SetDoCycleCollectionCallback(JSContext* cx, DoCycleCollectionCallback callback);

/**
 * Incremental GC defaults to enabled, but may be disabled for testing or in
 * embeddings that have not yet implemented barriers on their native classes.
 * There is not currently a way to re-enable incremental GC once it has been
 * disabled on the runtime.
 */
extern JS_PUBLIC_API(void)
DisableIncrementalGC(JSContext* cx);

/**
 * Returns true if incremental GC is enabled. Simply having incremental GC
 * enabled is not sufficient to ensure incremental collections are happening.
 * See the comment "Incremental GC" above for reasons why incremental GC may be
 * suppressed. Inspection of the "nonincremental reason" field of the
 * GCDescription returned by GCSliceCallback may help narrow down the cause if
 * collections are not happening incrementally when expected.
 */
extern JS_PUBLIC_API(bool)
IsIncrementalGCEnabled(JSContext* cx);

/**
 * Returns true while an incremental GC is ongoing, both when actively
 * collecting and between slices.
 */
extern JS_PUBLIC_API(bool)
IsIncrementalGCInProgress(JSContext* cx);

/**
 * Returns true while an incremental GC is ongoing, both when actively
 * collecting and between slices.
 */
extern JS_PUBLIC_API(bool)
IsIncrementalGCInProgress(JSRuntime* rt);

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
    JSContext* cx;

  public:
    explicit AutoDisableGenerationalGC(JSContext* cx);
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
 * Pass a subclass of this "abstract" class to callees to require that they
 * never GC. Subclasses can use assertions or the hazard analysis to ensure no
 * GC happens.
 */
class JS_PUBLIC_API(AutoRequireNoGC)
{
  protected:
    AutoRequireNoGC() {}
    ~AutoRequireNoGC() {}
};

/**
 * Diagnostic assert (see MOZ_DIAGNOSTIC_ASSERT) that GC cannot occur while this
 * class is live. This class does not disable the static rooting hazard
 * analysis.
 *
 * This works by entering a GC unsafe region, which is checked on allocation and
 * on GC.
 */
class JS_PUBLIC_API(AutoAssertNoGC) : public AutoRequireNoGC
{
#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
    JSContext* cx_;

  public:
    // This gets the context from TLS if it is not passed in.
    explicit AutoAssertNoGC(JSContext* cx = nullptr);
    ~AutoAssertNoGC();
#else
  public:
    explicit AutoAssertNoGC(JSContext* cx = nullptr) {}
    ~AutoAssertNoGC() {}
#endif
};

/**
 * Disable the static rooting hazard analysis in the live region and assert in
 * debug builds if any allocation that could potentially trigger a GC occurs
 * while this guard object is live. This is most useful to help the exact
 * rooting hazard analysis in complex regions, since it cannot understand
 * dataflow.
 *
 * Note: GC behavior is unpredictable even when deterministic and is generally
 *       non-deterministic in practice. The fact that this guard has not
 *       asserted is not a guarantee that a GC cannot happen in the guarded
 *       region. As a rule, anyone performing a GC unsafe action should
 *       understand the GC properties of all code in that region and ensure
 *       that the hazard analysis is correct for that code, rather than relying
 *       on this class.
 */
#ifdef DEBUG
class JS_PUBLIC_API(AutoSuppressGCAnalysis) : public AutoAssertNoGC
{
  public:
    explicit AutoSuppressGCAnalysis(JSContext* cx = nullptr) : AutoAssertNoGC(cx) {}
} JS_HAZ_GC_SUPPRESSED;
#else
class JS_PUBLIC_API(AutoSuppressGCAnalysis) : public AutoRequireNoGC
{
  public:
    explicit AutoSuppressGCAnalysis(JSContext* cx = nullptr) {}
} JS_HAZ_GC_SUPPRESSED;
#endif

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
#ifdef DEBUG
    AutoAssertGCCallback();
#else
    AutoAssertGCCallback() {}
#endif
};

/**
 * Place AutoCheckCannotGC in scopes that you believe can never GC. These
 * annotations will be verified both dynamically via AutoAssertNoGC, and
 * statically with the rooting hazard analysis (implemented by making the
 * analysis consider AutoCheckCannotGC to be a GC pointer, and therefore
 * complain if it is live across a GC call.) It is useful when dealing with
 * internal pointers to GC things where the GC thing itself may not be present
 * for the static analysis: e.g. acquiring inline chars from a JSString* on the
 * heap.
 *
 * We only do the assertion checking in DEBUG builds.
 */
#ifdef DEBUG
class JS_PUBLIC_API(AutoCheckCannotGC) : public AutoAssertNoGC
{
  public:
    explicit AutoCheckCannotGC(JSContext* cx = nullptr) : AutoAssertNoGC(cx) {}
} JS_HAZ_GC_INVALIDATED;
#else
class JS_PUBLIC_API(AutoCheckCannotGC) : public AutoRequireNoGC
{
  public:
    explicit AutoCheckCannotGC(JSContext* cx = nullptr) {}
} JS_HAZ_GC_INVALIDATED;
#endif

/*
 * Internal to Firefox.
 */
extern JS_FRIEND_API(void)
NotifyGCRootsRemoved(JSContext* cx);

} /* namespace JS */

/**
 * Register externally maintained GC roots.
 *
 * traceOp: the trace operation. For each root the implementation should call
 *          JS::TraceEdge whenever the root contains a traceable thing.
 * data:    the data argument to pass to each invocation of traceOp.
 */
extern JS_PUBLIC_API(bool)
JS_AddExtraGCRootsTracer(JSContext* cx, JSTraceDataOp traceOp, void* data);

/** Undo a call to JS_AddExtraGCRootsTracer. */
extern JS_PUBLIC_API(void)
JS_RemoveExtraGCRootsTracer(JSContext* cx, JSTraceDataOp traceOp, void* data);

extern JS_PUBLIC_API(void)
JS_GC(JSContext* cx);

extern JS_PUBLIC_API(void)
JS_MaybeGC(JSContext* cx);

extern JS_PUBLIC_API(void)
JS_SetGCCallback(JSContext* cx, JSGCCallback cb, void* data);

extern JS_PUBLIC_API(void)
JS_SetObjectsTenuredCallback(JSContext* cx, JSObjectsTenuredCallback cb,
                             void* data);

extern JS_PUBLIC_API(bool)
JS_AddFinalizeCallback(JSContext* cx, JSFinalizeCallback cb, void* data);

extern JS_PUBLIC_API(void)
JS_RemoveFinalizeCallback(JSContext* cx, JSFinalizeCallback cb);

/*
 * Weak pointers and garbage collection
 *
 * Weak pointers are by their nature not marked as part of garbage collection,
 * but they may need to be updated in two cases after a GC:
 *
 *  1) Their referent was found not to be live and is about to be finalized
 *  2) Their referent has been moved by a compacting GC
 *
 * To handle this, any part of the system that maintain weak pointers to
 * JavaScript GC things must register a callback with
 * JS_(Add,Remove)WeakPointer{ZoneGroup,Compartment}Callback(). This callback
 * must then call JS_UpdateWeakPointerAfterGC() on all weak pointers it knows
 * about.
 *
 * Since sweeping is incremental, we have several callbacks to avoid repeatedly
 * having to visit all embedder structures. The WeakPointerZonesCallback is
 * called once for each strongly connected group of zones, whereas the
 * WeakPointerCompartmentCallback is called once for each compartment that is
 * visited while sweeping. Structures that cannot contain references in more
 * than one compartment should sweep the relevant per-compartment structures
 * using the latter callback to minimizer per-slice overhead.
 *
 * The argument to JS_UpdateWeakPointerAfterGC() is an in-out param. If the
 * referent is about to be finalized the pointer will be set to null. If the
 * referent has been moved then the pointer will be updated to point to the new
 * location.
 *
 * Callers of this method are responsible for updating any state that is
 * dependent on the object's address. For example, if the object's address is
 * used as a key in a hashtable, then the object must be removed and
 * re-inserted with the correct hash.
 */

extern JS_PUBLIC_API(bool)
JS_AddWeakPointerZonesCallback(JSContext* cx, JSWeakPointerZonesCallback cb, void* data);

extern JS_PUBLIC_API(void)
JS_RemoveWeakPointerZonesCallback(JSContext* cx, JSWeakPointerZonesCallback cb);

extern JS_PUBLIC_API(bool)
JS_AddWeakPointerCompartmentCallback(JSContext* cx, JSWeakPointerCompartmentCallback cb,
                                     void* data);

extern JS_PUBLIC_API(void)
JS_RemoveWeakPointerCompartmentCallback(JSContext* cx, JSWeakPointerCompartmentCallback cb);

namespace JS {
template <typename T> class Heap;
}

extern JS_PUBLIC_API(void)
JS_UpdateWeakPointerAfterGC(JS::Heap<JSObject*>* objp);

extern JS_PUBLIC_API(void)
JS_UpdateWeakPointerAfterGCUnbarriered(JSObject** objp);

extern JS_PUBLIC_API(void)
JS_SetGCParameter(JSContext* cx, JSGCParamKey key, uint32_t value);

extern JS_PUBLIC_API(void)
JS_ResetGCParameter(JSContext* cx, JSGCParamKey key);

extern JS_PUBLIC_API(uint32_t)
JS_GetGCParameter(JSContext* cx, JSGCParamKey key);

extern JS_PUBLIC_API(void)
JS_SetGCParametersBasedOnAvailableMemory(JSContext* cx, uint32_t availMem);

/**
 * Create a new JSString whose chars member refers to external memory, i.e.,
 * memory requiring application-specific finalization.
 */
extern JS_PUBLIC_API(JSString*)
JS_NewExternalString(JSContext* cx, const char16_t* chars, size_t length,
                     const JSStringFinalizer* fin);

/**
 * Create a new JSString whose chars member may refer to external memory.
 * If a new external string is allocated, |*allocatedExternal| is set to true.
 * Otherwise the returned string is either not an external string or an
 * external string allocated by a previous call and |*allocatedExternal| is set
 * to false. If |*allocatedExternal| is false, |fin| won't be called.
 */
extern JS_PUBLIC_API(JSString*)
JS_NewMaybeExternalString(JSContext* cx, const char16_t* chars, size_t length,
                          const JSStringFinalizer* fin, bool* allocatedExternal);

/**
 * Return whether 'str' was created with JS_NewExternalString or
 * JS_NewExternalStringWithClosure.
 */
extern JS_PUBLIC_API(bool)
JS_IsExternalString(JSString* str);

/**
 * Return the 'fin' arg passed to JS_NewExternalString.
 */
extern JS_PUBLIC_API(const JSStringFinalizer*)
JS_GetExternalStringFinalizer(JSString* str);

namespace JS {

extern JS_PUBLIC_API(bool)
IsIdleGCTaskNeeded(JSRuntime* rt);

extern JS_PUBLIC_API(void)
RunIdleTimeGCTask(JSRuntime* rt);

} // namespace JS

namespace js {
namespace gc {

/**
 * Create an object providing access to the garbage collector's internal notion
 * of the current state of memory (both GC heap memory and GCthing-controlled
 * malloc memory.
 */
extern JS_PUBLIC_API(JSObject*)
NewMemoryInfoObject(JSContext* cx);

} /* namespace gc */
} /* namespace js */

#endif /* js_GCAPI_h */
