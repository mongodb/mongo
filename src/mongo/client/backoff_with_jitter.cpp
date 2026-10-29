// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/backoff_with_jitter.h"

#include "mongo/util/fail_point.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(setBackoffDelayForTesting);
MONGO_FAIL_POINT_DEFINE(returnMaxBackoffDelay);

Milliseconds BackoffWithJitter::getBackoffDelay() const {
    return getBackoffDelay(boost::none);
}

Milliseconds BackoffWithJitter::getBackoffDelay(
    boost::optional<Milliseconds> baseBackoffOverride) const {
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
    const auto baseBackoff =
        baseBackoffOverride ? baseBackoffOverride->count() : _baseBackoff.count();
    const auto maxDelay = static_cast<std::int64_t>(
        std::min(static_cast<double>(_maxBackoff.count()), baseBackoff * std::exp2(_attemptCount)));

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
