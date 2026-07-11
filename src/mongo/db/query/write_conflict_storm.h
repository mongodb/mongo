// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
