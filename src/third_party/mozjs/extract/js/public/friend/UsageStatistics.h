/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Telemetry and use counter functionality. */

#ifndef js_friend_UsageStatistics_h
#define js_friend_UsageStatistics_h

#include <stdint.h>  // uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

/*
 * Telemetry reasons passed to the accumulate telemetry callback.
 *
 * It's OK for these enum values to change as they will be mapped to a fixed
 * member of the mozilla::Telemetry::HistogramID enum by the callback.
 */
#define FOR_EACH_JS_METRIC(_)                   \
  _(GC_REASON_2, Enumeration)                   \
  _(GC_IS_COMPARTMENTAL, Boolean)               \
  _(GC_ZONE_COUNT, QuantityDistribution)        \
  _(GC_ZONES_COLLECTED, QuantityDistribution)   \
  _(GC_MS, TimeDuration_MS)                     \
  _(GC_BUDGET_MS_2, TimeDuration_MS)            \
  _(GC_BUDGET_WAS_INCREASED, Boolean)           \
  _(GC_SLICE_WAS_LONG, Boolean)                 \
  _(GC_BUDGET_OVERRUN, TimeDuration_US)         \
  _(GC_ANIMATION_MS, TimeDuration_MS)           \
  _(GC_MAX_PAUSE_MS_2, TimeDuration_MS)         \
  _(GC_PREPARE_MS, TimeDuration_MS)             \
  _(GC_MARK_MS, TimeDuration_MS)                \
  _(GC_SWEEP_MS, TimeDuration_MS)               \
  _(GC_COMPACT_MS, TimeDuration_MS)             \
  _(GC_MARK_ROOTS_US, TimeDuration_US)          \
  _(GC_MARK_GRAY_MS_2, TimeDuration_MS)         \
  _(GC_MARK_WEAK_MS, TimeDuration_MS)           \
  _(GC_SLICE_MS, TimeDuration_MS)               \
  _(GC_SLOW_PHASE, Enumeration)                 \
  _(GC_SLOW_TASK, Enumeration)                  \
  _(GC_MMU_50, Percentage)                      \
  _(GC_RESET, Boolean)                          \
  _(GC_RESET_REASON, Enumeration)               \
  _(GC_NON_INCREMENTAL, Boolean)                \
  _(GC_NON_INCREMENTAL_REASON, Enumeration)     \
  _(GC_MINOR_REASON, Enumeration)               \
  _(GC_MINOR_REASON_LONG, Enumeration)          \
  _(GC_MINOR_US, TimeDuration_US)               \
  _(GC_NURSERY_BYTES_2, MemoryDistribution)     \
  _(GC_PRETENURE_COUNT_2, QuantityDistribution) \
  _(GC_NURSERY_PROMOTION_RATE, Percentage)      \
  _(GC_TENURED_SURVIVAL_RATE, Percentage)       \
  _(GC_MARK_RATE_2, QuantityDistribution)       \
  _(GC_TIME_BETWEEN_S, TimeDuration_S)          \
  _(GC_TIME_BETWEEN_SLICES_MS, TimeDuration_MS) \
  _(GC_SLICE_COUNT, QuantityDistribution)       \
  _(DESERIALIZE_BYTES, MemoryDistribution)      \
  _(DESERIALIZE_ITEMS, QuantityDistribution)    \
  _(DESERIALIZE_US, TimeDuration_US)            \
  _(GC_EFFECTIVENESS, MemoryDistribution)       \
  _(GC_PARALLEL_MARK_SPEEDUP, Integer)          \
  _(GC_PARALLEL_MARK_UTILIZATION, Percentage)   \
  _(GC_PARALLEL_MARK_INTERRUPTIONS, Integer)    \
  _(GC_TASK_START_DELAY_US, TimeDuration_US)

// clang-format off
#define ENUM_DEF(NAME, _) NAME,
enum class JSMetric {
  FOR_EACH_JS_METRIC(ENUM_DEF)
  Count
};
#undef ENUM_DEF
// clang-format on

using JSAccumulateTelemetryDataCallback = void (*)(JSMetric, uint32_t);

extern JS_PUBLIC_API void JS_SetAccumulateTelemetryCallback(
    JSContext* cx, JSAccumulateTelemetryDataCallback callback);

/*
 * Use counter names passed to the accumulate use counter callback.
 *
 * It's OK to for these enum values to change as they will be mapped to a
 * fixed member of the mozilla::UseCounter enum by the callback.
 */

enum class JSUseCounter { ASMJS, WASM };

using JSSetUseCounterCallback = void (*)(JSObject*, JSUseCounter);

extern JS_PUBLIC_API void JS_SetSetUseCounterCallback(
    JSContext* cx, JSSetUseCounterCallback callback);

#endif  // js_friend_UsageStatistics_h
