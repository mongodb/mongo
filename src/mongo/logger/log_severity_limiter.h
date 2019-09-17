/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/logger/log_severity.h"
#include "mongo/logger/logstream_builder.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/time_support.h"

namespace mongo::logger {

/**
 * A thread-safe widget to chose a lower severity for a given key once every T milliseconds.
 *
 * Note that this class is thread-safe but not global. You must maintain an instance to preserve the
 * Date_t for the last time you were given the "limited" severity instead of the "normal" severity.
 * The recommended method for this style of lifetime extension is via a static local variable.
 * The MONGO_GET_LIMITED_SEVERITY macro below provides a relatively painless way to get the
 * appropriate severity out of a LogSeverityLimiter initialized as a static local variable in a
 * contained block scope.
 */
template <typename KeyT>
class LogSeverityLimiter {
public:
    static constexpr auto kDefaultPeriod = Seconds{1};
    static constexpr auto kDefaultLimitedDLevel = 0;
    static constexpr auto kDefaultNormalDLevel = 2;

public:
    /**
     * Construct a LogSeverityLimiter
     *
     * @param normalDLevel  The debug level to use most of the time
     * @param limitedDLevel The debug level to use at most at a specified interval
     * @param period        The minimum period before getLogSeverityFor returns the limited severity
     *                      again
     */
    explicit LogSeverityLimiter(Milliseconds period, int limitedDLevel, int normalDLevel)
        : _period{period},
          _limitedLogSeverity{LogstreamBuilder::severityCast(limitedDLevel)},
          _normalLogSeverity{LogstreamBuilder::severityCast(normalDLevel)} {
        // This widget has no point if limited isn't a lower verbosity than normal
        invariant(limitedDLevel < normalDLevel);
    }

    /**
     * Return the appropriate LogSeverity for the key provided at this point in time
     *
     * @param key   The key for this series of LogSeverity. This must have a copy ctor.
     */
    LogSeverity nextFor(const KeyT& key) {
        auto now = Date_t::now();

        stdx::lock_guard<Latch> lk(_mutex);
        auto& cutoff = _cutoffByKey[key];

        if (now > cutoff) {
            // This means that the next log batch at this level will be at least _period
            // milliseconds after this one. This does effectively skew.
            cutoff = now + _period;
            return _limitedLogSeverity;
        }
        return _normalLogSeverity;
    }

private:
    Milliseconds _period;

    LogSeverity _limitedLogSeverity;
    LogSeverity _normalLogSeverity;

    Mutex _mutex = MONGO_MAKE_LATCH("LogSeverityLimiter::_mutex");
    stdx::unordered_map<KeyT, Date_t> _cutoffByKey;
};

#define MONGO_GET_LIMITED_SEVERITY(KEY, PERIOD, LIMITED, NORMAL)                                \
    [&] {                                                                                       \
        using KeyT = std::decay_t<decltype(KEY)>;                                               \
        static auto limiter = mongo::logger::LogSeverityLimiter<KeyT>(PERIOD, LIMITED, NORMAL); \
        return limiter.nextFor(KEY);                                                            \
    }()

}  // namespace mongo::logger
