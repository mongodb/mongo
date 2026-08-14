// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_detail.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mongo::write_conflict_backoff {

// Attempts 1..kFastAttempts sleep 0/1/2ms so short contention resolves without a meaningful
// wait; the geometric ramp starts at attempt kFastAttempts + 1.
inline constexpr size_t kFastAttempts = 3;

// Multiplier applied to the ramp sleep on every attempt past kFastAttempts + 1. A steep ramp
// keeps the attempt count low under a storm; each retry execution costs CPU and inflicts
// conflicts on the other contenders.
inline constexpr int64_t kRampGrowthFactor = 4;

/**
 * Backoff millis (before adding jitter) for the plan-executor write-conflict retries; the final
 * sleep is jittered to [base, 2 * base]. The first attempt retries immediately and attempts
 * 2-3 sleep 1ms/2ms, so short contention resolves without a meaningful wait. From attempt 4
 * the sleep starts at 'rampStartMs' and multiplies by kRampGrowthFactor each attempt, capped
 * at 'capMs'. 'numAttempts' is 1-based.
 */
inline int64_t backoffBaseMillis(size_t numAttempts, int64_t rampStartMs, int64_t capMs) {
    if (numAttempts <= 1) {
        return 0;
    }
    if (numAttempts == 2) {
        return std::min<int64_t>(1, capMs);
    }
    if (numAttempts == 3) {
        return std::min<int64_t>(2, capMs);
    }
    int64_t base = rampStartMs;
    for (size_t i = 4; i < numAttempts && base < capMs; ++i) {
        base *= kRampGrowthFactor;
    }
    return std::min(base, capMs);
}

/**
 * Logs the write conflict and sleeps per backoffBaseMillis() with the ramp taken from the
 * internalQueryWriteConflictBackoff{RampStartMs,CapMs} knobs. 'attempt' is 1-based. Callers
 * must have released ticket resources before sleeping (see
 * internalQueryEnableWriteConflictBackoffWithoutTicket). The sleep honors interruption and
 * the operation deadline; an interrupt exception propagates to the caller mid-backoff.
 * RampStartMs == 0 is the kill switch: it routes to the legacy stepped backoff
 * (logWriteConflictAndBackoff), whose sleep is not interruptible.
 */
inline void logAndBackoff(OperationContext* opCtx,
                          size_t attempt,
                          std::string_view operation,
                          std::string_view reason,
                          const NamespaceStringOrUUID& nssOrUUID) {
    auto rampStartMs = internalQueryWriteConflictBackoffRampStartMs.loadRelaxed();
    if (MONGO_unlikely(rampStartMs == 0)) {
        logWriteConflictAndBackoff(attempt, operation, reason, nssOrUUID);
        return;
    }
    // Log at Info at most once per second process-wide; every other backoff logs at Debug(1).
    static logv2::SeveritySuppressor logSeverity{
        Seconds{1}, logv2::LogSeverity::Info(), logv2::LogSeverity::Debug(1)};
    auto severity = logSeverity();
    logv2::detail::doLog(13324301,
                         severity,
                         {logv2::LogComponent::kWrite},
                         "Caught WriteConflictException",
                         "operation"_attr = operation,
                         "reason"_attr = reason,
                         "namespace"_attr = toStringForLogging(nssOrUUID),
                         "attempts"_attr = attempt);
    auto base = backoffBaseMillis(
        attempt, rampStartMs, internalQueryWriteConflictBackoffCapMs.loadRelaxed());
    if (base == 0) {
        return;
    }
    // Full jitter: uniform in [base, 2*base] ms, so competing threads that hit this path
    // simultaneously spread their wakeups instead of re-contending as a synchronized burst.
    // Interruptible: killOp / maxTimeMS end the sleep by throwing, as in the
    // TemporarilyUnavailable backoff.
    auto& prng = opCtx->getClient()->getPrng();
    opCtx->sleepFor(Milliseconds(base + prng.nextInt64(base + 1)));
}

}  // namespace mongo::write_conflict_backoff
