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

#include "gc/GC.h"
#include "gc/GCInternals.h"
#include "gc/Memory.h"
#include "js/Printer.h"
#include "util/GetPidProvider.h"
#include "util/Text.h"
#include "vm/JSONPrinter.h"
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
    case JS::GCOptions::Shutdown:
      return "Shutdown";
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

static FILE* MaybeOpenFileFromEnv(const char* env,
                                  FILE* defaultFile = nullptr) {
  const char* value = getenv(env);
  if (!value) {
    return defaultFile;
  }

  FILE* file;
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
    if (!file || setvbuf(file, nullptr, _IOLBF, 256) != 0) {
      perror("Error opening log file");
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
using PhaseTable = EnumeratedArray<Phase, PhaseInfo, size_t(Phase::LIMIT)>;

// A table of PhaseKindInfo indexed by PhaseKind.
using PhaseKindTable =
    EnumeratedArray<PhaseKind, PhaseKindInfo, size_t(PhaseKind::LIMIT)>;

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

static TimeDuration TimeBetween(TimeStamp start, TimeStamp end) {
#ifndef XP_WIN
  MOZ_ASSERT(end >= start);
#else
  // Sadly this happens sometimes.
  if (end < start) {
    return TimeDuration::Zero();
  }
#endif
  return end - start;
}

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
  *total = *maxPause = TimeDuration::Zero();
  for (const auto& slice : slices_) {
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
  *total = *maxPause = TimeDuration::Zero();
  for (const auto& duration : sccTimes) {
    *total += duration;
    *maxPause = std::max(*maxPause, duration);
  }
}

using FragmentVector = Vector<UniqueChars, 8, SystemAllocPolicy>;

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
  TimeDuration total;
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
                 "Zones: %zu of %zu (-%zu); Compartments: %zu of %zu (-%zu); "
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
   * GCTelemetry.sys.mjs in sync.  See MAX_SLICE_KEYS.
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
  JSONPrinter json(printer, false);

  formatJsonSlice(sliceNum, json);
  return printer.release();
}

UniqueChars Statistics::renderNurseryJson() const {
  Sprinter printer(nullptr, false);
  if (!printer.init()) {
    return UniqueChars(nullptr);
  }
  JSONPrinter json(printer, false);
  gc->nursery().renderProfileJSON(json);
  return printer.release();
}

#ifdef DEBUG
void Statistics::log(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  if (gcDebugFile) {
    TimeDuration sinceStart =
        TimeBetween(TimeStamp::FirstTimeStamp(), TimeStamp::Now());
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
  JSONPrinter json(printer, false);

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
  MOZ_ASSERT(slice.endFaults >= slice.startFaults);
  size_t numFaults = slice.endFaults - slice.startFaults;
  if (numFaults != 0) {
    json.property("page_faults", numFaults);
  }
  json.property("start_timestamp", TimeBetween(originTime, slice.start),
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
      creationTime_(TimeStamp::Now()),
      tenuredAllocsSinceMinorGC(0),
      preTotalHeapBytes(0),
      postTotalHeapBytes(0),
      preCollectedHeapBytes(0),
      startingMinorGCNumber(0),
      startingMajorGCNumber(0),
      startingSliceNumber(0),
      sliceCallback(nullptr),
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
  gcProfileFile = MaybeOpenFileFromEnv("JS_GC_PROFILE_FILE", stderr);

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

TimeDuration Statistics::clearMaxGCPauseAccumulator() {
  TimeDuration prior = maxPauseInInterval;
  maxPauseInInterval = TimeDuration::Zero();
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
          TimeBetween(TimeStamp::ProcessCreation(), slices_[0].start)
              .ToSeconds();
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
    timeSinceLastGC = TimeBetween(gc->lastGCEndTime(), currentTime);
  }

  totalGCTime_ = TimeDuration::Zero();
}

void Statistics::measureInitialHeapSize() {
  MOZ_ASSERT(preCollectedHeapBytes == 0);
  for (GCZonesIter zone(gc); !zone.done(); zone.next()) {
    preCollectedHeapBytes += zone->gcHeapSize.bytes();
  }
}

void Statistics::endGC() {
  postTotalHeapBytes = gc->heapSize.bytes();

  sendGCTelemetry();
}

TimeDuration Statistics::sumTotalParallelTime(PhaseKind phaseKind) const {
  TimeDuration total;
  for (const SliceData& slice : slices_) {
    total += slice.totalParallelTimes[phaseKind];
  }
  return total;
}

void Statistics::sendGCTelemetry() {
  JSRuntime* runtime = gc->rt;
  // NOTE: "Compartmental" is term that was deprecated with the
  // introduction of zone-based GC, but the old telemetry probe
  // continues to be used.
  runtime->metrics().GC_IS_COMPARTMENTAL(!gc->fullGCRequested);
  runtime->metrics().GC_ZONE_COUNT(zoneStats.zoneCount);
  runtime->metrics().GC_ZONES_COLLECTED(zoneStats.collectedZoneCount);

  TimeDuration prepareTotal = phaseTimes[Phase::PREPARE];
  TimeDuration markTotal = SumPhase(PhaseKind::MARK, phaseTimes);
  TimeDuration markRootsTotal = SumPhase(PhaseKind::MARK_ROOTS, phaseTimes);

  // Gray and weak marking time is counted under MARK_WEAK and not MARK_GRAY.
  TimeDuration markWeakTotal = SumPhase(PhaseKind::MARK_WEAK, phaseTimes);
  TimeDuration markGrayNotWeak =
      SumPhase(PhaseKind::MARK_GRAY, phaseTimes) +
      SumPhase(PhaseKind::MARK_INCOMING_GRAY, phaseTimes);
  TimeDuration markGrayWeak = SumPhase(PhaseKind::MARK_GRAY_WEAK, phaseTimes);
  TimeDuration markGrayTotal = markGrayNotWeak + markGrayWeak;
  TimeDuration markNotGrayOrWeak = markTotal - markGrayNotWeak - markWeakTotal;
  if (markNotGrayOrWeak < TimeDuration::FromMilliseconds(0)) {
    markNotGrayOrWeak = TimeDuration::Zero();
  }

  size_t markCount = getCount(COUNT_CELLS_MARKED);

  runtime->metrics().GC_PREPARE_MS(prepareTotal);
  runtime->metrics().GC_MARK_MS(markNotGrayOrWeak);
  if (markTotal >= TimeDuration::FromMicroseconds(1)) {
    double markRate = double(markCount) / t(markTotal);
    runtime->metrics().GC_MARK_RATE_2(uint32_t(markRate));
  }
  runtime->metrics().GC_SWEEP_MS(phaseTimes[Phase::SWEEP]);
  if (gc->didCompactZones()) {
    runtime->metrics().GC_COMPACT_MS(phaseTimes[Phase::COMPACT]);
  }
  runtime->metrics().GC_MARK_ROOTS_US(markRootsTotal);
  runtime->metrics().GC_MARK_GRAY_MS_2(markGrayTotal);
  runtime->metrics().GC_MARK_WEAK_MS(markWeakTotal);
  runtime->metrics().GC_NON_INCREMENTAL(nonincremental());
  if (nonincremental()) {
    runtime->metrics().GC_NON_INCREMENTAL_REASON(
        uint32_t(nonincrementalReason_));
  }

#ifdef DEBUG
  // Reset happens non-incrementally, so only the last slice can be reset.
  for (size_t i = 0; i < slices_.length() - 1; i++) {
    MOZ_ASSERT(!slices_[i].wasReset());
  }
#endif
  const auto& lastSlice = slices_.back();
  runtime->metrics().GC_RESET(lastSlice.wasReset());
  if (lastSlice.wasReset()) {
    runtime->metrics().GC_RESET_REASON(uint32_t(lastSlice.resetReason));
  }

  TimeDuration total, longest;
  gcDuration(&total, &longest);

  runtime->metrics().GC_MS(total);
  runtime->metrics().GC_MAX_PAUSE_MS_2(longest);

  const double mmu50 = computeMMU(TimeDuration::FromMilliseconds(50));
  runtime->metrics().GC_MMU_50(mmu50 * 100.0);

  // Record scheduling telemetry for the main runtime but not for workers, which
  // are scheduled differently.
  if (!runtime->parentRuntime && timeSinceLastGC) {
    runtime->metrics().GC_TIME_BETWEEN_S(timeSinceLastGC);
    if (!nonincremental()) {
      runtime->metrics().GC_SLICE_COUNT(slices_.length());
    }
  }

  if (!lastSlice.wasReset() && preCollectedHeapBytes != 0) {
    size_t bytesSurvived = 0;
    for (ZonesIter zone(runtime, WithAtoms); !zone.done(); zone.next()) {
      if (zone->wasCollected()) {
        bytesSurvived += zone->gcHeapSize.retainedBytes();
      }
    }

    MOZ_ASSERT(preCollectedHeapBytes >= bytesSurvived);
    double survivalRate =
        100.0 * double(bytesSurvived) / double(preCollectedHeapBytes);
    runtime->metrics().GC_TENURED_SURVIVAL_RATE(survivalRate);

    // Calculate 'effectiveness' in MB / second, on main thread only for now.
    if (!runtime->parentRuntime) {
      size_t bytesFreed = preCollectedHeapBytes - bytesSurvived;
      TimeDuration clampedTotal =
          TimeDuration::Max(total, TimeDuration::FromMilliseconds(1));
      double effectiveness =
          (double(bytesFreed) / BYTES_PER_MB) / clampedTotal.ToSeconds();
      runtime->metrics().GC_EFFECTIVENESS(uint32_t(effectiveness));
    }
  }

  // Parallel marking stats.
  bool usedParallelMarking = false;
  if (gc->isParallelMarkingEnabled()) {
    TimeDuration wallTime = SumPhase(PhaseKind::PARALLEL_MARK, phaseTimes);
    TimeDuration parallelMarkTime =
        sumTotalParallelTime(PhaseKind::PARALLEL_MARK_MARK);
    TimeDuration parallelRunTime =
        parallelMarkTime + sumTotalParallelTime(PhaseKind::PARALLEL_MARK_OTHER);
    usedParallelMarking = wallTime && parallelMarkTime;
    if (usedParallelMarking) {
      uint32_t threadCount = gc->markers.length();
      double speedup = parallelMarkTime / wallTime;
      double utilization = parallelRunTime / (wallTime * threadCount);
      runtime->metrics().GC_PARALLEL_MARK_SPEEDUP(uint32_t(speedup * 100.0));
      runtime->metrics().GC_PARALLEL_MARK_UTILIZATION(
          std::clamp(utilization * 100.0, 0.0, 100.0));
      runtime->metrics().GC_PARALLEL_MARK_INTERRUPTIONS(
          getCount(COUNT_PARALLEL_MARK_INTERRUPTIONS));
    }
  }
  runtime->metrics().GC_PARALLEL_MARK(usedParallelMarking);
}

void Statistics::beginNurseryCollection() {
  count(COUNT_MINOR_GC);
  startingMinorGCNumber = gc->minorGCCount();
}

void Statistics::endNurseryCollection() { tenuredAllocsSinceMinorGC = 0; }

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

TimeDuration Statistics::SliceData::duration() const {
  return TimeBetween(start, end);
}

void Statistics::beginSlice(const ZoneGCStats& zoneStats, JS::GCOptions options,
                            const SliceBudget& budget, JS::GCReason reason,
                            bool budgetWasIncreased) {
  MOZ_ASSERT(phaseStack.empty() ||
             (phaseStack.length() == 1 && phaseStack[0] == Phase::MUTATOR));

  this->zoneStats = zoneStats;

  TimeStamp currentTime = TimeStamp::Now();

  bool first = !gc->isIncrementalGCInProgress();
  if (first) {
    beginGC(options, currentTime);
  }

  JSRuntime* runtime = gc->rt;
  if (!runtime->parentRuntime && !slices_.empty()) {
    TimeDuration timeSinceLastSlice =
        TimeBetween(slices_.back().end, currentTime);
    runtime->metrics().GC_TIME_BETWEEN_SLICES_MS(timeSinceLastSlice);
  }

  Maybe<Trigger> trigger = recordedTrigger;
  recordedTrigger.reset();

  if (!slices_.emplaceBack(budget, trigger, reason, currentTime,
                           GetPageFaultCount(), gc->state())) {
    // If we are OOM, set a flag to indicate we have missing slice data.
    aborted = true;
    return;
  }

  runtime->metrics().GC_REASON_2(uint32_t(reason));
  runtime->metrics().GC_BUDGET_WAS_INCREASED(budgetWasIncreased);

  // Slice callbacks should only fire for the outermost level.
  if (sliceCallback) {
    JSContext* cx = context();
    JS::GCDescription desc(!gc->fullGCRequested, false, options, reason);
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
    slice.end = TimeStamp::Now();
    slice.endFaults = GetPageFaultCount();
    slice.finalState = gc->state();

    log("end slice");

    sendSliceTelemetry(slice);

    sliceCount_++;

    totalGCTime_ += slice.duration();
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
    if (sliceCallback) {
      JSContext* cx = context();
      JS::GCDescription desc(!gc->fullGCRequested, last, gcOptions,
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

    phaseStartTimes = PhaseTimeStamps();
#ifdef DEBUG
    phaseEndTimes = PhaseTimeStamps();
#endif
    phaseTimes = PhaseTimes();

    phaseStartTimes[Phase::MUTATOR] = mutatorStartTime;
    phaseTimes[Phase::MUTATOR] = mutatorTime;
  }

  aborted = false;
}

void Statistics::sendSliceTelemetry(const SliceData& slice) {
  JSRuntime* runtime = gc->rt;
  TimeDuration sliceTime = slice.duration();
  runtime->metrics().GC_SLICE_MS(sliceTime);

  if (slice.budget.isTimeBudget()) {
    TimeDuration budgetDuration = slice.budget.timeBudgetDuration();
    runtime->metrics().GC_BUDGET_MS_2(budgetDuration);

    if (IsCurrentlyAnimating(runtime->lastAnimationTime, slice.end)) {
      runtime->metrics().GC_ANIMATION_MS(sliceTime);
    }

    bool wasLongSlice = false;
    if (sliceTime > budgetDuration) {
      // Record how long we went over budget.
      TimeDuration overrun = sliceTime - budgetDuration;
      runtime->metrics().GC_BUDGET_OVERRUN(overrun);

      // Long GC slices are those that go 50% or 5ms over their budget.
      wasLongSlice = (overrun > TimeDuration::FromMilliseconds(5)) ||
                     (overrun > (budgetDuration / int64_t(2)));

      // Record the longest phase in any long slice.
      if (wasLongSlice) {
        PhaseKind longest = LongestPhaseSelfTimeInMajorGC(slice.phaseTimes);
        reportLongestPhaseInMajorGC(longest, [runtime](auto sample) {
          runtime->metrics().GC_SLOW_PHASE(sample);
        });

        // If the longest phase was waiting for parallel tasks then record the
        // longest task.
        if (longest == PhaseKind::JOIN_PARALLEL_TASKS) {
          PhaseKind longestParallel =
              FindLongestPhaseKind(slice.maxParallelTimes);
          reportLongestPhaseInMajorGC(longestParallel, [runtime](auto sample) {
            runtime->metrics().GC_SLOW_TASK(sample);
          });
        }
      }
    }

    // Record `wasLongSlice` for all TimeBudget slices.
    runtime->metrics().GC_SLICE_WAS_LONG(wasLongSlice);
  }
}

template <typename Fn>
void Statistics::reportLongestPhaseInMajorGC(PhaseKind longest, Fn reportFn) {
  if (longest != PhaseKind::NONE) {
    uint8_t bucket = phaseKinds[longest].telemetryBucket;
    reportFn(bucket);
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

  timedGCTime = TimeDuration::Zero();
  phaseStartTimes[Phase::MUTATOR] = TimeStamp();
  phaseTimes[Phase::MUTATOR] = TimeDuration::Zero();
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
      timedGCTime += TimeBetween(timedGCStart, TimeStamp::Now());
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

  TimeStamp now = TimeStamp::Now();

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

  TimeStamp now = TimeStamp::Now();

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
              phases[phase].name,
              t(TimeBetween(TimeStamp::FirstTimeStamp(), now)),
              phases[kid].name,
              t(TimeBetween(TimeStamp::FirstTimeStamp(), phaseEndTimes[kid])));
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

  TimeDuration t = TimeBetween(phaseStartTimes[phase], now);
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

  slices_.back().totalParallelTimes[phaseKind] += duration;

  // Also record the maximum task time for each phase. Don't record times for
  // parent phases.
  TimeDuration& maxTime = slices_.back().maxParallelTimes[phaseKind];
  maxTime = std::max(maxTime, duration);
}

TimeStamp Statistics::beginSCC() { return TimeStamp::Now(); }

void Statistics::endSCC(unsigned scc, TimeStamp start) {
  if (scc >= sccTimes.length() && !sccTimes.resize(scc + 1)) {
    return;
  }

  sccTimes[scc] += TimeBetween(start, TimeStamp::Now());
}

/*
 * Calculate minimum mutator utilization for previous incremental GC.
 *
 * MMU (minimum mutator utilization) is a measure of how much garbage collection
 * is affecting the responsiveness of the system. MMU measurements are given
 * with respect to a certain window size. If we report MMU(50ms) = 80%, then
 * that means that, for any 50ms window of time, at least 80% of the window is
 * devoted to the mutator. In other words, the GC is running for at most 20% of
 * the window, or 10ms. The GC can run multiple slices during the 50ms window
 * as long as the total time it spends is at most 10ms.
 */
double Statistics::computeMMU(TimeDuration window) const {
  MOZ_ASSERT(window > TimeDuration::Zero());
  MOZ_ASSERT(!slices().empty());

  // Examine all ranges of slices from |startIndex| to |endIndex| inclusive
  // whose timestamps span less than the window duration. The time spent in GC
  // in each range is stored in |gcInRange| by maintaining a running total. The
  // maximum value of this after adjustment to the window size is recorded in
  // |maxGCInWindow|.

  size_t startIndex = 0;
  const SliceData* startSlice = &sliceAt(startIndex);
  TimeDuration gcInRange = startSlice->duration();
  if (gcInRange >= window) {
    return 0.0;
  }

  TimeDuration maxGCInWindow = gcInRange;

  for (size_t endIndex = 1; endIndex < slices().length(); endIndex++) {
    const SliceData* endSlice = &sliceAt(endIndex);
    if (endSlice->duration() >= window) {
      return 0.0;
    }

    gcInRange += endSlice->duration();

    while (TimeBetween(startSlice->end, endSlice->end) >= window) {
      gcInRange -= startSlice->duration();
      ++startIndex;
      MOZ_ASSERT(startIndex <= endIndex);
      startSlice = &sliceAt(startIndex);
    }

    TimeDuration totalInRange = TimeBetween(startSlice->start, endSlice->end);
    MOZ_ASSERT(gcInRange <= totalInRange);

    TimeDuration gcInWindow = gcInRange;
    if (totalInRange > window) {
      gcInWindow -= (totalInRange - window);
    }
    MOZ_ASSERT(gcInWindow <= window);

    if (gcInWindow > maxGCInWindow) {
      maxGCInWindow = gcInWindow;
    }
  }

  MOZ_ASSERT(maxGCInWindow >= TimeDuration::Zero());
  MOZ_ASSERT(maxGCInWindow <= window);
  return (window - maxGCInWindow) / window;
}

void Statistics::maybePrintProfileHeaders() {
  static int printedHeader = 0;
  if ((printedHeader++ % 200) == 0) {
    printProfileHeader();
    if (gc->nursery().enableProfiling()) {
      gc->nursery().printProfileHeader();
    }
  }
}

// The following macros define GC profile metadata fields that are printed
// before the timing information defined by FOR_EACH_GC_PROFILE_TIME.

#define FOR_EACH_GC_PROFILE_COMMON_METADATA(_) \
  _("PID", 7, "%7zu", pid)                     \
  _("Runtime", 14, "0x%12p", runtime)

#define FOR_EACH_GC_PROFILE_SLICE_METADATA(_)         \
  _("Timestamp", 10, "%10.6f", timestamp.ToSeconds()) \
  _("Reason", 20, "%-20.20s", reason)                 \
  _("States", 6, "%6s", formatGCStates(slice))        \
  _("FSNR", 4, "%4s", formatGCFlags(slice))           \
  _("SizeKB", 8, "%8zu", sizeKB)                      \
  _("Zs", 3, "%3zu", zoneCount)                       \
  _("Cs", 3, "%3zu", compartmentCount)                \
  _("Rs", 3, "%3zu", realmCount)                      \
  _("Budget", 6, "%6s", formatBudget(slice))

#define FOR_EACH_GC_PROFILE_METADATA(_)  \
  FOR_EACH_GC_PROFILE_COMMON_METADATA(_) \
  FOR_EACH_GC_PROFILE_SLICE_METADATA(_)

void Statistics::printProfileHeader() {
  if (!enableProfiling_) {
    return;
  }

  Sprinter sprinter;
  if (!sprinter.init()) {
    return;
  }
  sprinter.put(MajorGCProfilePrefix);

#define PRINT_METADATA_NAME(name, width, _1, _2) \
  sprinter.printf(" %-*s", width, name);

  FOR_EACH_GC_PROFILE_METADATA(PRINT_METADATA_NAME)
#undef PRINT_METADATA_NAME

#define PRINT_PROFILE_NAME(_1, text, _2) sprinter.printf(" %-6.6s", text);

  FOR_EACH_GC_PROFILE_TIME(PRINT_PROFILE_NAME)
#undef PRINT_PROFILE_NAME

  sprinter.put("\n");

  JS::UniqueChars str = sprinter.release();
  if (!str) {
    return;
  }
  fputs(str.get(), profileFile());
}

static TimeDuration SumAllPhaseKinds(const Statistics::PhaseKindTimes& times) {
  TimeDuration sum;
  for (PhaseKind kind : AllPhaseKinds()) {
    sum += times[kind];
  }
  return sum;
}

void Statistics::printSliceProfile() {
  maybePrintProfileHeaders();

  const SliceData& slice = slices_.back();
  ProfileDurations times = getProfileTimes(slice);
  updateTotalProfileTimes(times);

  Sprinter sprinter;
  if (!sprinter.init()) {
    return;
  }
  sprinter.put(MajorGCProfilePrefix);

  size_t pid = getpid();
  JSRuntime* runtime = gc->rt;
  TimeDuration timestamp = TimeBetween(creationTime(), slice.end);
  const char* reason = ExplainGCReason(slice.reason);
  size_t sizeKB = gc->heapSize.bytes() / 1024;
  size_t zoneCount = zoneStats.zoneCount;
  size_t compartmentCount = zoneStats.compartmentCount;
  size_t realmCount = zoneStats.realmCount;

#define PRINT_FIELD_VALUE(_1, _2, format, value) \
  sprinter.printf(" " format, value);

  FOR_EACH_GC_PROFILE_METADATA(PRINT_FIELD_VALUE)
#undef PRINT_FIELD_VALUE

  printProfileTimes(times, sprinter);

  JS::UniqueChars str = sprinter.release();
  if (!str) {
    return;
  }
  fputs(str.get(), profileFile());
}

Statistics::ProfileDurations Statistics::getProfileTimes(
    const SliceData& slice) const {
  ProfileDurations times;

  times[ProfileKey::Total] = slice.duration();
  times[ProfileKey::Background] = SumAllPhaseKinds(slice.totalParallelTimes);

#define GET_PROFILE_TIME(name, text, phase)                      \
  if (phase != PhaseKind::NONE) {                                \
    times[ProfileKey::name] = SumPhase(phase, slice.phaseTimes); \
  }
  FOR_EACH_GC_PROFILE_TIME(GET_PROFILE_TIME)
#undef GET_PROFILE_TIME

  return times;
}

void Statistics::updateTotalProfileTimes(const ProfileDurations& times) {
#define UPDATE_PROFILE_TIME(name, _, phase) \
  totalTimes_[ProfileKey::name] += times[ProfileKey::name];
  FOR_EACH_GC_PROFILE_TIME(UPDATE_PROFILE_TIME)
#undef UPDATE_PROFILE_TIME
}

const char* Statistics::formatGCStates(const SliceData& slice) {
  DebugOnly<int> r =
      SprintfLiteral(formatBuffer_, "%1d -> %1d", int(slice.initialState),
                     int(slice.finalState));
  MOZ_ASSERT(r > 0 && r < FormatBufferLength);
  return formatBuffer_;
}

const char* Statistics::formatGCFlags(const SliceData& slice) {
  bool fullGC = gc->fullGCRequested;
  bool shrinkingGC = gcOptions == JS::GCOptions::Shrink;
  bool nonIncrementalGC = nonincrementalReason_ != GCAbortReason::None;
  bool wasReset = slice.resetReason != GCAbortReason::None;

  MOZ_ASSERT(FormatBufferLength >= 5);
  formatBuffer_[0] = fullGC ? 'F' : ' ';
  formatBuffer_[1] = shrinkingGC ? 'S' : ' ';
  formatBuffer_[2] = nonIncrementalGC ? 'N' : ' ';
  formatBuffer_[3] = wasReset ? 'R' : ' ';
  formatBuffer_[4] = '\0';

  return formatBuffer_;
}

const char* Statistics::formatBudget(const SliceData& slice) {
  if (nonincrementalReason_ != GCAbortReason::None ||
      !slice.budget.isTimeBudget()) {
    formatBuffer_[0] = '\0';
    return formatBuffer_;
  }

  DebugOnly<int> r =
      SprintfLiteral(formatBuffer_, "%6" PRIi64, slice.budget.timeBudget());
  MOZ_ASSERT(r > 0 && r < FormatBufferLength);
  return formatBuffer_;
}

/* static */
void Statistics::printProfileTimes(const ProfileDurations& times,
                                   Sprinter& sprinter) {
  for (auto time : times) {
    int64_t millis = int64_t(time.ToMilliseconds());
    sprinter.printf(" %6" PRIi64, millis);
  }

  sprinter.put("\n");
}

constexpr size_t SliceMetadataFormatWidth() {
  size_t fieldCount = 0;
  size_t totalWidth = 0;

#define UPDATE_COUNT_AND_WIDTH(_1, width, _2, _3) \
  fieldCount++;                                   \
  totalWidth += width;
  FOR_EACH_GC_PROFILE_SLICE_METADATA(UPDATE_COUNT_AND_WIDTH)
#undef UPDATE_COUNT_AND_WIDTH

  // Add padding between fields.
  totalWidth += fieldCount - 1;

  return totalWidth;
}

void Statistics::printTotalProfileTimes() {
  if (!enableProfiling_) {
    return;
  }

  Sprinter sprinter;
  if (!sprinter.init()) {
    return;
  }
  sprinter.put(MajorGCProfilePrefix);

  size_t pid = getpid();
  JSRuntime* runtime = gc->rt;

#define PRINT_FIELD_VALUE(_1, _2, format, value) \
  sprinter.printf(" " format, value);

  FOR_EACH_GC_PROFILE_COMMON_METADATA(PRINT_FIELD_VALUE)
#undef PRINT_FIELD_VALUE

  // Use whole width of per-slice metadata to print total slices so the profile
  // totals that follow line up.
  size_t width = SliceMetadataFormatWidth();
  sprinter.printf(" %-*s", int(width), formatTotalSlices());
  printProfileTimes(totalTimes_, sprinter);

  JS::UniqueChars str = sprinter.release();
  if (!str) {
    return;
  }
  fputs(str.get(), profileFile());
}

const char* Statistics::formatTotalSlices() {
  DebugOnly<int> r = SprintfLiteral(
      formatBuffer_, "TOTALS: %7" PRIu64 " slices:", sliceCount_);
  MOZ_ASSERT(r > 0 && r < FormatBufferLength);
  return formatBuffer_;
}
