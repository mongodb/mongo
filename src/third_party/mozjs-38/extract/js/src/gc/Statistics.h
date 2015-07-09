/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Statistics_h
#define gc_Statistics_h

#include "mozilla/DebugOnly.h"
#include "mozilla/PodOperations.h"
#include "mozilla/UniquePtr.h"

#include "jsalloc.h"
#include "jsgc.h"
#include "jspubtd.h"

#include "js/GCAPI.h"
#include "js/Vector.h"

struct JSCompartment;

namespace js {

class GCParallelTask;

namespace gcstats {

enum Phase {
    PHASE_MUTATOR,
    PHASE_GC_BEGIN,
    PHASE_WAIT_BACKGROUND_THREAD,
    PHASE_MARK_DISCARD_CODE,
    PHASE_PURGE,
    PHASE_MARK,
    PHASE_UNMARK,
    PHASE_MARK_DELAYED,
    PHASE_SWEEP,
    PHASE_SWEEP_MARK,
    PHASE_SWEEP_MARK_TYPES,
    PHASE_SWEEP_MARK_INCOMING_BLACK,
    PHASE_SWEEP_MARK_WEAK,
    PHASE_SWEEP_MARK_INCOMING_GRAY,
    PHASE_SWEEP_MARK_GRAY,
    PHASE_SWEEP_MARK_GRAY_WEAK,
    PHASE_FINALIZE_START,
    PHASE_SWEEP_ATOMS,
    PHASE_SWEEP_SYMBOL_REGISTRY,
    PHASE_SWEEP_COMPARTMENTS,
    PHASE_SWEEP_DISCARD_CODE,
    PHASE_SWEEP_INNER_VIEWS,
    PHASE_SWEEP_CC_WRAPPER,
    PHASE_SWEEP_BASE_SHAPE,
    PHASE_SWEEP_INITIAL_SHAPE,
    PHASE_SWEEP_TYPE_OBJECT,
    PHASE_SWEEP_BREAKPOINT,
    PHASE_SWEEP_REGEXP,
    PHASE_SWEEP_MISC,
    PHASE_SWEEP_TYPES,
    PHASE_SWEEP_TYPES_BEGIN,
    PHASE_SWEEP_TYPES_END,
    PHASE_SWEEP_OBJECT,
    PHASE_SWEEP_STRING,
    PHASE_SWEEP_SCRIPT,
    PHASE_SWEEP_SHAPE,
    PHASE_SWEEP_JITCODE,
    PHASE_FINALIZE_END,
    PHASE_DESTROY,
    PHASE_COMPACT,
    PHASE_COMPACT_MOVE,
    PHASE_COMPACT_UPDATE,
    PHASE_COMPACT_UPDATE_CELLS,
    PHASE_GC_END,
    PHASE_MINOR_GC,
    PHASE_EVICT_NURSERY,
    PHASE_TRACE_HEAP,
    PHASE_MARK_ROOTS,
    PHASE_MARK_CCWS,
    PHASE_MARK_ROOTERS,
    PHASE_MARK_RUNTIME_DATA,
    PHASE_MARK_EMBEDDING,
    PHASE_MARK_COMPARTMENTS,

    PHASE_LIMIT,
    PHASE_NONE = PHASE_LIMIT,
    PHASE_MULTI_PARENTS
};

enum Stat {
    STAT_NEW_CHUNK,
    STAT_DESTROY_CHUNK,
    STAT_MINOR_GC,

    // Number of times a 'put' into a storebuffer overflowed, triggering a
    // compaction
    STAT_STOREBUFFER_OVERFLOW,

    // Number of arenas relocated by compacting GC.
    STAT_ARENA_RELOCATED,

    STAT_LIMIT
};

class StatisticsSerializer;

struct ZoneGCStats
{
    /* Number of zones collected in this GC. */
    int collectedZoneCount;

    /* Total number of zones in the Runtime at the start of this GC. */
    int zoneCount;

    /* Total number of comaprtments in all zones collected. */
    int collectedCompartmentCount;

    /* Total number of compartments in the Runtime at the start of this GC. */
    int compartmentCount;

    bool isCollectingAllZones() const { return collectedZoneCount == zoneCount; }

    ZoneGCStats()
      : collectedZoneCount(0), zoneCount(0), collectedCompartmentCount(0), compartmentCount(0)
    {}
};

/*
 * Struct for collecting timing statistics on a "phase tree". The tree is
 * specified as a limited DAG, but the timings are collected for the whole tree
 * that you would get by expanding out the DAG by duplicating subtrees rooted
 * at nodes with multiple parents.
 *
 * During execution, a child phase can be activated multiple times, and the
 * total time will be accumulated. (So for example, you can start and end
 * PHASE_MARK_ROOTS multiple times before completing the parent phase.)
 *
 * Incremental GC is represented by recording separate timing results for each
 * slice within the overall GC.
 */
struct Statistics
{
    /*
     * Phases are allowed to have multiple parents, though any path from root
     * to leaf is allowed at most one multi-parented phase. We keep a full set
     * of timings for each of the multi-parented phases, to be able to record
     * all the timings in the expanded tree induced by our dag.
     *
     * Note that this wastes quite a bit of space, since we have a whole
     * separate array of timing data containing all the phases. We could be
     * more clever and keep an array of pointers biased by the offset of the
     * multi-parented phase, and thereby preserve the simple
     * timings[slot][PHASE_*] indexing. But the complexity doesn't seem worth
     * the few hundred bytes of savings. If we want to extend things to full
     * DAGs, this decision should be reconsidered.
     */
    static const size_t MAX_MULTIPARENT_PHASES = 6;

    explicit Statistics(JSRuntime* rt);
    ~Statistics();

    void beginPhase(Phase phase);
    void endPhase(Phase phase);
    void endParallelPhase(Phase phase, const GCParallelTask* task);

    void beginSlice(const ZoneGCStats& zoneStats, JSGCInvocationKind gckind,
                    JS::gcreason::Reason reason);
    void endSlice();

    void startTimingMutator();
    bool stopTimingMutator(double& mutator_ms, double& gc_ms);

    void reset(const char* reason) {
        if (!aborted)
            slices.back().resetReason = reason;
    }
    void nonincremental(const char* reason) { nonincrementalReason = reason; }

    void count(Stat s) {
        MOZ_ASSERT(s < STAT_LIMIT);
        counts[s]++;
    }

    int64_t beginSCC();
    void endSCC(unsigned scc, int64_t start);

    char16_t* formatMessage();
    char16_t* formatJSON(uint64_t timestamp);
    UniqueChars formatDetailedMessage();

    JS::GCSliceCallback setSliceCallback(JS::GCSliceCallback callback);

    int64_t clearMaxGCPauseAccumulator();
    int64_t getMaxGCPauseSinceClear();

    // Return the current phase, suppressing the synthetic PHASE_MUTATOR phase.
    Phase currentPhase() {
        if (phaseNestingDepth == 0)
            return PHASE_NONE;
        if (phaseNestingDepth == 1)
            return phaseNesting[0] == PHASE_MUTATOR ? PHASE_NONE : phaseNesting[0];
        return phaseNesting[phaseNestingDepth - 1];
    }

    static const size_t MAX_NESTING = 20;

  private:
    JSRuntime* runtime;

    int64_t startupTime;

    FILE* fp;
    bool fullFormat;

    /*
     * GCs can't really nest, but a second GC can be triggered from within the
     * JSGC_END callback.
     */
    int gcDepth;

    ZoneGCStats zoneStats;

    JSGCInvocationKind gckind;

    const char* nonincrementalReason;

    struct SliceData {
        SliceData(JS::gcreason::Reason reason, int64_t start, size_t startFaults)
          : reason(reason), resetReason(nullptr), start(start), startFaults(startFaults)
        {
            for (size_t i = 0; i < MAX_MULTIPARENT_PHASES + 1; i++)
                mozilla::PodArrayZero(phaseTimes[i]);
        }

        JS::gcreason::Reason reason;
        const char* resetReason;
        int64_t start, end;
        size_t startFaults, endFaults;
        int64_t phaseTimes[MAX_MULTIPARENT_PHASES + 1][PHASE_LIMIT];

        int64_t duration() const { return end - start; }
    };

    Vector<SliceData, 8, SystemAllocPolicy> slices;

    /* Most recent time when the given phase started. */
    int64_t phaseStartTimes[PHASE_LIMIT];

    /* Bookkeeping for GC timings when timingMutator is true */
    int64_t timedGCStart;
    int64_t timedGCTime;

    /* Total time in a given phase for this GC. */
    int64_t phaseTimes[MAX_MULTIPARENT_PHASES + 1][PHASE_LIMIT];

    /* Total time in a given phase over all GCs. */
    int64_t phaseTotals[MAX_MULTIPARENT_PHASES + 1][PHASE_LIMIT];

    /* Number of events of this type for this GC. */
    unsigned int counts[STAT_LIMIT];

    /* Allocated space before the GC started. */
    size_t preBytes;

    /* Records the maximum GC pause in an API-controlled interval (in us). */
    int64_t maxPauseInInterval;

    /* Phases that are currently on stack. */
    Phase phaseNesting[MAX_NESTING];
    size_t phaseNestingDepth;
    size_t activeDagSlot;

    /*
     * To avoid recursive nesting, we discontinue a callback phase when any
     * other phases are started. Remember what phase to resume when the inner
     * phases are complete. (And because GCs can nest within the callbacks any
     * number of times, we need a whole stack of of phases to resume.)
     */
    Phase suspendedPhases[MAX_NESTING];
    size_t suspendedPhaseNestingDepth;

    /* Sweep times for SCCs of compartments. */
    Vector<int64_t, 0, SystemAllocPolicy> sccTimes;

    JS::GCSliceCallback sliceCallback;

    /*
     * True if we saw an OOM while allocating slices. The statistics for this
     * GC will be invalid.
     */
    bool aborted;

    void beginGC(JSGCInvocationKind kind);
    void endGC();

    void recordPhaseEnd(Phase phase);

    void gcDuration(int64_t* total, int64_t* maxPause);
    void sccDurations(int64_t* total, int64_t* maxPause);
    void printStats();
    bool formatData(StatisticsSerializer& ss, uint64_t timestamp);

    UniqueChars formatDescription();
    UniqueChars formatSliceDescription(unsigned i, const SliceData& slice);
    UniqueChars formatTotals();
    UniqueChars formatPhaseTimes(int64_t (*phaseTimes)[PHASE_LIMIT]);

    double computeMMU(int64_t resolution);
};

struct AutoGCSlice
{
    AutoGCSlice(Statistics& stats, const ZoneGCStats& zoneStats, JSGCInvocationKind gckind,
                JS::gcreason::Reason reason
                MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : stats(stats)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        stats.beginSlice(zoneStats, gckind, reason);
    }
    ~AutoGCSlice() { stats.endSlice(); }

    Statistics& stats;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

struct AutoPhase
{
    AutoPhase(Statistics& stats, Phase phase
              MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : stats(stats), task(nullptr), phase(phase), enabled(true)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        stats.beginPhase(phase);
    }

    AutoPhase(Statistics& stats, bool condition, Phase phase
              MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : stats(stats), task(nullptr), phase(phase), enabled(condition)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        if (enabled)
            stats.beginPhase(phase);
    }

    AutoPhase(Statistics& stats, const GCParallelTask& task, Phase phase
              MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : stats(stats), task(&task), phase(phase), enabled(true)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        if (enabled)
            stats.beginPhase(phase);
    }

    ~AutoPhase() {
        if (enabled) {
            if (task)
                stats.endParallelPhase(phase, task);
            else
                stats.endPhase(phase);
        }
    }

    Statistics& stats;
    const GCParallelTask* task;
    Phase phase;
    bool enabled;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

struct AutoSCC
{
    AutoSCC(Statistics& stats, unsigned scc
            MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : stats(stats), scc(scc)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        start = stats.beginSCC();
    }
    ~AutoSCC() {
        stats.endSCC(scc, start);
    }

    Statistics& stats;
    unsigned scc;
    int64_t start;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

const char* ExplainInvocationKind(JSGCInvocationKind gckind);
const char* ExplainReason(JS::gcreason::Reason reason);

} /* namespace gcstats */
} /* namespace js */

#endif /* gc_Statistics_h */
