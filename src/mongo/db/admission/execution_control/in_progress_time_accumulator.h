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

#include "mongo/platform/atomic_word.h"
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
class MONGO_MOD_PUBLIC InProgressTimeAccumulator {
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
    AtomicWord<int64_t> _count{0};

    // The following are owned by _mutex. '_accumulatedTicks' stores the time-integral of the
    // operation count in count × tick units since construction.
    int64_t _accumulatedTicks = 0;
    TickSource::Tick _lastUpdate;

    // Snapshot of totalMicros() published under _mutex, returned by non-blocking readers that find
    // the lock held.
    mutable AtomicWord<int64_t> _cachedTotalMicros{0};
};

}  // namespace mongo::admission::execution_control
