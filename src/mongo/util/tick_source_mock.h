// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Mock tick source that can be parameterized on a duration type.
 *
 * Its internal tick count will be tracked in the unit of the duration type parameter. For example,
 * for TickSourceMock<Milliseconds>, 1 tick = 1 millisecond. It advances its tick by a duration that
 * can be set, but that defaults to 0 milliseconds; by default it will only advance when advance()
 * is called.
 */
template <typename D = Milliseconds>
class TickSourceMock final : public TickSource {
public:
    TickSource::Tick getTicks() override {
        auto currentTicks = _currentTicks.load();
        advance(_durationToAdvanceBy);
        return currentTicks;
    };

    TickSource::Tick getTicksPerSecond() override {
        static_assert(D::period::num == 1,
                      "Cannot measure ticks per second for duration types larger than 1 second.");
        return D::period::den;
    };

    /**
     * Advance the ticks by the given amount of milliseconds.
     */
    void advance(const D& duration) {
        _currentTicks.fetchAndAdd(duration.count());
    }

    /**
     * Resets the tick count to the given value.
     */
    void reset(TickSource::Tick tick) {
        _currentTicks.store(std::move(tick));
    }

    /**
     * Set the amount that we want the tick source to advance by each time we read it.
     */
    void setAdvanceOnRead(const D& durationToAdvanceBy) {
        _durationToAdvanceBy = durationToAdvanceBy;
    }

private:
    // Start at 1 because some consumers (e.g. CurOp::startTime()) treat 0 as an "unstarted"
    // sentinel.
    Atomic<TickSource::Tick> _currentTicks{1};
    D _durationToAdvanceBy = D{0};
};

}  // namespace mongo
