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

#include <variant>

#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {
/**
 * The timer increments a counter by the time elapsed since its construction when it goes out of
 * scope.
 */
class ScopedTimer {
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

public:
    ScopedTimer(ScopedTimer&& other) = default;

    ScopedTimer(Nanoseconds* counter, TickSource* ts);
    ScopedTimer(Nanoseconds* counter, ClockSource* cs);

    ~ScopedTimer();

private:
    // Reference to the counter that we are incrementing with the elapsed time.
    Nanoseconds* const _counter;
    TickSource* _tickSource;
    ClockSource* _clockSource;

    Date_t _startCS;
    TickSource::Tick _startTS;
};

/**
 * The timer appends the time elapsed since its construction to a BSON Object.
 */
class TimeElapsedBuilderScopedTimer {
public:
    explicit TimeElapsedBuilderScopedTimer(ClockSource* clockSource,
                                           StringData description,
                                           BSONObjBuilder* builder);
    ~TimeElapsedBuilderScopedTimer();

private:
    ClockSource* _clockSource;
    StringData _description;
    Date_t _beginTime;
    BSONObjBuilder* _builder;
};

/*
 * This helper function only creates a TimeElapsedBuilderScopedTimer when a valid pointer to a
 * builder is passed in. This is used when timing startup tasks, so that tasks that run during
 * startup and outside of startup will only be timed when they are called during startup.
 */
boost::optional<TimeElapsedBuilderScopedTimer> createTimeElapsedBuilderScopedTimer(
    ClockSource* clockSource, StringData description, BSONObjBuilder* builder);
}  // namespace mongo
