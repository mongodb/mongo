// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <cstdint>

#include <boost/optional.hpp>

namespace mongo {

/**
 * Probabilistic query-memory load shedding, driven by process RSS. No per-operation
 * reservation/accounting: the decision is made at check-for-interrupt points from the latest RSS
 * sample and the operation's own tracked memory. See README.query_memory_load_shedding.md.
 */

/**
 * Starts the RSS-sampling monitor iff load shedding is enabled. Idempotent; a no-op when disabled
 * or on a router. Before launching the background sampling thread it publishes one RSS sample
 * synchronously on the calling thread, so the pressure signal is live immediately rather than only
 * after the first periodic tick.
 *
 * If the PeriodicRunner is not yet installed: callers that may run before it exists (the on-update
 * hook during startup-config application) pass 'ignoreWhenNoPeriodicRunner = true' to no-op and
 * rely on the explicit startup call to start the monitor later. The startup caller leaves it false
 * so a missing runner while enabled trips a tassert rather than silently leaving shedding off.
 */
[[MONGO_MOD_PUBLIC]] void startQueryMemoryRssMonitor(ServiceContext* serviceContext,
                                                     bool ignoreWhenNoPeriodicRunner = false);

/**
 * Stops the RSS-sampling monitor and clears the published RSS sample. Called from the low-mark
 * knob's on-update hook when the feature is disabled at runtime.
 */
[[MONGO_MOD_PUBLIC]] void stopQueryMemoryRssMonitor(ServiceContext* serviceContext);

/**
 * The load-shed decision, called from query-execution interrupt/yield points (see callers). Returns
 * QueryMemoryLimitExceeded (and marks the op killed) when the operation should be load-shed,
 * otherwise Status::OK() (disabled/exempt, RSS below the low mark, or the roll did not select it).
 * noexcept-compatible: never throws.
 */
[[MONGO_MOD_PUBLIC]] Status queryMemoryCheckLoadShedding(OperationContext* opCtx);

/**
 * Opt-in load-shedding eligibility. A user-facing read command marks its operation eligible at the
 * top of run(); only marked operations are ever shed. The flag is scoped to the query's
 * QueryLifespan, so it is set once on the originating command and persists across that query's
 * getMores (and while its cursor is idle) without being re-marked per request. Defaults false.
 */
[[MONGO_MOD_PUBLIC]] bool isOperationQueryMemorySheddingEligible(OperationContext* opCtx);
[[MONGO_MOD_PUBLIC]] void markOperationQueryMemorySheddingEligible(OperationContext* opCtx);

/**
 * Appends the load-shedding metrics to 'b'. No-op when disabled.
 */
void appendQueryMemoryLoadSheddingStats(ServiceContext* serviceContext, BSONObjBuilder& b);

/**
 * on_update hook for queryMemoryLoadSheddingLowMarkPercent: (re)starts the monitor on runtime
 * enable, and stops it (clearing the RSS sample) on runtime disable.
 */
Status onUpdateQueryMemoryLoadSheddingEnablement(std::int32_t newPercent);

/**
 * Cross-validators for the water marks: when enabled (low >= 0) the low mark must stay below the
 * high mark, so a setting like low=90/high=85 cannot silently disarm shedding.
 */
Status validateQueryMemoryLoadSheddingLowMark(std::int32_t lowMarkPercent,
                                              const boost::optional<TenantId>&);
Status validateQueryMemoryLoadSheddingHighMark(std::int32_t highMarkPercent,
                                               const boost::optional<TenantId>&);

namespace query_memory_load_shedding_detail {
/**
 * Per-check shed probability in [0, 1]. Pure (no RNG or global state) for testability. Returns 0
 * at/below the low mark and for degenerate inputs, and 1 at/above the high mark. See the definition
 * (and the README) for the formula.
 */
double shedProbability(int64_t rssBytes,
                       int64_t memLimitBytes,
                       int64_t opTrackedBytes,
                       int32_t lowMarkPercent,
                       int32_t highMarkPercent,
                       int64_t sizeReferenceBytes,
                       Milliseconds sinceLastCheck);

/**
 * Test-only read accessor for an operation's dt baseline (the internal lastEvalTime). Exposed here
 * (read-only, in the testability namespace) so tests can observe the priming/throttle behavior
 * without the load-shedding state's reset rules being mutable from outside the implementation.
 */
Date_t lastEvalTimeForTest(OperationContext* opCtx);
}  // namespace query_memory_load_shedding_detail

}  // namespace mongo
