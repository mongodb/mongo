/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Statistics.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/IntegerRange.h"
#include "mozilla/PodOperations.h"
#include "mozilla/UniquePtr.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>

#include "jsprf.h"
#include "jsutil.h"

#include "gc/Memory.h"
#include "vm/Debugger.h"
#include "vm/HelperThreads.h"
#include "vm/Runtime.h"
#include "vm/Time.h"

using namespace js;
using namespace js::gc;
using namespace js::gcstats;

using mozilla::MakeRange;
using mozilla::PodArrayZero;
using mozilla::PodZero;

/* Except for the first and last, slices of less than 10ms are not reported. */
static const int64_t SLICE_MIN_REPORT_TIME = 10 * PRMJ_USEC_PER_MSEC;

/*
 * If this fails, then you can either delete this assertion and allow all
 * larger-numbered reasons to pile up in the last telemetry bucket, or switch
 * to GC_REASON_3 and bump the max value.
 */
JS_STATIC_ASSERT(JS::gcreason::NUM_TELEMETRY_REASONS >= JS::gcreason::NUM_REASONS);

const char*
js::gcstats::ExplainInvocationKind(JSGCInvocationKind gckind)
{
    MOZ_ASSERT(gckind == GC_NORMAL || gckind == GC_SHRINK);
    if (gckind == GC_NORMAL)
         return "Normal";
    else
         return "Shrinking";
}

const char*
js::gcstats::ExplainReason(JS::gcreason::Reason reason)
{
    switch (reason) {
#define SWITCH_REASON(name)                         \
        case JS::gcreason::name:                    \
          return #name;
        GCREASONS(SWITCH_REASON)

        default:
          MOZ_CRASH("bad GC reason");
#undef SWITCH_REASON
    }
}

static double
t(int64_t t)
{
    return double(t) / PRMJ_USEC_PER_MSEC;
}

struct PhaseInfo
{
    Phase index;
    const char* name;
    Phase parent;
    const uint8_t telemetryBucket;
};

// The zeroth entry in the timing arrays is used for phases that have a
// unique lineage.
static const size_t PHASE_DAG_NONE = 0;

// These are really just fields of PhaseInfo, but I have to initialize them
// programmatically, which prevents making phases[] const. (And marking these
// fields mutable does not work on Windows; the whole thing gets created in
// read-only memory anyway.)
struct ExtraPhaseInfo
{
    // Depth in the tree of each phase type
    size_t depth;

    // Index into the set of parallel arrays of timing data, for parents with
    // at least one multi-parented child
    size_t dagSlot;
};

static const Phase PHASE_NO_PARENT = PHASE_LIMIT;

struct DagChildEdge {
    Phase parent;
    Phase child;
} dagChildEdges[] = {
    { PHASE_MARK, PHASE_MARK_ROOTS },
    { PHASE_MINOR_GC, PHASE_MARK_ROOTS },
    { PHASE_TRACE_HEAP, PHASE_MARK_ROOTS },
    { PHASE_EVICT_NURSERY, PHASE_MARK_ROOTS },
    { PHASE_COMPACT_UPDATE, PHASE_MARK_ROOTS }
};

/*
 * Note that PHASE_MUTATOR, PHASE_GC_BEGIN, and PHASE_GC_END never have any
 * child phases. If beginPhase is called while one of these is active, they
 * will automatically be suspended and resumed when the phase stack is next
 * empty. Timings for these phases are thus exclusive of any other phase.
 */

static const PhaseInfo phases[] = {
    { PHASE_MUTATOR, "Mutator Running", PHASE_NO_PARENT, 0 },
    { PHASE_GC_BEGIN, "Begin Callback", PHASE_NO_PARENT, 1 },
    { PHASE_WAIT_BACKGROUND_THREAD, "Wait Background Thread", PHASE_NO_PARENT, 2 },
    { PHASE_MARK_DISCARD_CODE, "Mark Discard Code", PHASE_NO_PARENT, 3 },
    { PHASE_RELAZIFY_FUNCTIONS, "Relazify Functions", PHASE_NO_PARENT, 4 },
    { PHASE_PURGE, "Purge", PHASE_NO_PARENT, 5 },
    { PHASE_MARK, "Mark", PHASE_NO_PARENT, 6 },
        { PHASE_UNMARK, "Unmark", PHASE_MARK, 7 },
        /* PHASE_MARK_ROOTS */
        { PHASE_MARK_DELAYED, "Mark Delayed", PHASE_MARK, 8 },
    { PHASE_SWEEP, "Sweep", PHASE_NO_PARENT, 9 },
        { PHASE_SWEEP_MARK, "Mark During Sweeping", PHASE_SWEEP, 10 },
            { PHASE_SWEEP_MARK_TYPES, "Mark Types During Sweeping", PHASE_SWEEP_MARK, 11 },
            { PHASE_SWEEP_MARK_INCOMING_BLACK, "Mark Incoming Black Pointers", PHASE_SWEEP_MARK, 12 },
            { PHASE_SWEEP_MARK_WEAK, "Mark Weak", PHASE_SWEEP_MARK, 13 },
            { PHASE_SWEEP_MARK_INCOMING_GRAY, "Mark Incoming Gray Pointers", PHASE_SWEEP_MARK, 14 },
            { PHASE_SWEEP_MARK_GRAY, "Mark Gray", PHASE_SWEEP_MARK, 15 },
            { PHASE_SWEEP_MARK_GRAY_WEAK, "Mark Gray and Weak", PHASE_SWEEP_MARK, 16 },
        { PHASE_FINALIZE_START, "Finalize Start Callbacks", PHASE_SWEEP, 17 },
            { PHASE_WEAK_ZONEGROUP_CALLBACK, "Per-Slice Weak Callback", PHASE_FINALIZE_START, 57 },
            { PHASE_WEAK_COMPARTMENT_CALLBACK, "Per-Compartment Weak Callback", PHASE_FINALIZE_START, 58 },
        { PHASE_SWEEP_ATOMS, "Sweep Atoms", PHASE_SWEEP, 18 },
        { PHASE_SWEEP_SYMBOL_REGISTRY, "Sweep Symbol Registry", PHASE_SWEEP, 19 },
        { PHASE_SWEEP_COMPARTMENTS, "Sweep Compartments", PHASE_SWEEP, 20 },
            { PHASE_SWEEP_DISCARD_CODE, "Sweep Discard Code", PHASE_SWEEP_COMPARTMENTS, 21 },
            { PHASE_SWEEP_INNER_VIEWS, "Sweep Inner Views", PHASE_SWEEP_COMPARTMENTS, 22 },
            { PHASE_SWEEP_CC_WRAPPER, "Sweep Cross Compartment Wrappers", PHASE_SWEEP_COMPARTMENTS, 23 },
            { PHASE_SWEEP_BASE_SHAPE, "Sweep Base Shapes", PHASE_SWEEP_COMPARTMENTS, 24 },
            { PHASE_SWEEP_INITIAL_SHAPE, "Sweep Initial Shapes", PHASE_SWEEP_COMPARTMENTS, 25 },
            { PHASE_SWEEP_TYPE_OBJECT, "Sweep Type Objects", PHASE_SWEEP_COMPARTMENTS, 26 },
            { PHASE_SWEEP_BREAKPOINT, "Sweep Breakpoints", PHASE_SWEEP_COMPARTMENTS, 27 },
            { PHASE_SWEEP_REGEXP, "Sweep Regexps", PHASE_SWEEP_COMPARTMENTS, 28 },
            { PHASE_SWEEP_MISC, "Sweep Miscellaneous", PHASE_SWEEP_COMPARTMENTS, 29 },
            { PHASE_SWEEP_TYPES, "Sweep type information", PHASE_SWEEP_COMPARTMENTS, 30 },
                { PHASE_SWEEP_TYPES_BEGIN, "Sweep type tables and compilations", PHASE_SWEEP_TYPES, 31 },
                { PHASE_SWEEP_TYPES_END, "Free type arena", PHASE_SWEEP_TYPES, 32 },
        { PHASE_SWEEP_OBJECT, "Sweep Object", PHASE_SWEEP, 33 },
        { PHASE_SWEEP_STRING, "Sweep String", PHASE_SWEEP, 34 },
        { PHASE_SWEEP_SCRIPT, "Sweep Script", PHASE_SWEEP, 35 },
        { PHASE_SWEEP_SHAPE, "Sweep Shape", PHASE_SWEEP, 36 },
        { PHASE_SWEEP_JITCODE, "Sweep JIT code", PHASE_SWEEP, 37 },
        { PHASE_FINALIZE_END, "Finalize End Callback", PHASE_SWEEP, 38 },
        { PHASE_DESTROY, "Deallocate", PHASE_SWEEP, 39 },
    { PHASE_COMPACT, "Compact", PHASE_NO_PARENT, 40 },
        { PHASE_COMPACT_MOVE, "Compact Move", PHASE_COMPACT, 41 },
        { PHASE_COMPACT_UPDATE, "Compact Update", PHASE_COMPACT, 42 },
            /* PHASE_MARK_ROOTS */
            { PHASE_COMPACT_UPDATE_CELLS, "Compact Update Cells", PHASE_COMPACT_UPDATE, 43 },
    { PHASE_GC_END, "End Callback", PHASE_NO_PARENT, 44 },
    { PHASE_MINOR_GC, "All Minor GCs", PHASE_NO_PARENT, 45 },
        /* PHASE_MARK_ROOTS */
    { PHASE_EVICT_NURSERY, "Minor GCs to Evict Nursery", PHASE_NO_PARENT, 46 },
        /* PHASE_MARK_ROOTS */
    { PHASE_TRACE_HEAP, "Trace Heap", PHASE_NO_PARENT, 47 },
        /* PHASE_MARK_ROOTS */
    { PHASE_BARRIER, "Barriers", PHASE_NO_PARENT, 55 },
        { PHASE_UNMARK_GRAY, "Unmark gray", PHASE_BARRIER, 56 },
    { PHASE_MARK_ROOTS, "Mark Roots", PHASE_MULTI_PARENTS, 48 },
        { PHASE_BUFFER_GRAY_ROOTS, "Buffer Gray Roots", PHASE_MARK_ROOTS, 49 },
        { PHASE_MARK_CCWS, "Mark Cross Compartment Wrappers", PHASE_MARK_ROOTS, 50 },
        { PHASE_MARK_ROOTERS, "Mark Rooters", PHASE_MARK_ROOTS, 51 },
        { PHASE_MARK_RUNTIME_DATA, "Mark Runtime-wide Data", PHASE_MARK_ROOTS, 52 },
        { PHASE_MARK_EMBEDDING, "Mark Embedding", PHASE_MARK_ROOTS, 53 },
        { PHASE_MARK_COMPARTMENTS, "Mark Compartments", PHASE_MARK_ROOTS, 54 },
    { PHASE_LIMIT, nullptr, PHASE_NO_PARENT, 58 }

    // Current number of telemetryBuckets is 58. If you insert new phases
    // somewhere, start at that number and count up. Do not change any existing
    // numbers.
};

static ExtraPhaseInfo phaseExtra[PHASE_LIMIT] = { { 0, 0 } };

// Mapping from all nodes with a multi-parented child to a Vector of all
// multi-parented children and their descendants. (Single-parented children will
// not show up in this list.)
static mozilla::Vector<Phase> dagDescendants[Statistics::NumTimingArrays];

struct AllPhaseIterator {
    int current;
    int baseLevel;
    size_t activeSlot;
    mozilla::Vector<Phase>::Range descendants;

    explicit AllPhaseIterator(const Statistics::PhaseTimeTable table)
      : current(0)
      , baseLevel(0)
      , activeSlot(PHASE_DAG_NONE)
      , descendants(dagDescendants[PHASE_DAG_NONE].all()) /* empty range */
    {
    }

    void get(Phase* phase, size_t* dagSlot, size_t* level = nullptr) {
        MOZ_ASSERT(!done());
        *dagSlot = activeSlot;
        *phase = descendants.empty() ? Phase(current) : descendants.front();
        if (level)
            *level = phaseExtra[*phase].depth + baseLevel;
    }

    void advance() {
        MOZ_ASSERT(!done());

        if (!descendants.empty()) {
            descendants.popFront();
            if (!descendants.empty())
                return;

            ++current;
            activeSlot = PHASE_DAG_NONE;
            baseLevel = 0;
            return;
        }

        if (phaseExtra[current].dagSlot != PHASE_DAG_NONE) {
            activeSlot = phaseExtra[current].dagSlot;
            descendants = dagDescendants[activeSlot].all();
            MOZ_ASSERT(!descendants.empty());
            baseLevel += phaseExtra[current].depth + 1;
            return;
        }

        ++current;
    }

    bool done() const {
        return phases[current].parent == PHASE_MULTI_PARENTS;
    }
};

void
Statistics::gcDuration(int64_t* total, int64_t* maxPause) const
{
    *total = *maxPause = 0;
    for (const SliceData* slice = slices.begin(); slice != slices.end(); slice++) {
        *total += slice->duration();
        if (slice->duration() > *maxPause)
            *maxPause = slice->duration();
    }
    if (*maxPause > maxPauseInInterval)
        maxPauseInInterval = *maxPause;
}

void
Statistics::sccDurations(int64_t* total, int64_t* maxPause)
{
    *total = *maxPause = 0;
    for (size_t i = 0; i < sccTimes.length(); i++) {
        *total += sccTimes[i];
        *maxPause = Max(*maxPause, sccTimes[i]);
    }
}

typedef Vector<UniqueChars, 8, SystemAllocPolicy> FragmentVector;

static UniqueChars
Join(const FragmentVector& fragments, const char* separator = "") {
    const size_t separatorLength = strlen(separator);
    size_t length = 0;
    for (size_t i = 0; i < fragments.length(); ++i) {
        length += fragments[i] ? strlen(fragments[i].get()) : 0;
        if (i < (fragments.length() - 1))
            length += separatorLength;
    }

    char* joined = js_pod_malloc<char>(length + 1);
    joined[length] = '\0';

    char* cursor = joined;
    for (size_t i = 0; i < fragments.length(); ++i) {
        if (fragments[i])
            strcpy(cursor, fragments[i].get());
        cursor += fragments[i] ? strlen(fragments[i].get()) : 0;
        if (i < (fragments.length() - 1)) {
            if (separatorLength)
                strcpy(cursor, separator);
            cursor += separatorLength;
        }
    }

    return UniqueChars(joined);
}

static int64_t
SumChildTimes(size_t phaseSlot, Phase phase, const Statistics::PhaseTimeTable phaseTimes)
{
    // Sum the contributions from single-parented children.
    int64_t total = 0;
    for (unsigned i = 0; i < PHASE_LIMIT; i++) {
        if (phases[i].parent == phase)
            total += phaseTimes[phaseSlot][i];
    }

    // Sum the contributions from multi-parented children.
    size_t dagSlot = phaseExtra[phase].dagSlot;
    if (dagSlot != PHASE_DAG_NONE) {
        for (size_t i = 0; i < mozilla::ArrayLength(dagChildEdges); i++) {
            if (dagChildEdges[i].parent == phase) {
                Phase child = dagChildEdges[i].child;
                total += phaseTimes[dagSlot][child];
            }
        }
    }
    return total;
}

UniqueChars
Statistics::formatCompactSliceMessage() const
{
    // Skip if we OOM'ed.
    if (slices.length() == 0)
        return UniqueChars(nullptr);

    const size_t index = slices.length() - 1;
    const SliceData& slice = slices[index];

    char budgetDescription[200];
    slice.budget.describe(budgetDescription, sizeof(budgetDescription) - 1);

    const char* format =
        "GC Slice %u - Pause: %.3fms of %s budget (@ %.3fms); Reason: %s; Reset: %s%s; Times: ";
    char buffer[1024];
    memset(buffer, 0, sizeof(buffer));
    JS_snprintf(buffer, sizeof(buffer), format, index,
                t(slice.duration()), budgetDescription, t(slice.start - slices[0].start),
                ExplainReason(slice.reason),
                slice.resetReason ? "yes - " : "no", slice.resetReason ? slice.resetReason : "");

    FragmentVector fragments;
    if (!fragments.append(make_string_copy(buffer)) ||
        !fragments.append(formatCompactSlicePhaseTimes(slices[index].phaseTimes)))
    {
        return UniqueChars(nullptr);
    }
    return Join(fragments);
}

UniqueChars
Statistics::formatCompactSummaryMessage() const
{
    const double bytesPerMiB = 1024 * 1024;

    FragmentVector fragments;
    if (!fragments.append(make_string_copy("Summary - ")))
        return UniqueChars(nullptr);

    int64_t total, longest;
    gcDuration(&total, &longest);

    const double mmu20 = computeMMU(20 * PRMJ_USEC_PER_MSEC);
    const double mmu50 = computeMMU(50 * PRMJ_USEC_PER_MSEC);

    char buffer[1024];
    if (!nonincrementalReason_) {
        JS_snprintf(buffer, sizeof(buffer),
                    "Max Pause: %.3fms; MMU 20ms: %.1f%%; MMU 50ms: %.1f%%; Total: %.3fms; ",
                    t(longest), mmu20 * 100., mmu50 * 100., t(total));
    } else {
        JS_snprintf(buffer, sizeof(buffer), "Non-Incremental: %.3fms (%s); ",
                    t(total), nonincrementalReason_);
    }
    if (!fragments.append(make_string_copy(buffer)))
        return UniqueChars(nullptr);

    JS_snprintf(buffer, sizeof(buffer),
                "Zones: %d of %d; Compartments: %d of %d; HeapSize: %.3f MiB; "\
                "HeapChange (abs): %+d (%d); ",
                zoneStats.collectedZoneCount, zoneStats.zoneCount,
                zoneStats.collectedCompartmentCount, zoneStats.compartmentCount,
                double(preBytes) / bytesPerMiB,
                counts[STAT_NEW_CHUNK] - counts[STAT_DESTROY_CHUNK],
                counts[STAT_NEW_CHUNK] + counts[STAT_DESTROY_CHUNK]);
    if (!fragments.append(make_string_copy(buffer)))
        return UniqueChars(nullptr);

    MOZ_ASSERT_IF(counts[STAT_ARENA_RELOCATED], gckind == GC_SHRINK);
    if (gckind == GC_SHRINK) {
        JS_snprintf(buffer, sizeof(buffer),
                    "Kind: %s; Relocated: %.3f MiB; ",
                    ExplainInvocationKind(gckind),
                    double(ArenaSize * counts[STAT_ARENA_RELOCATED]) / bytesPerMiB);
        if (!fragments.append(make_string_copy(buffer)))
            return UniqueChars(nullptr);
    }

    return Join(fragments);
}

UniqueChars
Statistics::formatCompactSlicePhaseTimes(const PhaseTimeTable phaseTimes) const
{
    static const int64_t MaxUnaccountedTimeUS = 100;

    FragmentVector fragments;
    char buffer[128];
    for (AllPhaseIterator iter(phaseTimes); !iter.done(); iter.advance()) {
        Phase phase;
        size_t dagSlot;
        size_t level;
        iter.get(&phase, &dagSlot, &level);
        MOZ_ASSERT(level < 4);

        int64_t ownTime = phaseTimes[dagSlot][phase];
        int64_t childTime = SumChildTimes(dagSlot, phase, phaseTimes);
        if (ownTime > MaxUnaccountedTimeUS) {
            JS_snprintf(buffer, sizeof(buffer), "%s: %.3fms", phases[phase].name, t(ownTime));
            if (!fragments.append(make_string_copy(buffer)))
                return UniqueChars(nullptr);

            if (childTime && (ownTime - childTime) > MaxUnaccountedTimeUS) {
                MOZ_ASSERT(level < 3);
                JS_snprintf(buffer, sizeof(buffer), "%s: %.3fms", "Other", t(ownTime - childTime));
                if (!fragments.append(make_string_copy(buffer)))
                    return UniqueChars(nullptr);
            }
        }
    }
    return Join(fragments, ", ");
}

UniqueChars
Statistics::formatDetailedMessage()
{
    FragmentVector fragments;

    if (!fragments.append(formatDetailedDescription()))
        return UniqueChars(nullptr);

    if (slices.length() > 1) {
        for (unsigned i = 0; i < slices.length(); i++) {
            if (!fragments.append(formatDetailedSliceDescription(i, slices[i])))
                return UniqueChars(nullptr);
            if (!fragments.append(formatDetailedPhaseTimes(slices[i].phaseTimes)))
                return UniqueChars(nullptr);
        }
    }
    if (!fragments.append(formatDetailedTotals()))
        return UniqueChars(nullptr);
    if (!fragments.append(formatDetailedPhaseTimes(phaseTimes)))
        return UniqueChars(nullptr);

    return Join(fragments);
}

UniqueChars
Statistics::formatDetailedDescription()
{
    const double bytesPerMiB = 1024 * 1024;

    int64_t sccTotal, sccLongest;
    sccDurations(&sccTotal, &sccLongest);

    double mmu20 = computeMMU(20 * PRMJ_USEC_PER_MSEC);
    double mmu50 = computeMMU(50 * PRMJ_USEC_PER_MSEC);

    const char* format =
"=================================================================\n\
  Invocation Kind: %s\n\
  Reason: %s\n\
  Incremental: %s%s\n\
  Zones Collected: %d of %d\n\
  Compartments Collected: %d of %d\n\
  MinorGCs since last GC: %d\n\
  Store Buffer Overflows: %d\n\
  MMU 20ms:%.1f%%; 50ms:%.1f%%\n\
  SCC Sweep Total (MaxPause): %.3fms (%.3fms)\n\
  HeapSize: %.3f MiB\n\
  Chunk Delta (magnitude): %+d  (%d)\n\
  Arenas Relocated: %.3f MiB\n\
";
    char buffer[1024];
    memset(buffer, 0, sizeof(buffer));
    JS_snprintf(buffer, sizeof(buffer), format,
                ExplainInvocationKind(gckind),
                ExplainReason(slices[0].reason),
                nonincrementalReason_ ? "no - " : "yes",
                                                  nonincrementalReason_ ? nonincrementalReason_ : "",
                zoneStats.collectedZoneCount, zoneStats.zoneCount,
                zoneStats.collectedCompartmentCount, zoneStats.compartmentCount,
                counts[STAT_MINOR_GC],
                counts[STAT_STOREBUFFER_OVERFLOW],
                mmu20 * 100., mmu50 * 100.,
                t(sccTotal), t(sccLongest),
                double(preBytes) / bytesPerMiB,
                counts[STAT_NEW_CHUNK] - counts[STAT_DESTROY_CHUNK], counts[STAT_NEW_CHUNK] +
                                                                     counts[STAT_DESTROY_CHUNK],
                double(ArenaSize * counts[STAT_ARENA_RELOCATED]) / bytesPerMiB);
    return make_string_copy(buffer);
}

UniqueChars
Statistics::formatDetailedSliceDescription(unsigned i, const SliceData& slice)
{
    char budgetDescription[200];
    slice.budget.describe(budgetDescription, sizeof(budgetDescription) - 1);

    const char* format =
"\
  ---- Slice %u ----\n\
    Reason: %s\n\
    Reset: %s%s\n\
    Page Faults: %ld\n\
    Pause: %.3fms of %s budget (@ %.3fms)\n\
";
    char buffer[1024];
    memset(buffer, 0, sizeof(buffer));
    JS_snprintf(buffer, sizeof(buffer), format, i,
                ExplainReason(slice.reason),
                slice.resetReason ? "yes - " : "no", slice.resetReason ? slice.resetReason : "",
                uint64_t(slice.endFaults - slice.startFaults),
                t(slice.duration()), budgetDescription, t(slice.start - slices[0].start));
    return make_string_copy(buffer);
}

UniqueChars
Statistics::formatDetailedPhaseTimes(const PhaseTimeTable phaseTimes)
{
    static const char* LevelToIndent[] = { "", "  ", "    ", "      " };
    static const int64_t MaxUnaccountedChildTimeUS = 50;

    FragmentVector fragments;
    char buffer[128];
    for (AllPhaseIterator iter(phaseTimes); !iter.done(); iter.advance()) {
        Phase phase;
        size_t dagSlot;
        size_t level;
        iter.get(&phase, &dagSlot, &level);
        MOZ_ASSERT(level < 4);

        int64_t ownTime = phaseTimes[dagSlot][phase];
        int64_t childTime = SumChildTimes(dagSlot, phase, phaseTimes);
        if (ownTime > 0) {
            JS_snprintf(buffer, sizeof(buffer), "      %s%s: %.3fms\n",
                        LevelToIndent[level], phases[phase].name, t(ownTime));
            if (!fragments.append(make_string_copy(buffer)))
                return UniqueChars(nullptr);

            if (childTime && (ownTime - childTime) > MaxUnaccountedChildTimeUS) {
                MOZ_ASSERT(level < 3);
                JS_snprintf(buffer, sizeof(buffer), "      %s%s: %.3fms\n",
                            LevelToIndent[level + 1], "Other", t(ownTime - childTime));
                if (!fragments.append(make_string_copy(buffer)))
                    return UniqueChars(nullptr);
            }
        }
    }
    return Join(fragments);
}

UniqueChars
Statistics::formatDetailedTotals()
{
    int64_t total, longest;
    gcDuration(&total, &longest);

    const char* format =
"\
  ---- Totals ----\n\
    Total Time: %.3fms\n\
    Max Pause: %.3fms\n\
";
    char buffer[1024];
    memset(buffer, 0, sizeof(buffer));
    JS_snprintf(buffer, sizeof(buffer), format, t(total), t(longest));
    return make_string_copy(buffer);
}

UniqueChars
Statistics::formatJsonMessage(uint64_t timestamp)
{
    MOZ_ASSERT(!aborted);

    FragmentVector fragments;

    if (!fragments.append(make_string_copy("{")) ||
        !fragments.append(formatJsonDescription(timestamp)) ||
        !fragments.append(make_string_copy("\"slices\":[")))
    {
        return UniqueChars(nullptr);
    }

    for (unsigned i = 0; i < slices.length(); i++) {
        if (!fragments.append(make_string_copy("{")) ||
            !fragments.append(formatJsonSliceDescription(i, slices[i])) ||
            !fragments.append(make_string_copy("\"times\":{")) ||
            !fragments.append(formatJsonPhaseTimes(slices[i].phaseTimes)) ||
            !fragments.append(make_string_copy("}}")) ||
            (i < (slices.length() - 1) && !fragments.append(make_string_copy(","))))
        {
            return UniqueChars(nullptr);
        }
    }

    if (!fragments.append(make_string_copy("],\"totals\":{")) ||
        !fragments.append(formatJsonPhaseTimes(phaseTimes)) ||
        !fragments.append(make_string_copy("}}")))
    {
        return UniqueChars(nullptr);
    }

    return Join(fragments);
}

UniqueChars
Statistics::formatJsonDescription(uint64_t timestamp)
{
    int64_t total, longest;
    gcDuration(&total, &longest);

    int64_t sccTotal, sccLongest;
    sccDurations(&sccTotal, &sccLongest);

    double mmu20 = computeMMU(20 * PRMJ_USEC_PER_MSEC);
    double mmu50 = computeMMU(50 * PRMJ_USEC_PER_MSEC);

    const char *format =
        "\"timestamp\":%llu,"
        "\"max_pause\":%llu.%03llu,"
        "\"total_time\":%llu.%03llu,"
        "\"zones_collected\":%d,"
        "\"total_zones\":%d,"
        "\"total_compartments\":%d,"
        "\"minor_gcs\":%d,"
        "\"store_buffer_overflows\":%d,"
        "\"mmu_20ms\":%d,"
        "\"mmu_50ms\":%d,"
        "\"scc_sweep_total\":%llu.%03llu,"
        "\"scc_sweep_max_pause\":%llu.%03llu,"
        "\"nonincremental_reason\":\"%s\","
        "\"allocated\":%u,"
        "\"added_chunks\":%d,"
        "\"removed_chunks\":%d,";
    char buffer[1024];
    memset(buffer, 0, sizeof(buffer));
    JS_snprintf(buffer, sizeof(buffer), format,
                (unsigned long long)timestamp,
                longest / 1000, longest % 1000,
                total / 1000, total % 1000,
                zoneStats.collectedZoneCount,
                zoneStats.zoneCount,
                zoneStats.compartmentCount,
                counts[STAT_MINOR_GC],
                counts[STAT_STOREBUFFER_OVERFLOW],
                int(mmu20 * 100),
                int(mmu50 * 100),
                sccTotal / 1000, sccTotal % 1000,
                sccLongest / 1000, sccLongest % 1000,
                nonincrementalReason_ ? nonincrementalReason_ : "none",
                unsigned(preBytes / 1024 / 1024),
                counts[STAT_NEW_CHUNK],
                counts[STAT_DESTROY_CHUNK]);
    return make_string_copy(buffer);
}

UniqueChars
Statistics::formatJsonSliceDescription(unsigned i, const SliceData& slice)
{
    int64_t duration = slices[i].duration();
    int64_t when = slices[i].start - slices[0].start;
    char budgetDescription[200];
    slice.budget.describe(budgetDescription, sizeof(budgetDescription) - 1);
    int64_t pageFaults = slices[i].endFaults - slices[i].startFaults;

    const char* format =
        "\"slice\":%d,"
        "\"pause\":%llu.%03llu,"
        "\"when\":%llu.%03llu,"
        "\"reason\":\"%s\","
        "\"budget\":\"%s\","
        "\"page_faults\":%llu,"
        "\"start_timestamp\":%llu,"
        "\"end_timestamp\":%llu,";
    char buffer[1024];
    memset(buffer, 0, sizeof(buffer));
    JS_snprintf(buffer, sizeof(buffer), format,
                i,
                duration / 1000, duration % 1000,
                when / 1000, when % 1000,
                ExplainReason(slices[i].reason),
                budgetDescription,
                pageFaults,
                slices[i].start,
                slices[i].end);
    return make_string_copy(buffer);
}

UniqueChars
FilterJsonKey(const char*const buffer)
{
    char* mut = strdup(buffer);
    char* c = mut;
    while (*c) {
        if (!isalpha(*c))
            *c = '_';
        else if (isupper(*c))
            *c = tolower(*c);
        ++c;
    }
    return UniqueChars(mut);
}

UniqueChars
Statistics::formatJsonPhaseTimes(const PhaseTimeTable phaseTimes)
{
    FragmentVector fragments;
    char buffer[128];
    for (AllPhaseIterator iter(phaseTimes); !iter.done(); iter.advance()) {
        Phase phase;
        size_t dagSlot;
        iter.get(&phase, &dagSlot);

        UniqueChars name = FilterJsonKey(phases[phase].name);
        int64_t ownTime = phaseTimes[dagSlot][phase];
        JS_snprintf(buffer, sizeof(buffer), "\"%s\":%llu.%03llu",
                    name.get(), ownTime / 1000, ownTime % 1000);

        if (!fragments.append(make_string_copy(buffer)))
            return UniqueChars(nullptr);
    }
    return Join(fragments, ",");
}

Statistics::Statistics(JSRuntime* rt)
  : runtime(rt),
    startupTime(PRMJ_Now()),
    fp(nullptr),
    gcDepth(0),
    nonincrementalReason_(nullptr),
    timedGCStart(0),
    preBytes(0),
    maxPauseInInterval(0),
    phaseNestingDepth(0),
    activeDagSlot(PHASE_DAG_NONE),
    suspendedPhaseNestingDepth(0),
    sliceCallback(nullptr),
    aborted(false)
{
    PodArrayZero(phaseTotals);
    PodArrayZero(counts);
    PodArrayZero(phaseStartTimes);
    for (auto d : MakeRange(NumTimingArrays))
        PodArrayZero(phaseTimes[d]);

    static bool initialized = false;
    if (!initialized) {
        initialized = true;

        for (size_t i = 0; i < PHASE_LIMIT; i++) {
            MOZ_ASSERT(phases[i].index == i);
            for (size_t j = 0; j < PHASE_LIMIT; j++)
                MOZ_ASSERT_IF(i != j, phases[i].telemetryBucket != phases[j].telemetryBucket);
        }

        // Create a static table of descendants for every phase with multiple
        // children. This assumes that all descendants come linearly in the
        // list, which is reasonable since full dags are not supported; any
        // path from the leaf to the root must encounter at most one node with
        // multiple parents.
        size_t dagSlot = 0;
        for (size_t i = 0; i < mozilla::ArrayLength(dagChildEdges); i++) {
            Phase parent = dagChildEdges[i].parent;
            if (!phaseExtra[parent].dagSlot)
                phaseExtra[parent].dagSlot = ++dagSlot;

            Phase child = dagChildEdges[i].child;
            MOZ_ASSERT(phases[child].parent == PHASE_MULTI_PARENTS);
            int j = child;
            do {
                dagDescendants[phaseExtra[parent].dagSlot].append(Phase(j));
                j++;
            } while (j != PHASE_LIMIT && phases[j].parent != PHASE_MULTI_PARENTS);
        }
        MOZ_ASSERT(dagSlot <= MaxMultiparentPhases - 1);

        // Fill in the depth of each node in the tree. Multi-parented nodes
        // have depth 0.
        mozilla::Vector<Phase> stack;
        stack.append(PHASE_LIMIT); // Dummy entry to avoid special-casing the first node
        for (int i = 0; i < PHASE_LIMIT; i++) {
            if (phases[i].parent == PHASE_NO_PARENT ||
                phases[i].parent == PHASE_MULTI_PARENTS)
            {
                stack.clear();
            } else {
                while (stack.back() != phases[i].parent)
                    stack.popBack();
            }
            phaseExtra[i].depth = stack.length();
            stack.append(Phase(i));
        }
    }

    char* env = getenv("MOZ_GCTIMER");
    if (env) {
        if (strcmp(env, "none") == 0) {
            fp = nullptr;
        } else if (strcmp(env, "stdout") == 0) {
            fp = stdout;
        } else if (strcmp(env, "stderr") == 0) {
            fp = stderr;
        } else {
            fp = fopen(env, "a");
            if (!fp)
                MOZ_CRASH("Failed to open MOZ_GCTIMER log file.");
        }
    }
}

Statistics::~Statistics()
{
    if (fp && fp != stdout && fp != stderr)
        fclose(fp);
}

JS::GCSliceCallback
Statistics::setSliceCallback(JS::GCSliceCallback newCallback)
{
    JS::GCSliceCallback oldCallback = sliceCallback;
    sliceCallback = newCallback;
    return oldCallback;
}

int64_t
Statistics::clearMaxGCPauseAccumulator()
{
    int64_t prior = maxPauseInInterval;
    maxPauseInInterval = 0;
    return prior;
}

int64_t
Statistics::getMaxGCPauseSinceClear()
{
    return maxPauseInInterval;
}

static int64_t
SumPhase(Phase phase, const Statistics::PhaseTimeTable times)
{
    int64_t sum = 0;
    for (auto i : MakeRange(Statistics::NumTimingArrays))
        sum += times[i][phase];
    return sum;
}

static Phase
LongestPhase(const Statistics::PhaseTimeTable times)
{
    int64_t longestTime = 0;
    Phase longestPhase = PHASE_NONE;
    for (size_t i = 0; i < PHASE_LIMIT; ++i) {
        int64_t phaseTime = SumPhase(Phase(i), times);
        if (phaseTime > longestTime) {
            longestTime = phaseTime;
            longestPhase = Phase(i);
        }
    }
    return longestPhase;
}

void
Statistics::printStats()
{
    if (aborted) {
        fprintf(fp, "OOM during GC statistics collection. The report is unavailable for this GC.\n");
    } else {
        UniqueChars msg = formatDetailedMessage();
        if (msg)
            fprintf(fp, "GC(T+%.3fs) %s\n", t(slices[0].start - startupTime) / 1000.0, msg.get());
    }
    fflush(fp);
}

void
Statistics::beginGC(JSGCInvocationKind kind)
{
    slices.clearAndFree();
    sccTimes.clearAndFree();
    gckind = kind;
    nonincrementalReason_ = nullptr;

    preBytes = runtime->gc.usage.gcBytes();
}

void
Statistics::endGC()
{
    for (auto j : MakeRange(NumTimingArrays))
        for (int i = 0; i < PHASE_LIMIT; i++)
            phaseTotals[j][i] += phaseTimes[j][i];

    int64_t total, longest;
    gcDuration(&total, &longest);

    int64_t sccTotal, sccLongest;
    sccDurations(&sccTotal, &sccLongest);

    runtime->addTelemetry(JS_TELEMETRY_GC_IS_COMPARTMENTAL, !zoneStats.isCollectingAllZones());
    runtime->addTelemetry(JS_TELEMETRY_GC_MS, t(total));
    runtime->addTelemetry(JS_TELEMETRY_GC_MAX_PAUSE_MS, t(longest));
    int64_t markTotal = SumPhase(PHASE_MARK, phaseTimes);
    int64_t markRootsTotal = SumPhase(PHASE_MARK_ROOTS, phaseTimes);
    runtime->addTelemetry(JS_TELEMETRY_GC_MARK_MS, t(markTotal));
    runtime->addTelemetry(JS_TELEMETRY_GC_SWEEP_MS, t(phaseTimes[PHASE_DAG_NONE][PHASE_SWEEP]));
    runtime->addTelemetry(JS_TELEMETRY_GC_MARK_ROOTS_MS, t(markRootsTotal));
    runtime->addTelemetry(JS_TELEMETRY_GC_MARK_GRAY_MS, t(phaseTimes[PHASE_DAG_NONE][PHASE_SWEEP_MARK_GRAY]));
    runtime->addTelemetry(JS_TELEMETRY_GC_NON_INCREMENTAL, !!nonincrementalReason_);
    runtime->addTelemetry(JS_TELEMETRY_GC_INCREMENTAL_DISABLED, !runtime->gc.isIncrementalGCAllowed());
    runtime->addTelemetry(JS_TELEMETRY_GC_SCC_SWEEP_TOTAL_MS, t(sccTotal));
    runtime->addTelemetry(JS_TELEMETRY_GC_SCC_SWEEP_MAX_PAUSE_MS, t(sccLongest));

    if (!aborted) {
        double mmu50 = computeMMU(50 * PRMJ_USEC_PER_MSEC);
        runtime->addTelemetry(JS_TELEMETRY_GC_MMU_50, mmu50 * 100);
    }

    if (fp)
        printStats();

    // Clear the timers at the end of a GC because we accumulate time in
    // between GCs for some (which come before PHASE_GC_BEGIN in the list.)
    PodZero(&phaseStartTimes[PHASE_GC_BEGIN], PHASE_LIMIT - PHASE_GC_BEGIN);
    for (size_t d = PHASE_DAG_NONE; d < NumTimingArrays; d++)
        PodZero(&phaseTimes[d][PHASE_GC_BEGIN], PHASE_LIMIT - PHASE_GC_BEGIN);

    // Clear the OOM flag but only if we are not in a nested GC.
    if (gcDepth == 1)
        aborted = false;
}

void
Statistics::beginSlice(const ZoneGCStats& zoneStats, JSGCInvocationKind gckind,
                       SliceBudget budget, JS::gcreason::Reason reason)
{
    gcDepth++;
    this->zoneStats = zoneStats;

    bool first = !runtime->gc.isIncrementalGCInProgress();
    if (first)
        beginGC(gckind);

    SliceData data(budget, reason, PRMJ_Now(), JS_GetCurrentEmbedderTime(), GetPageFaultCount());
    if (!slices.append(data)) {
        // If we are OOM, set a flag to indicate we have missing slice data.
        aborted = true;
        return;
    }

    runtime->addTelemetry(JS_TELEMETRY_GC_REASON, reason);

    // Slice callbacks should only fire for the outermost level.
    if (gcDepth == 1) {
        bool wasFullGC = zoneStats.isCollectingAllZones();
        if (sliceCallback)
            (*sliceCallback)(runtime, first ? JS::GC_CYCLE_BEGIN : JS::GC_SLICE_BEGIN,
                             JS::GCDescription(!wasFullGC, gckind, reason));
    }
}

void
Statistics::endSlice()
{
    if (!aborted) {
        slices.back().end = PRMJ_Now();
        slices.back().endTimestamp = JS_GetCurrentEmbedderTime();
        slices.back().endFaults = GetPageFaultCount();

        int64_t sliceTime = slices.back().end - slices.back().start;
        runtime->addTelemetry(JS_TELEMETRY_GC_SLICE_MS, t(sliceTime));
        runtime->addTelemetry(JS_TELEMETRY_GC_RESET, !!slices.back().resetReason);

        if (slices.back().budget.isTimeBudget()) {
            int64_t budget_ms = slices.back().budget.timeBudget.budget;
            runtime->addTelemetry(JS_TELEMETRY_GC_BUDGET_MS, budget_ms);
            if (budget_ms == runtime->gc.defaultSliceBudget())
                runtime->addTelemetry(JS_TELEMETRY_GC_ANIMATION_MS, t(sliceTime));

            // Record any phase that goes more than 2x over its budget.
            if (sliceTime > 2 * budget_ms * 1000) {
                Phase longest = LongestPhase(slices.back().phaseTimes);
                runtime->addTelemetry(JS_TELEMETRY_GC_SLOW_PHASE, phases[longest].telemetryBucket);
            }
        }
    }

    bool last = !runtime->gc.isIncrementalGCInProgress();
    if (last)
        endGC();

    // Slice callbacks should only fire for the outermost level.
    if (gcDepth == 1 && !aborted) {
        bool wasFullGC = zoneStats.isCollectingAllZones();
        if (sliceCallback)
            (*sliceCallback)(runtime, last ? JS::GC_CYCLE_END : JS::GC_SLICE_END,
                             JS::GCDescription(!wasFullGC, gckind, slices.back().reason));
    }

    /* Do this after the slice callback since it uses these values. */
    if (last)
        PodArrayZero(counts);

    gcDepth--;
    MOZ_ASSERT(gcDepth >= 0);
}

bool
Statistics::startTimingMutator()
{
    if (phaseNestingDepth != 0) {
        // Should only be called from outside of GC.
        MOZ_ASSERT(phaseNestingDepth == 1);
        MOZ_ASSERT(phaseNesting[0] == PHASE_MUTATOR);
        return false;
    }

    MOZ_ASSERT(suspendedPhaseNestingDepth == 0);

    timedGCTime = 0;
    phaseStartTimes[PHASE_MUTATOR] = 0;
    phaseTimes[PHASE_DAG_NONE][PHASE_MUTATOR] = 0;
    timedGCStart = 0;

    beginPhase(PHASE_MUTATOR);
    return true;
}

bool
Statistics::stopTimingMutator(double& mutator_ms, double& gc_ms)
{
    // This should only be called from outside of GC, while timing the mutator.
    if (phaseNestingDepth != 1 || phaseNesting[0] != PHASE_MUTATOR)
        return false;

    endPhase(PHASE_MUTATOR);
    mutator_ms = t(phaseTimes[PHASE_DAG_NONE][PHASE_MUTATOR]);
    gc_ms = t(timedGCTime);

    return true;
}

void
Statistics::beginPhase(Phase phase)
{
    Phase parent = phaseNestingDepth ? phaseNesting[phaseNestingDepth - 1] : PHASE_NO_PARENT;

    // Re-entry is allowed during callbacks, so pause callback phases while
    // other phases are in progress, auto-resuming after they end. As a result,
    // nested GC time will not be accounted against the callback phases.
    //
    // Reuse this mechanism for managing PHASE_MUTATOR.
    if (parent == PHASE_GC_BEGIN || parent == PHASE_GC_END || parent == PHASE_MUTATOR) {
        MOZ_ASSERT(suspendedPhaseNestingDepth < mozilla::ArrayLength(suspendedPhases));
        suspendedPhases[suspendedPhaseNestingDepth++] = parent;
        recordPhaseEnd(parent);
        parent = phaseNestingDepth ? phaseNesting[phaseNestingDepth - 1] : PHASE_NO_PARENT;
    }

    // Guard against any other re-entry.
    MOZ_ASSERT(!phaseStartTimes[phase]);

    MOZ_ASSERT(phases[phase].index == phase);
    MOZ_ASSERT(phaseNestingDepth < MAX_NESTING);
    MOZ_ASSERT(phases[phase].parent == parent || phases[phase].parent == PHASE_MULTI_PARENTS);

    phaseNesting[phaseNestingDepth] = phase;
    phaseNestingDepth++;

    if (phases[phase].parent == PHASE_MULTI_PARENTS)
        activeDagSlot = phaseExtra[parent].dagSlot;

    phaseStartTimes[phase] = PRMJ_Now();
}

void
Statistics::recordPhaseEnd(Phase phase)
{
    int64_t now = PRMJ_Now();

    if (phase == PHASE_MUTATOR)
        timedGCStart = now;

    phaseNestingDepth--;

    int64_t t = now - phaseStartTimes[phase];
    if (!slices.empty())
        slices.back().phaseTimes[activeDagSlot][phase] += t;
    phaseTimes[activeDagSlot][phase] += t;
    phaseStartTimes[phase] = 0;
}

void
Statistics::endPhase(Phase phase)
{
    recordPhaseEnd(phase);

    if (phases[phase].parent == PHASE_MULTI_PARENTS)
        activeDagSlot = PHASE_DAG_NONE;

    // When emptying the stack, we may need to resume a callback phase
    // (PHASE_GC_BEGIN/END) or return to timing the mutator (PHASE_MUTATOR).
    if (phaseNestingDepth == 0 && suspendedPhaseNestingDepth > 0) {
        Phase resumePhase = suspendedPhases[--suspendedPhaseNestingDepth];
        if (resumePhase == PHASE_MUTATOR)
            timedGCTime += PRMJ_Now() - timedGCStart;
        beginPhase(resumePhase);
    }
}

void
Statistics::endParallelPhase(Phase phase, const GCParallelTask* task)
{
    phaseNestingDepth--;

    if (!slices.empty())
        slices.back().phaseTimes[PHASE_DAG_NONE][phase] += task->duration();
    phaseTimes[PHASE_DAG_NONE][phase] += task->duration();
    phaseStartTimes[phase] = 0;
}

int64_t
Statistics::beginSCC()
{
    return PRMJ_Now();
}

void
Statistics::endSCC(unsigned scc, int64_t start)
{
    if (scc >= sccTimes.length() && !sccTimes.resize(scc + 1))
        return;

    sccTimes[scc] += PRMJ_Now() - start;
}

/*
 * MMU (minimum mutator utilization) is a measure of how much garbage collection
 * is affecting the responsiveness of the system. MMU measurements are given
 * with respect to a certain window size. If we report MMU(50ms) = 80%, then
 * that means that, for any 50ms window of time, at least 80% of the window is
 * devoted to the mutator. In other words, the GC is running for at most 20% of
 * the window, or 10ms. The GC can run multiple slices during the 50ms window
 * as long as the total time it spends is at most 10ms.
 */
double
Statistics::computeMMU(int64_t window) const
{
    MOZ_ASSERT(!slices.empty());

    int64_t gc = slices[0].end - slices[0].start;
    int64_t gcMax = gc;

    if (gc >= window)
        return 0.0;

    int startIndex = 0;
    for (size_t endIndex = 1; endIndex < slices.length(); endIndex++) {
        gc += slices[endIndex].end - slices[endIndex].start;

        while (slices[endIndex].end - slices[startIndex].end >= window) {
            gc -= slices[startIndex].end - slices[startIndex].start;
            startIndex++;
        }

        int64_t cur = gc;
        if (slices[endIndex].end - slices[startIndex].start > window)
            cur -= (slices[endIndex].end - slices[startIndex].start - window);
        if (cur > gcMax)
            gcMax = cur;
    }

    return double(window - gcMax) / window;
}
