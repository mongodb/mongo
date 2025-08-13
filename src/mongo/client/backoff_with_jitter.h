/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/platform/random.h"
#include "mongo/util/duration.h"

#include <cstdint>

namespace mongo {

/**
 * Generic implementation of a retry with full jitter and exponential backoff.
 *
 * The amount of delay follows this formula: j * min(maxBackoff, baseBackoff * 2^i)
 *  - Where i is the amount of retry attempts
 *  - Where j is the random jitter between 0 and 1
 *
 *  It is expected users increment once to mark that the first request has been performed.
 */
struct BackoffWithJitter {
    BackoffWithJitter(Milliseconds baseBackoff, Milliseconds maxBackoff)
        : _baseBackoff{baseBackoff}, _maxBackoff{maxBackoff} {
        invariant(_baseBackoff <= _maxBackoff);
    }

    Milliseconds getBackoffDelay() const {
        if (_attemptCount == 0) {
            return Milliseconds{0};
        }

        const std::int64_t minDelay = 0;
        const auto maxDelay =
            static_cast<std::int64_t>(std::min(static_cast<double>(_maxBackoff.count()),
                                               _baseBackoff.count() * std::exp2(_attemptCount)));

        return Milliseconds{std::uniform_int_distribution{minDelay, maxDelay}(_randomEngine())};
    }

    Milliseconds getBackoffDelayAndIncrementAttemptCount() {
        const auto delay = getBackoffDelay();
        ++_attemptCount;
        return delay;
    }

    void incrementAttemptCount() {
        ++_attemptCount;
    }

    void setAttemptCount_forTest(std::uint32_t value) {
        _attemptCount = value;
    }

    /**
     * As the random engine is thread local, consider calling this method for each threads in a
     * threaded test.
     */
    static void initRandomEngineWithSeed_forTest(std::uint32_t seed) {
        _randomEngine() = XorShift128{seed};
    }

private:
    Milliseconds _baseBackoff;
    Milliseconds _maxBackoff;
    std::size_t _attemptCount = 0;

    static XorShift128& _randomEngine();
};

}  // namespace mongo
