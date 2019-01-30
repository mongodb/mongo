/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * GC Scheduling Overview
 * ======================
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
 *  bunch of JS::gcreason reasons in GCAPI.h. These fall into a few categories
 *  that generally coincide with one or more of the above factors.
 *
 *  Embedding reasons:
 *
 *   1) Do a GC now because the embedding knows something useful about the
 *      zone's memory retention state. These are gcreasons like LOAD_END,
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
 *      example, CC_WAITING is where the embedding requires the mark bits
 *      to be set correct. Also, EVICT_NURSERY where we need to work on the tenured
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
 *  11) Do an incremental, zonal GC with reason TOO_MUCH_MALLOC when we have
 *      malloced more than JSGC_MAX_MALLOC_BYTES in a zone since the last GC.
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
 *               10% of the current gcTriggerBytes worth of GC memory.
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

namespace js {
namespace gc {

enum TriggerKind
{
    NoTrigger = 0,
    IncrementalTrigger,
    NonIncrementalTrigger
};

/*
 * Encapsulates all of the GC tunables. These are effectively constant and
 * should only be modified by setParameter.
 */
class GCSchedulingTunables
{
    /*
     * JSGC_MAX_BYTES
     *
     * Maximum nominal heap before last ditch GC.
     */
    UnprotectedData<size_t> gcMaxBytes_;

    /*
     * JSGC_MAX_MALLOC_BYTES
     *
     * Initial malloc bytes threshold.
     */
    UnprotectedData<size_t> maxMallocBytes_;

    /*
     * JSGC_MAX_NURSERY_BYTES
     *
     * Maximum nursery size for each zone group.
     */
    ActiveThreadData<size_t> gcMaxNurseryBytes_;

    /*
     * JSGC_ALLOCATION_THRESHOLD
     *
     * The base value used to compute zone->threshold.gcTriggerBytes(). When
     * usage.gcBytes() surpasses threshold.gcTriggerBytes() for a zone, the
     * zone may be scheduled for a GC, depending on the exact circumstances.
     */
    ActiveThreadOrGCTaskData<size_t> gcZoneAllocThresholdBase_;

    /*
     * JSGC_ALLOCATION_THRESHOLD_FACTOR
     *
     * Fraction of threshold.gcBytes() which triggers an incremental GC.
     */
    UnprotectedData<double> allocThresholdFactor_;

    /*
     * JSGC_ALLOCATION_THRESHOLD_FACTOR_AVOID_INTERRUPT
     *
     * The same except when doing so would interrupt an already running GC.
     */
    UnprotectedData<double> allocThresholdFactorAvoidInterrupt_;

    /*
     * Number of bytes to allocate between incremental slices in GCs triggered
     * by the zone allocation threshold.
     *
     * This value does not have a JSGCParamKey parameter yet.
     */
    UnprotectedData<size_t> zoneAllocDelayBytes_;

    /*
     * JSGC_DYNAMIC_HEAP_GROWTH
     *
     * Totally disables |highFrequencyGC|, the HeapGrowthFactor, and other
     * tunables that make GC non-deterministic.
     */
    ActiveThreadData<bool> dynamicHeapGrowthEnabled_;

    /*
     * JSGC_HIGH_FREQUENCY_TIME_LIMIT
     *
     * We enter high-frequency mode if we GC a twice within this many
     * microseconds. This value is stored directly in microseconds.
     */
    ActiveThreadData<uint64_t> highFrequencyThresholdUsec_;

    /*
     * JSGC_HIGH_FREQUENCY_LOW_LIMIT
     * JSGC_HIGH_FREQUENCY_HIGH_LIMIT
     * JSGC_HIGH_FREQUENCY_HEAP_GROWTH_MAX
     * JSGC_HIGH_FREQUENCY_HEAP_GROWTH_MIN
     *
     * When in the |highFrequencyGC| mode, these parameterize the per-zone
     * "HeapGrowthFactor" computation.
     */
    ActiveThreadData<uint64_t> highFrequencyLowLimitBytes_;
    ActiveThreadData<uint64_t> highFrequencyHighLimitBytes_;
    ActiveThreadData<double> highFrequencyHeapGrowthMax_;
    ActiveThreadData<double> highFrequencyHeapGrowthMin_;

    /*
     * JSGC_LOW_FREQUENCY_HEAP_GROWTH
     *
     * When not in |highFrequencyGC| mode, this is the global (stored per-zone)
     * "HeapGrowthFactor".
     */
    ActiveThreadData<double> lowFrequencyHeapGrowth_;

    /*
     * JSGC_DYNAMIC_MARK_SLICE
     *
     * Doubles the length of IGC slices when in the |highFrequencyGC| mode.
     */
    ActiveThreadData<bool> dynamicMarkSliceEnabled_;

    /*
     * JSGC_MIN_EMPTY_CHUNK_COUNT
     * JSGC_MAX_EMPTY_CHUNK_COUNT
     *
     * Controls the number of empty chunks reserved for future allocation.
     */
    UnprotectedData<uint32_t> minEmptyChunkCount_;
    UnprotectedData<uint32_t> maxEmptyChunkCount_;

  public:
    GCSchedulingTunables();

    size_t gcMaxBytes() const { return gcMaxBytes_; }
    size_t maxMallocBytes() const { return maxMallocBytes_; }
    size_t gcMaxNurseryBytes() const { return gcMaxNurseryBytes_; }
    size_t gcZoneAllocThresholdBase() const { return gcZoneAllocThresholdBase_; }
    double allocThresholdFactor() const { return allocThresholdFactor_; }
    double allocThresholdFactorAvoidInterrupt() const { return allocThresholdFactorAvoidInterrupt_; }
    size_t zoneAllocDelayBytes() const { return zoneAllocDelayBytes_; }
    bool isDynamicHeapGrowthEnabled() const { return dynamicHeapGrowthEnabled_; }
    uint64_t highFrequencyThresholdUsec() const { return highFrequencyThresholdUsec_; }
    uint64_t highFrequencyLowLimitBytes() const { return highFrequencyLowLimitBytes_; }
    uint64_t highFrequencyHighLimitBytes() const { return highFrequencyHighLimitBytes_; }
    double highFrequencyHeapGrowthMax() const { return highFrequencyHeapGrowthMax_; }
    double highFrequencyHeapGrowthMin() const { return highFrequencyHeapGrowthMin_; }
    double lowFrequencyHeapGrowth() const { return lowFrequencyHeapGrowth_; }
    bool isDynamicMarkSliceEnabled() const { return dynamicMarkSliceEnabled_; }
    unsigned minEmptyChunkCount(const AutoLockGC&) const { return minEmptyChunkCount_; }
    unsigned maxEmptyChunkCount() const { return maxEmptyChunkCount_; }

    MOZ_MUST_USE bool setParameter(JSGCParamKey key, uint32_t value, const AutoLockGC& lock);
    void resetParameter(JSGCParamKey key, const AutoLockGC& lock);

    void setMaxMallocBytes(size_t value);

private:
    void setHighFrequencyLowLimit(uint64_t value);
    void setHighFrequencyHighLimit(uint64_t value);
    void setHighFrequencyHeapGrowthMin(double value);
    void setHighFrequencyHeapGrowthMax(double value);
    void setLowFrequencyHeapGrowth(double value);
    void setMinEmptyChunkCount(uint32_t value);
    void setMaxEmptyChunkCount(uint32_t value);
};

class GCSchedulingState
{
    /*
     * Influences how we schedule and run GC's in several subtle ways. The most
     * important factor is in how it controls the "HeapGrowthFactor". The
     * growth factor is a measure of how large (as a percentage of the last GC)
     * the heap is allowed to grow before we try to schedule another GC.
     */
    ActiveThreadData<bool> inHighFrequencyGCMode_;

  public:
    GCSchedulingState()
      : inHighFrequencyGCMode_(false)
    {}

    bool inHighFrequencyGCMode() const { return inHighFrequencyGCMode_; }

    void updateHighFrequencyMode(uint64_t lastGCTime, uint64_t currentTime,
                                 const GCSchedulingTunables& tunables) {
        inHighFrequencyGCMode_ =
            tunables.isDynamicHeapGrowthEnabled() && lastGCTime &&
            lastGCTime + tunables.highFrequencyThresholdUsec() > currentTime;
    }
};

class MemoryCounter
{
    // Bytes counter to measure memory pressure for GC scheduling. It counts
    // upwards from zero.
    mozilla::Atomic<size_t, mozilla::ReleaseAcquire> bytes_;

    // GC trigger threshold for memory allocations.
    size_t maxBytes_;

    // The counter value at the start of a GC.
    ActiveThreadData<size_t> bytesAtStartOfGC_;

    // Which kind of GC has been triggered if any.
    mozilla::Atomic<TriggerKind, mozilla::ReleaseAcquire> triggered_;

  public:
    MemoryCounter();

    size_t bytes() const { return bytes_; }
    size_t maxBytes() const { return maxBytes_; }
    TriggerKind triggered() const { return triggered_; }

    void setMax(size_t newMax, const AutoLockGC& lock);

    void update(size_t bytes) {
        bytes_ += bytes;
    }

    void adopt(MemoryCounter& other);

    TriggerKind shouldTriggerGC(const GCSchedulingTunables& tunables) const {
        if (MOZ_LIKELY(bytes_ < maxBytes_ * tunables.allocThresholdFactor()))
            return NoTrigger;

        if (bytes_ < maxBytes_)
            return IncrementalTrigger;

        return NonIncrementalTrigger;
    }

    bool shouldResetIncrementalGC(const GCSchedulingTunables& tunables) const {
        return bytes_ > maxBytes_ * tunables.allocThresholdFactorAvoidInterrupt();
    }

    void recordTrigger(TriggerKind trigger);

    void updateOnGCStart();
    void updateOnGCEnd(const GCSchedulingTunables& tunables, const AutoLockGC& lock);

  private:
    void reset();
};

// This class encapsulates the data that determines when we need to do a zone GC.
class ZoneHeapThreshold
{
    // The "growth factor" for computing our next thresholds after a GC.
    GCLockData<double> gcHeapGrowthFactor_;

    // GC trigger threshold for allocations on the GC heap.
    mozilla::Atomic<size_t, mozilla::Relaxed> gcTriggerBytes_;

  public:
    ZoneHeapThreshold()
      : gcHeapGrowthFactor_(3.0),
        gcTriggerBytes_(0)
    {}

    double gcHeapGrowthFactor() const { return gcHeapGrowthFactor_; }
    size_t gcTriggerBytes() const { return gcTriggerBytes_; }
    double eagerAllocTrigger(bool highFrequencyGC) const;

    void updateAfterGC(size_t lastBytes, JSGCInvocationKind gckind,
                       const GCSchedulingTunables& tunables, const GCSchedulingState& state,
                       const AutoLockGC& lock);
    void updateForRemovedArena(const GCSchedulingTunables& tunables);

  private:
    static double computeZoneHeapGrowthFactorForHeapSize(size_t lastBytes,
                                                         const GCSchedulingTunables& tunables,
                                                         const GCSchedulingState& state);
    static size_t computeZoneTriggerBytes(double growthFactor, size_t lastBytes,
                                          JSGCInvocationKind gckind,
                                          const GCSchedulingTunables& tunables,
                                          const AutoLockGC& lock);
};

} // namespace gc
} // namespace js

#endif // gc_Scheduling_h
