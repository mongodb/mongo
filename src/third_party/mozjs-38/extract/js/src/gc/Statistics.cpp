/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Statistics.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/PodOperations.h"
#include "mozilla/UniquePtr.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>

#include "jsprf.h"
#include "jsutil.h"
#include "prmjtime.h"

#include "gc/Memory.h"
#include "vm/HelperThreads.h"
#include "vm/Runtime.h"

using namespace js;
using namespace js::gc;
using namespace js::gcstats;

using mozilla::PodArrayZero;
using mozilla::PodZero;

/* Except for the first and last, slices of less than 10ms are not reported. */
static const int64_t SLICE_MIN_REPORT_TIME = 10 * PRMJ_USEC_PER_MSEC;

class gcstats::StatisticsSerializer
{
    typedef Vector<char, 128, SystemAllocPolicy> CharBuffer;
    CharBuffer buf_;
    bool asJSON_;
    bool needComma_;
    bool oom_;

    static const int MaxFieldValueLength = 128;

  public:
    enum Mode {
        AsJSON = true,
        AsText = false
    };

    explicit StatisticsSerializer(Mode asJSON)
      : buf_(), asJSON_(asJSON), needComma_(false), oom_(false)
    {}

    bool isJSON() { return asJSON_; }

    bool isOOM() { return oom_; }

    void endLine() {
        if (!asJSON_) {
            p("\n");
            needComma_ = false;
        }
    }

    void extra(const char* str) {
        if (!asJSON_) {
            needComma_ = false;
            p(str);
        }
    }

    void appendString(const char* name, const char* value) {
        put(name, value, "", true);
    }

    void appendNumber(const char* name, const char* vfmt, const char* units, ...) {
        va_list va;
        va_start(va, units);
        append(name, vfmt, va, units);
        va_end(va);
    }

    void appendDecimal(const char* name, const char* units, double d) {
        if (d < 0)
            d = 0;
        if (asJSON_)
            appendNumber(name, "%d.%d", units, (int)d, (int)(d * 10.) % 10);
        else
            appendNumber(name, "%.1f", units, d);
    }

    void appendIfNonzeroMS(const char* name, double v) {
        if (asJSON_ || v >= 0.1)
            appendDecimal(name, "ms", v);
    }

    void beginObject(const char* name) {
        if (needComma_)
            pJSON(", ");
        if (asJSON_ && name) {
            putKey(name);
            pJSON(": ");
        }
        pJSON("{");
        needComma_ = false;
    }

    void endObject() {
        needComma_ = false;
        pJSON("}");
        needComma_ = true;
    }

    void beginArray(const char* name) {
        if (needComma_)
            pJSON(", ");
        if (asJSON_)
            putKey(name);
        pJSON(": [");
        needComma_ = false;
    }

    void endArray() {
        needComma_ = false;
        pJSON("]");
        needComma_ = true;
    }

    char16_t* finishJSString() {
        char* buf = finishCString();
        if (!buf)
            return nullptr;

        size_t nchars = strlen(buf);
        char16_t* out = js_pod_malloc<char16_t>(nchars + 1);
        if (!out) {
            oom_ = true;
            js_free(buf);
            return nullptr;
        }

        CopyAndInflateChars(out, buf, nchars);
        js_free(buf);

        out[nchars] = 0;
        return out;
    }

    char* finishCString() {
        if (oom_)
            return nullptr;

        buf_.append('\0');

        char* buf = buf_.extractRawBuffer();
        if (!buf)
            oom_ = true;

        return buf;
    }

  private:
    void append(const char* name, const char* vfmt,
                va_list va, const char* units)
    {
        char val[MaxFieldValueLength];
        JS_vsnprintf(val, MaxFieldValueLength, vfmt, va);
        put(name, val, units, false);
    }

    void p(const char* cstr) {
        if (oom_)
            return;

        if (!buf_.append(cstr, strlen(cstr)))
            oom_ = true;
    }

    void p(const char c) {
        if (oom_)
            return;

        if (!buf_.append(c))
            oom_ = true;
    }

    void pJSON(const char* str) {
        if (asJSON_)
            p(str);
    }

    void put(const char* name, const char* val, const char* units, bool valueIsQuoted) {
        if (needComma_)
            p(", ");
        needComma_ = true;

        putKey(name);
        p(": ");
        if (valueIsQuoted)
            putQuoted(val);
        else
            p(val);
        if (!asJSON_)
            p(units);
    }

    void putQuoted(const char* str) {
        pJSON("\"");
        p(str);
        pJSON("\"");
    }

    void putKey(const char* str) {
        if (!asJSON_) {
            p(str);
            return;
        }

        p("\"");
        const char* c = str;
        while (*c) {
            if (*c == ' ' || *c == '\t')
                p('_');
            else if (isupper(*c))
                p(tolower(*c));
            else if (*c == '+')
                p("added_");
            else if (*c == '-')
                p("removed_");
            else if (*c != '(' && *c != ')')
                p(*c);
            c++;
        }
        p("\"");
    }
};

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
    { PHASE_MUTATOR, "Mutator Running", PHASE_NO_PARENT },
    { PHASE_GC_BEGIN, "Begin Callback", PHASE_NO_PARENT },
    { PHASE_WAIT_BACKGROUND_THREAD, "Wait Background Thread", PHASE_NO_PARENT },
    { PHASE_MARK_DISCARD_CODE, "Mark Discard Code", PHASE_NO_PARENT },
    { PHASE_PURGE, "Purge", PHASE_NO_PARENT },
    { PHASE_MARK, "Mark", PHASE_NO_PARENT },
        { PHASE_UNMARK, "Unmark", PHASE_MARK },
        /* PHASE_MARK_ROOTS */
        { PHASE_MARK_DELAYED, "Mark Delayed", PHASE_MARK },
    { PHASE_SWEEP, "Sweep", PHASE_NO_PARENT },
        { PHASE_SWEEP_MARK, "Mark During Sweeping", PHASE_SWEEP },
            { PHASE_SWEEP_MARK_TYPES, "Mark Types During Sweeping", PHASE_SWEEP_MARK },
            { PHASE_SWEEP_MARK_INCOMING_BLACK, "Mark Incoming Black Pointers", PHASE_SWEEP_MARK },
            { PHASE_SWEEP_MARK_WEAK, "Mark Weak", PHASE_SWEEP_MARK },
            { PHASE_SWEEP_MARK_INCOMING_GRAY, "Mark Incoming Gray Pointers", PHASE_SWEEP_MARK },
            { PHASE_SWEEP_MARK_GRAY, "Mark Gray", PHASE_SWEEP_MARK },
            { PHASE_SWEEP_MARK_GRAY_WEAK, "Mark Gray and Weak", PHASE_SWEEP_MARK },
        { PHASE_FINALIZE_START, "Finalize Start Callback", PHASE_SWEEP },
        { PHASE_SWEEP_ATOMS, "Sweep Atoms", PHASE_SWEEP },
        { PHASE_SWEEP_SYMBOL_REGISTRY, "Sweep Symbol Registry", PHASE_SWEEP },
        { PHASE_SWEEP_COMPARTMENTS, "Sweep Compartments", PHASE_SWEEP },
            { PHASE_SWEEP_DISCARD_CODE, "Sweep Discard Code", PHASE_SWEEP_COMPARTMENTS },
            { PHASE_SWEEP_INNER_VIEWS, "Sweep Inner Views", PHASE_SWEEP_COMPARTMENTS },
            { PHASE_SWEEP_CC_WRAPPER, "Sweep Cross Compartment Wrappers", PHASE_SWEEP_COMPARTMENTS },
            { PHASE_SWEEP_BASE_SHAPE, "Sweep Base Shapes", PHASE_SWEEP_COMPARTMENTS },
            { PHASE_SWEEP_INITIAL_SHAPE, "Sweep Initial Shapes", PHASE_SWEEP_COMPARTMENTS },
            { PHASE_SWEEP_TYPE_OBJECT, "Sweep Type Objects", PHASE_SWEEP_COMPARTMENTS },
            { PHASE_SWEEP_BREAKPOINT, "Sweep Breakpoints", PHASE_SWEEP_COMPARTMENTS },
            { PHASE_SWEEP_REGEXP, "Sweep Regexps", PHASE_SWEEP_COMPARTMENTS },
            { PHASE_SWEEP_MISC, "Sweep Miscellaneous", PHASE_SWEEP_COMPARTMENTS },
            { PHASE_SWEEP_TYPES, "Sweep type information", PHASE_SWEEP_COMPARTMENTS },
                { PHASE_SWEEP_TYPES_BEGIN, "Sweep type tables and compilations", PHASE_SWEEP_TYPES },
                { PHASE_SWEEP_TYPES_END, "Free type arena", PHASE_SWEEP_TYPES },
        { PHASE_SWEEP_OBJECT, "Sweep Object", PHASE_SWEEP },
        { PHASE_SWEEP_STRING, "Sweep String", PHASE_SWEEP },
        { PHASE_SWEEP_SCRIPT, "Sweep Script", PHASE_SWEEP },
        { PHASE_SWEEP_SHAPE, "Sweep Shape", PHASE_SWEEP },
        { PHASE_SWEEP_JITCODE, "Sweep JIT code", PHASE_SWEEP },
        { PHASE_FINALIZE_END, "Finalize End Callback", PHASE_SWEEP },
        { PHASE_DESTROY, "Deallocate", PHASE_SWEEP },
    { PHASE_COMPACT, "Compact", PHASE_NO_PARENT },
        { PHASE_COMPACT_MOVE, "Compact Move", PHASE_COMPACT },
        { PHASE_COMPACT_UPDATE, "Compact Update", PHASE_COMPACT },
            /* PHASE_MARK_ROOTS */
            { PHASE_COMPACT_UPDATE_CELLS, "Compact Update Cells", PHASE_COMPACT_UPDATE, },
    { PHASE_GC_END, "End Callback", PHASE_NO_PARENT },
    { PHASE_MINOR_GC, "All Minor GCs", PHASE_NO_PARENT },
        /* PHASE_MARK_ROOTS */
    { PHASE_EVICT_NURSERY, "Minor GCs to Evict Nursery", PHASE_NO_PARENT },
        /* PHASE_MARK_ROOTS */
    { PHASE_TRACE_HEAP, "Trace Heap", PHASE_NO_PARENT },
        /* PHASE_MARK_ROOTS */
    { PHASE_MARK_ROOTS, "Mark Roots", PHASE_MULTI_PARENTS },
        { PHASE_MARK_CCWS, "Mark Cross Compartment Wrappers", PHASE_MARK_ROOTS },
        { PHASE_MARK_ROOTERS, "Mark Rooters", PHASE_MARK_ROOTS },
        { PHASE_MARK_RUNTIME_DATA, "Mark Runtime-wide Data", PHASE_MARK_ROOTS },
        { PHASE_MARK_EMBEDDING, "Mark Embedding", PHASE_MARK_ROOTS },
        { PHASE_MARK_COMPARTMENTS, "Mark Compartments", PHASE_MARK_ROOTS },
    { PHASE_LIMIT, nullptr, PHASE_NO_PARENT }
};

static ExtraPhaseInfo phaseExtra[PHASE_LIMIT] = { { 0, 0 } };

// Mapping from all nodes with a multi-parented child to a Vector of all
// multi-parented children and their descendants. (Single-parented children will
// not show up in this list.)
static mozilla::Vector<Phase> dagDescendants[Statistics::MAX_MULTIPARENT_PHASES + 1];

struct AllPhaseIterator {
    int current;
    int baseLevel;
    size_t activeSlot;
    mozilla::Vector<Phase>::Range descendants;

    explicit AllPhaseIterator(int64_t (*table)[PHASE_LIMIT])
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

static void
FormatPhaseTimes(StatisticsSerializer& ss, const char* name, int64_t (*times)[PHASE_LIMIT])
{
    ss.beginObject(name);

    for (AllPhaseIterator iter(times); !iter.done(); iter.advance()) {
        Phase phase;
        size_t dagSlot;
        iter.get(&phase, &dagSlot);
        ss.appendIfNonzeroMS(phases[phase].name, t(times[dagSlot][phase]));
    }
    ss.endObject();
}

void
Statistics::gcDuration(int64_t* total, int64_t* maxPause)
{
    *total = *maxPause = 0;
    for (SliceData* slice = slices.begin(); slice != slices.end(); slice++) {
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

bool
Statistics::formatData(StatisticsSerializer& ss, uint64_t timestamp)
{
    MOZ_ASSERT(!aborted);

    int64_t total, longest;
    gcDuration(&total, &longest);

    int64_t sccTotal, sccLongest;
    sccDurations(&sccTotal, &sccLongest);

    double mmu20 = computeMMU(20 * PRMJ_USEC_PER_MSEC);
    double mmu50 = computeMMU(50 * PRMJ_USEC_PER_MSEC);

    ss.beginObject(nullptr);
    if (ss.isJSON())
        ss.appendNumber("Timestamp", "%llu", "", (unsigned long long)timestamp);
    if (slices.length() > 1 || ss.isJSON())
        ss.appendDecimal("Max Pause", "ms", t(longest));
    else
        ss.appendString("Reason", ExplainReason(slices[0].reason));
    ss.appendDecimal("Total Time", "ms", t(total));
    ss.appendNumber("Zones Collected", "%d", "", zoneStats.collectedZoneCount);
    ss.appendNumber("Total Zones", "%d", "", zoneStats.zoneCount);
    ss.appendNumber("Total Compartments", "%d", "", zoneStats.compartmentCount);
    ss.appendNumber("Minor GCs", "%d", "", counts[STAT_MINOR_GC]);
    ss.appendNumber("Store Buffer Overflows", "%d", "", counts[STAT_STOREBUFFER_OVERFLOW]);
    ss.appendNumber("MMU (20ms)", "%d", "%", int(mmu20 * 100));
    ss.appendNumber("MMU (50ms)", "%d", "%", int(mmu50 * 100));
    ss.appendDecimal("SCC Sweep Total", "ms", t(sccTotal));
    ss.appendDecimal("SCC Sweep Max Pause", "ms", t(sccLongest));
    if (nonincrementalReason || ss.isJSON()) {
        ss.appendString("Nonincremental Reason",
                        nonincrementalReason ? nonincrementalReason : "none");
    }
    ss.appendNumber("Allocated", "%u", "MB", unsigned(preBytes / 1024 / 1024));
    ss.appendNumber("+Chunks", "%d", "", counts[STAT_NEW_CHUNK]);
    ss.appendNumber("-Chunks", "%d", "", counts[STAT_DESTROY_CHUNK]);
    ss.endLine();

    if (slices.length() > 1 || ss.isJSON()) {
        ss.beginArray("Slices");
        for (size_t i = 0; i < slices.length(); i++) {
            int64_t width = slices[i].duration();
            if (i != 0 && i != slices.length() - 1 && width < SLICE_MIN_REPORT_TIME &&
                !slices[i].resetReason && !ss.isJSON())
            {
                continue;
            }

            ss.beginObject(nullptr);
            ss.extra("    ");
            ss.appendNumber("Slice", "%d", "", i);
            ss.appendDecimal("Pause", "", t(width));
            ss.extra(" (");
            ss.appendDecimal("When", "ms", t(slices[i].start - slices[0].start));
            ss.appendString("Reason", ExplainReason(slices[i].reason));
            if (ss.isJSON()) {
                ss.appendDecimal("Page Faults", "",
                                 double(slices[i].endFaults - slices[i].startFaults));

                ss.appendNumber("Start Timestamp", "%llu", "", (unsigned long long)slices[i].start);
                ss.appendNumber("End Timestamp", "%llu", "", (unsigned long long)slices[i].end);
            }
            if (slices[i].resetReason)
                ss.appendString("Reset", slices[i].resetReason);
            ss.extra("): ");
            FormatPhaseTimes(ss, "Times", slices[i].phaseTimes);
            ss.endLine();
            ss.endObject();
        }
        ss.endArray();
    }
    ss.extra("    Totals: ");
    FormatPhaseTimes(ss, "Totals", phaseTimes);
    ss.endObject();

    return !ss.isOOM();
}

typedef Vector<UniqueChars, 8, SystemAllocPolicy> FragmentVector;

static UniqueChars
Join(const FragmentVector& fragments) {
    size_t length = 0;
    for (size_t i = 0; i < fragments.length(); ++i)
        length += fragments[i] ? strlen(fragments[i].get()) : 0;

    char* joined = js_pod_malloc<char>(length + 1);
    joined[length] = '\0';

    char* cursor = joined;
    for (size_t i = 0; i < fragments.length(); ++i) {
        if (fragments[i])
            strcpy(cursor, fragments[i].get());
        cursor += fragments[i] ? strlen(fragments[i].get()) : 0;
    }

    return UniqueChars(joined);
}

UniqueChars
Statistics::formatDescription()
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
                nonincrementalReason ? "no - " : "yes",
                                                  nonincrementalReason ? nonincrementalReason : "",
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
Statistics::formatSliceDescription(unsigned i, const SliceData& slice)
{
    const char* format =
"\
  ---- Slice %u ----\n\
    Reason: %s\n\
    Reset: %s%s\n\
    Page Faults: %ld\n\
    Pause: %.3fms  (@ %.3fms)\n\
";
    char buffer[1024];
    memset(buffer, 0, sizeof(buffer));
    JS_snprintf(buffer, sizeof(buffer), format, i,
                ExplainReason(slice.reason),
                slice.resetReason ? "yes - " : "no", slice.resetReason ? slice.resetReason : "",
                uint64_t(slice.endFaults - slice.startFaults),
                t(slice.duration()), t(slice.start - slices[0].start));
    return make_string_copy(buffer);
}

UniqueChars
Statistics::formatTotals()
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

static int64_t
SumChildTimes(size_t phaseSlot, Phase phase, int64_t (*phaseTimes)[PHASE_LIMIT])
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
Statistics::formatPhaseTimes(int64_t (*phaseTimes)[PHASE_LIMIT])
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
Statistics::formatDetailedMessage()
{
    FragmentVector fragments;

    if (!fragments.append(formatDescription()))
        return UniqueChars(nullptr);

    if (slices.length() > 1) {
        for (unsigned i = 0; i < slices.length(); i++) {
            if (!fragments.append(formatSliceDescription(i, slices[i])))
                return UniqueChars(nullptr);
            if (!fragments.append(formatPhaseTimes(slices[i].phaseTimes)))
                return UniqueChars(nullptr);
        }
    }
    if (!fragments.append(formatTotals()))
        return UniqueChars(nullptr);
    if (!fragments.append(formatPhaseTimes(phaseTimes)))
        return UniqueChars(nullptr);

    return Join(fragments);
}

char16_t*
Statistics::formatMessage()
{
    StatisticsSerializer ss(StatisticsSerializer::AsText);
    formatData(ss, 0);
    return ss.finishJSString();
}

char16_t*
Statistics::formatJSON(uint64_t timestamp)
{
    StatisticsSerializer ss(StatisticsSerializer::AsJSON);
    formatData(ss, timestamp);
    return ss.finishJSString();
}

Statistics::Statistics(JSRuntime* rt)
  : runtime(rt),
    startupTime(PRMJ_Now()),
    fp(nullptr),
    fullFormat(false),
    gcDepth(0),
    nonincrementalReason(nullptr),
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
    for (size_t d = 0; d < MAX_MULTIPARENT_PHASES + 1; d++)
        PodArrayZero(phaseTimes[d]);

    static bool initialized = false;
    if (!initialized) {
        initialized = true;

        for (size_t i = 0; i < PHASE_LIMIT; i++)
            MOZ_ASSERT(phases[i].index == i);

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
        MOZ_ASSERT(dagSlot <= MAX_MULTIPARENT_PHASES);

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
    if (!env || strcmp(env, "none") == 0) {
        fp = nullptr;
        return;
    }

    if (strcmp(env, "stdout") == 0) {
        fullFormat = false;
        fp = stdout;
    } else if (strcmp(env, "stderr") == 0) {
        fullFormat = false;
        fp = stderr;
    } else {
        fullFormat = true;

        fp = fopen(env, "a");
        MOZ_ASSERT(fp);
    }
}

Statistics::~Statistics()
{
    if (fp) {
        if (fullFormat) {
            StatisticsSerializer ss(StatisticsSerializer::AsText);
            FormatPhaseTimes(ss, "", phaseTotals);
            char* msg = ss.finishCString();
            if (msg) {
                fprintf(fp, "TOTALS\n%s\n\n-------\n", msg);
                js_free(msg);
            }
        }

        if (fp != stdout && fp != stderr)
            fclose(fp);
    }
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
SumPhase(Phase phase, int64_t (*times)[PHASE_LIMIT])
{
    int64_t sum = 0;
    for (size_t i = 0; i < Statistics::MAX_MULTIPARENT_PHASES + 1; i++)
        sum += times[i][phase];
    return sum;
}

void
Statistics::printStats()
{
    if (aborted) {
        if (fullFormat)
            fprintf(fp, "OOM during GC statistics collection. The report is unavailable for this GC.\n");
        fflush(fp);
        return;
    }

    if (fullFormat) {
        UniqueChars msg = formatDetailedMessage();
        if (msg)
            fprintf(fp, "GC(T+%.3fs) %s\n", t(slices[0].start - startupTime) / 1000.0, msg.get());
    } else {
        int64_t total, longest;
        gcDuration(&total, &longest);

        int64_t markTotal = SumPhase(PHASE_MARK, phaseTimes);
        fprintf(fp, "%f %f %f\n",
                t(total),
                t(markTotal),
                t(phaseTimes[PHASE_DAG_NONE][PHASE_SWEEP]));
        MOZ_ASSERT(phaseExtra[PHASE_SWEEP].dagSlot == PHASE_DAG_NONE);
    }
    fflush(fp);
}

void
Statistics::beginGC(JSGCInvocationKind kind)
{
    slices.clearAndFree();
    sccTimes.clearAndFree();
    gckind = kind;
    nonincrementalReason = nullptr;

    preBytes = runtime->gc.usage.gcBytes();
}

void
Statistics::endGC()
{
    for (size_t j = 0; j < MAX_MULTIPARENT_PHASES + 1; j++)
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
    runtime->addTelemetry(JS_TELEMETRY_GC_NON_INCREMENTAL, !!nonincrementalReason);
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
    for (size_t d = PHASE_DAG_NONE; d < MAX_MULTIPARENT_PHASES + 1; d++)
        PodZero(&phaseTimes[d][PHASE_GC_BEGIN], PHASE_LIMIT - PHASE_GC_BEGIN);

    aborted = false;
}

void
Statistics::beginSlice(const ZoneGCStats& zoneStats, JSGCInvocationKind gckind,
                       JS::gcreason::Reason reason)
{
    this->zoneStats = zoneStats;

    bool first = !runtime->gc.isIncrementalGCInProgress();
    if (first)
        beginGC(gckind);

    SliceData data(reason, PRMJ_Now(), GetPageFaultCount());
    if (!slices.append(data)) {
        // OOM testing fails if we CrashAtUnhandlableOOM here.
        aborted = true;
        return;
    }

    runtime->addTelemetry(JS_TELEMETRY_GC_REASON, reason);

    // Slice callbacks should only fire for the outermost level
    if (++gcDepth == 1) {
        bool wasFullGC = zoneStats.isCollectingAllZones();
        if (sliceCallback)
            (*sliceCallback)(runtime, first ? JS::GC_CYCLE_BEGIN : JS::GC_SLICE_BEGIN,
                             JS::GCDescription(!wasFullGC, gckind));
    }
}

void
Statistics::endSlice()
{
    if (!aborted) {
        slices.back().end = PRMJ_Now();
        slices.back().endFaults = GetPageFaultCount();

        runtime->addTelemetry(JS_TELEMETRY_GC_SLICE_MS, t(slices.back().end - slices.back().start));
        runtime->addTelemetry(JS_TELEMETRY_GC_RESET, !!slices.back().resetReason);
    }

    bool last = !runtime->gc.isIncrementalGCInProgress();
    if (last)
        endGC();

    // Slice callbacks should only fire for the outermost level
    if (--gcDepth == 0) {
        bool wasFullGC = zoneStats.isCollectingAllZones();
        if (sliceCallback)
            (*sliceCallback)(runtime, last ? JS::GC_CYCLE_END : JS::GC_SLICE_END,
                             JS::GCDescription(!wasFullGC, gckind));
    }

    /* Do this after the slice callback since it uses these values. */
    if (last)
        PodArrayZero(counts);
}

void
Statistics::startTimingMutator()
{
    // Should only be called from outside of GC
    MOZ_ASSERT(phaseNestingDepth == 0);
    MOZ_ASSERT(suspendedPhaseNestingDepth == 0);

    timedGCTime = 0;
    phaseStartTimes[PHASE_MUTATOR] = 0;
    phaseTimes[PHASE_DAG_NONE][PHASE_MUTATOR] = 0;
    timedGCStart = 0;

    beginPhase(PHASE_MUTATOR);
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
Statistics::computeMMU(int64_t window)
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
