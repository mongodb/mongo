/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/write_conflict_storm.h"

#include "mongo/db/client.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/aligned.h"

#include <fmt/format.h>

namespace mongo {

int adaptiveWriteConflictRetryLimit(int limitMax, int limitMin, int threshold, int waiters) {
    if (limitMax <= 0) {
        return 0;
    }
    if (limitMin < 0) {
        limitMin = 0;
    }
    if (limitMin > limitMax) {
        limitMin = limitMax;
    }
    if (threshold <= 0) {
        return limitMax;
    }
    // Saturate at limitMin once waiters reaches threshold.
    const int clampedWaiters = waiters > threshold ? threshold : waiters;
    return limitMax - (limitMax - limitMin) * clampedWaiters / threshold;
}

namespace {

// Count of plan executors (both classic and express) currently in a WCE retry streak.
// CacheExclusive puts the gauge on its own cache line.
CacheExclusive<Atomic<int32_t>> gWriteConflictRetryWaiters{};

// serverStatus / FTDC mirror.
auto& writeConflictRetryWaitersGauge =
    *MetricBuilder<Atomic64Metric>("operation.writeConflictRetryWaiters");

auto& writeConflictRetryLimitHitMetric =
    *MetricBuilder<Counter64>{"operation.writeConflictRetryLimitHit"};

}  // namespace

void wceWaitersAcquire() {
    const int32_t newVal = gWriteConflictRetryWaiters->fetchAndAddRelaxed(1) + 1;
    writeConflictRetryWaitersGauge.set(newVal);
}

void wceWaitersRelease() noexcept {
    const int32_t newVal = gWriteConflictRetryWaiters->fetchAndSubtractRelaxed(1) - 1;
    writeConflictRetryWaitersGauge.set(newVal);
}

int32_t wceWaitersCount() {
    return gWriteConflictRetryWaiters->loadRelaxed();
}

void wceRecordLimitHit() {
    writeConflictRetryLimitHitMetric.increment();
}

WCStormWaiterGuard::~WCStormWaiterGuard() {
    if (_active) {
        wceWaitersRelease();
    }
}

void WCStormWaiterGuard::activate() {
    if (!_active) {
        wceWaitersAcquire();
        _active = true;
    }
}

void WCStormWaiterGuard::release() noexcept {
    if (_active) {
        wceWaitersRelease();
        _active = false;
    }
}

bool WCStormWaiterGuard::active() const noexcept {
    return _active;
}

void checkWriteConflictStorm(OperationContext* opCtx,
                             WCStormWaiterGuard& guard,
                             size_t writeConflictsInARow) {
    auto* const client = opCtx->getClient();
    if (!client || !client->isFromUserConnection() || client->isInternalClient())
        return;

    const int limitMax = internalQueryWriteConflictRetryLimitMax.loadRelaxed();
    if (limitMax <= 0)
        return;

    guard.activate();

    const int limitMin = internalQueryWriteConflictRetryLimitMin.loadRelaxed();
    const int threshold = internalQueryWriteConflictRetryLimitWaitersThreshold.loadRelaxed();
    const int waiters = wceWaitersCount();
    const int effectiveLimit =
        adaptiveWriteConflictRetryLimit(limitMax, limitMin, threshold, waiters);
    if (effectiveLimit > 0 && writeConflictsInARow > static_cast<size_t>(effectiveLimit)) {
        wceRecordLimitHit();
        throwWriteConflictRetryLimitExceededException(
            fmt::format("Aborting after {} consecutive WriteConflictExceptions; adaptive limit "
                        "was {} (limitMax={}, limitMin={}, threshold={}, waiters={}).",
                        writeConflictsInARow,
                        effectiveLimit,
                        limitMax,
                        limitMin,
                        threshold,
                        waiters));
    }
}

}  // namespace mongo
