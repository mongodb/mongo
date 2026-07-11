// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/tick_source.h"

#include <cstdint>
#include <mutex>

namespace mongo::admission::execution_control {

/**
 * Accumulates the total time operations spend in some state — waiting for an execution ticket, or
 * holding one and processing — *including time still being spent right now*. It is the
 * time-integral of the number of operations currently in that state: the area under the "operations
 * currently in state" step function over time.
 *
 * Callers feed it transitions (an operation entering or leaving the state) via onCountChange(). On
 * each transition it folds the elapsed interval, weighted by the number of operations in the state
 * during that interval, into the running total in tick units (count × elapsed ticks). totalMicros()
 * then projects forward to the current instant, so an operation still in the state right now keeps
 * contributing in real time rather than only once it leaves, converting to microseconds only when
 * read. For queued time the transitions are TicketAdmissionStats
 * startedQueueing/finishedQueueing; for processing time they are admissions/releases.
 *
 * The read methods are non-blocking, which is required because they may be evaluated on the
 * FTDC/serverStatus collection thread: they must never stall behind a writer.
 * totalMicros() therefore only *tries* to take the lock; if a writer holds it, it returns the value
 * most recently published under the lock (by the last transition or read), which is at most one
 * transition stale. currentCount() reads an atomic that is only mutated under the lock, so it is
 * exact and never blocks. Writers still serialize among themselves, but that is off the collection
 * path.
 *
 * Uses a monotonic TickSource so durations are unaffected by wall-clock adjustments.
 */
class [[MONGO_MOD_PUBLIC]] InProgressTimeAccumulator {
public:
    explicit InProgressTimeAccumulator(TickSource* tickSource = globalSystemTickSource())
        : _tickSource(tickSource), _lastUpdate(tickSource->getTicks()) {}

    /**
     * Records a net change to the number of operations in the state since the previous transition:
     * 'delta' is positive when more operations entered than left, negative otherwise.
     */
    void onCountChange(int64_t delta) {
        std::lock_guard lk(_mutex);
        _foldElapsed(lk);
        auto newCount = _count.loadRelaxed() + delta;
        // newCount should never be negative, assert it in debug mode and clamp it to 0 in release
        // mode, no need to throw an exception here because it's not a data consistency problem.
        dassert(newCount >= 0);
        _count.storeRelaxed(std::max<int64_t>(0, newCount));

        _cachedTotalMicros.storeRelaxed(
            _tickSource->ticksTo<Microseconds>(_accumulatedTicks).count());
    }

    /**
     * The total time spent in the state so far, including the in-progress time of operations
     * currently in it. Non-blocking: if a writer holds the lock, returns the last published value
     * instead of waiting. Does not otherwise mutate observable state, so reads stay consistent.
     */
    int64_t totalMicros() const {
        std::unique_lock lk(_mutex, std::try_to_lock);
        if (!lk.owns_lock()) {
            return _cachedTotalMicros.loadRelaxed();
        }
        const int64_t total =
            _tickSource->ticksTo<Microseconds>(_accumulatedTicks + _inProgressTicks(lk)).count();
        _cachedTotalMicros.storeRelaxed(total);
        return total;
    }

    /**
     * The number of operations currently in the state. Non-blocking and exact: '_count' is only
     * written under the lock, so an atomic load observes a committed value.
     */
    int64_t currentCount() const {
        return _count.loadRelaxed();
    }

private:
    // Time accrued by the operations currently in the state, between the last folded instant and
    // 'now', in count × tick units.
    int64_t _inProgressTicksAt(WithLock, TickSource::Tick now) const {
        const int64_t count = _count.loadRelaxed();
        if (count == 0) {
            return 0;
        }
        return count * (now - _lastUpdate);
    }

    int64_t _inProgressTicks(WithLock lk) const {
        return _inProgressTicksAt(lk, _tickSource->getTicks());
    }

    // Folds the interval since the last transition into the running total and advances _lastUpdate.
    // Reads the clock once so the folded interval and the new _lastUpdate share the same tick
    // instant.
    void _foldElapsed(WithLock lk) {
        const auto now = _tickSource->getTicks();
        _accumulatedTicks += _inProgressTicksAt(lk, now);
        _lastUpdate = now;
    }

    TickSource* const _tickSource;

    mutable std::mutex _mutex;

    // Written only under _mutex; read without the lock by currentCount() and _inProgressTicks().
    Atomic<int64_t> _count{0};

    // The following are owned by _mutex. '_accumulatedTicks' stores the time-integral of the
    // operation count in count × tick units since construction.
    int64_t _accumulatedTicks = 0;
    TickSource::Tick _lastUpdate;

    // Snapshot of totalMicros() published under _mutex, returned by non-blocking readers that find
    // the lock held.
    mutable Atomic<int64_t> _cachedTotalMicros{0};
};

}  // namespace mongo::admission::execution_control
