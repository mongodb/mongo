/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/util/duration.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"

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
