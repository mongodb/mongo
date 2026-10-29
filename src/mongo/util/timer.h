// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Time tracking object.
 */
class Timer {
public:
    /**
     * Creates a timer with the system default tick source. Should not be created before global
     * initialization completes.
     */
    Timer();

    /**
     * Creates a timer using the specified tick source. Caller retains ownership of TickSource and
     * must keep it in scope until Timer goes out of scope.
     */
    explicit Timer(TickSource* tickSource);

    long long micros() const {
        auto t = _pausedAt == kNotPaused ? _now() : _pausedAt;
        return static_cast<long long>(_microsPerCount * (t - _old));
    }

    int millis() const {
        return static_cast<int>(micros() / 1000);
    }
    int seconds() const {
        return static_cast<int>(micros() / 1000000);
    }
    int minutes() const {
        return seconds() / 60;
    }

    Microseconds elapsed() const {
        return Microseconds{micros()};
    }

    void pause() {
        invariant(_pausedAt == kNotPaused);
        _pausedAt = _now();
    }

    void unpause() {
        invariant(_pausedAt != kNotPaused);
        _old += (_now() - std::exchange(_pausedAt, kNotPaused));
    }

    bool isRunning() const {
        return _pausedAt == kNotPaused;
    }

    void reset() {
        _pausedAt = kNotPaused;
        _old = _now();
    }

private:
    long long _now() const;

    static constexpr long long kNotPaused = -1;

    TickSource* _tickSource;
    // Derived value from _countsPerSecond. This represents the conversion ratio
    // from clock ticks to microseconds.
    double _microsPerCount;
    long long _pausedAt{kNotPaused};
    long long _old;
};

}  // namespace mongo
