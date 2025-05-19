/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
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

#include "js/CharacterEncoding.h"  // JS::UTF8Chars
#include "js/GCAnnotations.h"
#include "js/shadow/Zone.h"
#include "js/SliceBudget.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"

class JS_PUBLIC_API JSTracer;

namespace js {
namespace gc {
class GCRuntime;
}  // namespace gc
class JS_PUBLIC_API SliceBudget;
namespace gcstats {
struct Statistics;
}  // namespace gcstats
}  // namespace js

namespace JS {

// Options used when starting a GC.
enum class GCOptions : uint32_t {
  // Normal GC invocation.
  //
  // Some objects that are unreachable from the program may still be alive after
  // collection because of internal references
  Normal = 0,

  // A shrinking GC.
  //
  // Try to release as much memory as possible by clearing internal caches,
  // aggressively discarding JIT code and decommitting unused chunks. This
  // ensures all unreferenced objects are removed from the system.
  //
  // Finally, compact the GC heap.
  Shrink = 1,

  // A shutdown GC.
  //
  // This does more drastic cleanup as part of system shutdown, including:
  //  - clearing WeakRef kept object sets
  //  - not marking FinalizationRegistry roots
  //  - repeating collection if JS::NotifyGCRootsRemoved was called
  //  - skipping scheduling of various future work that won't be needed
  //
  // Note that this assumes that no JS will run after this point!
  Shutdown = 2
};

}  // namespace JS

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
  JSGC_MAX_BYTES = 0,

  /**
   * Maximum size of the generational GC nurseries.
   *
   * This will be rounded to the nearest gc::ChunkSize.
   *
   * Pref: javascript.options.mem.nursery.max_kb
   * Default: JS::DefaultNurseryMaxBytes
   */
  JSGC_MAX_NURSERY_BYTES = 2,

  /** Amount of bytes allocated by the GC. */
  JSGC_BYTES = 3,

  /** Number of times GC has been invoked. Includes both major and minor GC. */
  JSGC_NUMBER = 4,

  /**
   * Whether incremental GC is enabled. If not, GC will always run to
   * completion.
   *
   * prefs: javascript.options.mem.gc_incremental.
   * Default: false
   */
  JSGC_INCREMENTAL_GC_ENABLED = 5,

  /**
   * Whether per-zone GC is enabled. If not, all zones are collected every time.
   *
   * prefs: javascript.options.mem.gc_per_zone
   * Default: false
   */
  JSGC_PER_ZONE_GC_ENABLED = 6,

  /** Number of cached empty GC chunks. */
  JSGC_UNUSED_CHUNKS = 7,

  /** Total number of allocated GC chunks. */
  JSGC_TOTAL_CHUNKS = 8,

  /**
   * Max milliseconds to spend in an incremental GC slice.
   *
   * A value of zero means there is no maximum.
   *
   * Pref: javascript.options.mem.gc_incremental_slice_ms
   * Default: DefaultTimeBudgetMS.
   */
  JSGC_SLICE_TIME_BUDGET_MS = 9,

  /**
   * The "do we collect?" decision depends on various parameters and can be
   * summarised as:
   *
   *   ZoneSize > Max(ThresholdBase, LastSize) * GrowthFactor * ThresholdFactor
   *
   * Where
   *   ZoneSize: Current size of this zone.
   *   LastSize: Heap size immediately after the most recent collection.
   *   ThresholdBase: The JSGC_ALLOCATION_THRESHOLD parameter
   *   GrowthFactor: A number above 1, calculated based on some of the
   *                 following parameters.
   *                 See computeZoneHeapGrowthFactorForHeapSize() in GC.cpp
   *   ThresholdFactor: 1.0 to trigger an incremental collections or between
   *                    JSGC_SMALL_HEAP_INCREMENTAL_LIMIT and
   *                    JSGC_LARGE_HEAP_INCREMENTAL_LIMIT to trigger a
   *                    non-incremental collection.
   *
   * The RHS of the equation above is calculated and sets
   * zone->gcHeapThreshold.bytes(). When gcHeapSize.bytes() exeeds
   * gcHeapThreshold.bytes() for a zone, the zone may be scheduled for a GC.
   */

  /**
   * GCs less than this far apart in milliseconds will be considered
   * 'high-frequency GCs'.
   *
   * Pref: javascript.options.mem.gc_high_frequency_time_limit_ms
   * Default: HighFrequencyThreshold
   */
  JSGC_HIGH_FREQUENCY_TIME_LIMIT = 11,

  /**
   * Upper limit for classifying a heap as small (MB).
   *
   * Dynamic heap growth thresholds are based on whether the heap is small,
   * medium or large. Heaps smaller than this size are classified as small;
   * larger heaps are classified as medium or large.
   *
   * Pref: javascript.options.mem.gc_small_heap_size_max_mb
   * Default: SmallHeapSizeMaxBytes
   */
  JSGC_SMALL_HEAP_SIZE_MAX = 12,

  /**
   * Lower limit for classifying a heap as large (MB).
   *
   * Dynamic heap growth thresholds are based on whether the heap is small,
   * medium or large. Heaps larger than this size are classified as large;
   * smaller heaps are classified as small or medium.
   *
   * Pref: javascript.options.mem.gc_large_heap_size_min_mb
   * Default: LargeHeapSizeMinBytes
   */
  JSGC_LARGE_HEAP_SIZE_MIN = 13,

  /**
   * Heap growth factor for small heaps in the high-frequency GC state.
   *
   * Pref: javascript.options.mem.gc_high_frequency_small_heap_growth
   * Default: HighFrequencySmallHeapGrowth
   */
  JSGC_HIGH_FREQUENCY_SMALL_HEAP_GROWTH = 14,

  /**
   * Heap growth factor for large heaps in the high-frequency GC state.
   *
   * Pref: javascript.options.mem.gc_high_frequency_large_heap_growth
   * Default: HighFrequencyLargeHeapGrowth
   */
  JSGC_HIGH_FREQUENCY_LARGE_HEAP_GROWTH = 15,

  /**
   * Heap growth factor for low frequency GCs.
   *
   * This factor is applied regardless of the size of the heap when not in the
   * high-frequency GC state.
   *
   * Pref: javascript.options.mem.gc_low_frequency_heap_growth
   * Default: LowFrequencyHeapGrowth
   */
  JSGC_LOW_FREQUENCY_HEAP_GROWTH = 16,

  /**
   * Whether balanced heap limits are enabled.
   *
   * If this is set to true then heap limits are calculated in a way designed to
   * balance memory usage optimally between many heaps.
   *
   * Otherwise, heap limits are set based on a linear multiple of the retained
   * size after the last collection.
   *
   * Pref: javascript.options.mem.gc_balanced_heap_limits
   * Default: BalancedHeapLimitsEnabled
   */
  JSGC_BALANCED_HEAP_LIMITS_ENABLED = 17,

  /**
   * Heap growth parameter for balanced heap limit calculation.
   *
   * This parameter trades off GC time for memory usage. Smaller values result
   * in lower memory use and larger values result in less time spent collecting.
   *
   * Heap limits are set to the heap's retained size plus some extra space. The
   * extra space is calculated based on several factors but is scaled
   * proportionally to this parameter.
   *
   * Pref: javascript.options.mem.gc_heap_growth_factor
   * Default: HeapGrowthFactor
   */
  JSGC_HEAP_GROWTH_FACTOR = 18,

  /**
   * Lower limit for collecting a zone (MB).
   *
   * Zones smaller than this size will not normally be collected.
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
   * We never keep more than this many unused chunks in the free chunk pool.
   *
   * Pref: javascript.options.mem.gc_max_empty_chunk_count
   * Default: MaxEmptyChunkCount
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
   * Whether parallel marking is enabled.
   *
   * Pref: javascript.options.mem.gc_parallel_marking
   * Default: ParallelMarkingEnabled
   */
  JSGC_PARALLEL_MARKING_ENABLED = 24,

  /**
   * Limit of how far over the incremental trigger threshold we allow the heap
   * to grow before finishing a collection non-incrementally, for small heaps.
   *
   * We trigger an incremental GC when a trigger threshold is reached but the
   * collection may not be fast enough to keep up with the mutator. At some
   * point we finish the collection non-incrementally.
   *
   * Default: SmallHeapIncrementalLimit
   * Pref: javascript.options.mem.gc_small_heap_incremental_limit
   */
  JSGC_SMALL_HEAP_INCREMENTAL_LIMIT = 25,

  /**
   * Limit of how far over the incremental trigger threshold we allow the heap
   * to grow before finishing a collection non-incrementally, for large heaps.
   *
   * Default: LargeHeapIncrementalLimit
   * Pref: javascript.options.mem.gc_large_heap_incremental_limit
   */
  JSGC_LARGE_HEAP_INCREMENTAL_LIMIT = 26,

  /**
   * Free space bytes threshold for eager nursery collection.
   *
   * Default: NurseryChunkUsableSize / 4
   * Pref: javascript.options.mem.nursery_eager_collection_threshold_kb
   */
  JSGC_NURSERY_EAGER_COLLECTION_THRESHOLD_KB = 27,

  /**
   * Free space fraction threshold for eager nursery collection. This is a
   * percentage (from 0 to 99).
   *
   * Default: 25
   * Pref: javascript.options.mem.nursery_eager_collection_threshold_percent
   */
  JSGC_NURSERY_EAGER_COLLECTION_THRESHOLD_PERCENT = 30,

  /**
   * Minimum size of the generational GC nurseries.
   *
   * This value will be rounded to the nearest Nursery::SubChunkStep if below
   * gc::ChunkSize, otherwise it'll be rounded to the nearest gc::ChunkSize.
   *
   * Default: Nursery::SubChunkLimit
   * Pref: javascript.options.mem.nursery.min_kb
   */
  JSGC_MIN_NURSERY_BYTES = 31,

  /**
   * The minimum time to allow between triggering last ditch GCs in seconds.
   *
   * Default: 60 seconds
   * Pref: None
   */
  JSGC_MIN_LAST_DITCH_GC_PERIOD = 32,

  /**
   * The delay (in heapsize kilobytes) between slices of an incremental GC.
   *
   * Default: ZoneAllocDelayBytes
   */
  JSGC_ZONE_ALLOC_DELAY_KB = 33,

  /*
   * The current size of the nursery.
   *
   * This parameter is read-only.
   */
  JSGC_NURSERY_BYTES = 34,

  /**
   * Retained size base value for calculating malloc heap threshold.
   *
   * Default: MallocThresholdBase
   */
  JSGC_MALLOC_THRESHOLD_BASE = 35,

  /**
   * Whether incremental weakmap marking is enabled.
   *
   * Pref: javascript.options.mem.incremental_weakmap
   * Default: IncrementalWeakMarkEnabled
   */
  JSGC_INCREMENTAL_WEAKMAP_ENABLED = 37,

  /**
   * The chunk size in bytes for this system.
   *
   * This parameter is read-only.
   */
  JSGC_CHUNK_BYTES = 38,

  /**
   * The number of background threads to use for parallel GC work for each CPU
   * core, expressed as an integer percentage.
   *
   * Pref: javascript.options.mem.gc_helper_thread_ratio
   */
  JSGC_HELPER_THREAD_RATIO = 39,

  /**
   * The maximum number of background threads to use for parallel GC work.
   *
   * Pref: javascript.options.mem.gc_max_helper_threads
   */
  JSGC_MAX_HELPER_THREADS = 40,

  /**
   * The number of background threads to use for parallel GC work.
   *
   * This parameter is read-only and is set based on the
   * JSGC_HELPER_THREAD_RATIO and JSGC_MAX_HELPER_THREADS parameters.
   */
  JSGC_HELPER_THREAD_COUNT = 41,

  /**
   * A number that is incremented on every major GC slice.
   */
  JSGC_MAJOR_GC_NUMBER = 44,

  /**
   * A number that is incremented on every minor GC.
   */
  JSGC_MINOR_GC_NUMBER = 45,

  /**
   * JS::MaybeRunNurseryCollection will collect the nursery if it hasn't been
   * collected in this many milliseconds.
   *
   * Default: 5000
   * Pref: javascript.options.mem.nursery_eager_collection_timeout_ms
   */
  JSGC_NURSERY_EAGER_COLLECTION_TIMEOUT_MS = 46,

  /**
   * The system page size in KB.
   *
   * This parameter is read-only.
   */
  JSGC_SYSTEM_PAGE_SIZE_KB = 47,

  /**
   * In an incremental GC, this determines the point at which to start
   * increasing the slice budget and frequency of allocation triggered slices to
   * try to avoid reaching the incremental limit and finishing the collection
   * synchronously.
   *
   * The threshold is calculated by subtracting this value from the heap's
   * incremental limit.
   */
  JSGC_URGENT_THRESHOLD_MB = 48,

  /**
   * Get the number of threads used for parallel marking.
   *
   * Pref: None.
   */
  JSGC_MARKING_THREAD_COUNT = 49,

  /**
   * The heap size above which to use parallel marking.
   *
   * Pref: javascript.options.mem.gc_parallel_marking_threshold_mb
   * Default: ParallelMarkingThresholdMB
   */
  JSGC_PARALLEL_MARKING_THRESHOLD_MB = 50,

  /**
   * Whether the semispace nursery is enabled.
   *
   * Pref: javascript.options.mem.gc_experimental_semispace_nursery
   * Default: SemispaceNurseryEnabled
   */
  JSGC_SEMISPACE_NURSERY_ENABLED = 51,

  /**
   * Set the maximum number of threads to use for parallel marking, if enabled.
   *
   * The actual number used is calculated based on the number of available
   * helper threads and can be found by getting the JSGC_MARKING_THREAD_COUNT
   * parameter.
   *
   * Pref: javascript.options.mem.gc_max_parallel_marking_threads
   * Default: 2.
   */
  JSGC_MAX_MARKING_THREADS = 52,

  /**
   * Whether to automatically generate missing allocation sites so data about
   * them can be gathered.
   *
   * Pref: None, this is an internal engine feature.
   * Default: false.
   */
  JSGC_GENERATE_MISSING_ALLOC_SITES = 53,

  /**
   * A number that is incremented every GC slice.
   */
  JSGC_SLICE_NUMBER = 54,

} JSGCParamKey;

/*
 * Generic trace operation that calls JS::TraceEdge on each traceable thing's
 * location reachable from data.
 */
typedef void (*JSTraceDataOp)(JSTracer* trc, void* data);

/*
 * Trace hook used to trace gray roots incrementally.
 *
 * This should return whether tracing is finished. It will be called repeatedly
 * in subsequent GC slices until it returns true.
 *
 * While tracing this should check the budget and return false if it has been
 * exceeded. When passed an unlimited budget it should always return true.
 */
typedef bool (*JSGrayRootsTracer)(JSTracer* trc, js::SliceBudget& budget,
                                  void* data);

typedef enum JSGCStatus { JSGC_BEGIN, JSGC_END } JSGCStatus;

typedef void (*JSObjectsTenuredCallback)(JS::GCContext* gcx, void* data);

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

typedef void (*JSFinalizeCallback)(JS::GCContext* gcx, JSFinalizeStatus status,
                                   void* data);

typedef void (*JSWeakPointerZonesCallback)(JSTracer* trc, void* data);

typedef void (*JSWeakPointerCompartmentCallback)(JSTracer* trc,
                                                 JS::Compartment* comp,
                                                 void* data);

/*
 * This is called to tell the embedding that a FinalizationRegistry object has
 * cleanup work, and that the engine should be called back at an appropriate
 * later time to perform this cleanup, by calling the function |doCleanup|.
 *
 * This callback must not do anything that could cause GC.
 */
using JSHostCleanupFinalizationRegistryCallback =
    void (*)(JSFunction* doCleanup, JSObject* incumbentGlobal, void* data);

/**
 * Each external string has a pointer to JSExternalStringCallbacks. Embedders
 * can use this to implement custom finalization or memory reporting behavior.
 */
struct JSExternalStringCallbacks {
  /**
   * Finalizes external strings created by JS_NewExternalStringLatin1 or
   * JS_NewExternalUCString. The finalizer can be called off the main
   * thread.
   */
  virtual void finalize(JS::Latin1Char* chars) const = 0;
  virtual void finalize(char16_t* chars) const = 0;

  /**
   * Callback used by memory reporting to ask the embedder how much memory an
   * external string is keeping alive.  The embedder is expected to return a
   * value that corresponds to the size of the allocation that will be released
   * by the finalizer callback above.
   *
   * Implementations of this callback MUST NOT do anything that can cause GC.
   */
  virtual size_t sizeOfBuffer(const JS::Latin1Char* chars,
                              mozilla::MallocSizeOf mallocSizeOf) const = 0;
  virtual size_t sizeOfBuffer(const char16_t* chars,
                              mozilla::MallocSizeOf mallocSizeOf) const = 0;
};

namespace JS {

#define GCREASONS(D)                                                   \
  /* Reasons internal to the JS engine. */                             \
  D(API, 0)                                                            \
  D(EAGER_ALLOC_TRIGGER, 1)                                            \
  D(DESTROY_RUNTIME, 2)                                                \
  D(ROOTS_REMOVED, 3)                                                  \
  D(LAST_DITCH, 4)                                                     \
  D(TOO_MUCH_MALLOC, 5)                                                \
  D(ALLOC_TRIGGER, 6)                                                  \
  D(DEBUG_GC, 7)                                                       \
  D(COMPARTMENT_REVIVED, 8)                                            \
  D(RESET, 9)                                                          \
  D(OUT_OF_NURSERY, 10)                                                \
  D(EVICT_NURSERY, 11)                                                 \
  D(SHARED_MEMORY_LIMIT, 13)                                           \
  D(EAGER_NURSERY_COLLECTION, 14)                                      \
  D(BG_TASK_FINISHED, 15)                                              \
  D(ABORT_GC, 16)                                                      \
  D(FULL_WHOLE_CELL_BUFFER, 17)                                        \
  D(FULL_GENERIC_BUFFER, 18)                                           \
  D(FULL_VALUE_BUFFER, 19)                                             \
  D(FULL_CELL_PTR_OBJ_BUFFER, 20)                                      \
  D(FULL_SLOT_BUFFER, 21)                                              \
  D(FULL_SHAPE_BUFFER, 22)                                             \
  D(TOO_MUCH_WASM_MEMORY, 23)                                          \
  D(DISABLE_GENERATIONAL_GC, 24)                                       \
  D(FINISH_GC, 25)                                                     \
  D(PREPARE_FOR_TRACING, 26)                                           \
  D(FULL_WASM_ANYREF_BUFFER, 27)                                       \
  D(FULL_CELL_PTR_STR_BUFFER, 28)                                      \
  D(TOO_MUCH_JIT_CODE, 29)                                             \
  D(FULL_CELL_PTR_BIGINT_BUFFER, 30)                                   \
  D(NURSERY_TRAILERS, 31)                                              \
  D(NURSERY_MALLOC_BUFFERS, 32)                                        \
                                                                       \
  /*                                                                   \
   * Reasons from Firefox.                                             \
   *                                                                   \
   * The JS engine attaches special meanings to some of these reasons. \
   */                                                                  \
  D(DOM_WINDOW_UTILS, FIRST_FIREFOX_REASON)                            \
  D(COMPONENT_UTILS, 34)                                               \
  D(MEM_PRESSURE, 35)                                                  \
  D(CC_FINISHED, 36)                                                   \
  D(CC_FORCED, 37)                                                     \
  D(LOAD_END, 38)                                                      \
  D(UNUSED3, 39)                                                       \
  D(PAGE_HIDE, 40)                                                     \
  D(NSJSCONTEXT_DESTROY, 41)                                           \
  D(WORKER_SHUTDOWN, 42)                                               \
  D(SET_DOC_SHELL, 43)                                                 \
  D(DOM_UTILS, 44)                                                     \
  D(DOM_IPC, 45)                                                       \
  D(DOM_WORKER, 46)                                                    \
  D(INTER_SLICE_GC, 47)                                                \
  D(UNUSED1, 48)                                                       \
  D(FULL_GC_TIMER, 49)                                                 \
  D(SHUTDOWN_CC, 50)                                                   \
  D(UNUSED2, 51)                                                       \
  D(USER_INACTIVE, 52)                                                 \
  D(XPCONNECT_SHUTDOWN, 53)                                            \
  D(DOCSHELL, 54)                                                      \
  D(HTML_PARSER, 55)                                                   \
  D(DOM_TESTUTILS, 56)                                                 \
  D(PREPARE_FOR_PAGELOAD, 57)                                          \
                                                                       \
  /* Reasons reserved for embeddings. */                               \
  D(RESERVED1, FIRST_RESERVED_REASON)                                  \
  D(RESERVED2, 91)                                                     \
  D(RESERVED3, 92)                                                     \
  D(RESERVED4, 93)                                                     \
  D(RESERVED5, 94)                                                     \
  D(RESERVED6, 95)                                                     \
  D(RESERVED7, 96)                                                     \
  D(RESERVED8, 97)                                                     \
  D(RESERVED9, 98)

enum class GCReason {
  FIRST_FIREFOX_REASON = 33,
  FIRST_RESERVED_REASON = 90,

#define MAKE_REASON(name, val) name = val,
  GCREASONS(MAKE_REASON)
#undef MAKE_REASON
      NO_REASON,
  NUM_REASONS,

  /*
   * For telemetry, we want to keep a fixed max bucket size over time so we
   * don't have to switch histograms. 100 is conservative; but the cost of extra
   * buckets seems to be low while the cost of switching histograms is high.
   */
  NUM_TELEMETRY_REASONS = 100
};

/**
 * Get a statically allocated C string explaining the given GC reason.
 */
extern JS_PUBLIC_API const char* ExplainGCReason(JS::GCReason reason);

/**
 * Return true if the GC reason is internal to the JS engine.
 */
extern JS_PUBLIC_API bool InternalGCReason(JS::GCReason reason);

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
extern JS_PUBLIC_API void PrepareZoneForGC(JSContext* cx, Zone* zone);

/**
 * Schedule all zones to be collected in the next GC.
 */
extern JS_PUBLIC_API void PrepareForFullGC(JSContext* cx);

/**
 * When performing an incremental GC, the zones that were selected for the
 * previous incremental slice must be selected in subsequent slices as well.
 * This function selects those slices automatically.
 */
extern JS_PUBLIC_API void PrepareForIncrementalGC(JSContext* cx);

/**
 * Returns true if any zone in the system has been scheduled for GC with one of
 * the functions above or by the JS engine.
 */
extern JS_PUBLIC_API bool IsGCScheduled(JSContext* cx);

/**
 * Undoes the effect of the Prepare methods above. The given zone will not be
 * collected in the next GC.
 */
extern JS_PUBLIC_API void SkipZoneForGC(JSContext* cx, Zone* zone);

/*
 * Non-Incremental GC:
 *
 * The following functions perform a non-incremental GC.
 */

/**
 * Performs a non-incremental collection of all selected zones.
 */
extern JS_PUBLIC_API void NonIncrementalGC(JSContext* cx, JS::GCOptions options,
                                           GCReason reason);

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
 *  - The GC parameter JSGC_INCREMENTAL_GC_ENABLED must be true.
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
extern JS_PUBLIC_API void StartIncrementalGC(JSContext* cx,
                                             JS::GCOptions options,
                                             GCReason reason,
                                             const js::SliceBudget& budget);

/**
 * Perform a slice of an ongoing incremental collection. When this function
 * returns, the collection may not be complete. It must be called repeatedly
 * until !IsIncrementalGCInProgress(cx).
 *
 * Note: SpiderMonkey's GC is not realtime. Slices in practice may be longer or
 *       shorter than the requested interval.
 */
extern JS_PUBLIC_API void IncrementalGCSlice(JSContext* cx, GCReason reason,
                                             const js::SliceBudget& budget);

/**
 * Return whether an incremental GC has work to do on the foreground thread and
 * would make progress if a slice was run now. If this returns false then the GC
 * is waiting for background threads to finish their work and a slice started
 * now would return immediately.
 */
extern JS_PUBLIC_API bool IncrementalGCHasForegroundWork(JSContext* cx);

/**
 * If IsIncrementalGCInProgress(cx), this call finishes the ongoing collection
 * by performing an arbitrarily long slice. If !IsIncrementalGCInProgress(cx),
 * this is equivalent to NonIncrementalGC. When this function returns,
 * IsIncrementalGCInProgress(cx) will always be false.
 */
extern JS_PUBLIC_API void FinishIncrementalGC(JSContext* cx, GCReason reason);

/**
 * If IsIncrementalGCInProgress(cx), this call aborts the ongoing collection and
 * performs whatever work needs to be done to return the collector to its idle
 * state. This may take an arbitrarily long time. When this function returns,
 * IsIncrementalGCInProgress(cx) will always be false.
 */
extern JS_PUBLIC_API void AbortIncrementalGC(JSContext* cx);

namespace dbg {

// The `JS::dbg::GarbageCollectionEvent` class is essentially a view of the
// `js::gcstats::Statistics` data without the uber implementation-specific bits.
// It should generally be palatable for web developers.
class GarbageCollectionEvent {
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
      : majorGCNumber_(majorGCNum),
        reason(nullptr),
        nonincrementalReason(nullptr),
        collections() {}

  using Ptr = js::UniquePtr<GarbageCollectionEvent>;
  static Ptr Create(JSRuntime* rt, ::js::gcstats::Statistics& stats,
                    uint64_t majorGCNumber);

  JSObject* toJSObject(JSContext* cx) const;

  uint64_t majorGCNumber() const { return majorGCNumber_; }
};

}  // namespace dbg

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

struct JS_PUBLIC_API GCDescription {
  bool isZone_;
  bool isComplete_;
  JS::GCOptions options_;
  GCReason reason_;

  GCDescription(bool isZone, bool isComplete, JS::GCOptions options,
                GCReason reason)
      : isZone_(isZone),
        isComplete_(isComplete),
        options_(options),
        reason_(reason) {}

  char16_t* formatSliceMessage(JSContext* cx) const;
  char16_t* formatSummaryMessage(JSContext* cx) const;

  mozilla::TimeStamp startTime(JSContext* cx) const;
  mozilla::TimeStamp endTime(JSContext* cx) const;
  mozilla::TimeStamp lastSliceStart(JSContext* cx) const;
  mozilla::TimeStamp lastSliceEnd(JSContext* cx) const;

  JS::UniqueChars sliceToJSONProfiler(JSContext* cx) const;
  JS::UniqueChars formatJSONProfiler(JSContext* cx) const;

  JS::dbg::GarbageCollectionEvent::Ptr toGCEvent(JSContext* cx) const;
};

extern JS_PUBLIC_API UniqueChars MinorGcToJSON(JSContext* cx);

typedef void (*GCSliceCallback)(JSContext* cx, GCProgress progress,
                                const GCDescription& desc);

/**
 * The GC slice callback is called at the beginning and end of each slice. This
 * callback may be used for GC notifications as well as to perform additional
 * marking.
 */
extern JS_PUBLIC_API GCSliceCallback
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
using GCNurseryCollectionCallback = void (*)(JSContext* cx,
                                             GCNurseryProgress progress,
                                             GCReason reason, void* data);

/**
 * Add and remove nursery collection callbacks for the given runtime. These will
 * be called at the start and end of every nursery collection.
 */
extern JS_PUBLIC_API bool AddGCNurseryCollectionCallback(
    JSContext* cx, GCNurseryCollectionCallback callback, void* data);
extern JS_PUBLIC_API void RemoveGCNurseryCollectionCallback(
    JSContext* cx, GCNurseryCollectionCallback callback, void* data);

typedef void (*DoCycleCollectionCallback)(JSContext* cx);

/**
 * The purge gray callback is called after any COMPARTMENT_REVIVED GC in which
 * the majority of compartments have been marked gray.
 */
extern JS_PUBLIC_API DoCycleCollectionCallback
SetDoCycleCollectionCallback(JSContext* cx, DoCycleCollectionCallback callback);

using CreateSliceBudgetCallback = js::SliceBudget (*)(JS::GCReason reason,
                                                      int64_t millis);

/**
 * Called when generating a GC slice budget. It allows the embedding to control
 * the duration of slices and potentially check an interrupt flag as well. For
 * internally triggered GCs, the given millis parameter is the JS engine's
 * internal scheduling decision, which the embedding can choose to ignore.
 * (Otherwise, it will be the value that was passed to eg
 * JS::IncrementalGCSlice()).
 */
extern JS_PUBLIC_API void SetCreateGCSliceBudgetCallback(
    JSContext* cx, CreateSliceBudgetCallback cb);

/**
 * Incremental GC defaults to enabled, but may be disabled for testing or in
 * embeddings that have not yet implemented barriers on their native classes.
 * There is not currently a way to re-enable incremental GC once it has been
 * disabled on the runtime.
 */
extern JS_PUBLIC_API void DisableIncrementalGC(JSContext* cx);

/**
 * Returns true if incremental GC is enabled. Simply having incremental GC
 * enabled is not sufficient to ensure incremental collections are happening.
 * See the comment "Incremental GC" above for reasons why incremental GC may be
 * suppressed. Inspection of the "nonincremental reason" field of the
 * GCDescription returned by GCSliceCallback may help narrow down the cause if
 * collections are not happening incrementally when expected.
 */
extern JS_PUBLIC_API bool IsIncrementalGCEnabled(JSContext* cx);

/**
 * Returns true while an incremental GC is ongoing, both when actively
 * collecting and between slices.
 */
extern JS_PUBLIC_API bool IsIncrementalGCInProgress(JSContext* cx);

/**
 * Returns true while an incremental GC is ongoing, both when actively
 * collecting and between slices.
 */
extern JS_PUBLIC_API bool IsIncrementalGCInProgress(JSRuntime* rt);

/**
 * Returns true if the most recent GC ran incrementally.
 */
extern JS_PUBLIC_API bool WasIncrementalGC(JSRuntime* rt);

/*
 * Generational GC:
 */

/**
 * Ensure that generational GC is disabled within some scope.
 *
 * This evicts the nursery and discards JIT code so it is not a lightweight
 * operation.
 */
class JS_PUBLIC_API AutoDisableGenerationalGC {
  JSContext* cx;

 public:
  explicit AutoDisableGenerationalGC(JSContext* cx);
  ~AutoDisableGenerationalGC();
};

/**
 * Returns true if generational allocation and collection is currently enabled
 * on the given runtime.
 */
extern JS_PUBLIC_API bool IsGenerationalGCEnabled(JSRuntime* rt);

/**
 * Pass a subclass of this "abstract" class to callees to require that they
 * never GC. Subclasses can use assertions or the hazard analysis to ensure no
 * GC happens.
 */
class JS_PUBLIC_API AutoRequireNoGC {
 protected:
  AutoRequireNoGC() = default;
  ~AutoRequireNoGC() = default;
};

/**
 * Diagnostic assert (see MOZ_DIAGNOSTIC_ASSERT) that GC cannot occur while this
 * class is live. This class does not disable the static rooting hazard
 * analysis.
 *
 * This works by entering a GC unsafe region, which is checked on allocation and
 * on GC.
 */
class JS_PUBLIC_API AutoAssertNoGC : public AutoRequireNoGC {
#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
 protected:
  JSContext* cx_;  // nullptr if inactive.

 public:
  // Nullptr here means get the context from TLS. It does not mean inactive
  // (though cx_ may end up nullptr, and thus inactive, if TLS has not yet been
  // initialized.)
  explicit AutoAssertNoGC(JSContext* cx = nullptr);
  AutoAssertNoGC(AutoAssertNoGC&& other) : cx_(other.cx_) {
    other.cx_ = nullptr;
  }
  ~AutoAssertNoGC();

  void reset();
#else
 public:
  explicit AutoAssertNoGC(JSContext* cx = nullptr) {}
  ~AutoAssertNoGC() {}

  void reset() {}
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
class JS_PUBLIC_API AutoSuppressGCAnalysis : public AutoAssertNoGC {
 public:
  explicit AutoSuppressGCAnalysis(JSContext* cx = nullptr)
      : AutoAssertNoGC(cx) {}
} JS_HAZ_GC_SUPPRESSED;
#else
class JS_PUBLIC_API AutoSuppressGCAnalysis : public AutoRequireNoGC {
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
class JS_PUBLIC_API AutoAssertGCCallback : public AutoSuppressGCAnalysis {
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
class JS_PUBLIC_API AutoCheckCannotGC : public AutoAssertNoGC {
 public:
  explicit AutoCheckCannotGC(JSContext* cx = nullptr) : AutoAssertNoGC(cx) {}
#  ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
  AutoCheckCannotGC(const AutoCheckCannotGC& other)
      : AutoCheckCannotGC(other.cx_) {}
#  else
  AutoCheckCannotGC(const AutoCheckCannotGC& other) : AutoCheckCannotGC() {}
#  endif
  AutoCheckCannotGC(AutoCheckCannotGC&& other)
      : AutoAssertNoGC(std::forward<AutoAssertNoGC>(other)) {}
#else
class JS_PUBLIC_API AutoCheckCannotGC : public AutoRequireNoGC {
 public:
  explicit AutoCheckCannotGC(JSContext* cx = nullptr) {}
  AutoCheckCannotGC(const AutoCheckCannotGC& other) : AutoCheckCannotGC() {}
  AutoCheckCannotGC(AutoCheckCannotGC&& other) : AutoCheckCannotGC() {}
  void reset() {}
#endif
} JS_HAZ_GC_INVALIDATED JS_HAZ_GC_REF;

extern JS_PUBLIC_API void SetLowMemoryState(JSContext* cx, bool newState);

/*
 * Internal to Firefox.
 */
extern JS_PUBLIC_API void NotifyGCRootsRemoved(JSContext* cx);

} /* namespace JS */

typedef void (*JSGCCallback)(JSContext* cx, JSGCStatus status,
                             JS::GCReason reason, void* data);

/**
 * Register externally maintained GC roots.
 *
 * traceOp: the trace operation. For each root the implementation should call
 *          JS::TraceEdge whenever the root contains a traceable thing.
 * data:    the data argument to pass to each invocation of traceOp.
 */
extern JS_PUBLIC_API bool JS_AddExtraGCRootsTracer(JSContext* cx,
                                                   JSTraceDataOp traceOp,
                                                   void* data);

/** Undo a call to JS_AddExtraGCRootsTracer. */
extern JS_PUBLIC_API void JS_RemoveExtraGCRootsTracer(JSContext* cx,
                                                      JSTraceDataOp traceOp,
                                                      void* data);

extern JS_PUBLIC_API void JS_GC(JSContext* cx,
                                JS::GCReason reason = JS::GCReason::API);

extern JS_PUBLIC_API void JS_MaybeGC(JSContext* cx);

extern JS_PUBLIC_API void JS_SetGCCallback(JSContext* cx, JSGCCallback cb,
                                           void* data);

extern JS_PUBLIC_API void JS_SetObjectsTenuredCallback(
    JSContext* cx, JSObjectsTenuredCallback cb, void* data);

extern JS_PUBLIC_API bool JS_AddFinalizeCallback(JSContext* cx,
                                                 JSFinalizeCallback cb,
                                                 void* data);

extern JS_PUBLIC_API void JS_RemoveFinalizeCallback(JSContext* cx,
                                                    JSFinalizeCallback cb);

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
 * The return value of JS_UpdateWeakPointerAfterGC() indicates whether the
 * referent is still alive. If the referent is is about to be finalized, this
 * will return false.
 *
 * Callers of this method are responsible for updating any state that is
 * dependent on the object's address. For example, if the object's address is
 * used as a key in a hashtable, then the object must be removed and
 * re-inserted with the correct hash.
 */

extern JS_PUBLIC_API bool JS_AddWeakPointerZonesCallback(
    JSContext* cx, JSWeakPointerZonesCallback cb, void* data);

extern JS_PUBLIC_API void JS_RemoveWeakPointerZonesCallback(
    JSContext* cx, JSWeakPointerZonesCallback cb);

extern JS_PUBLIC_API bool JS_AddWeakPointerCompartmentCallback(
    JSContext* cx, JSWeakPointerCompartmentCallback cb, void* data);

extern JS_PUBLIC_API void JS_RemoveWeakPointerCompartmentCallback(
    JSContext* cx, JSWeakPointerCompartmentCallback cb);

namespace JS {
template <typename T>
class Heap;
}

extern JS_PUBLIC_API bool JS_UpdateWeakPointerAfterGC(
    JSTracer* trc, JS::Heap<JSObject*>* objp);

extern JS_PUBLIC_API bool JS_UpdateWeakPointerAfterGCUnbarriered(
    JSTracer* trc, JSObject** objp);

extern JS_PUBLIC_API void JS_SetGCParameter(JSContext* cx, JSGCParamKey key,
                                            uint32_t value);

extern JS_PUBLIC_API void JS_ResetGCParameter(JSContext* cx, JSGCParamKey key);

extern JS_PUBLIC_API uint32_t JS_GetGCParameter(JSContext* cx,
                                                JSGCParamKey key);

extern JS_PUBLIC_API void JS_SetGCParametersBasedOnAvailableMemory(
    JSContext* cx, uint32_t availMemMB);

/**
 * Create a new JSString whose chars member refers to external memory, i.e.,
 * memory requiring application-specific finalization.
 */
extern JS_PUBLIC_API JSString* JS_NewExternalStringLatin1(
    JSContext* cx, const JS::Latin1Char* chars, size_t length,
    const JSExternalStringCallbacks* callbacks);
extern JS_PUBLIC_API JSString* JS_NewExternalUCString(
    JSContext* cx, const char16_t* chars, size_t length,
    const JSExternalStringCallbacks* callbacks);

/**
 * Create a new JSString whose chars member may refer to external memory.
 * If a new external string is allocated, |*allocatedExternal| is set to true.
 * Otherwise the returned string is either not an external string or an
 * external string allocated by a previous call and |*allocatedExternal| is set
 * to false. If |*allocatedExternal| is false, |fin| won't be called.
 */
extern JS_PUBLIC_API JSString* JS_NewMaybeExternalStringLatin1(
    JSContext* cx, const JS::Latin1Char* chars, size_t length,
    const JSExternalStringCallbacks* callbacks, bool* allocatedExternal);
extern JS_PUBLIC_API JSString* JS_NewMaybeExternalUCString(
    JSContext* cx, const char16_t* chars, size_t length,
    const JSExternalStringCallbacks* callbacks, bool* allocatedExternal);

/**
 * Similar to JS_NewMaybeExternalStringLatin1.
 *
 * Create an external Latin1 string if the utf8 buffer contains only ASCII
 * chars, otherwise copy the chars into a non-external string.
 */
extern JS_PUBLIC_API JSString* JS_NewMaybeExternalStringUTF8(
    JSContext* cx, const JS::UTF8Chars& utf8,
    const JSExternalStringCallbacks* callbacks, bool* allocatedExternal);

/**
 * Return the 'callbacks' arg passed to JS_NewExternalStringLatin1,
 * JS_NewExternalUCString, JS_NewMaybeExternalStringLatin1,
 * or JS_NewMaybeExternalUCString.
 */
extern JS_PUBLIC_API const JSExternalStringCallbacks*
JS_GetExternalStringCallbacks(JSString* str);

namespace JS {

/**
 * Check whether the nursery should be eagerly collected, this is before it is
 * full.
 *
 * The idea is that this can be called when the host environment has some idle
 * time which it can use to for GC activity.
 *
 * Returns GCReason::NO_REASON to indicate no collection is desired.
 */
extern JS_PUBLIC_API GCReason WantEagerMinorGC(JSRuntime* rt);

extern JS_PUBLIC_API GCReason WantEagerMajorGC(JSRuntime* rt);

/**
 * Check whether the nursery should be eagerly collected as per WantEagerMajorGC
 * above, and if so run a collection.
 *
 * The idea is that this can be called when the host environment has some idle
 * time which it can use to for GC activity.
 */
extern JS_PUBLIC_API void MaybeRunNurseryCollection(JSRuntime* rt,
                                                    JS::GCReason reason);

extern JS_PUBLIC_API void RunNurseryCollection(
    JSRuntime* rt, JS::GCReason reason,
    mozilla::TimeDuration aSinceLastMinorGC);

extern JS_PUBLIC_API void SetHostCleanupFinalizationRegistryCallback(
    JSContext* cx, JSHostCleanupFinalizationRegistryCallback cb, void* data);

/**
 * Clear kept alive objects in JS WeakRef.
 * https://tc39.es/proposal-weakrefs/#sec-clear-kept-objects
 */
extern JS_PUBLIC_API void ClearKeptObjects(JSContext* cx);

inline JS_PUBLIC_API bool NeedGrayRootsForZone(Zone* zoneArg) {
  shadow::Zone* zone = shadow::Zone::from(zoneArg);
  return zone->isGCMarkingBlackAndGray() || zone->isGCCompacting();
}

extern JS_PUBLIC_API bool AtomsZoneIsCollecting(JSRuntime* runtime);
extern JS_PUBLIC_API bool IsAtomsZone(Zone* zone);

}  // namespace JS

namespace js {
namespace gc {

/**
 * Create an object providing access to the garbage collector's internal notion
 * of the current state of memory (both GC heap memory and GCthing-controlled
 * malloc memory.
 */
extern JS_PUBLIC_API JSObject* NewMemoryInfoObject(JSContext* cx);

/*
 * Get the GCContext for the current context.
 *
 * This is here to allow the browser to call finalizers for dead nursery
 * objects. This is a dangerous operation - only use this if you know what
 * you're doing!
 */
extern JS_PUBLIC_API JS::GCContext* GetGCContext(JSContext* cx);

} /* namespace gc */
} /* namespace js */

#ifdef JS_GC_ZEAL

namespace JS {

static constexpr uint32_t ShellDefaultGCZealFrequency = 100;
static constexpr uint32_t BrowserDefaultGCZealFrequency = 5000;

extern JS_PUBLIC_API void GetGCZealBits(JSContext* cx, uint32_t* zealBits,
                                        uint32_t* frequency,
                                        uint32_t* nextScheduled);

extern JS_PUBLIC_API void SetGCZeal(JSContext* cx, uint8_t zeal,
                                    uint32_t frequency);

extern JS_PUBLIC_API void UnsetGCZeal(JSContext* cx, uint8_t zeal);

extern JS_PUBLIC_API void ScheduleGC(JSContext* cx, uint32_t count);

}  // namespace JS

#endif

#endif /* js_GCAPI_h */
