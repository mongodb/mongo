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

#include "mongo/client/backoff_with_jitter.h"

#include "mongo/util/fail_point.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(setBackoffDelayForTesting);
MONGO_FAIL_POINT_DEFINE(returnMaxBackoffDelay);

Milliseconds BackoffWithJitter::getBackoffDelay() const {
    if (_attemptCount == 0) {
        return Milliseconds{0};
    }

    if (auto fp = setBackoffDelayForTesting.scoped(); MONGO_unlikely(fp.isActive())) {
        auto delayMs = fp.getData()["backoffDelayMs"];
        tassert(10864503,
                "The failpoint `setBackoffDelayForTesting` must contain the expected backoff "
                "delay.",
                delayMs.ok());
        const auto delayMillisecs = Milliseconds(delayMs.safeNumberLong());
        return delayMillisecs;
    }

    const std::int64_t minDelay = 0;
    const auto maxDelay = static_cast<std::int64_t>(std::min(
        static_cast<double>(_maxBackoff.count()), _baseBackoff.count() * std::exp2(_attemptCount)));

    if (MONGO_unlikely(returnMaxBackoffDelay.shouldFail())) {
        return Milliseconds(maxDelay);
    }

    return Milliseconds{std::uniform_int_distribution{minDelay, maxDelay}(_randomEngine())};
}

XorShift128& BackoffWithJitter::_randomEngine() {
    static thread_local XorShift128 random{SecureRandom{}.nextUInt32()};
    return random;
}

}  // namespace mongo
