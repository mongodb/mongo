/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Statistics.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/Sprintf.h"
#include "mozilla/TimeStamp.h"

#include <algorithm>
#include <stdarg.h>
#include <stdio.h>
#include <type_traits>

#include "debugger/DebugAPI.h"
#include "gc/GC.h"
#include "gc/GCInternals.h"
#include "gc/Memory.h"
#include "js/friend/UsageStatistics.h"  // JS_TELEMETRY_*
#include "util/GetPidProvider.h"
#include "util/Text.h"
#include "vm/HelperThreads.h"
#include "vm/Runtime.h"
#include "vm/Time.h"

#include "gc/PrivateIterators-inl.h"

using namespace js;
using namespace js::gc;
using namespace js::gcstats;

using mozilla::DebugOnly;
using mozilla::EnumeratedArray;
using mozilla::Maybe;
using mozilla::TimeDuration;
using mozilla::TimeStamp;

static const size_t BYTES_PER_MB = 1024 * 1024;

/*
 * If this fails, then you can either delete this assertion and allow all
 * larger-numbered reasons to pile up in the last telemetry bucket, or switch
 * to GC_REASON_3 and bump the max value.
 */
static_assert(JS::GCReason::NUM_TELEMETRY_REASONS >= JS::GCReason::NUM_REASONS);

static inline auto AllPhaseKinds() {
  return mozilla::MakeEnumeratedRange(PhaseKind::FIRST, PhaseKind::LIMIT);
}

static inline auto MajorGCPhaseKinds() {
  return mozilla::MakeEnumeratedRange(PhaseKind::GC_BEGIN,
                                      PhaseKind(size_t(PhaseKind::GC_END) + 1));
}

static const char* ExplainGCOptions(JS::GCOptions options) {
  switch (options) {
    case JS::GCOptions::Normal:
      return "Normal";
    case JS::GCOptions::Shrink:
      return "Shrink";
  }

  MOZ_CRASH("Unexpected GCOptions value");
}

JS_PUBLIC_API const char* JS::ExplainGCReason(JS::GCReason reason) {
  switch (reason) {
#define SWITCH_REASON(name, _) \
  case JS::GCReason::name:     \
    return #name;
    GCREASONS(SWITCH_REASON)
#undef SWITCH_REASON

    case JS::GCReason::NO_REASON:
      return "NO_REASON";

    default:
      MOZ_CRASH("bad GC reason");
  }
}

JS_PUBLIC_API bool JS::InternalGCReason(JS::GCReason reason) {
  return reason < JS::GCReason::FIRST_FIREFOX_REASON;
}

const char* js::gcstats::ExplainAbortReason(GCAbortReason reason) {
  switch (reason) {
#define SWITCH_REASON(name, _) \
  case GCAbortReason::name:    \
    return #name;
    GC_ABORT_REASONS(SWITCH_REASON)

    default:
      MOZ_CRASH("bad GC abort reason");
#undef SWITCH_REASON
  }
}

static FILE* MaybeOpenFileFromEnv(const char* env) {
  FILE* file;
  const char* value = getenv(env);

  if (!value) {
    return nullptr;
  }

  if (strcmp(value, "none") == 0) {
    file = nullptr;
  } else if (strcmp(value, "stdout") == 0) {
    file = stdout;
  } else if (strcmp(value, "stderr") == 0) {
    file = stderr;
  } else {
    char path[300];
    if (value[0] != '/') {
      const char* dir = getenv("MOZ_UPLOAD_DIR");
      if (dir) {
        SprintfLiteral(path, "%s/%s", dir, value);
        value = path;
      }
    }

    file = fopen(value, "a");
    if (!file) {
      perror("opening log file");
      MOZ_CRASH("Failed to open log file.");
    }
  }

  return file;
}

struct PhaseKindInfo {
  Phase firstPhase;
  uint8_t telemetryBucket;
  const char* name;
};

// PhaseInfo objects form a tree.
struct PhaseInfo {
  Phase parent;
  Phase firstChild;
  Phase nextSibling;
  Phase nextWithPhaseKind;
  PhaseKind phaseKind;
  uint8_t depth;
  const char* name;
  const char* path;
};

// A table of PhaseInfo indexed by Phase.
using PhaseTable = EnumeratedArray<Phase, Phase::LIMIT, PhaseInfo>;

// A table of PhaseKindInfo indexed by PhaseKind.
using PhaseKindTable =
    EnumeratedArray<PhaseKind, PhaseKind::LIMIT, PhaseKindInfo>;

#include "gc/StatsPhasesGenerated.inc"

// Iterate the phases in a phase kind.
class PhaseIter {
  Phase phase;

 public:
  explicit PhaseIter(PhaseKind kind) : phase(phaseKinds[kind].firstPhase) {}
  bool done() const { return phase == Phase::NONE; }
  void next() { phase = phases[phase].nextWithPhaseKind; }
  Phase get() const { return phase; }
  operator Phase() const { return phase; }
};

static double t(TimeDuration duration) { return duration.ToMilliseconds(); }

inline JSContext* Statistics::context() {
  return gc->rt->mainContextFromOwnThread();
}

inline Phase Statistics::currentPhase() const {
  return phaseStack.empty() ? Phase::NONE : phaseStack.back();
}

PhaseKind Statistics::currentPhaseKind() const {
  // Public API to get the current phase kind, suppressing the synthetic
  // PhaseKind::MUTATOR phase.

  Phase phase = currentPhase();
  MOZ_ASSERT_IF(phase == Phase::MUTATOR, phaseStack.length() == 1);
  if (phase == Phase::NONE || phase == Phase::MUTATOR) {
    return PhaseKind::NONE;
  }

  return phases[phase].phaseKind;
}

static Phase LookupPhaseWithParent(PhaseKind phaseKind, Phase parentPhase) {
  for (PhaseIter phase(phaseKind); !phase.done(); phase.next()) {
    if (phases[phase].parent == parentPhase) {
      return phase;
    }
  }

  return Phase::NONE;
}

static const char* PhaseKindName(PhaseKind kind) {
  if (kind == PhaseKind::NONE) {
    return "NONE";
  }

  return phaseKinds[kind].name;
}

Phase Statistics::lookupChildPhase(PhaseKind phaseKind) const {
  if (phaseKind == PhaseKind::IMPLICIT_SUSPENSION) {
    return Phase::IMPLICIT_SUSPENSION;
  }
  if (phaseKind == PhaseKind::EXPLICIT_SUSPENSION) {
    return Phase::EXPLICIT_SUSPENSION;
  }

  MOZ_ASSERT(phaseKind < PhaseKind::LIMIT);

  // Search all expanded phases that correspond to the required
  // phase to find the one whose parent is the current expanded phase.
  Phase phase = LookupPhaseWithParent(phaseKind, currentPhase());

  if (phase == Phase::NONE) {
    MOZ_CRASH_UNSAFE_PRINTF(
        "Child phase kind %s not found under current phase kind %s",
        PhaseKindName(phaseKind), PhaseKindName(currentPhaseKind()));
  }

  return phase;
}

inline auto AllPhases() {
  return mozilla::MakeEnumeratedRange(Phase::FIRST, Phase::LIMIT);
}

void Statistics::gcDuration(TimeDuration* total, TimeDuration* maxPause) const {
  *total = *maxPause = 0;
  for (auto& slice : slices_) {
    *total += slice.duration();
    if (slice.duration() > *maxPause) {
      *maxPause = slice.duration();
    }
  }
  if (*maxPause > maxPauseInInterval) {
    maxPauseInInterval = *maxPause;
  }
}

void Statistics::sccDurations(TimeDuration* total,
                              TimeDuration* maxPause) const {
  *total = *maxPause = 0;
  for (size_t i = 0; i < sccTimes.length(); i++) {
    *total += sccTimes[i];
    *maxPause = std::max(*maxPause, sccTimes[i]);
  }
}

typedef Vector<UniqueChars, 8, SystemAllocPolicy> FragmentVector;

static UniqueChars Join(const FragmentVector& fragments,
                        const char* separator = "") {
  const size_t separatorLength = strlen(separator);
  size_t length = 0;
  for (size_t i = 0; i < fragments.length(); ++i) {
    length += fragments[i] ? strlen(fragments[i].get()) : 0;
    if (i < (fragments.length() - 1)) {
      length += separatorLength;
    }
  }

  char* joined = js_pod_malloc<char>(length + 1);
  if (!joined) {
    return UniqueChars();
  }

  joined[length] = '\0';
  char* cursor = joined;
  for (size_t i = 0; i < fragments.length(); ++i) {
    if (fragments[i]) {
      strcpy(cursor, fragments[i].get());
    }
    cursor += fragments[i] ? strlen(fragments[i].get()) : 0;
    if (i < (fragments.length() - 1)) {
      if (separatorLength) {
        strcpy(cursor, separator);
      }
      cursor += separatorLength;
    }
  }

  return UniqueChars(joined);
}

static TimeDuration SumChildTimes(Phase phase,
                                  const Statistics::PhaseTimes& phaseTimes) {
  TimeDuration total = 0;
  for (phase = phases[phase].firstChild; phase != Phase::NONE;
       phase = phases[phase].nextSibling) {
    total += phaseTimes[phase];
  }
  return total;
}

UniqueChars Statistics::formatCompactSliceMessage() const {
  // Skip if we OOM'ed.
  if (slices_.length() == 0) {
    return UniqueChars(nullptr);
  }

  const size_t index = slices_.length() - 1;
  const SliceData& slice = slices_.back();

  char budgetDescription[200];
  slice.budget.describe(budgetDescription, sizeof(budgetDescription) - 1);

  const char* format =
      "GC Slice %u - Pause: %.3fms of %s budget (@ %.3fms); Reason: %s; Reset: "
      "%s%s; Times: ";
  char buffer[1024];
  SprintfLiteral(buffer, format, index, t(slice.duration()), budgetDescription,
                 t(slice.start - slices_[0].start),
                 ExplainGCReason(slice.reason),
                 slice.wasReset() ? "yes - " : "no",
                 slice.wasReset() ? ExplainAbortReason(slice.resetReason) : "");

  FragmentVector fragments;
  if (!fragments.append(DuplicateString(buffer)) ||
      !fragments.append(
          formatCompactSlicePhaseTimes(slices_[index].phaseTimes))) {
    return UniqueChars(nullptr);
  }
  return Join(fragments);
}

UniqueChars Statistics::formatCompactSummaryMessage() const {
  FragmentVector fragments;
  if (!fragments.append(DuplicateString("Summary - "))) {
    return UniqueChars(nullptr);
  }

  TimeDuration total, longest;
  gcDuration(&total, &longest);

  const double mmu20 = computeMMU(TimeDuration::FromMilliseconds(20));
  const double mmu50 = computeMMU(TimeDuration::FromMilliseconds(50));

  char buffer[1024];
  if (!nonincremental()) {
    SprintfLiteral(buffer,
                   "Max Pause: %.3fms; MMU 20ms: %.1f%%; MMU 50ms: %.1f%%; "
                   "Total: %.3fms; ",
                   t(longest), mmu20 * 100., mmu50 * 100., t(total));
  } else {
    SprintfLiteral(buffer, "Non-Incremental: %.3fms (%s); ", t(total),
                   ExplainAbortReason(nonincrementalReason_));
  }
  if (!fragments.append(DuplicateString(buffer))) {
    return UniqueChars(nullptr);
  }

  SprintfLiteral(buffer,
                 "Zones: %d of %d (-%d); Compartments: %d of %d (-%d); "
                 "HeapSize: %.3f MiB; "
                 "HeapChange (abs): %+d (%u); ",
                 zoneStats.collectedZoneCount, zoneStats.zoneCount,
                 zoneStats.sweptZoneCount, zoneStats.collectedCompartmentCount,
                 zoneStats.compartmentCount, zoneStats.sweptCompartmentCount,
                 double(preTotalHeapBytes) / BYTES_PER_MB,
                 int32_t(counts[COUNT_NEW_CHUNK] - counts[COUNT_DESTROY_CHUNK]),
                 counts[COUNT_NEW_CHUNK] + counts[COUNT_DESTROY_CHUNK]);
  if (!fragments.append(DuplicateString(buffer))) {
    return UniqueChars(nullptr);
  }

  MOZ_ASSERT_IF(counts[COUNT_ARENA_RELOCATED],
                gcOptions == JS::GCOptions::Shrink);
  if (gcOptions == JS::GCOptions::Shrink) {
    SprintfLiteral(
        buffer, "Kind: %s; Relocated: %.3f MiB; ", ExplainGCOptions(gcOptions),
        double(ArenaSize * counts[COUNT_ARENA_RELOCATED]) / BYTES_PER_MB);
    if (!fragments.append(DuplicateString(buffer))) {
      return UniqueChars(nullptr);
    }
  }

  return Join(fragments);
}

UniqueChars Statistics::formatCompactSlicePhaseTimes(
    const PhaseTimes& phaseTimes) const {
  static const TimeDuration MaxUnaccountedTime =
      TimeDuration::FromMicroseconds(100);

  FragmentVector fragments;
  char buffer[128];
  for (auto phase : AllPhases()) {
    DebugOnly<uint8_t> level = phases[phase].depth;
    MOZ_ASSERT(level < 4);

    TimeDuration ownTime = phaseTimes[phase];
    TimeDuration childTime = SumChildTimes(phase, phaseTimes);
    if (ownTime > MaxUnaccountedTime) {
      SprintfLiteral(buffer, "%s: %.3fms", phases[phase].name, t(ownTime));
      if (!fragments.append(DuplicateString(buffer))) {
        return UniqueChars(nullptr);
      }

      if (childTime && (ownTime - childTime) > MaxUnaccountedTime) {
        MOZ_ASSERT(level < 3);
        SprintfLiteral(buffer, "%s: %.3fms", "Other", t(ownTime - childTime));
        if (!fragments.append(DuplicateString(buffer))) {
          return UniqueChars(nullptr);
        }
      }
    }
  }
  return Join(fragments, ", ");
}

UniqueChars Statistics::formatDetailedMessage() const {
  FragmentVector fragments;

  if (!fragments.append(formatDetailedDescription())) {
    return UniqueChars(nullptr);
  }

  if (!slices_.empty()) {
    for (unsigned i = 0; i < slices_.length(); i++) {
      if (!fragments.append(formatDetailedSliceDescription(i, slices_[i]))) {
        return UniqueChars(nullptr);
      }
      if (!fragments.append(formatDetailedPhaseTimes(slices_[i].phaseTimes))) {
        return UniqueChars(nullptr);
      }
    }
  }
  if (!fragments.append(formatDetailedTotals())) {
    return UniqueChars(nullptr);
  }
  if (!fragments.append(formatDetailedPhaseTimes(phaseTimes))) {
    return UniqueChars(nullptr);
  }

  return Join(fragments);
}

UniqueChars Statistics::formatDetailedDescription() const {
  TimeDuration sccTotal, sccLongest;
  sccDurations(&sccTotal, &sccLongest);

  const double mmu20 = computeMMU(TimeDuration::FromMilliseconds(20));
  const double mmu50 = computeMMU(TimeDuration::FromMilliseconds(50));

  const char* format =
      "=================================================================\n\
  Invocation Kind: %s\n\
  Reason: %s\n\
  Incremental: %s%s\n\
  Zones Collected: %d of %d (-%d)\n\
  Compartments Collected: %d of %d (-%d)\n\
  MinorGCs since last GC: %d\n\
  Store Buffer Overflows: %d\n\
  MMU 20ms:%.1f%%; 50ms:%.1f%%\n\
  SCC Sweep Total (MaxPause): %.3fms (%.3fms)\n\
  HeapSize: %.3f MiB\n\
  Chunk Delta (magnitude): %+d  (%d)\n\
  Arenas Relocated: %.3f MiB\n\
";

  char buffer[1024];
  SprintfLiteral(
      buffer, format, ExplainGCOptions(gcOptions),
      ExplainGCReason(slices_[0].reason), nonincremental() ? "no - " : "yes",
      nonincremental() ? ExplainAbortReason(nonincrementalReason_) : "",
      zoneStats.collectedZoneCount, zoneStats.zoneCount,
      zoneStats.sweptZoneCount, zoneStats.collectedCompartmentCount,
      zoneStats.compartmentCount, zoneStats.sweptCompartmentCount,
      getCount(COUNT_MINOR_GC), getCount(COUNT_STOREBUFFER_OVERFLOW),
      mmu20 * 100., mmu50 * 100., t(sccTotal), t(sccLongest),
      double(preTotalHeapBytes) / BYTES_PER_MB,
      getCount(COUNT_NEW_CHUNK) - getCount(COUNT_DESTROY_CHUNK),
      getCount(COUNT_NEW_CHUNK) + getCount(COUNT_DESTROY_CHUNK),
      double(ArenaSize * getCount(COUNT_ARENA_RELOCATED)) / BYTES_PER_MB);

  return DuplicateString(buffer);
}

UniqueChars Statistics::formatDetailedSliceDescription(
    unsigned i, const SliceData& slice) const {
  char budgetDescription[200];
  slice.budget.describe(budgetDescription, sizeof(budgetDescription) - 1);

  const char* format =
      "\
  ---- Slice %u ----\n\
    Reason: %s\n\
    Trigger: %s\n\
    Reset: %s%s\n\
    State: %s -> %s\n\
    Page Faults: %" PRIu64
      "\n\
    Pause: %.3fms of %s budget (@ %.3fms)\n\
";

  char triggerBuffer[100] = "n/a";
  if (slice.trigger) {
    Trigger trigger = slice.trigger.value();
    SprintfLiteral(triggerBuffer, "%.3f MiB of %.3f MiB threshold\n",
                   double(trigger.amount) / BYTES_PER_MB,
                   double(trigger.threshold) / BYTES_PER_MB);
  }

  char buffer[1024];
  SprintfLiteral(
      buffer, format, i, ExplainGCReason(slice.reason), triggerBuffer,
      slice.wasReset() ? "yes - " : "no",
      slice.wasReset() ? ExplainAbortReason(slice.resetReason) : "",
      gc::StateName(slice.initialState), gc::StateName(slice.finalState),
      uint64_t(slice.endFaults - slice.startFaults), t(slice.duration()),
      budgetDescription, t(slice.start - slices_[0].start));
  return DuplicateString(buffer);
}

static bool IncludePhase(TimeDuration duration) {
  // Don't include durations that will print as "0.000ms".
  return duration.ToMilliseconds() >= 0.001;
}

UniqueChars Statistics::formatDetailedPhaseTimes(
    const PhaseTimes& phaseTimes) const {
  static const TimeDuration MaxUnaccountedChildTime =
      TimeDuration::FromMicroseconds(50);

  FragmentVector fragments;
  char buffer[128];
  for (auto phase : AllPhases()) {
    uint8_t level = phases[phase].depth;
    TimeDuration ownTime = phaseTimes[phase];
    TimeDuration childTime = SumChildTimes(phase, phaseTimes);
    if (IncludePhase(ownTime)) {
      SprintfLiteral(buffer, "      %*s%s: %.3fms\n", level * 2, "",
                     phases[phase].name, t(ownTime));
      if (!fragments.append(DuplicateString(buffer))) {
        return UniqueChars(nullptr);
      }

      if (childTime && (ownTime - childTime) > MaxUnaccountedChildTime) {
        SprintfLiteral(buffer, "      %*s%s: %.3fms\n", (level + 1) * 2, "",
                       "Other", t(ownTime - childTime));
        if (!fragments.append(DuplicateString(buffer))) {
          return UniqueChars(nullptr);
        }
      }
    }
  }
  return Join(fragments);
}

UniqueChars Statistics::formatDetailedTotals() const {
  TimeDuration total, longest;
  gcDuration(&total, &longest);

  const char* format =
      "\
  ---- Totals ----\n\
    Total Time: %.3fms\n\
    Max Pause: %.3fms\n\
";
  char buffer[1024];
  SprintfLiteral(buffer, format, t(total), t(longest));
  return DuplicateString(buffer);
}

void Statistics::formatJsonSlice(size_t sliceNum, JSONPrinter& json) const {
  /*
   * We number each of the slice properties to keep the code in
   * GCTelemetry.jsm in sync.  See MAX_SLICE_KEYS.
   */
  json.beginObject();
  formatJsonSliceDescription(sliceNum, slices_[sliceNum], json);  // # 1-11

  json.beginObjectProperty("times");  // # 12
  formatJsonPhaseTimes(slices_[sliceNum].phaseTimes, json);
  json.endObject();

  json.endObject();
}

UniqueChars Statistics::renderJsonSlice(size_t sliceNum) const {
  Sprinter printer(nullptr, false);
  if (!printer.init()) {
    return UniqueChars(nullptr);
  }
  JSONPrinter json(printer);

  formatJsonSlice(sliceNum, json);
  return printer.release();
}

UniqueChars Statistics::renderNurseryJson() const {
  Sprinter printer(nullptr, false);
  if (!printer.init()) {
    return UniqueChars(nullptr);
  }
  JSONPrinter json(printer);
  gc->nursery().renderProfileJSON(json);
  return printer.release();
}

#ifdef DEBUG
void Statistics::log(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  if (gcDebugFile) {
    TimeDuration sinceStart = TimeStamp::Now() - TimeStamp::ProcessCreation();
    fprintf(gcDebugFile, "%12.3f: ", sinceStart.ToMicroseconds());
    vfprintf(gcDebugFile, fmt, args);
    fprintf(gcDebugFile, "\n");
    fflush(gcDebugFile);
  }
  va_end(args);
}
#endif

UniqueChars Statistics::renderJsonMessage() const {
  /*
   * The format of the JSON message is specified by the GCMajorMarkerPayload
   * type in profiler.firefox.com
   * https://github.com/firefox-devtools/profiler/blob/master/src/types/markers.js#L62
   *
   * All the properties listed here are created within the timings property
   * of the GCMajor marker.
   */
  if (aborted) {
    return DuplicateString("{status:\"aborted\"}");  // May return nullptr
  }

  Sprinter printer(nullptr, false);
  if (!printer.init()) {
    return UniqueChars(nullptr);
  }
  JSONPrinter json(printer);

  json.beginObject();
  json.property("status", "completed");
  formatJsonDescription(json);

  json.beginObjectProperty("totals");
  formatJsonPhaseTimes(phaseTimes, json);
  json.endObject();

  json.endObject();

  return printer.release();
}

void Statistics::formatJsonDescription(JSONPrinter& json) const {
  // If you change JSON properties here, please update:
  // Firefox Profiler:
  //   https://github.com/firefox-devtools/profiler

  TimeDuration total, longest;
  gcDuration(&total, &longest);
  json.property("max_pause", longest, JSONPrinter::MILLISECONDS);
  json.property("total_time", total, JSONPrinter::MILLISECONDS);
  // We might be able to omit reason if profiler.firefox.com was able to retrive
  // it from the first slice.  But it doesn't do this yet.
  json.property("reason", ExplainGCReason(slices_[0].reason));
  json.property("zones_collected", zoneStats.collectedZoneCount);
  json.property("total_zones", zoneStats.zoneCount);
  json.property("total_compartments", zoneStats.compartmentCount);
  json.property("minor_gcs", getCount(COUNT_MINOR_GC));
  json.property("minor_gc_number", gc->minorGCCount());
  json.property("major_gc_number", gc->majorGCCount());
  uint32_t storebufferOverflows = getCount(COUNT_STOREBUFFER_OVERFLOW);
  if (storebufferOverflows) {
    json.property("store_buffer_overflows", storebufferOverflows);
  }
  json.property("slices", slices_.length());

  const double mmu20 = computeMMU(TimeDuration::FromMilliseconds(20));
  const double mmu50 = computeMMU(TimeDuration::FromMilliseconds(50));
  json.property("mmu_20ms", int(mmu20 * 100));
  json.property("mmu_50ms", int(mmu50 * 100));

  TimeDuration sccTotal, sccLongest;
  sccDurations(&sccTotal, &sccLongest);
  json.property("scc_sweep_total", sccTotal, JSONPrinter::MILLISECONDS);
  json.property("scc_sweep_max_pause", sccLongest, JSONPrinter::MILLISECONDS);

  if (nonincrementalReason_ != GCAbortReason::None) {
    json.property("nonincremental_reason",
                  ExplainAbortReason(nonincrementalReason_));
  }
  json.property("allocated_bytes", preTotalHeapBytes);
  json.property("post_heap_size", postTotalHeapBytes);

  uint32_t addedChunks = getCount(COUNT_NEW_CHUNK);
  if (addedChunks) {
    json.property("added_chunks", addedChunks);
  }
  uint32_t removedChunks = getCount(COUNT_DESTROY_CHUNK);
  if (removedChunks) {
    json.property("removed_chunks", removedChunks);
  }
  json.property("major_gc_number", startingMajorGCNumber);
  json.property("minor_gc_number", startingMinorGCNumber);
  json.property("slice_number", startingSliceNumber);
}

void Statistics::formatJsonSliceDescription(unsigned i, const SliceData& slice,
                                            JSONPrinter& json) const {
  // If you change JSON properties here, please update:
  // Firefox Profiler:
  //   https://github.com/firefox-devtools/profiler
  //
  char budgetDescription[200];
  slice.budget.describe(budgetDescription, sizeof(budgetDescription) - 1);
  TimeStamp originTime = TimeStamp::ProcessCreation();

  json.property("slice", i);
  json.property("pause", slice.duration(), JSONPrinter::MILLISECONDS);
  json.property("reason", ExplainGCReason(slice.reason));
  json.property("initial_state", gc::StateName(slice.initialState));
  json.property("final_state", gc::StateName(slice.finalState));
  json.property("budget", budgetDescription);
  json.property("major_gc_number", startingMajorGCNumber);
  if (slice.trigger) {
    Trigger trigger = slice.trigger.value();
    json.property("trigger_amount", trigger.amount);
    json.property("trigger_threshold", trigger.threshold);
  }
  int64_t numFaults = slice.endFaults - slice.startFaults;
  if (numFaults != 0) {
    json.property("page_faults", numFaults);
  }
  json.property("start_timestamp", slice.start - originTime,
                JSONPrinter::SECONDS);
}

void Statistics::formatJsonPhaseTimes(const PhaseTimes& phaseTimes,
                                      JSONPrinter& json) const {
  for (auto phase : AllPhases()) {
    TimeDuration ownTime = phaseTimes[phase];
    if (!ownTime.IsZero()) {
      json.property(phases[phase].path, ownTime, JSONPrinter::MILLISECONDS);
    }
  }
}

Statistics::Statistics(GCRuntime* gc)
    : gc(gc),
      gcTimerFile(nullptr),
      gcDebugFile(nullptr),
      nonincrementalReason_(GCAbortReason::None),
      creationTime_(ReallyNow()),
      allocsSinceMinorGC({0, 0}),
      preTotalHeapBytes(0),
      postTotalHeapBytes(0),
      preCollectedHeapBytes(0),
      startingMinorGCNumber(0),
      startingMajorGCNumber(0),
      startingSliceNumber(0),
      maxPauseInInterval(0),
      sliceCallback(nullptr),
      nurseryCollectionCallback(nullptr),
      aborted(false),
      enableProfiling_(false),
      sliceCount_(0) {
  for (auto& count : counts) {
    count = 0;
  }

  for (auto& stat : stats) {
    stat = 0;
  }

#ifdef DEBUG
  for (const auto& duration : totalTimes_) {
    using ElementType = std::remove_reference_t<decltype(duration)>;
    static_assert(!std::is_trivially_constructible_v<ElementType>,
                  "Statistics::Statistics will only initialize "
                  "totalTimes_'s elements if their default constructor is "
                  "non-trivial");
    MOZ_ASSERT(duration.IsZero(),
               "totalTimes_ default-initialization should have "
               "default-initialized every element of totalTimes_ to zero");
  }
#endif

  MOZ_ALWAYS_TRUE(phaseStack.reserve(MAX_PHASE_NESTING));
  MOZ_ALWAYS_TRUE(suspendedPhases.reserve(MAX_SUSPENDED_PHASES));

  gcTimerFile = MaybeOpenFileFromEnv("MOZ_GCTIMER");
  gcDebugFile = MaybeOpenFileFromEnv("JS_GC_DEBUG");

  gc::ReadProfileEnv("JS_GC_PROFILE",
                     "Report major GCs taking more than N milliseconds for "
                     "all or just the main runtime\n",
                     &enableProfiling_, &profileWorkers_, &profileThreshold_);
}

Statistics::~Statistics() {
  if (gcTimerFile && gcTimerFile != stdout && gcTimerFile != stderr) {
    fclose(gcTimerFile);
  }
  if (gcDebugFile && gcDebugFile != stdout && gcDebugFile != stderr) {
    fclose(gcDebugFile);
  }
}

/* static */
bool Statistics::initialize() {
#ifdef DEBUG
  // Sanity check generated tables.
  for (auto i : AllPhases()) {
    auto parent = phases[i].parent;
    if (parent != Phase::NONE) {
      MOZ_ASSERT(phases[i].depth == phases[parent].depth + 1);
    }
    auto firstChild = phases[i].firstChild;
    if (firstChild != Phase::NONE) {
      MOZ_ASSERT(i == phases[firstChild].parent);
      MOZ_ASSERT(phases[i].depth == phases[firstChild].depth - 1);
    }
    auto nextSibling = phases[i].nextSibling;
    if (nextSibling != Phase::NONE) {
      MOZ_ASSERT(parent == phases[nextSibling].parent);
      MOZ_ASSERT(phases[i].depth == phases[nextSibling].depth);
    }
    auto nextWithPhaseKind = phases[i].nextWithPhaseKind;
    if (nextWithPhaseKind != Phase::NONE) {
      MOZ_ASSERT(phases[i].phaseKind == phases[nextWithPhaseKind].phaseKind);
      MOZ_ASSERT(parent != phases[nextWithPhaseKind].parent);
    }
  }
  for (auto i : AllPhaseKinds()) {
    MOZ_ASSERT(phases[phaseKinds[i].firstPhase].phaseKind == i);
    for (auto j : AllPhaseKinds()) {
      MOZ_ASSERT_IF(i != j, phaseKinds[i].telemetryBucket !=
                                phaseKinds[j].telemetryBucket);
    }
  }
#endif

  return true;
}

JS::GCSliceCallback Statistics::setSliceCallback(
    JS::GCSliceCallback newCallback) {
  JS::GCSliceCallback oldCallback = sliceCallback;
  sliceCallback = newCallback;
  return oldCallback;
}

JS::GCNurseryCollectionCallback Statistics::setNurseryCollectionCallback(
    JS::GCNurseryCollectionCallback newCallback) {
  auto oldCallback = nurseryCollectionCallback;
  nurseryCollectionCallback = newCallback;
  return oldCallback;
}

TimeDuration Statistics::clearMaxGCPauseAccumulator() {
  TimeDuration prior = maxPauseInInterval;
  maxPauseInInterval = 0;
  return prior;
}

TimeDuration Statistics::getMaxGCPauseSinceClear() {
  return maxPauseInInterval;
}

// Sum up the time for a phase, including instances of the phase with different
// parents.
static TimeDuration SumPhase(PhaseKind phaseKind,
                             const Statistics::PhaseTimes& times) {
  TimeDuration sum;
  for (PhaseIter phase(phaseKind); !phase.done(); phase.next()) {
    sum += times[phase];
  }
  return sum;
}

static bool CheckSelfTime(Phase parent, Phase child,
                          const Statistics::PhaseTimes& times,
                          const Statistics::PhaseTimes& selfTimes,
                          TimeDuration childTime) {
  if (selfTimes[parent] < childTime) {
    fprintf(
        stderr,
        "Parent %s time = %.3fms with %.3fms remaining, child %s time %.3fms\n",
        phases[parent].name, times[parent].ToMilliseconds(),
        selfTimes[parent].ToMilliseconds(), phases[child].name,
        childTime.ToMilliseconds());
    fflush(stderr);
    return false;
  }

  return true;
}

static PhaseKind FindLongestPhaseKind(const Statistics::PhaseKindTimes& times) {
  TimeDuration longestTime;
  PhaseKind phaseKind = PhaseKind::NONE;
  for (auto i : MajorGCPhaseKinds()) {
    if (times[i] > longestTime) {
      longestTime = times[i];
      phaseKind = i;
    }
  }

  return phaseKind;
}

static PhaseKind LongestPhaseSelfTimeInMajorGC(
    const Statistics::PhaseTimes& times) {
  // Start with total times per expanded phase, including children's times.
  Statistics::PhaseTimes selfTimes(times);

  // We have the total time spent in each phase, including descendant times.
  // Loop over the children and subtract their times from their parent's self
  // time.
  for (auto i : AllPhases()) {
    Phase parent = phases[i].parent;
    if (parent != Phase::NONE) {
      bool ok = CheckSelfTime(parent, i, times, selfTimes, times[i]);

      // This happens very occasionally in release builds and frequently
      // in Windows debug builds. Skip collecting longest phase telemetry
      // if it does.
#ifndef XP_WIN
      MOZ_ASSERT(ok, "Inconsistent time data; see bug 1400153");
#endif
      if (!ok) {
        return PhaseKind::NONE;
      }

      selfTimes[parent] -= times[i];
    }
  }

  // Sum expanded phases corresponding to the same phase.
  Statistics::PhaseKindTimes phaseKindTimes;
  for (auto i : AllPhaseKinds()) {
    phaseKindTimes[i] = SumPhase(i, selfTimes);
  }

  return FindLongestPhaseKind(phaseKindTimes);
}

void Statistics::printStats() {
  if (aborted) {
    fprintf(gcTimerFile,
            "OOM during GC statistics collection. The report is unavailable "
            "for this GC.\n");
  } else {
    UniqueChars msg = formatDetailedMessage();
    if (msg) {
      double secSinceStart =
          (slices_[0].start - TimeStamp::ProcessCreation()).ToSeconds();
      fprintf(gcTimerFile, "GC(T+%.3fs) %s\n", secSinceStart, msg.get());
    }
  }
  fflush(gcTimerFile);
}

void Statistics::beginGC(JS::GCOptions options, const TimeStamp& currentTime) {
  slices_.clearAndFree();
  sccTimes.clearAndFree();
  gcOptions = options;
  nonincrementalReason_ = GCAbortReason::None;

  preTotalHeapBytes = gc->heapSize.bytes();

  preCollectedHeapBytes = 0;

  startingMajorGCNumber = gc->majorGCCount();
  startingSliceNumber = gc->gcNumber();

  if (gc->lastGCEndTime()) {
    timeSinceLastGC = currentTime - gc->lastGCEndTime();
  }
}

void Statistics::measureInitialHeapSize() {
  MOZ_ASSERT(preCollectedHeapBytes == 0);
  for (GCZonesIter zone(gc); !zone.done(); zone.next()) {
    preCollectedHeapBytes += zone->gcHeapSize.bytes();
  }
}

void Statistics::adoptHeapSizeDuringIncrementalGC(Zone* mergedZone) {
  // A zone is being merged into a zone that's currently being collected so we
  // need to adjust our record of the total size of heap for collected zones.
  MOZ_ASSERT(gc->isIncrementalGCInProgress());
  preCollectedHeapBytes += mergedZone->gcHeapSize.bytes();
}

void Statistics::endGC() {
  postTotalHeapBytes = gc->heapSize.bytes();

  sendGCTelemetry();
}

void Statistics::sendGCTelemetry() {
  JSRuntime* runtime = gc->rt;
  runtime->addTelemetry(JS_TELEMETRY_GC_IS_ZONE_GC,
                        !zoneStats.isFullCollection());
  TimeDuration prepareTotal = SumPhase(PhaseKind::PREPARE, phaseTimes);
  TimeDuration markTotal = SumPhase(PhaseKind::MARK, phaseTimes);
  TimeDuration markRootsTotal = SumPhase(PhaseKind::MARK_ROOTS, phaseTimes);
  TimeDuration markWeakTotal = phaseTimes[Phase::SWEEP_MARK_WEAK] +
                               phaseTimes[Phase::SWEEP_MARK_GRAY_WEAK];
  TimeDuration markGrayTotal = phaseTimes[Phase::SWEEP_MARK_GRAY] +
                               phaseTimes[Phase::SWEEP_MARK_GRAY_WEAK];
  size_t markCount = gc->marker.getMarkCount();
  double markRate = markCount / t(markTotal);
  runtime->addTelemetry(JS_TELEMETRY_GC_PREPARE_MS, t(prepareTotal));
  runtime->addTelemetry(JS_TELEMETRY_GC_MARK_MS, t(markTotal));
  runtime->addTelemetry(JS_TELEMETRY_GC_MARK_RATE_2, markRate);
  runtime->addTelemetry(JS_TELEMETRY_GC_SWEEP_MS, t(phaseTimes[Phase::SWEEP]));
  if (gc->didCompactZones()) {
    runtime->addTelemetry(JS_TELEMETRY_GC_COMPACT_MS,
                          t(phaseTimes[Phase::COMPACT]));
  }
  runtime->addTelemetry(JS_TELEMETRY_GC_MARK_ROOTS_US,
                        markRootsTotal.ToMicroseconds());
  runtime->addTelemetry(JS_TELEMETRY_GC_MARK_GRAY_MS_2, t(markGrayTotal));
  runtime->addTelemetry(JS_TELEMETRY_GC_MARK_WEAK_MS, t(markWeakTotal));
  runtime->addTelemetry(JS_TELEMETRY_GC_NON_INCREMENTAL, nonincremental());
  if (nonincremental()) {
    runtime->addTelemetry(JS_TELEMETRY_GC_NON_INCREMENTAL_REASON,
                          uint32_t(nonincrementalReason_));
  }

#ifdef DEBUG
  // Reset happens non-incrementally, so only the last slice can be reset.
  for (size_t i = 0; i < slices_.length() - 1; i++) {
    MOZ_ASSERT(!slices_[i].wasReset());
  }
#endif
  const auto& lastSlice = slices_.back();
  runtime->addTelemetry(JS_TELEMETRY_GC_RESET, lastSlice.wasReset());
  if (lastSlice.wasReset()) {
    runtime->addTelemetry(JS_TELEMETRY_GC_RESET_REASON,
                          uint32_t(lastSlice.resetReason));
  }

  TimeDuration total, longest;
  gcDuration(&total, &longest);

  runtime->addTelemetry(JS_TELEMETRY_GC_MS, t(total));
  runtime->addTelemetry(JS_TELEMETRY_GC_MAX_PAUSE_MS_2, t(longest));

  const double mmu50 = computeMMU(TimeDuration::FromMilliseconds(50));
  runtime->addTelemetry(JS_TELEMETRY_GC_MMU_50, mmu50 * 100);

  // Record scheduling telemetry for the main runtime but not for workers, which
  // are scheduled differently.
  if (!runtime->parentRuntime && timeSinceLastGC) {
    runtime->addTelemetry(JS_TELEMETRY_GC_TIME_BETWEEN_S,
                          timeSinceLastGC.ToSeconds());
    if (!nonincremental()) {
      runtime->addTelemetry(JS_TELEMETRY_GC_SLICE_COUNT, slices_.length());
    }
  }

  if (!lastSlice.wasReset()) {
    size_t bytesSurvived = 0;
    for (ZonesIter zone(runtime, WithAtoms); !zone.done(); zone.next()) {
      if (zone->wasCollected()) {
        bytesSurvived += zone->gcHeapSize.retainedBytes();
      }
    }

    MOZ_ASSERT(preCollectedHeapBytes >= bytesSurvived);
    double survialRate =
        100.0 * double(bytesSurvived) / double(preCollectedHeapBytes);
    runtime->addTelemetry(JS_TELEMETRY_GC_TENURED_SURVIVAL_RATE,
                          uint32_t(survialRate));

    // Calculate 'effectiveness' in MB / second, on main thread only for now.
    if (!runtime->parentRuntime) {
      size_t bytesFreed = preCollectedHeapBytes - bytesSurvived;
      TimeDuration clampedTotal =
          TimeDuration::Max(total, TimeDuration::FromMilliseconds(1));
      double effectiveness =
          (double(bytesFreed) / BYTES_PER_MB) / clampedTotal.ToSeconds();
      runtime->addTelemetry(JS_TELEMETRY_GC_EFFECTIVENESS,
                            uint32_t(effectiveness));
    }
  }
}

void Statistics::beginNurseryCollection(JS::GCReason reason) {
  count(COUNT_MINOR_GC);
  startingMinorGCNumber = gc->minorGCCount();
  if (nurseryCollectionCallback) {
    (*nurseryCollectionCallback)(
        context(), JS::GCNurseryProgress::GC_NURSERY_COLLECTION_START, reason);
  }
}

void Statistics::endNurseryCollection(JS::GCReason reason) {
  if (nurseryCollectionCallback) {
    (*nurseryCollectionCallback)(
        context(), JS::GCNurseryProgress::GC_NURSERY_COLLECTION_END, reason);
  }

  allocsSinceMinorGC = {0, 0};
}

Statistics::SliceData::SliceData(const SliceBudget& budget,
                                 Maybe<Trigger> trigger, JS::GCReason reason,
                                 TimeStamp start, size_t startFaults,
                                 gc::State initialState)
    : budget(budget),
      reason(reason),
      trigger(trigger),
      initialState(initialState),
      start(start),
      startFaults(startFaults) {}

void Statistics::beginSlice(const ZoneGCStats& zoneStats, JS::GCOptions options,
                            const SliceBudget& budget, JS::GCReason reason) {
  MOZ_ASSERT(phaseStack.empty() ||
             (phaseStack.length() == 1 && phaseStack[0] == Phase::MUTATOR));

  this->zoneStats = zoneStats;

  TimeStamp currentTime = ReallyNow();

  bool first = !gc->isIncrementalGCInProgress();
  if (first) {
    beginGC(options, currentTime);
  }

  JSRuntime* runtime = gc->rt;
  if (!runtime->parentRuntime && !slices_.empty()) {
    TimeDuration timeSinceLastSlice = currentTime - slices_.back().end;
    runtime->addTelemetry(JS_TELEMETRY_GC_TIME_BETWEEN_SLICES_MS,
                          uint32_t(timeSinceLastSlice.ToMilliseconds()));
  }

  Maybe<Trigger> trigger = recordedTrigger;
  recordedTrigger.reset();

  if (!slices_.emplaceBack(budget, trigger, reason, currentTime,
                           GetPageFaultCount(), gc->state())) {
    // If we are OOM, set a flag to indicate we have missing slice data.
    aborted = true;
    return;
  }

  runtime->addTelemetry(JS_TELEMETRY_GC_REASON, uint32_t(reason));

  // Slice callbacks should only fire for the outermost level.
  bool wasFullGC = zoneStats.isFullCollection();
  if (sliceCallback) {
    JSContext* cx = context();
    JS::GCDescription desc(!wasFullGC, false, options, reason);
    if (first) {
      (*sliceCallback)(cx, JS::GC_CYCLE_BEGIN, desc);
    }
    (*sliceCallback)(cx, JS::GC_SLICE_BEGIN, desc);
  }

  log("begin slice");
}

void Statistics::endSlice() {
  MOZ_ASSERT(phaseStack.empty() ||
             (phaseStack.length() == 1 && phaseStack[0] == Phase::MUTATOR));

  if (!aborted) {
    auto& slice = slices_.back();
    slice.end = ReallyNow();
    slice.endFaults = GetPageFaultCount();
    slice.finalState = gc->state();

    log("end slice");

    sendSliceTelemetry(slice);

    sliceCount_++;
  }

  bool last = !gc->isIncrementalGCInProgress();
  if (last) {
    if (gcTimerFile) {
      printStats();
    }

    if (!aborted) {
      endGC();
    }
  }

  if (!aborted &&
      ShouldPrintProfile(gc->rt, enableProfiling_, profileWorkers_,
                         profileThreshold_, slices_.back().duration())) {
    printSliceProfile();
  }

  // Slice callbacks should only fire for the outermost level.
  if (!aborted) {
    bool wasFullGC = zoneStats.isFullCollection();
    if (sliceCallback) {
      JSContext* cx = context();
      JS::GCDescription desc(!wasFullGC, last, gcOptions,
                             slices_.back().reason);
      (*sliceCallback)(cx, JS::GC_SLICE_END, desc);
      if (last) {
        (*sliceCallback)(cx, JS::GC_CYCLE_END, desc);
      }
    }
  }

  // Do this after the slice callback since it uses these values.
  if (last) {
    for (auto& count : counts) {
      count = 0;
    }

    // Clear the timers at the end of a GC, preserving the data for
    // PhaseKind::MUTATOR.
    auto mutatorStartTime = phaseStartTimes[Phase::MUTATOR];
    auto mutatorTime = phaseTimes[Phase::MUTATOR];

    for (mozilla::TimeStamp& t : phaseStartTimes) {
      t = TimeStamp();
    }
#ifdef DEBUG
    for (mozilla::TimeStamp& t : phaseEndTimes) {
      t = TimeStamp();
    }
#endif

    for (TimeDuration& duration : phaseTimes) {
      duration = TimeDuration();
      MOZ_ASSERT(duration.IsZero());
    }

    phaseStartTimes[Phase::MUTATOR] = mutatorStartTime;
    phaseTimes[Phase::MUTATOR] = mutatorTime;
  }

  aborted = false;
}

void Statistics::sendSliceTelemetry(const SliceData& slice) {
  JSRuntime* runtime = gc->rt;
  TimeDuration sliceTime = slice.end - slice.start;
  runtime->addTelemetry(JS_TELEMETRY_GC_SLICE_MS, t(sliceTime));

  if (slice.budget.isTimeBudget()) {
    int64_t budget_ms = slice.budget.timeBudget();
    runtime->addTelemetry(JS_TELEMETRY_GC_BUDGET_MS_2, budget_ms);
    if (IsCurrentlyAnimating(runtime->lastAnimationTime, slice.end)) {
      runtime->addTelemetry(JS_TELEMETRY_GC_ANIMATION_MS, t(sliceTime));
    }

    // Record any phase that goes 1.5 times or 5ms over its budget.
    double longSliceThreshold = std::min(1.5 * budget_ms, budget_ms + 5.0);
    if (sliceTime.ToMilliseconds() > longSliceThreshold) {
      PhaseKind longest = LongestPhaseSelfTimeInMajorGC(slice.phaseTimes);
      reportLongestPhaseInMajorGC(longest, JS_TELEMETRY_GC_SLOW_PHASE);

      // If the longest phase was waiting for parallel tasks then record the
      // longest task.
      if (longest == PhaseKind::JOIN_PARALLEL_TASKS) {
        PhaseKind longestParallel =
            FindLongestPhaseKind(slice.maxParallelTimes);
        reportLongestPhaseInMajorGC(longestParallel, JS_TELEMETRY_GC_SLOW_TASK);
      }
    }

    // Record how long we went over budget.
    int64_t overrun = int64_t(sliceTime.ToMicroseconds()) - (1000 * budget_ms);
    if (overrun > 0) {
      runtime->addTelemetry(JS_TELEMETRY_GC_BUDGET_OVERRUN, uint32_t(overrun));
    }
  }
}

void Statistics::reportLongestPhaseInMajorGC(PhaseKind longest,
                                             int telemetryId) {
  JSRuntime* runtime = gc->rt;
  if (longest != PhaseKind::NONE) {
    uint8_t bucket = phaseKinds[longest].telemetryBucket;
    runtime->addTelemetry(telemetryId, bucket);
  }
}

bool Statistics::startTimingMutator() {
  if (phaseStack.length() != 0) {
    // Should only be called from outside of GC.
    MOZ_ASSERT(phaseStack.length() == 1);
    MOZ_ASSERT(phaseStack[0] == Phase::MUTATOR);
    return false;
  }

  MOZ_ASSERT(suspendedPhases.empty());

  timedGCTime = 0;
  phaseStartTimes[Phase::MUTATOR] = TimeStamp();
  phaseTimes[Phase::MUTATOR] = 0;
  timedGCStart = TimeStamp();

  beginPhase(PhaseKind::MUTATOR);
  return true;
}

bool Statistics::stopTimingMutator(double& mutator_ms, double& gc_ms) {
  // This should only be called from outside of GC, while timing the mutator.
  if (phaseStack.length() != 1 || phaseStack[0] != Phase::MUTATOR) {
    return false;
  }

  endPhase(PhaseKind::MUTATOR);
  mutator_ms = t(phaseTimes[Phase::MUTATOR]);
  gc_ms = t(timedGCTime);

  return true;
}

void Statistics::suspendPhases(PhaseKind suspension) {
  MOZ_ASSERT(suspension == PhaseKind::EXPLICIT_SUSPENSION ||
             suspension == PhaseKind::IMPLICIT_SUSPENSION);
  while (!phaseStack.empty()) {
    MOZ_ASSERT(suspendedPhases.length() < MAX_SUSPENDED_PHASES);
    Phase parent = phaseStack.back();
    suspendedPhases.infallibleAppend(parent);
    recordPhaseEnd(parent);
  }
  suspendedPhases.infallibleAppend(lookupChildPhase(suspension));
}

void Statistics::resumePhases() {
  MOZ_ASSERT(suspendedPhases.back() == Phase::EXPLICIT_SUSPENSION ||
             suspendedPhases.back() == Phase::IMPLICIT_SUSPENSION);
  suspendedPhases.popBack();

  while (!suspendedPhases.empty() &&
         suspendedPhases.back() != Phase::EXPLICIT_SUSPENSION &&
         suspendedPhases.back() != Phase::IMPLICIT_SUSPENSION) {
    Phase resumePhase = suspendedPhases.popCopy();
    if (resumePhase == Phase::MUTATOR) {
      timedGCTime += ReallyNow() - timedGCStart;
    }
    recordPhaseBegin(resumePhase);
  }
}

void Statistics::beginPhase(PhaseKind phaseKind) {
  // No longer timing these phases. We should never see these.
  MOZ_ASSERT(phaseKind != PhaseKind::GC_BEGIN &&
             phaseKind != PhaseKind::GC_END);

  // PhaseKind::MUTATOR is suspended while performing GC.
  if (currentPhase() == Phase::MUTATOR) {
    suspendPhases(PhaseKind::IMPLICIT_SUSPENSION);
  }

  recordPhaseBegin(lookupChildPhase(phaseKind));
}

void Statistics::recordPhaseBegin(Phase phase) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(gc->rt));

  // Guard against any other re-entry.
  MOZ_ASSERT(!phaseStartTimes[phase]);

  MOZ_ASSERT(phaseStack.length() < MAX_PHASE_NESTING);

  Phase current = currentPhase();
  MOZ_ASSERT(phases[phase].parent == current);

  TimeStamp now = ReallyNow();

  if (current != Phase::NONE) {
    MOZ_ASSERT(now >= phaseStartTimes[currentPhase()],
               "Inconsistent time data; see bug 1400153");
    if (now < phaseStartTimes[currentPhase()]) {
      now = phaseStartTimes[currentPhase()];
      aborted = true;
    }
  }

  phaseStack.infallibleAppend(phase);
  phaseStartTimes[phase] = now;
  log("begin: %s", phases[phase].path);
}

void Statistics::recordPhaseEnd(Phase phase) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(gc->rt));

  MOZ_ASSERT(phaseStartTimes[phase]);

  TimeStamp now = ReallyNow();

  // Make sure this phase ends after it starts.
  MOZ_ASSERT(now >= phaseStartTimes[phase],
             "Inconsistent time data; see bug 1400153");

#ifdef DEBUG
  // Make sure this phase ends after all of its children. Note that some
  // children might not have run in this instance, in which case they will
  // have run in a previous instance of this parent or not at all.
  for (Phase kid = phases[phase].firstChild; kid != Phase::NONE;
       kid = phases[kid].nextSibling) {
    if (phaseEndTimes[kid].IsNull()) {
      continue;
    }
    if (phaseEndTimes[kid] > now) {
      fprintf(stderr,
              "Parent %s ended at %.3fms, before child %s ended at %.3fms?\n",
              phases[phase].name, t(now - TimeStamp::ProcessCreation()),
              phases[kid].name,
              t(phaseEndTimes[kid] - TimeStamp::ProcessCreation()));
    }
    MOZ_ASSERT(phaseEndTimes[kid] <= now,
               "Inconsistent time data; see bug 1400153");
  }
#endif

  if (now < phaseStartTimes[phase]) {
    now = phaseStartTimes[phase];
    aborted = true;
  }

  if (phase == Phase::MUTATOR) {
    timedGCStart = now;
  }

  phaseStack.popBack();

  TimeDuration t = now - phaseStartTimes[phase];
  if (!slices_.empty()) {
    slices_.back().phaseTimes[phase] += t;
  }
  phaseTimes[phase] += t;
  phaseStartTimes[phase] = TimeStamp();

#ifdef DEBUG
  phaseEndTimes[phase] = now;
  log("end: %s", phases[phase].path);
#endif
}

void Statistics::endPhase(PhaseKind phaseKind) {
  Phase phase = currentPhase();
  MOZ_ASSERT(phase != Phase::NONE);
  MOZ_ASSERT(phases[phase].phaseKind == phaseKind);

  recordPhaseEnd(phase);

  // When emptying the stack, we may need to return to timing the mutator
  // (PhaseKind::MUTATOR).
  if (phaseStack.empty() && !suspendedPhases.empty() &&
      suspendedPhases.back() == Phase::IMPLICIT_SUSPENSION) {
    resumePhases();
  }
}

void Statistics::recordParallelPhase(PhaseKind phaseKind,
                                     TimeDuration duration) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(gc->rt));

  if (aborted) {
    return;
  }

  // Record the maximum task time for each phase. Don't record times for parent
  // phases.
  TimeDuration& time = slices_.back().maxParallelTimes[phaseKind];
  time = std::max(time, duration);
}

TimeStamp Statistics::beginSCC() { return ReallyNow(); }

void Statistics::endSCC(unsigned scc, TimeStamp start) {
  if (scc >= sccTimes.length() && !sccTimes.resize(scc + 1)) {
    return;
  }

  sccTimes[scc] += ReallyNow() - start;
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
double Statistics::computeMMU(TimeDuration window) const {
  MOZ_ASSERT(!slices_.empty());

  TimeDuration gc = slices_[0].end - slices_[0].start;
  TimeDuration gcMax = gc;

  if (gc >= window) {
    return 0.0;
  }

  int startIndex = 0;
  for (size_t endIndex = 1; endIndex < slices_.length(); endIndex++) {
    auto* startSlice = &slices_[startIndex];
    auto& endSlice = slices_[endIndex];
    gc += endSlice.end - endSlice.start;

    while (endSlice.end - startSlice->end >= window) {
      gc -= startSlice->end - startSlice->start;
      startSlice = &slices_[++startIndex];
    }

    TimeDuration cur = gc;
    if (endSlice.end - startSlice->start > window) {
      cur -= (endSlice.end - startSlice->start - window);
    }
    if (cur > gcMax) {
      gcMax = cur;
    }
  }

  return double((window - gcMax) / window);
}

void Statistics::maybePrintProfileHeaders() {
  static int printedHeader = 0;
  if ((printedHeader++ % 200) == 0) {
    printProfileHeader();
    if (gc->nursery().enableProfiling()) {
      Nursery::printProfileHeader();
    }
  }
}

void Statistics::printProfileHeader() {
  if (!enableProfiling_) {
    return;
  }

  fprintf(
      stderr,
      "MajorGC: PID    Runtime        Timestamp  Reason               States "
      "FSNR   budget total ");
#define PRINT_PROFILE_HEADER(name, text, phase) \
  fprintf(stderr, " %-6.6s", text);
  FOR_EACH_GC_PROFILE_TIME(PRINT_PROFILE_HEADER)
#undef PRINT_PROFILE_HEADER
  fprintf(stderr, "\n");
}

/* static */
void Statistics::printProfileTimes(const ProfileDurations& times) {
  for (auto time : times) {
    fprintf(stderr, " %6" PRIi64, static_cast<int64_t>(time.ToMilliseconds()));
  }
  fprintf(stderr, "\n");
}

void Statistics::printSliceProfile() {
  const SliceData& slice = slices_.back();

  maybePrintProfileHeaders();

  TimeDuration ts = slice.end - creationTime();

  bool shrinking = gcOptions == JS::GCOptions::Shrink;
  bool reset = slice.resetReason != GCAbortReason::None;
  bool nonIncremental = nonincrementalReason_ != GCAbortReason::None;
  bool full = zoneStats.isFullCollection();

  fprintf(
      stderr, "MajorGC: %6zu %14p %10.6f %-20.20s %1d -> %1d %1s%1s%1s%1s  ",
      size_t(getpid()), gc->rt, ts.ToSeconds(), ExplainGCReason(slice.reason),
      int(slice.initialState), int(slice.finalState), full ? "F" : "",
      shrinking ? "S" : "", nonIncremental ? "N" : "", reset ? "R" : "");

  if (!nonIncremental && !slice.budget.isUnlimited() &&
      slice.budget.isTimeBudget()) {
    fprintf(stderr, " %6" PRIi64, slice.budget.timeBudget());
  } else {
    fprintf(stderr, "       ");
  }

  ProfileDurations times;
  times[ProfileKey::Total] = slice.duration();
  totalTimes_[ProfileKey::Total] += times[ProfileKey::Total];

#define GET_PROFILE_TIME(name, text, phase)                    \
  times[ProfileKey::name] = SumPhase(phase, slice.phaseTimes); \
  totalTimes_[ProfileKey::name] += times[ProfileKey::name];
  FOR_EACH_GC_PROFILE_TIME(GET_PROFILE_TIME)
#undef GET_PROFILE_TIME

  printProfileTimes(times);
}

void Statistics::printTotalProfileTimes() {
  if (enableProfiling_) {
    fprintf(stderr,
            "MajorGC: %6zu %14p TOTALS: %7" PRIu64
            " slices:                             ",
            size_t(getpid()), gc->rt, sliceCount_);
    printProfileTimes(totalTimes_);
  }
}
