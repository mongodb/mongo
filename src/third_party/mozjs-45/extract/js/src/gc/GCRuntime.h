/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GCRuntime_h
#define gc_GCRuntime_h

#include "mozilla/Atomics.h"

#include "jsfriendapi.h"
#include "jsgc.h"

#include "gc/Heap.h"
#include "gc/Nursery.h"
#include "gc/Statistics.h"
#include "gc/StoreBuffer.h"
#include "gc/Tracer.h"

/* Perform validation of incremental marking in debug builds but not on B2G. */
#if defined(DEBUG) && !defined(MOZ_B2G)
#define JS_GC_MARKING_VALIDATION
#endif

namespace js {

class AutoLockGC;
class VerifyPreTracer;

namespace gc {

typedef Vector<JS::Zone*, 4, SystemAllocPolicy> ZoneVector;

class MarkingValidator;
class AutoTraceSession;
struct MovingTracer;

class ChunkPool
{
    Chunk* head_;
    size_t count_;

  public:
    ChunkPool() : head_(nullptr), count_(0) {}

    size_t count() const { return count_; }

    Chunk* head() { MOZ_ASSERT(head_); return head_; }
    Chunk* pop();
    void push(Chunk* chunk);
    Chunk* remove(Chunk* chunk);

#ifdef DEBUG
    bool contains(Chunk* chunk) const;
    bool verify() const;
#endif

    // Pool mutation does not invalidate an Iter unless the mutation
    // is of the Chunk currently being visited by the Iter.
    class Iter {
      public:
        explicit Iter(ChunkPool& pool) : current_(pool.head_) {}
        bool done() const { return !current_; }
        void next();
        Chunk* get() const { return current_; }
        operator Chunk*() const { return get(); }
        Chunk* operator->() const { return get(); }
      private:
        Chunk* current_;
    };
};

// Performs extra allocation off the main thread so that when memory is
// required on the main thread it will already be available and waiting.
class BackgroundAllocTask : public GCParallelTask
{
    // Guarded by the GC lock.
    JSRuntime* runtime;
    ChunkPool& chunkPool_;

    const bool enabled_;

  public:
    BackgroundAllocTask(JSRuntime* rt, ChunkPool& pool);
    bool enabled() const { return enabled_; }

  protected:
    virtual void run() override;
};

/*
 * Encapsulates all of the GC tunables. These are effectively constant and
 * should only be modified by setParameter.
 */
class GCSchedulingTunables
{
    /*
     * Soft limit on the number of bytes we are allowed to allocate in the GC
     * heap. Attempts to allocate gcthings over this limit will return null and
     * subsequently invoke the standard OOM machinery, independent of available
     * physical memory.
     */
    size_t gcMaxBytes_;

    /*
     * The base value used to compute zone->trigger.gcBytes(). When
     * usage.gcBytes() surpasses threshold.gcBytes() for a zone, the zone may
     * be scheduled for a GC, depending on the exact circumstances.
     */
    size_t gcZoneAllocThresholdBase_;

    /* Fraction of threshold.gcBytes() which triggers an incremental GC. */
    double zoneAllocThresholdFactor_;

    /*
     * Number of bytes to allocate between incremental slices in GCs triggered
     * by the zone allocation threshold.
     */
    size_t zoneAllocDelayBytes_;

    /*
     * Totally disables |highFrequencyGC|, the HeapGrowthFactor, and other
     * tunables that make GC non-deterministic.
     */
    bool dynamicHeapGrowthEnabled_;

    /*
     * We enter high-frequency mode if we GC a twice within this many
     * microseconds. This value is stored directly in microseconds.
     */
    uint64_t highFrequencyThresholdUsec_;

    /*
     * When in the |highFrequencyGC| mode, these parameterize the per-zone
     * "HeapGrowthFactor" computation.
     */
    uint64_t highFrequencyLowLimitBytes_;
    uint64_t highFrequencyHighLimitBytes_;
    double highFrequencyHeapGrowthMax_;
    double highFrequencyHeapGrowthMin_;

    /*
     * When not in |highFrequencyGC| mode, this is the global (stored per-zone)
     * "HeapGrowthFactor".
     */
    double lowFrequencyHeapGrowth_;

    /*
     * Doubles the length of IGC slices when in the |highFrequencyGC| mode.
     */
    bool dynamicMarkSliceEnabled_;

    /*
     * Controls the number of empty chunks reserved for future allocation.
     */
    unsigned minEmptyChunkCount_;
    unsigned maxEmptyChunkCount_;

  public:
    GCSchedulingTunables()
      : gcMaxBytes_(0),
        gcZoneAllocThresholdBase_(30 * 1024 * 1024),
        zoneAllocThresholdFactor_(0.9),
        zoneAllocDelayBytes_(1024 * 1024),
        dynamicHeapGrowthEnabled_(false),
        highFrequencyThresholdUsec_(1000 * 1000),
        highFrequencyLowLimitBytes_(100 * 1024 * 1024),
        highFrequencyHighLimitBytes_(500 * 1024 * 1024),
        highFrequencyHeapGrowthMax_(3.0),
        highFrequencyHeapGrowthMin_(1.5),
        lowFrequencyHeapGrowth_(1.5),
        dynamicMarkSliceEnabled_(false),
        minEmptyChunkCount_(1),
        maxEmptyChunkCount_(30)
    {}

    size_t gcMaxBytes() const { return gcMaxBytes_; }
    size_t gcZoneAllocThresholdBase() const { return gcZoneAllocThresholdBase_; }
    double zoneAllocThresholdFactor() const { return zoneAllocThresholdFactor_; }
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

    void setParameter(JSGCParamKey key, uint32_t value, const AutoLockGC& lock);
};

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
 *      PERIODIC_FULL_GC
 *      ----------------
 *      When we return to the event loop and it has been 20 seconds since we've
 *      done a GC, we start an incremenal, all-zones, shrinking GC.
 *
 *          Assumptions:
 *            -> Our triggers are incomplete.
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
 *      is, unfortunatly, not usefully able to augment our other GC heap
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
class GCSchedulingState
{
    /*
     * Influences how we schedule and run GC's in several subtle ways. The most
     * important factor is in how it controls the "HeapGrowthFactor". The
     * growth factor is a measure of how large (as a percentage of the last GC)
     * the heap is allowed to grow before we try to schedule another GC.
     */
    bool inHighFrequencyGCMode_;

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

template<typename F>
struct Callback {
    F op;
    void* data;

    Callback()
      : op(nullptr), data(nullptr)
    {}
    Callback(F op, void* data)
      : op(op), data(data)
    {}
};

template<typename F>
using CallbackVector = Vector<Callback<F>, 4, SystemAllocPolicy>;

template <typename T, typename Iter0, typename Iter1>
class ChainedIter
{
    Iter0 iter0_;
    Iter1 iter1_;

  public:
    ChainedIter(const Iter0& iter0, const Iter1& iter1)
      : iter0_(iter0), iter1_(iter1)
    {}

    bool done() const { return iter0_.done() && iter1_.done(); }
    void next() {
        MOZ_ASSERT(!done());
        if (!iter0_.done()) {
            iter0_.next();
        } else {
            MOZ_ASSERT(!iter1_.done());
            iter1_.next();
        }
    }
    T get() const {
        MOZ_ASSERT(!done());
        if (!iter0_.done())
            return iter0_.get();
        MOZ_ASSERT(!iter1_.done());
        return iter1_.get();
    }

    operator T() const { return get(); }
    T operator->() const { return get(); }
};

typedef js::HashMap<Value*,
                    const char*,
                    js::DefaultHasher<Value*>,
                    js::SystemAllocPolicy> RootedValueMap;

class GCRuntime
{
  public:
    explicit GCRuntime(JSRuntime* rt);
    bool init(uint32_t maxbytes, uint32_t maxNurseryBytes);
    void finishRoots();
    void finish();

    inline int zeal();
    inline bool upcomingZealousGC();
    inline bool needZealousGC();

    bool addRoot(Value* vp, const char* name);
    void removeRoot(Value* vp);
    void setMarkStackLimit(size_t limit, AutoLockGC& lock);

    void setParameter(JSGCParamKey key, uint32_t value, AutoLockGC& lock);
    uint32_t getParameter(JSGCParamKey key, const AutoLockGC& lock);

    bool triggerGC(JS::gcreason::Reason reason);
    void maybeAllocTriggerZoneGC(Zone* zone, const AutoLockGC& lock);
    bool triggerZoneGC(Zone* zone, JS::gcreason::Reason reason);
    bool maybeGC(Zone* zone);
    void maybePeriodicFullGC();
    void minorGC(JS::gcreason::Reason reason) {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_MINOR_GC);
        minorGCImpl(reason, nullptr);
    }
    void minorGC(JSContext* cx, JS::gcreason::Reason reason);
    void evictNursery(JS::gcreason::Reason reason = JS::gcreason::EVICT_NURSERY) {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_EVICT_NURSERY);
        minorGCImpl(reason, nullptr);
    }
    void clearPostBarrierCallbacks();
    bool gcIfRequested(JSContext* cx = nullptr);
    void gc(JSGCInvocationKind gckind, JS::gcreason::Reason reason);
    void startGC(JSGCInvocationKind gckind, JS::gcreason::Reason reason, int64_t millis = 0);
    void gcSlice(JS::gcreason::Reason reason, int64_t millis = 0);
    void finishGC(JS::gcreason::Reason reason);
    void abortGC();
    void startDebugGC(JSGCInvocationKind gckind, SliceBudget& budget);
    void debugGCSlice(SliceBudget& budget);

    void triggerFullGCForAtoms() {
        MOZ_ASSERT(fullGCForAtomsRequested_);
        fullGCForAtomsRequested_ = false;
        triggerGC(JS::gcreason::ALLOC_TRIGGER);
    }

    void runDebugGC();
    inline void poke();

    enum TraceOrMarkRuntime {
        TraceRuntime,
        MarkRuntime
    };
    void markRuntime(JSTracer* trc, TraceOrMarkRuntime traceOrMark = TraceRuntime);

    void notifyDidPaint();
    void shrinkBuffers();
    void onOutOfMallocMemory();
    void onOutOfMallocMemory(const AutoLockGC& lock);

#ifdef JS_GC_ZEAL
    const void* addressOfZealMode() { return &zealMode; }
    void getZeal(uint8_t* zeal, uint32_t* frequency, uint32_t* nextScheduled);
    void setZeal(uint8_t zeal, uint32_t frequency);
    bool parseAndSetZeal(const char* str);
    void setNextScheduled(uint32_t count);
    void verifyPreBarriers();
    void maybeVerifyPreBarriers(bool always);
    bool selectForMarking(JSObject* object);
    void clearSelectedForMarking();
    void setDeterministic(bool enable);
#endif

    size_t maxMallocBytesAllocated() { return maxMallocBytes; }

    uint64_t nextCellUniqueId() {
        MOZ_ASSERT(nextCellUniqueId_ > 0);
        return nextCellUniqueId_++;
    }

  public:
    // Internal public interface
    js::gc::State state() const { return incrementalState; }
    bool isHeapCompacting() const { return state() == COMPACT; }
    bool isForegroundSweeping() const { return state() == SWEEP; }
    bool isBackgroundSweeping() { return helperState.isBackgroundSweeping(); }
    void waitBackgroundSweepEnd() { helperState.waitBackgroundSweepEnd(); }
    void waitBackgroundSweepOrAllocEnd() {
        helperState.waitBackgroundSweepEnd();
        allocTask.cancel(GCParallelTask::CancelAndWait);
    }

    void requestMinorGC(JS::gcreason::Reason reason);

#ifdef DEBUG

    bool onBackgroundThread() { return helperState.onBackgroundThread(); }

    bool currentThreadOwnsGCLock() {
        return lockOwner.value == PR_GetCurrentThread();
    }

#endif // DEBUG

    void assertCanLock() {
        MOZ_ASSERT(!currentThreadOwnsGCLock());
    }

    void lockGC() {
        PR_Lock(lock);
#ifdef DEBUG
        MOZ_ASSERT(!lockOwner.value);
        lockOwner.value = PR_GetCurrentThread();
#endif
    }

    void unlockGC() {
#ifdef DEBUG
        MOZ_ASSERT(lockOwner.value == PR_GetCurrentThread());
        lockOwner.value = nullptr;
#endif
        PR_Unlock(lock);
    }

#ifdef DEBUG
    bool isAllocAllowed() { return noGCOrAllocationCheck == 0; }
    void disallowAlloc() { ++noGCOrAllocationCheck; }
    void allowAlloc() {
        MOZ_ASSERT(!isAllocAllowed());
        --noGCOrAllocationCheck;
    }

    bool isInsideUnsafeRegion() { return inUnsafeRegion != 0; }
    void enterUnsafeRegion() { ++inUnsafeRegion; }
    void leaveUnsafeRegion() {
        MOZ_ASSERT(inUnsafeRegion > 0);
        --inUnsafeRegion;
    }

    bool isStrictProxyCheckingEnabled() { return disableStrictProxyCheckingCount == 0; }
    void disableStrictProxyChecking() { ++disableStrictProxyCheckingCount; }
    void enableStrictProxyChecking() {
        MOZ_ASSERT(disableStrictProxyCheckingCount > 0);
        --disableStrictProxyCheckingCount;
    }
#endif

    void setAlwaysPreserveCode() { alwaysPreserveCode = true; }

    bool isIncrementalGCAllowed() const { return incrementalAllowed; }
    void disallowIncrementalGC() { incrementalAllowed = false; }

    bool isIncrementalGCEnabled() const { return mode == JSGC_MODE_INCREMENTAL && incrementalAllowed; }
    bool isIncrementalGCInProgress() const { return state() != gc::NO_INCREMENTAL; }

    bool isGenerationalGCEnabled() const { return generationalDisabled == 0; }
    void disableGenerationalGC();
    void enableGenerationalGC();

    void disableCompactingGC();
    void enableCompactingGC();
    bool isCompactingGCEnabled() const;

    void setGrayRootsTracer(JSTraceDataOp traceOp, void* data);
    bool addBlackRootsTracer(JSTraceDataOp traceOp, void* data);
    void removeBlackRootsTracer(JSTraceDataOp traceOp, void* data);

    void setMaxMallocBytes(size_t value);
    int32_t getMallocBytes() const { return mallocBytesUntilGC; }
    void resetMallocBytes();
    bool isTooMuchMalloc() const { return mallocBytesUntilGC <= 0; }
    void updateMallocCounter(JS::Zone* zone, size_t nbytes);
    void onTooMuchMalloc();

    void setGCCallback(JSGCCallback callback, void* data);
    void callGCCallback(JSGCStatus status) const;
    bool addFinalizeCallback(JSFinalizeCallback callback, void* data);
    void removeFinalizeCallback(JSFinalizeCallback func);
    bool addWeakPointerZoneGroupCallback(JSWeakPointerZoneGroupCallback callback, void* data);
    void removeWeakPointerZoneGroupCallback(JSWeakPointerZoneGroupCallback callback);
    bool addWeakPointerCompartmentCallback(JSWeakPointerCompartmentCallback callback, void* data);
    void removeWeakPointerCompartmentCallback(JSWeakPointerCompartmentCallback callback);
    JS::GCSliceCallback setSliceCallback(JS::GCSliceCallback callback);

    void setValidate(bool enable);
    void setFullCompartmentChecks(bool enable);

    bool isManipulatingDeadZones() { return manipulatingDeadZones; }
    void setManipulatingDeadZones(bool value) { manipulatingDeadZones = value; }
    unsigned objectsMarkedInDeadZonesCount() { return objectsMarkedInDeadZones; }
    void incObjectsMarkedInDeadZone() {
        MOZ_ASSERT(manipulatingDeadZones);
        ++objectsMarkedInDeadZones;
    }

    JS::Zone* getCurrentZoneGroup() { return currentZoneGroup; }
    void setFoundBlackGrayEdges() { foundBlackGrayEdges = true; }

    uint64_t gcNumber() const { return number; }

    uint64_t minorGCCount() const { return minorGCNumber; }
    void incMinorGcNumber() { ++minorGCNumber; ++number; }

    uint64_t majorGCCount() const { return majorGCNumber; }
    void incMajorGcNumber() { ++majorGCNumber; ++number; }

    int64_t defaultSliceBudget() const { return defaultTimeBudget_; }

    bool isIncrementalGc() const { return isIncremental; }
    bool isFullGc() const { return isFull; }

    bool shouldCleanUpEverything() { return cleanUpEverything; }

    bool areGrayBitsValid() const { return grayBitsValid; }
    void setGrayBitsInvalid() { grayBitsValid = false; }

    bool minorGCRequested() const { return minorGCTriggerReason != JS::gcreason::NO_REASON; }
    bool majorGCRequested() const { return majorGCTriggerReason != JS::gcreason::NO_REASON; }
    bool isGcNeeded() { return minorGCRequested() || majorGCRequested(); }

    bool fullGCForAtomsRequested() const { return fullGCForAtomsRequested_; }

    double computeHeapGrowthFactor(size_t lastBytes);
    size_t computeTriggerBytes(double growthFactor, size_t lastBytes);

    JSGCMode gcMode() const { return mode; }
    void setGCMode(JSGCMode m) {
        mode = m;
        marker.setGCMode(mode);
    }

    inline void updateOnFreeArenaAlloc(const ChunkInfo& info);
    inline void updateOnArenaFree(const ChunkInfo& info);

    ChunkPool& fullChunks(const AutoLockGC& lock) { return fullChunks_; }
    ChunkPool& availableChunks(const AutoLockGC& lock) { return availableChunks_; }
    ChunkPool& emptyChunks(const AutoLockGC& lock) { return emptyChunks_; }
    const ChunkPool& fullChunks(const AutoLockGC& lock) const { return fullChunks_; }
    const ChunkPool& availableChunks(const AutoLockGC& lock) const { return availableChunks_; }
    const ChunkPool& emptyChunks(const AutoLockGC& lock) const { return emptyChunks_; }
    typedef ChainedIter<Chunk*, ChunkPool::Iter, ChunkPool::Iter> NonEmptyChunksIter;
    NonEmptyChunksIter allNonEmptyChunks() {
        return NonEmptyChunksIter(ChunkPool::Iter(availableChunks_), ChunkPool::Iter(fullChunks_));
    }

#ifdef JS_GC_ZEAL
    void startVerifyPreBarriers();
    bool endVerifyPreBarriers();
    void finishVerifier();
    bool isVerifyPreBarriersEnabled() const { return !!verifyPreData; }
#else
    bool isVerifyPreBarriersEnabled() const { return false; }
#endif

    // Free certain LifoAlloc blocks from the background sweep thread.
    void freeUnusedLifoBlocksAfterSweeping(LifoAlloc* lifo);
    void freeAllLifoBlocksAfterSweeping(LifoAlloc* lifo);

    // Public here for ReleaseArenaLists and FinalizeTypedArenas.
    void releaseArena(ArenaHeader* aheader, const AutoLockGC& lock);

    void releaseHeldRelocatedArenas();
    void releaseHeldRelocatedArenasWithoutUnlocking(const AutoLockGC& lock);

    // Allocator
    template <AllowGC allowGC>
    bool checkAllocatorState(JSContext* cx, AllocKind kind);
    template <AllowGC allowGC>
    JSObject* tryNewNurseryObject(JSContext* cx, size_t thingSize, size_t nDynamicSlots,
                                  const Class* clasp);
    template <AllowGC allowGC>
    static JSObject* tryNewTenuredObject(ExclusiveContext* cx, AllocKind kind, size_t thingSize,
                                         size_t nDynamicSlots);
    template <typename T, AllowGC allowGC>
    static T* tryNewTenuredThing(ExclusiveContext* cx, AllocKind kind, size_t thingSize);
    static void* refillFreeListInGC(Zone* zone, AllocKind thingKind);

  private:
    enum IncrementalProgress
    {
        NotFinished = 0,
        Finished
    };

    void minorGCImpl(JS::gcreason::Reason reason, Nursery::ObjectGroupList* pretenureGroups);

    // For ArenaLists::allocateFromArena()
    friend class ArenaLists;
    Chunk* pickChunk(const AutoLockGC& lock,
                     AutoMaybeStartBackgroundAllocation& maybeStartBGAlloc);
    ArenaHeader* allocateArena(Chunk* chunk, Zone* zone, AllocKind kind, const AutoLockGC& lock);
    void arenaAllocatedDuringGC(JS::Zone* zone, ArenaHeader* arena);

    // Allocator internals
    bool gcIfNeededPerAllocation(JSContext* cx);
    template <typename T>
    static void checkIncrementalZoneState(ExclusiveContext* cx, T* t);
    static void* refillFreeListFromAnyThread(ExclusiveContext* cx, AllocKind thingKind,
                                             size_t thingSize);
    static void* refillFreeListFromMainThread(JSContext* cx, AllocKind thingKind,
                                              size_t thingSize);
    static void* refillFreeListOffMainThread(ExclusiveContext* cx, AllocKind thingKind);

    /*
     * Return the list of chunks that can be released outside the GC lock.
     * Must be called either during the GC or with the GC lock taken.
     */
    ChunkPool expireEmptyChunkPool(bool shrinkBuffers, const AutoLockGC& lock);
    void freeEmptyChunks(JSRuntime* rt, const AutoLockGC& lock);
    void prepareToFreeChunk(ChunkInfo& info);

    friend class BackgroundAllocTask;
    friend class AutoMaybeStartBackgroundAllocation;
    inline bool wantBackgroundAllocation(const AutoLockGC& lock) const;
    void startBackgroundAllocTaskIfIdle();

    void requestMajorGC(JS::gcreason::Reason reason);
    SliceBudget defaultBudget(JS::gcreason::Reason reason, int64_t millis);
    void budgetIncrementalGC(SliceBudget& budget);
    void resetIncrementalGC(const char* reason);

    // Assert if the system state is such that we should never
    // receive a request to do GC work.
    void checkCanCallAPI();

    // Check if the system state is such that GC has been supressed
    // or otherwise delayed.
    bool checkIfGCAllowedInCurrentState(JS::gcreason::Reason reason);

    gcstats::ZoneGCStats scanZonesBeforeGC();
    void collect(bool nonincrementalByAPI, SliceBudget budget, JS::gcreason::Reason reason);
    bool gcCycle(bool nonincrementalByAPI, SliceBudget& budget, JS::gcreason::Reason reason);
    void incrementalCollectSlice(SliceBudget& budget, JS::gcreason::Reason reason);

    void pushZealSelectedObjects();
    void purgeRuntime();
    bool beginMarkPhase(JS::gcreason::Reason reason);
    bool shouldPreserveJITCode(JSCompartment* comp, int64_t currentTime,
                               JS::gcreason::Reason reason);
    void bufferGrayRoots();
    void markCompartments();
    IncrementalProgress drainMarkStack(SliceBudget& sliceBudget, gcstats::Phase phase);
    template <class CompartmentIterT> void markWeakReferences(gcstats::Phase phase);
    void markWeakReferencesInCurrentGroup(gcstats::Phase phase);
    template <class ZoneIterT, class CompartmentIterT> void markGrayReferences(gcstats::Phase phase);
    void markBufferedGrayRoots(JS::Zone* zone);
    void markGrayReferencesInCurrentGroup(gcstats::Phase phase);
    void markAllWeakReferences(gcstats::Phase phase);
    void markAllGrayReferences(gcstats::Phase phase);

    void beginSweepPhase(bool lastGC);
    void findZoneGroups();
    bool findZoneEdgesForWeakMaps();
    void getNextZoneGroup();
    void endMarkingZoneGroup();
    void beginSweepingZoneGroup();
    bool shouldReleaseObservedTypes();
    void endSweepingZoneGroup();
    IncrementalProgress sweepPhase(SliceBudget& sliceBudget);
    void endSweepPhase(bool lastGC);
    void sweepZones(FreeOp* fop, bool lastGC);
    void decommitAllWithoutUnlocking(const AutoLockGC& lock);
    void decommitArenas(AutoLockGC& lock);
    void expireChunksAndArenas(bool shouldShrink, AutoLockGC& lock);
    void queueZonesForBackgroundSweep(ZoneList& zones);
    void sweepBackgroundThings(ZoneList& zones, LifoAlloc& freeBlocks, ThreadType threadType);
    void assertBackgroundSweepingFinished();
    bool shouldCompact();
    IncrementalProgress beginCompactPhase();
    IncrementalProgress compactPhase(JS::gcreason::Reason reason, SliceBudget& sliceBudget);
    void endCompactPhase(JS::gcreason::Reason reason);
    void sweepTypesAfterCompacting(Zone* zone);
    void sweepZoneAfterCompacting(Zone* zone);
    bool relocateArenas(Zone* zone, JS::gcreason::Reason reason, ArenaHeader*& relocatedListOut,
                        SliceBudget& sliceBudget);
    void updateAllCellPointersParallel(MovingTracer* trc, Zone* zone);
    void updateAllCellPointersSerial(MovingTracer* trc, Zone* zone);
    void updatePointersToRelocatedCells(Zone* zone);
    void protectAndHoldArenas(ArenaHeader* arenaList);
    void unprotectHeldRelocatedArenas();
    void releaseRelocatedArenas(ArenaHeader* arenaList);
    void releaseRelocatedArenasWithoutUnlocking(ArenaHeader* arenaList, const AutoLockGC& lock);
    void finishCollection(JS::gcreason::Reason reason);

    void computeNonIncrementalMarkingForValidation();
    void validateIncrementalMarking();
    void finishMarkingValidation();

#ifdef DEBUG
    void checkForCompartmentMismatches();
#endif

    void callFinalizeCallbacks(FreeOp* fop, JSFinalizeStatus status) const;
    void callWeakPointerZoneGroupCallbacks() const;
    void callWeakPointerCompartmentCallbacks(JSCompartment* comp) const;

  public:
    JSRuntime* rt;

    /* Embedders can use this zone however they wish. */
    JS::Zone* systemZone;

    /* List of compartments and zones (protected by the GC lock). */
    js::gc::ZoneVector zones;

    js::Nursery nursery;
    js::gc::StoreBuffer storeBuffer;

    js::gcstats::Statistics stats;

    js::GCMarker marker;

    /* Track heap usage for this runtime. */
    HeapUsage usage;

    /* GC scheduling state and parameters. */
    GCSchedulingTunables tunables;
    GCSchedulingState schedulingState;

    MemProfiler mMemProfiler;

  private:
    // When empty, chunks reside in the emptyChunks pool and are re-used as
    // needed or eventually expired if not re-used. The emptyChunks pool gets
    // refilled from the background allocation task heuristically so that empty
    // chunks should always available for immediate allocation without syscalls.
    ChunkPool             emptyChunks_;

    // Chunks which have had some, but not all, of their arenas allocated live
    // in the available chunk lists. When all available arenas in a chunk have
    // been allocated, the chunk is removed from the available list and moved
    // to the fullChunks pool. During a GC, if all arenas are free, the chunk
    // is moved back to the emptyChunks pool and scheduled for eventual
    // release.
    ChunkPool             availableChunks_;

    // When all arenas in a chunk are used, it is moved to the fullChunks pool
    // so as to reduce the cost of operations on the available lists.
    ChunkPool             fullChunks_;

    RootedValueMap rootsHash;

    size_t maxMallocBytes;

    // An incrementing id used to assign unique ids to cells that require one.
    uint64_t nextCellUniqueId_;

    /*
     * Number of the committed arenas in all GC chunks including empty chunks.
     */
    mozilla::Atomic<uint32_t, mozilla::ReleaseAcquire> numArenasFreeCommitted;
    VerifyPreTracer* verifyPreData;
    bool chunkAllocationSinceLastGC;
    int64_t nextFullGCTime;
    int64_t lastGCTime;

    JSGCMode mode;

    mozilla::Atomic<size_t, mozilla::ReleaseAcquire> numActiveZoneIters;

    uint64_t decommitThreshold;

    /* During shutdown, the GC needs to clean up every possible object. */
    bool cleanUpEverything;

    // Gray marking must be done after all black marking is complete. However,
    // we do not have write barriers on XPConnect roots. Therefore, XPConnect
    // roots must be accumulated in the first slice of incremental GC. We
    // accumulate these roots in each zone's gcGrayRoots vector and then mark
    // them later, after black marking is complete for each compartment. This
    // accumulation can fail, but in that case we switch to non-incremental GC.
    enum class GrayBufferState {
        Unused,
        Okay,
        Failed
    };
    GrayBufferState grayBufferState;
    bool hasBufferedGrayRoots() const { return grayBufferState == GrayBufferState::Okay; }

    // Clear each zone's gray buffers, but do not change the current state.
    void resetBufferedGrayRoots() const;

    // Reset the gray buffering state to Unused.
    void clearBufferedGrayRoots() {
        grayBufferState = GrayBufferState::Unused;
        resetBufferedGrayRoots();
    }

    /*
     * The gray bits can become invalid if UnmarkGray overflows the stack. A
     * full GC will reset this bit, since it fills in all the gray bits.
     */
    bool grayBitsValid;

    mozilla::Atomic<JS::gcreason::Reason, mozilla::Relaxed> majorGCTriggerReason;

    JS::gcreason::Reason minorGCTriggerReason;

    /* Perform full GC if rt->keepAtoms() becomes false. */
    bool fullGCForAtomsRequested_;

    /* Incremented at the start of every minor GC. */
    uint64_t minorGCNumber;

    /* Incremented at the start of every major GC. */
    uint64_t majorGCNumber;

    /* The major GC number at which to release observed type information. */
    uint64_t jitReleaseNumber;

    /* Incremented on every GC slice. */
    uint64_t number;

    /* The number at the time of the most recent GC's first slice. */
    uint64_t startNumber;

    /* Whether the currently running GC can finish in multiple slices. */
    bool isIncremental;

    /* Whether all compartments are being collected in first GC slice. */
    bool isFull;

    /* Whether the heap will be compacted at the end of GC. */
    bool isCompacting;

    /* The invocation kind of the current GC, taken from the first slice. */
    JSGCInvocationKind invocationKind;

    /* The initial GC reason, taken from the first slice. */
    JS::gcreason::Reason initialReason;

    /*
     * If this is 0, all cross-compartment proxies must be registered in the
     * wrapper map. This checking must be disabled temporarily while creating
     * new wrappers. When non-zero, this records the recursion depth of wrapper
     * creation.
     */
    mozilla::DebugOnly<uintptr_t> disableStrictProxyCheckingCount;

    /*
     * The current incremental GC phase. This is also used internally in
     * non-incremental GC.
     */
    js::gc::State incrementalState;

    /* Indicates that the last incremental slice exhausted the mark stack. */
    bool lastMarkSlice;

    /* Whether any sweeping will take place in the separate GC helper thread. */
    bool sweepOnBackgroundThread;

    /* Whether observed type information is being released in the current GC. */
    bool releaseObservedTypes;

    /* Whether any black->gray edges were found during marking. */
    bool foundBlackGrayEdges;

    /* Singly linekd list of zones to be swept in the background. */
    ZoneList backgroundSweepZones;

    /*
     * Free LIFO blocks are transferred to this allocator before being freed on
     * the background GC thread.
     */
    js::LifoAlloc freeLifoAlloc;

    /* Index of current zone group (for stats). */
    unsigned zoneGroupIndex;

    /*
     * Incremental sweep state.
     */
    JS::Zone* zoneGroups;
    JS::Zone* currentZoneGroup;
    bool sweepingTypes;
    unsigned finalizePhase;
    JS::Zone* sweepZone;
    unsigned sweepKindIndex;
    bool abortSweepAfterCurrentGroup;

    /*
     * Concurrent sweep infrastructure.
     */
    void startTask(GCParallelTask& task, gcstats::Phase phase);
    void joinTask(GCParallelTask& task, gcstats::Phase phase);

    /*
     * List head of arenas allocated during the sweep phase.
     */
    js::gc::ArenaHeader* arenasAllocatedDuringSweep;

    /*
     * Incremental compacting state.
     */
    bool startedCompacting;
    js::gc::ZoneList zonesToMaybeCompact;
    ArenaHeader* relocatedArenasToRelease;

#ifdef JS_GC_MARKING_VALIDATION
    js::gc::MarkingValidator* markingValidator;
#endif

    /*
     * Indicates that a GC slice has taken place in the middle of an animation
     * frame, rather than at the beginning. In this case, the next slice will be
     * delayed so that we don't get back-to-back slices.
     */
    bool interFrameGC;

    /* Default budget for incremental GC slice. See js/SliceBudget.h. */
    int64_t defaultTimeBudget_;

    /*
     * We disable incremental GC if we encounter a js::Class with a trace hook
     * that does not implement write barriers.
     */
    bool incrementalAllowed;

    /*
     * GGC can be enabled from the command line while testing.
     */
    unsigned generationalDisabled;

    /*
     * Whether compacting GC can is enabled globally.
     */
    bool compactingEnabled;

    /*
     * Some code cannot tolerate compacting GC so it can be disabled temporarily
     * with AutoDisableCompactingGC which uses this counter.
     */
    unsigned compactingDisabledCount;

    /*
     * This is true if we are in the middle of a brain transplant (e.g.,
     * JS_TransplantObject) or some other operation that can manipulate
     * dead zones.
     */
    bool manipulatingDeadZones;

    /*
     * This field is incremented each time we mark an object inside a
     * zone with no incoming cross-compartment pointers. Typically if
     * this happens it signals that an incremental GC is marking too much
     * stuff. At various times we check this counter and, if it has changed, we
     * run an immediate, non-incremental GC to clean up the dead
     * zones. This should happen very rarely.
     */
    unsigned objectsMarkedInDeadZones;

    bool poked;

    /*
     * These options control the zealousness of the GC. The fundamental values
     * are nextScheduled and gcDebugCompartmentGC. At every allocation,
     * nextScheduled is decremented. When it reaches zero, we do either a full
     * or a compartmental GC, based on debugCompartmentGC.
     *
     * At this point, if zeal_ is one of the types that trigger periodic
     * collection, then nextScheduled is reset to the value of zealFrequency.
     * Otherwise, no additional GCs take place.
     *
     * You can control these values in several ways:
     *   - Set the JS_GC_ZEAL environment variable
     *   - Call gczeal() or schedulegc() from inside shell-executed JS code
     *     (see the help for details)
     *
     * If gcZeal_ == 1 then we perform GCs in select places (during MaybeGC and
     * whenever a GC poke happens). This option is mainly useful to embedders.
     *
     * We use zeal_ == 4 to enable write barrier verification. See the comment
     * in jsgc.cpp for more information about this.
     *
     * zeal_ values from 8 to 10 periodically run different types of
     * incremental GC.
     *
     * zeal_ value 14 performs periodic shrinking collections.
     */
#ifdef JS_GC_ZEAL
    int zealMode;
    int zealFrequency;
    int nextScheduled;
    bool deterministicOnly;
    int incrementalLimit;

    js::Vector<JSObject*, 0, js::SystemAllocPolicy> selectedForMarking;
#endif

    bool validate;
    bool fullCompartmentChecks;

    Callback<JSGCCallback> gcCallback;
    CallbackVector<JSFinalizeCallback> finalizeCallbacks;
    CallbackVector<JSWeakPointerZoneGroupCallback> updateWeakPointerZoneGroupCallbacks;
    CallbackVector<JSWeakPointerCompartmentCallback> updateWeakPointerCompartmentCallbacks;

    /*
     * Malloc counter to measure memory pressure for GC scheduling. It runs
     * from maxMallocBytes down to zero.
     */
    mozilla::Atomic<ptrdiff_t, mozilla::ReleaseAcquire> mallocBytesUntilGC;

    /*
     * Whether a GC has been triggered as a result of mallocBytesUntilGC
     * falling below zero.
     */
    mozilla::Atomic<bool, mozilla::ReleaseAcquire> mallocGCTriggered;

    /*
     * The trace operations to trace embedding-specific GC roots. One is for
     * tracing through black roots and the other is for tracing through gray
     * roots. The black/gray distinction is only relevant to the cycle
     * collector.
     */
    CallbackVector<JSTraceDataOp> blackRootTracers;
    Callback<JSTraceDataOp> grayRootTracer;

    /* Always preserve JIT code during GCs, for testing. */
    bool alwaysPreserveCode;

#ifdef DEBUG
    /*
     * Some regions of code are hard for the static rooting hazard analysis to
     * understand. In those cases, we trade the static analysis for a dynamic
     * analysis. When this is non-zero, we should assert if we trigger, or
     * might trigger, a GC.
     */
    int inUnsafeRegion;

    size_t noGCOrAllocationCheck;
#endif

    /* Synchronize GC heap access between main thread and GCHelperState. */
    PRLock* lock;
    mozilla::DebugOnly<mozilla::Atomic<PRThread*>> lockOwner;

    BackgroundAllocTask allocTask;
    GCHelperState helperState;

    /*
     * During incremental sweeping, this field temporarily holds the arenas of
     * the current AllocKind being swept in order of increasing free space.
     */
    SortedArenaList incrementalSweepList;

    friend class js::GCHelperState;
    friend class js::gc::MarkingValidator;
    friend class js::gc::AutoTraceSession;
    friend class AutoEnterIteration;
};

/* Prevent compartments and zones from being collected during iteration. */
class MOZ_RAII AutoEnterIteration {
    GCRuntime* gc;

  public:
    explicit AutoEnterIteration(GCRuntime* gc_) : gc(gc_) {
        ++gc->numActiveZoneIters;
    }

    ~AutoEnterIteration() {
        MOZ_ASSERT(gc->numActiveZoneIters);
        --gc->numActiveZoneIters;
    }
};

#ifdef JS_GC_ZEAL
inline int
GCRuntime::zeal() {
    return zealMode;
}

inline bool
GCRuntime::upcomingZealousGC() {
    return nextScheduled == 1;
}

inline bool
GCRuntime::needZealousGC() {
    if (nextScheduled > 0 && --nextScheduled == 0) {
        if (zealMode == ZealAllocValue ||
            zealMode == ZealGenerationalGCValue ||
            (zealMode >= ZealIncrementalRootsThenFinish &&
             zealMode <= ZealIncrementalMultipleSlices) ||
            zealMode == ZealCompactValue)
        {
            nextScheduled = zealFrequency;
        }
        return true;
    }
    return false;
}
#else
inline int GCRuntime::zeal() { return 0; }
inline bool GCRuntime::upcomingZealousGC() { return false; }
inline bool GCRuntime::needZealousGC() { return false; }
#endif

} /* namespace gc */
} /* namespace js */

#endif
