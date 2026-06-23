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

#pragma once

#include <cstddef>
#include <cstdint>

namespace mongo {

class OperationContext;

/**
 * Computes the effective per-op WriteConflictException retry cap given the configured upper bound
 * (limitMax), lower bound (limitMin), waiter threshold, and current waiter count.
 *
 *   - limitMax <= 0  -> limit disabled, returns 0 (caller treats as "no limit").
 *   - threshold <= 0 -> no adaptive scaling; always returns limitMax.
 *   - waiters >= threshold -> returns limitMin (saturated pressure).
 *   - otherwise: linear interpolation. Integer-division floor biases slightly toward more
 *     retries at mid-load; exercised in unit tests.
 */
int adaptiveWriteConflictRetryLimit(int limitMax, int limitMin, int threshold, int waiters);

// Process-wide count of plan executors in a WriteConflictException (WCE) retry streak.
// Shared by both PlanExecutorImpl and PlanExecutorExpress so adaptive scaling sees total pressure.
void wceWaitersAcquire();
void wceWaitersRelease() noexcept;
int32_t wceWaitersCount();
void wceRecordLimitHit();

// RAII guard that registers this operation as a write-conflict storm waiter.
// activate() is idempotent; destructor decrements the global counter if active.
// release() explicitly ends the streak (classic executor resets on every successful step).
class WCStormWaiterGuard {
public:
    WCStormWaiterGuard() = default;
    ~WCStormWaiterGuard();
    void activate();
    void release() noexcept;
    [[nodiscard]] bool active() const noexcept;
    WCStormWaiterGuard(const WCStormWaiterGuard&) = delete;
    WCStormWaiterGuard& operator=(const WCStormWaiterGuard&) = delete;
    WCStormWaiterGuard(WCStormWaiterGuard&&) = delete;
    WCStormWaiterGuard& operator=(WCStormWaiterGuard&&) = delete;

private:
    bool _active = false;
};

// Rejects user write operations via WriteConflictRetryLimitExceeded when the
// per-op retry cap is exceeded. No-op for internal connections.
// Must be called while the write ticket is still held (before any yield).
void checkWriteConflictStorm(OperationContext* opCtx,
                             WCStormWaiterGuard& guard,
                             size_t writeConflictsInARow);

}  // namespace mongo
