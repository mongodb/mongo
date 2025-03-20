/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * [SMDOC] GC Scheduling
 *
 * GC Scheduling Overview
 * ======================
 *
 * See also GC scheduling from Firefox's perspective here:
 * https://searchfox.org/mozilla-central/source/dom/base/CCGCScheduler.cpp
 *
 * Scheduling GC's in SpiderMonkey/Firefox is tremendously complicated because
 * of the large number of subtle, cross-cutting, and widely dispersed factors
 * that must be taken into account. A summary of some of the more important
 * factors follows.
 *
 * Cost factors:
 *
 *   * GC too soon and we'll revisit an object graph almost identical to the
 *     one we just visited; since we are unlikely to find new garbage, the
 *     traversal will be largely overhead. We rely heavily on external factors
 *     to signal us that we are likely to find lots of garbage: e.g. "a tab
 *     just got closed".
 *
 *   * GC too late and we'll run out of memory to allocate (e.g. Out-Of-Memory,
 *     hereafter simply abbreviated to OOM). If this happens inside
 *     SpiderMonkey we may be able to recover, but most embedder allocations
 *     will simply crash on OOM, even if the GC has plenty of free memory it
 *     could surrender.
 *
 *   * Memory fragmentation: if we fill the process with GC allocations, a
 *     request for a large block of contiguous memory may fail because no
 *     contiguous block is free, despite having enough memory available to
 *     service the request.
 *
 *   * Management overhead: if our GC heap becomes large, we create extra
 *     overhead when managing the GC's structures, even if the allocations are
 *     mostly unused.
 *
 * Heap Management Factors:
 *
 *   * GC memory: The GC has its own allocator that it uses to make fixed size
 *     allocations for GC managed things. In cases where the GC thing requires
 *     larger or variable sized memory to implement itself, it is responsible
 *     for using the system heap.
 *
 *   * C Heap Memory: Rather than allowing for large or variable allocations,
 *     the SpiderMonkey GC allows GC things to hold pointers to C heap memory.
 *     It is the responsibility of the thing to free this memory with a custom
 *     finalizer (with the sole exception of NativeObject, which knows about
 *     slots and elements for performance reasons). C heap memory has different
 *     performance and overhead tradeoffs than GC internal memory, which need
 *     to be considered with scheduling a GC.
 *
 * Application Factors:
 *
 *   * Most applications allocate heavily at startup, then enter a processing
 *     stage where memory utilization remains roughly fixed with a slower
 *     allocation rate. This is not always the case, however, so while we may
 *     optimize for this pattern, we must be able to handle arbitrary
 *     allocation patterns.
 *
 * Other factors:
 *
 *   * Other memory: This is memory allocated outside the purview of the GC.
 *     Data mapped by the system for code libraries, data allocated by those
 *     libraries, data in the JSRuntime that is used to manage the engine,
 *     memory used by the embedding that is not attached to a GC thing, memory
 *     used by unrelated processes running on the hardware that use space we
 *     could otherwise use for allocation, etc. While we don't have to manage
 *     it, we do have to take it into account when scheduling since it affects
 *     when we will OOM.
 *
 *   * Physical Reality: All real machines have limits on the number of bits
 *     that they are physically able to store. While modern operating systems
 *     can generally make additional space available with swapping, at some
 *     point there are simply no more bits to allocate. There is also the
 *     factor of address space limitations, particularly on 32bit machines.
 *
 *   * Platform Factors: Each OS makes use of wildly different memory
 *     management techniques. These differences result in different performance
 *     tradeoffs, different fragmentation patterns, and different hard limits
 *     on the amount of physical and/or virtual memory that we can use before
 *     OOMing.
 *
 *
 * Reasons for scheduling GC
 * -------------------------
 *
 *  While code generally takes the above factors into account in only an ad-hoc
 *  fashion, the API forces the user to pick a "reason" for the GC. We have a
 *  bunch of JS::GCReason reasons in GCAPI.h. These fall into a few categories
 *  that generally coincide with one or more of the above factors.
 *
 *  Embedding reasons:
 *
 *   1) Do a GC now because the embedding knows something useful about the
 *      zone's memory retention state. These are GCReasons like LOAD_END,
 *      PAGE_HIDE, SET_NEW_DOCUMENT, DOM_UTILS. Mostly, Gecko uses these to
 *      indicate that a significant fraction of the scheduled zone's memory is
 *      probably reclaimable.
 *
 *   2) Do some known amount of GC work now because the embedding knows now is
 *      a good time to do a long, unblockable operation of a known duration.
 *      These are INTER_SLICE_GC and REFRESH_FRAME.
 *
 *  Correctness reasons:
 *
 *   3) Do a GC now because correctness depends on some GC property. For
 *      example, CC_FORCED is where the embedding requires the mark bits to be
 *      set correctly. Also, EVICT_NURSERY where we need to work on the tenured
 *      heap.
 *
 *   4) Do a GC because we are shutting down: e.g. SHUTDOWN_CC or DESTROY_*.
 *
 *   5) Do a GC because a compartment was accessed between GC slices when we
 *      would have otherwise discarded it. We have to do a second GC to clean
 *      it up: e.g. COMPARTMENT_REVIVED.
 *
 *  Emergency Reasons:
 *
 *   6) Do an all-zones, non-incremental GC now because the embedding knows it
 *      cannot wait: e.g. MEM_PRESSURE.
 *
 *   7) OOM when fetching a new Chunk results in a LAST_DITCH GC.
 *
 *  Heap Size Limitation Reasons:
 *
 *   8) Do an incremental, zonal GC with reason MAYBEGC when we discover that
 *      the gc's allocated size is approaching the current trigger. This is
 *      called MAYBEGC because we make this check in the MaybeGC function.
 *      MaybeGC gets called at the top of the main event loop. Normally, it is
 *      expected that this callback will keep the heap size limited. It is
 *      relatively inexpensive, because it is invoked with no JS running and
 *      thus few stack roots to scan. For this reason, the GC's "trigger" bytes
 *      is less than the GC's "max" bytes as used by the trigger below.
 *
 *   9) Do an incremental, zonal GC with reason MAYBEGC when we go to allocate
 *      a new GC thing and find that the GC heap size has grown beyond the
 *      configured maximum (JSGC_MAX_BYTES). We trigger this GC by returning
 *      nullptr and then calling maybeGC at the top level of the allocator.
 *      This is then guaranteed to fail the "size greater than trigger" check
 *      above, since trigger is always less than max. After performing the GC,
 *      the allocator unconditionally returns nullptr to force an OOM exception
 *      is raised by the script.
 *
 *      Note that this differs from a LAST_DITCH GC where we actually run out
 *      of memory (i.e., a call to a system allocator fails) when trying to
 *      allocate. Unlike above, LAST_DITCH GC only happens when we are really
 *      out of memory, not just when we cross an arbitrary trigger; despite
 *      this, it may still return an allocation at the end and allow the script
 *      to continue, if the LAST_DITCH GC was able to free up enough memory.
 *
 *  10) Do a GC under reason ALLOC_TRIGGER when we are over the GC heap trigger
 *      limit, but in the allocator rather than in a random call to maybeGC.
 *      This occurs if we allocate too much before returning to the event loop
 *      and calling maybeGC; this is extremely common in benchmarks and
 *      long-running Worker computations. Note that this uses a wildly
 *      different mechanism from the above in that it sets the interrupt flag
 *      and does the GC at the next loop head, before the next alloc, or
 *      maybeGC. The reason for this is that this check is made after the
 *      allocation and we cannot GC with an uninitialized thing in the heap.
 *
 *  11) Do an incremental, zonal GC with reason TOO_MUCH_MALLOC when the total
 * amount of malloced memory is greater than the malloc trigger limit for the
 * zone.
 *
 *
 * Size Limitation Triggers Explanation
 * ------------------------------------
 *
 *  The GC internally is entirely unaware of the context of the execution of
 *  the mutator. It sees only:
 *
 *   A) Allocated size: this is the amount of memory currently requested by the
 *      mutator. This quantity is monotonically increasing: i.e. the allocation
 *      rate is always >= 0. It is also easy for the system to track.
 *
 *   B) Retained size: this is the amount of memory that the mutator can
 *      currently reach. Said another way, it is the size of the heap
 *      immediately after a GC (modulo background sweeping). This size is very
 *      costly to know exactly and also extremely hard to estimate with any
 *      fidelity.
 *
 *   For reference, a common allocated vs. retained graph might look like:
 *
 *       |                                  **         **
 *       |                       **       ** *       **
 *       |                     ** *     **   *     **
 *       |           *       **   *   **     *   **
 *       |          **     **     * **       * **
 *      s|         * *   **       ** +  +    **
 *      i|        *  *  *      +  +       +  +     +
 *      z|       *   * * +  +                   +     +  +
 *      e|      *    **+
 *       |     *     +
 *       |    *    +
 *       |   *   +
 *       |  *  +
 *       | * +
 *       |*+
 *       +--------------------------------------------------
 *                               time
 *                                           *** = allocated
 *                                           +++ = retained
 *
 *           Note that this is a bit of a simplification
 *           because in reality we track malloc and GC heap
 *           sizes separately and have a different level of
 *           granularity and accuracy on each heap.
 *
 *   This presents some obvious implications for Mark-and-Sweep collectors.
 *   Namely:
 *       -> t[marking] ~= size[retained]
 *       -> t[sweeping] ~= size[allocated] - size[retained]
 *
 *   In a non-incremental collector, maintaining low latency and high
 *   responsiveness requires that total GC times be as low as possible. Thus,
 *   in order to stay responsive when we did not have a fully incremental
 *   collector, our GC triggers were focused on minimizing collection time.
 *   Furthermore, since size[retained] is not under control of the GC, all the
 *   GC could do to control collection times was reduce sweep times by
 *   minimizing size[allocated], per the equation above.
 *
 *   The result of the above is GC triggers that focus on size[allocated] to
 *   the exclusion of other important factors and default heuristics that are
 *   not optimal for a fully incremental collector. On the other hand, this is
 *   not all bad: minimizing size[allocated] also minimizes the chance of OOM
 *   and sweeping remains one of the hardest areas to further incrementalize.
 *
 *      EAGER_ALLOC_TRIGGER
 *      -------------------
 *      Occurs when we return to the event loop and find our heap is getting
 *      largish, but before t[marking] OR t[sweeping] is too large for a
 *      responsive non-incremental GC. This is intended to be the common case
 *      in normal web applications: e.g. we just finished an event handler and
 *      the few objects we allocated when computing the new whatzitz have
 *      pushed us slightly over the limit. After this GC we rescale the new
 *      EAGER_ALLOC_TRIGGER trigger to 150% of size[retained] so that our
 *      non-incremental GC times will always be proportional to this size
 *      rather than being dominated by sweeping.
 *
 *      As a concession to mutators that allocate heavily during their startup
 *      phase, we have a highFrequencyGCMode that ups the growth rate to 300%
 *      of the current size[retained] so that we'll do fewer longer GCs at the
 *      end of the mutator startup rather than more, smaller GCs.
 *
 *          Assumptions:
 *            -> Responsiveness is proportional to t[marking] + t[sweeping].
 *            -> size[retained] is proportional only to GC allocations.
 *
 *      ALLOC_TRIGGER (non-incremental)
 *      -------------------------------
 *      If we do not return to the event loop before getting all the way to our
 *      gc trigger bytes then MAYBEGC will never fire. To avoid OOMing, we
 *      succeed the current allocation and set the script interrupt so that we
 *      will (hopefully) do a GC before we overflow our max and have to raise
 *      an OOM exception for the script.
 *
 *          Assumptions:
 *            -> Common web scripts will return to the event loop before using
 *               10% of the current triggerBytes worth of GC memory.
 *
 *      ALLOC_TRIGGER (incremental)
 *      ---------------------------
 *      In practice the above trigger is rough: if a website is just on the
 *      cusp, sometimes it will trigger a non-incremental GC moments before
 *      returning to the event loop, where it could have done an incremental
 *      GC. Thus, we recently added an incremental version of the above with a
 *      substantially lower threshold, so that we have a soft limit here. If
 *      IGC can collect faster than the allocator generates garbage, even if
 *      the allocator does not return to the event loop frequently, we should
 *      not have to fall back to a non-incremental GC.
 *
 *      INCREMENTAL_TOO_SLOW
 *      --------------------
 *      Do a full, non-incremental GC if we overflow ALLOC_TRIGGER during an
 *      incremental GC. When in the middle of an incremental GC, we suppress
 *      our other triggers, so we need a way to backstop the IGC if the
 *      mutator allocates faster than the IGC can clean things up.
 *
 *      TOO_MUCH_MALLOC
 *      ---------------
 *      Performs a GC before size[allocated] - size[retained] gets too large
 *      for non-incremental sweeping to be fast in the case that we have
 *      significantly more malloc allocation than GC allocation. This is meant
 *      to complement MAYBEGC triggers. We track this by counting malloced
 *      bytes; the counter gets reset at every GC since we do not always have a
 *      size at the time we call free. Because of this, the malloc heuristic
 *      is, unfortunately, not usefully able to augment our other GC heap
 *      triggers and is limited to this singular heuristic.
 *
 *          Assumptions:
 *            -> EITHER size[allocated_by_malloc] ~= size[allocated_by_GC]
 *                 OR   time[sweeping] ~= size[allocated_by_malloc]
 *            -> size[retained] @ t0 ~= size[retained] @ t1
 *               i.e. That the mutator is in steady-state operation.
 *
 *      LAST_DITCH_GC
 *      -------------
 *      Does a GC because we are out of memory.
 *
 *          Assumptions:
 *            -> size[retained] < size[available_memory]
 */

#ifndef gc_Scheduling_h
#define gc_Scheduling_h

#include "mozilla/Atomics.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Maybe.h"
#include "mozilla/TimeStamp.h"

#include "gc/GCEnum.h"
#include "js/AllocPolicy.h"
#include "js/GCAPI.h"
#include "js/HashTable.h"
#include "js/HeapAPI.h"
#include "threading/ProtectedData.h"

// Macro to define scheduling tunables for GC parameters. Expands its argument
// repeatedly with the following arguments:
//   - key:     the JSGCParamKey value for this parameter
//   - type:    the storage type
//   - name:    the name of GCSchedulingTunables getter method
//   - convert: a helper class defined in Scheduling.cpp that provides
//              conversion methods
//   - check:   a helper function defined in Scheduling.cppto check the value is
//              valid
//   - default: the initial value and that assigned by resetParameter
#define FOR_EACH_GC_TUNABLE(_)                                                 \
  /*                                                                           \
   * JSGC_MAX_BYTES                                                            \
   *                                                                           \
   * Maximum nominal heap before last ditch GC.                                \
   */                                                                          \
  _(JSGC_MAX_BYTES, size_t, gcMaxBytes, ConvertSize, NoCheck, 0xffffffff)      \
                                                                               \
  /*                                                                           \
   * JSGC_MIN_NURSERY_BYTES                                                    \
   * JSGC_MAX_NURSERY_BYTES                                                    \
   *                                                                           \
   * Minimum and maximum nursery size for each runtime.                        \
   */                                                                          \
  _(JSGC_MIN_NURSERY_BYTES, size_t, gcMinNurseryBytes, ConvertNurseryBytes,    \
    CheckNurserySize, 256 * 1024)                                              \
  _(JSGC_MAX_NURSERY_BYTES, size_t, gcMaxNurseryBytes, ConvertNurseryBytes,    \
    CheckNurserySize, JS::DefaultNurseryMaxBytes)                              \
                                                                               \
  /*                                                                           \
   * JSGC_ALLOCATION_THRESHOLD                                                 \
   *                                                                           \
   *                                                                           \
   * The base value used to compute zone->threshold.bytes(). When              \
   * gcHeapSize.bytes() exceeds threshold.bytes() for a zone, the zone may be  \
   * scheduled for a GC, depending on the exact circumstances.                 \
   */                                                                          \
  _(JSGC_ALLOCATION_THRESHOLD, size_t, gcZoneAllocThresholdBase, ConvertMB,    \
    NoCheck, 27 * 1024 * 1024)                                                 \
                                                                               \
  /*                                                                           \
   * JSGC_SMALL_HEAP_SIZE_MAX                                                  \
   * JSGC_LARGE_HEAP_SIZE_MIN                                                  \
   *                                                                           \
   * Used to classify heap sizes into one of small, medium or large. This      \
   * affects the calcuation of the incremental GC trigger and the heap growth  \
   * factor in high frequency GC mode.                                         \
   */                                                                          \
  _(JSGC_SMALL_HEAP_SIZE_MAX, size_t, smallHeapSizeMaxBytes, ConvertMB,        \
    NoCheck, 100 * 1024 * 1024)                                                \
  _(JSGC_LARGE_HEAP_SIZE_MIN, size_t, largeHeapSizeMinBytes, ConvertMB,        \
    CheckNonZero, 500 * 1024 * 1024)                                           \
                                                                               \
  /*                                                                           \
   * JSGC_SMALL_HEAP_INCREMENTAL_LIMIT                                         \
   * JSGC_LARGE_HEAP_INCREMENTAL_LIMIT                                         \
   *                                                                           \
   * Multiple of threshold.bytes() which triggers a non-incremental GC.        \
   *                                                                           \
   * The small heap limit must be greater than 1.3 to maintain performance on  \
   * splay-latency.                                                            \
   */                                                                          \
  _(JSGC_SMALL_HEAP_INCREMENTAL_LIMIT, double, smallHeapIncrementalLimit,      \
    ConvertTimes100, CheckIncrementalLimit, 1.50)                              \
  _(JSGC_LARGE_HEAP_INCREMENTAL_LIMIT, double, largeHeapIncrementalLimit,      \
    ConvertTimes100, CheckIncrementalLimit, 1.10)                              \
                                                                               \
  /*                                                                           \
   * JSGC_HIGH_FREQUENCY_TIME_LIMIT                                            \
   *                                                                           \
   * We enter high-frequency mode if we GC a twice within this many            \
   * millisconds.                                                              \
   */                                                                          \
  _(JSGC_HIGH_FREQUENCY_TIME_LIMIT, mozilla::TimeDuration,                     \
    highFrequencyThreshold, ConvertMillis, NoCheck,                            \
    mozilla::TimeDuration::FromSeconds(1))                                     \
                                                                               \
  /*                                                                           \
   * JSGC_LOW_FREQUENCY_HEAP_GROWTH                                            \
   *                                                                           \
   * When not in |highFrequencyGC| mode, this is the global (stored per-zone)  \
   * "HeapGrowthFactor".                                                       \
   */                                                                          \
  _(JSGC_LOW_FREQUENCY_HEAP_GROWTH, double, lowFrequencyHeapGrowth,            \
    ConvertTimes100, CheckHeapGrowth, 1.5)                                     \
                                                                               \
  /*                                                                           \
   * JSGC_HIGH_FREQUENCY_SMALL_HEAP_GROWTH                                     \
   * JSGC_HIGH_FREQUENCY_LARGE_HEAP_GROWTH                                     \
   *                                                                           \
   * When in the |highFrequencyGC| mode, these parameterize the per-zone       \
   * "HeapGrowthFactor" computation.                                           \
   */                                                                          \
  _(JSGC_HIGH_FREQUENCY_SMALL_HEAP_GROWTH, double,                             \
    highFrequencySmallHeapGrowth, ConvertTimes100, CheckHeapGrowth, 3.0)       \
  _(JSGC_HIGH_FREQUENCY_LARGE_HEAP_GROWTH, double,                             \
    highFrequencyLargeHeapGrowth, ConvertTimes100, CheckHeapGrowth, 1.5)       \
                                                                               \
  /*                                                                           \
   * JSGC_MALLOC_THRESHOLD_BASE                                                \
   *                                                                           \
   * The base value used to compute the GC trigger for malloc allocated        \
   * memory.                                                                   \
   */                                                                          \
  _(JSGC_MALLOC_THRESHOLD_BASE, size_t, mallocThresholdBase, ConvertMB,        \
    NoCheck, 38 * 1024 * 1024)                                                 \
                                                                               \
  /*                                                                           \
   * Number of bytes to allocate between incremental slices in GCs triggered   \
   * by the zone allocation threshold.                                         \
   */                                                                          \
  _(JSGC_ZONE_ALLOC_DELAY_KB, size_t, zoneAllocDelayBytes, ConvertKB,          \
    CheckNonZero, 1024 * 1024)                                                 \
                                                                               \
  /*                                                                           \
   * JSGC_URGENT_THRESHOLD_MB                                                  \
   *                                                                           \
   * The point before reaching the non-incremental limit at which to start     \
   * increasing the slice budget and frequency of allocation triggered slices. \
   */                                                                          \
  _(JSGC_URGENT_THRESHOLD_MB, size_t, urgentThresholdBytes, ConvertMB,         \
    NoCheck, 16 * 1024 * 1024)                                                 \
                                                                               \
  /*                                                                           \
   * JSGC_NURSERY_EAGER_COLLECTION_THRESHOLD_KB                                \
   * JSGC_NURSERY_EAGER_COLLECTION_THRESHOLD_PERCENT                           \
   * JSGC_NURSERY_EAGER_COLLECTION_TIMEOUT_MS                                  \
   *                                                                           \
   * JS::MaybeRunNurseryCollection will run a minor GC if the free space falls \
   * below a threshold or if it hasn't been collected for too long.            \
   *                                                                           \
   * To avoid making this too eager, two thresholds must be met. The free      \
   * space must fall below a size threshold and the fraction of free space     \
   * remaining must also fall below a threshold.                               \
   *                                                                           \
   * See Nursery::wantEagerCollection() for more details.                      \
   */                                                                          \
  _(JSGC_NURSERY_EAGER_COLLECTION_THRESHOLD_KB, size_t,                        \
    nurseryEagerCollectionThresholdBytes, ConvertKB, NoCheck, ChunkSize / 4)   \
  _(JSGC_NURSERY_EAGER_COLLECTION_THRESHOLD_PERCENT, double,                   \
    nurseryEagerCollectionThresholdPercent, ConvertTimes100,                   \
    CheckNonZeroUnitRange, 0.25)                                               \
  _(JSGC_NURSERY_EAGER_COLLECTION_TIMEOUT_MS, mozilla::TimeDuration,           \
    nurseryEagerCollectionTimeout, ConvertMillis, NoCheck,                     \
    mozilla::TimeDuration::FromSeconds(5))                                     \
                                                                               \
  /*                                                                           \
   * JSGC_BALANCED_HEAP_LIMITS_ENABLED                                         \
   * JSGC_HEAP_GROWTH_FACTOR                                                   \
   */                                                                          \
  _(JSGC_BALANCED_HEAP_LIMITS_ENABLED, bool, balancedHeapLimitsEnabled,        \
    ConvertBool, NoCheck, false)                                               \
  _(JSGC_HEAP_GROWTH_FACTOR, double, heapGrowthFactor, ConvertDouble, NoCheck, \
    50.0)                                                                      \
                                                                               \
  /*                                                                           \
   * JSGC_MIN_LAST_DITCH_GC_PERIOD                                             \
   *                                                                           \
   * Last ditch GC is skipped if allocation failure occurs less than this many \
   * seconds from the previous one.                                            \
   */                                                                          \
  _(JSGC_MIN_LAST_DITCH_GC_PERIOD, mozilla::TimeDuration,                      \
    minLastDitchGCPeriod, ConvertSeconds, NoCheck,                             \
    TimeDuration::FromSeconds(60))                                             \
                                                                               \
  /*                                                                           \
   * JSGC_PARALLEL_MARKING_THRESHOLD_MB                                        \
   */                                                                          \
  _(JSGC_PARALLEL_MARKING_THRESHOLD_MB, size_t, parallelMarkingThresholdBytes, \
    ConvertMB, NoCheck, 4 * 1024 * 1024)                                       \
                                                                               \
  /*                                                                           \
   * JSGC_GENERATE_MISSING_ALLOC_SITES                                         \
   */                                                                          \
  _(JSGC_GENERATE_MISSING_ALLOC_SITES, bool, generateMissingAllocSites,        \
    ConvertBool, NoCheck, false)

namespace js {

class ZoneAllocator;

namespace gc {

struct Cell;

/*
 * Default settings for tuning the GC.  Some of these can be set at runtime,
 * This list is not complete, some tuning parameters are not listed here.
 *
 * If you change the values here, please also consider changing them in
 * modules/libpref/init/all.js where they are duplicated for the Firefox
 * preferences.
 */
namespace TuningDefaults {

/* JSGC_MIN_EMPTY_CHUNK_COUNT */
static const uint32_t MinEmptyChunkCount = 1;

/* JSGC_MAX_EMPTY_CHUNK_COUNT */
static const uint32_t MaxEmptyChunkCount = 30;

/* JSGC_SLICE_TIME_BUDGET_MS */
static const int64_t DefaultTimeBudgetMS = 0;  // Unlimited by default.

/* JSGC_INCREMENTAL_GC_ENABLED */
static const bool IncrementalGCEnabled = false;

/* JSGC_PER_ZONE_GC_ENABLED */
static const bool PerZoneGCEnabled = false;

/* JSGC_COMPACTING_ENABLED */
static const bool CompactingEnabled = true;

/* JSGC_PARALLEL_MARKING_ENABLED */
static const bool ParallelMarkingEnabled = false;

/* JSGC_INCREMENTAL_WEAKMAP_ENABLED */
static const bool IncrementalWeakMapMarkingEnabled = true;

/* JSGC_SEMISPACE_NURSERY_ENABLED */
static const bool SemispaceNurseryEnabled = false;

/* JSGC_HELPER_THREAD_RATIO */
static const double HelperThreadRatio = 0.5;

/* JSGC_MAX_HELPER_THREADS */
static const size_t MaxHelperThreads = 8;

/* JSGC_MAX_MARKING_THREADS */
static const size_t MaxMarkingThreads = 2;

}  // namespace TuningDefaults

/*
 * Encapsulates all of the GC tunables. These are effectively constant and
 * should only be modified by setParameter.
 */
class GCSchedulingTunables {
#define DEFINE_TUNABLE_FIELD(key, type, name, convert, check, default) \
  MainThreadOrGCTaskData<type> name##_;
  FOR_EACH_GC_TUNABLE(DEFINE_TUNABLE_FIELD)
#undef DEFINE_TUNABLE_FIELD

 public:
  GCSchedulingTunables();

#define DEFINE_TUNABLE_ACCESSOR(key, type, name, convert, check, default) \
  type name() const { return name##_; }
  FOR_EACH_GC_TUNABLE(DEFINE_TUNABLE_ACCESSOR)
#undef DEFINE_TUNABLE_ACCESSOR

  uint32_t getParameter(JSGCParamKey key);
  [[nodiscard]] bool setParameter(JSGCParamKey key, uint32_t value);
  void resetParameter(JSGCParamKey key);

 private:
  void maintainInvariantsAfterUpdate(JSGCParamKey updated);
  void checkInvariants();
};

class GCSchedulingState {
  /*
   * Influences how we schedule and run GC's in several subtle ways. The most
   * important factor is in how it controls the "HeapGrowthFactor". The
   * growth factor is a measure of how large (as a percentage of the last GC)
   * the heap is allowed to grow before we try to schedule another GC.
   */
  mozilla::Atomic<bool, mozilla::ReleaseAcquire> inHighFrequencyGCMode_;

 public:
  GCSchedulingState() : inHighFrequencyGCMode_(false) {}

  bool inHighFrequencyGCMode() const { return inHighFrequencyGCMode_; }

  void updateHighFrequencyMode(const mozilla::TimeStamp& lastGCTime,
                               const mozilla::TimeStamp& currentTime,
                               const GCSchedulingTunables& tunables);
  void updateHighFrequencyModeForReason(JS::GCReason reason);
};

struct TriggerResult {
  bool shouldTrigger;
  size_t usedBytes;
  size_t thresholdBytes;
};

using AtomicByteCount = mozilla::Atomic<size_t, mozilla::ReleaseAcquire>;

/*
 * Tracks the size of allocated data. This is used for both GC and malloc data.
 * It automatically maintains the memory usage relationship between parent and
 * child instances, i.e. between those in a GCRuntime and its Zones.
 */
class HeapSize {
  /*
   * The number of bytes in use. For GC heaps this is approximate to the nearest
   * ArenaSize. It is atomic because it is updated by both the active and GC
   * helper threads.
   */
  AtomicByteCount bytes_;

  /*
   * The number of bytes in use at the start of the last collection.
   */
  MainThreadData<size_t> initialBytes_;

  /*
   * The number of bytes retained after the last collection. This is updated
   * dynamically during incremental GC. It does not include allocations that
   * happen during a GC.
   */
  AtomicByteCount retainedBytes_;

 public:
  explicit HeapSize() {
    MOZ_ASSERT(bytes_ == 0);
    MOZ_ASSERT(retainedBytes_ == 0);
  }

  size_t bytes() const { return bytes_; }
  size_t initialBytes() const { return initialBytes_; }
  size_t retainedBytes() const { return retainedBytes_; }

  void updateOnGCStart() { retainedBytes_ = initialBytes_ = bytes(); }

  void addGCArena() { addBytes(ArenaSize); }
  void removeGCArena() {
    MOZ_ASSERT(retainedBytes_ >= ArenaSize);
    removeBytes(ArenaSize, true /* only sweeping removes arenas */);
    MOZ_ASSERT(retainedBytes_ <= bytes_);
  }

  void addBytes(size_t nbytes) {
    mozilla::DebugOnly<size_t> initialBytes(bytes_);
    MOZ_ASSERT(initialBytes + nbytes > initialBytes);
    bytes_ += nbytes;
  }
  void removeBytes(size_t nbytes, bool updateRetainedSize) {
    if (updateRetainedSize) {
      MOZ_ASSERT(retainedBytes_ >= nbytes);
      retainedBytes_ -= nbytes;
    }
    MOZ_ASSERT(bytes_ >= nbytes);
    bytes_ -= nbytes;
  }
};

/*
 * Like HeapSize, but also updates a 'parent' HeapSize. Used for per-zone heap
 * size data that also updates a runtime-wide parent.
 */
class HeapSizeChild : public HeapSize {
 public:
  void addGCArena(HeapSize& parent) {
    HeapSize::addGCArena();
    parent.addGCArena();
  }

  void removeGCArena(HeapSize& parent) {
    HeapSize::removeGCArena();
    parent.removeGCArena();
  }

  void addBytes(size_t nbytes, HeapSize& parent) {
    HeapSize::addBytes(nbytes);
    parent.addBytes(nbytes);
  }

  void removeBytes(size_t nbytes, bool updateRetainedSize, HeapSize& parent) {
    HeapSize::removeBytes(nbytes, updateRetainedSize);
    parent.removeBytes(nbytes, updateRetainedSize);
  }
};

class PerZoneGCHeapSize : public HeapSizeChild {
 public:
  size_t freedBytes() const { return freedBytes_; }
  void clearFreedBytes() { freedBytes_ = 0; }

  void removeGCArena(HeapSize& parent) {
    HeapSizeChild::removeGCArena(parent);
    freedBytes_ += ArenaSize;
  }

  void removeBytes(size_t nbytes, bool updateRetainedSize, HeapSize& parent) {
    HeapSizeChild::removeBytes(nbytes, updateRetainedSize, parent);
    freedBytes_ += nbytes;
  }

 private:
  AtomicByteCount freedBytes_;
};

// Heap size thresholds used to trigger GC. This is an abstract base class for
// GC heap and malloc thresholds defined below.
class HeapThreshold {
 protected:
  HeapThreshold()
      : startBytes_(SIZE_MAX),
        incrementalLimitBytes_(SIZE_MAX),
        sliceBytes_(SIZE_MAX) {}

  // The threshold at which to start a new incremental collection.
  //
  // This can be read off main thread during collection, for example by sweep
  // tasks that resize tables.
  MainThreadOrGCTaskData<size_t> startBytes_;

  // The threshold at which start a new non-incremental collection or finish an
  // ongoing collection non-incrementally.
  MainThreadData<size_t> incrementalLimitBytes_;

  // The threshold at which to trigger a slice during an ongoing incremental
  // collection.
  MainThreadData<size_t> sliceBytes_;

 public:
  size_t startBytes() const { return startBytes_; }
  size_t sliceBytes() const { return sliceBytes_; }
  size_t incrementalLimitBytes() const { return incrementalLimitBytes_; }
  size_t eagerAllocTrigger(bool highFrequencyGC) const;
  size_t incrementalBytesRemaining(const HeapSize& heapSize) const;

  void setSliceThreshold(ZoneAllocator* zone, const HeapSize& heapSize,
                         const GCSchedulingTunables& tunables,
                         bool waitingOnBGTask);
  void clearSliceThreshold() { sliceBytes_ = SIZE_MAX; }
  bool hasSliceThreshold() const { return sliceBytes_ != SIZE_MAX; }

 protected:
  static double computeZoneHeapGrowthFactorForHeapSize(
      size_t lastBytes, const GCSchedulingTunables& tunables,
      const GCSchedulingState& state);

  void setIncrementalLimitFromStartBytes(size_t retainedBytes,
                                         const GCSchedulingTunables& tunables);
};

// A heap threshold that is based on a multiple of the retained size after the
// last collection adjusted based on collection frequency and retained
// size. This is used to determine when to do a zone GC based on GC heap size.
class GCHeapThreshold : public HeapThreshold {
 public:
  void updateStartThreshold(size_t lastBytes,
                            mozilla::Maybe<double> allocationRate,
                            mozilla::Maybe<double> collectionRate,
                            const GCSchedulingTunables& tunables,
                            const GCSchedulingState& state, bool isAtomsZone);

 private:
  // This is our original algorithm for calculating heap limits.
  static size_t computeZoneTriggerBytes(double growthFactor, size_t lastBytes,
                                        const GCSchedulingTunables& tunables);

  // This is the algorithm described in the optimal heap limits paper.
  static double computeBalancedHeapLimit(size_t lastBytes,
                                         double allocationRate,
                                         double collectionRate,
                                         const GCSchedulingTunables& tunables);
};

// A heap threshold that is calculated as a constant multiple of the retained
// size after the last collection. This is used to determines when to do a zone
// GC based on malloc data.
class MallocHeapThreshold : public HeapThreshold {
 public:
  void updateStartThreshold(size_t lastBytes,
                            const GCSchedulingTunables& tunables,
                            const GCSchedulingState& state);

 private:
  static size_t computeZoneTriggerBytes(double growthFactor, size_t lastBytes,
                                        size_t baseBytes);
};

// A fixed threshold that's used to determine when we need to do a zone GC based
// on allocated JIT code.
class JitHeapThreshold : public HeapThreshold {
 public:
  explicit JitHeapThreshold(size_t bytes) { startBytes_ = bytes; }
};

#ifdef DEBUG

// Counts memory associated with GC things in a zone.
//
// This records details of the cell (or non-cell pointer) the memory allocation
// is associated with to check the correctness of the information provided. This
// is not present in opt builds.
class MemoryTracker {
 public:
  MemoryTracker();
  void fixupAfterMovingGC();
  void checkEmptyOnDestroy();

  // Track memory by associated GC thing pointer.
  void trackGCMemory(Cell* cell, size_t nbytes, MemoryUse use);
  void untrackGCMemory(Cell* cell, size_t nbytes, MemoryUse use);
  void swapGCMemory(Cell* a, Cell* b, MemoryUse use);

  // Track memory by associated non-GC thing pointer.
  void registerNonGCMemory(void* mem, MemoryUse use);
  void unregisterNonGCMemory(void* mem, MemoryUse use);
  void moveNonGCMemory(void* dst, void* src, MemoryUse use);
  void incNonGCMemory(void* mem, size_t nbytes, MemoryUse use);
  void decNonGCMemory(void* mem, size_t nbytes, MemoryUse use);

 private:
  template <typename Ptr>
  struct Key {
    Key(Ptr* ptr, MemoryUse use);
    Ptr* ptr() const;
    MemoryUse use() const;

   private:
#  ifdef JS_64BIT
    // Pack this into a single word on 64 bit platforms.
    uintptr_t ptr_ : 56;
    uintptr_t use_ : 8;
#  else
    uintptr_t ptr_ : 32;
    uintptr_t use_ : 8;
#  endif
  };

  template <typename Ptr>
  struct Hasher {
    using KeyT = Key<Ptr>;
    using Lookup = KeyT;
    static HashNumber hash(const Lookup& l);
    static bool match(const KeyT& key, const Lookup& l);
    static void rekey(KeyT& k, const KeyT& newKey);
  };

  template <typename Ptr>
  using Map = HashMap<Key<Ptr>, size_t, Hasher<Ptr>, SystemAllocPolicy>;
  using GCMap = Map<Cell>;
  using NonGCMap = Map<void>;

  static bool isGCMemoryUse(MemoryUse use);
  static bool isNonGCMemoryUse(MemoryUse use);
  static bool allowMultipleAssociations(MemoryUse use);

  size_t getAndRemoveEntry(const Key<Cell>& key, LockGuard<Mutex>& lock);

  Mutex mutex MOZ_UNANNOTATED;

  // Map containing the allocated size associated with (cell, use) pairs.
  GCMap gcMap;

  // Map containing the allocated size associated (non-cell pointer, use) pairs.
  NonGCMap nonGCMap;
};

#endif  // DEBUG

static inline double LinearInterpolate(double x, double x0, double y0,
                                       double x1, double y1) {
  MOZ_ASSERT(x0 < x1);

  if (x < x0) {
    return y0;
  }

  if (x < x1) {
    return y0 + (y1 - y0) * ((x - x0) / (x1 - x0));
  }

  return y1;
}

}  // namespace gc
}  // namespace js

#endif  // gc_Scheduling_h
