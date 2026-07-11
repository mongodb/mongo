// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <utility>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Mock clock source that returns a fixed time until explicitly advanced.
 *
 * Each ClockSourceMock that is constructed tracks the same shared global understanding of time.
 */
class ClockSourceMock : public ClockSource {
public:
    static constexpr auto kInitialNow = Date_t::fromMillisSinceEpoch(1);

    /**
     * Constructs a ClockSourceMock with the current time set to the Unix epoch.
     */
    ClockSourceMock() {
        _tracksSystemClock = false;
    }

    Milliseconds getPrecision() override;
    Date_t now() override;
    void setAlarm(Date_t when, unique_function<void()> action) override;

    /**
     * Advances the current time by the given value.
     */
    void advance(Milliseconds ms);

    /**
     * Resets the current time to the given value.
     */
    void reset(Date_t newNow = kInitialNow);
};

/**
 * Mock clock source where reading the clock also advances the current time by a fixed interval.
 */
class AutoAdvancingClockSourceMock : public ClockSourceMock {
public:
    AutoAdvancingClockSourceMock(Milliseconds increment) : _increment(increment) {}

    Date_t now() override {
        ClockSourceMock::advance(_increment);
        return ClockSourceMock::now();
    }

private:
    const Milliseconds _increment;
};

class SharedClockSourceAdapter final : public ClockSource {
public:
    explicit SharedClockSourceAdapter(std::shared_ptr<ClockSource> source)
        : _source(std::move(source)) {
        _tracksSystemClock = _source->tracksSystemClock();
    }

    Milliseconds getPrecision() override {
        return _source->getPrecision();
    }

    Date_t now() override {
        return _source->now();
    }

    void setAlarm(Date_t when, unique_function<void()> action) override {
        _source->setAlarm(when, std::move(action));
    }

private:
    const std::shared_ptr<ClockSource> _source;
};

}  // namespace mongo
