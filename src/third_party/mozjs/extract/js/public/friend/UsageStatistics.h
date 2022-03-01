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
#define MAP_JS_TELEMETRY(_)                 \
  _(JS_TELEMETRY_GC_REASON)                 \
  _(JS_TELEMETRY_GC_IS_ZONE_GC)             \
  _(JS_TELEMETRY_GC_MS)                     \
  _(JS_TELEMETRY_GC_BUDGET_MS_2)            \
  _(JS_TELEMETRY_GC_BUDGET_OVERRUN)         \
  _(JS_TELEMETRY_GC_ANIMATION_MS)           \
  _(JS_TELEMETRY_GC_MAX_PAUSE_MS_2)         \
  _(JS_TELEMETRY_GC_PREPARE_MS)             \
  _(JS_TELEMETRY_GC_MARK_MS)                \
  _(JS_TELEMETRY_GC_SWEEP_MS)               \
  _(JS_TELEMETRY_GC_COMPACT_MS)             \
  _(JS_TELEMETRY_GC_MARK_ROOTS_US)          \
  _(JS_TELEMETRY_GC_MARK_GRAY_MS_2)         \
  _(JS_TELEMETRY_GC_MARK_WEAK_MS)           \
  _(JS_TELEMETRY_GC_SLICE_MS)               \
  _(JS_TELEMETRY_GC_SLOW_PHASE)             \
  _(JS_TELEMETRY_GC_SLOW_TASK)              \
  _(JS_TELEMETRY_GC_MMU_50)                 \
  _(JS_TELEMETRY_GC_RESET)                  \
  _(JS_TELEMETRY_GC_RESET_REASON)           \
  _(JS_TELEMETRY_GC_NON_INCREMENTAL)        \
  _(JS_TELEMETRY_GC_NON_INCREMENTAL_REASON) \
  _(JS_TELEMETRY_GC_MINOR_REASON)           \
  _(JS_TELEMETRY_GC_MINOR_REASON_LONG)      \
  _(JS_TELEMETRY_GC_MINOR_US)               \
  _(JS_TELEMETRY_GC_NURSERY_BYTES)          \
  _(JS_TELEMETRY_GC_PRETENURE_COUNT_2)      \
  _(JS_TELEMETRY_GC_NURSERY_PROMOTION_RATE) \
  _(JS_TELEMETRY_GC_TENURED_SURVIVAL_RATE)  \
  _(JS_TELEMETRY_GC_MARK_RATE_2)            \
  _(JS_TELEMETRY_GC_TIME_BETWEEN_S)         \
  _(JS_TELEMETRY_GC_TIME_BETWEEN_SLICES_MS) \
  _(JS_TELEMETRY_GC_SLICE_COUNT)            \
  _(JS_TELEMETRY_DESERIALIZE_BYTES)         \
  _(JS_TELEMETRY_DESERIALIZE_ITEMS)         \
  _(JS_TELEMETRY_DESERIALIZE_US)            \
  _(JS_TELEMETRY_GC_EFFECTIVENESS)

// clang-format off
#define ENUM_DEF(NAME) NAME ,
enum {
  MAP_JS_TELEMETRY(ENUM_DEF)
  JS_TELEMETRY_END
};
#undef ENUM_DEF
// clang-format on

using JSAccumulateTelemetryDataCallback = void (*)(int, uint32_t, const char*);

extern JS_PUBLIC_API void JS_SetAccumulateTelemetryCallback(
    JSContext* cx, JSAccumulateTelemetryDataCallback callback);

/*
 * Use counter names passed to the accumulate use counter callback.
 *
 * It's OK to for these enum values to change as they will be mapped to a
 * fixed member of the mozilla::UseCounter enum by the callback.
 */

enum class JSUseCounter { ASMJS, WASM, WASM_DUPLICATE_IMPORTS };

using JSSetUseCounterCallback = void (*)(JSObject*, JSUseCounter);

extern JS_PUBLIC_API void JS_SetSetUseCounterCallback(
    JSContext* cx, JSSetUseCounterCallback callback);

#endif  // js_friend_UsageStatistics_h
