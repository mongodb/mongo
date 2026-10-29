// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/random.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <cstdint>

#include <boost/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

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

    Milliseconds getBackoffDelay() const;

    /**
     * Same as getBackoffDelay(), but when 'baseBackoffOverride' is present, it is used in place
     * of '_baseBackoff'. When 'baseBackoffOverride' is boost::none, behavior is identical to
     * getBackoffDelay().
     */
    Milliseconds getBackoffDelay(boost::optional<Milliseconds> baseBackoffOverride) const;

    Milliseconds getBackoffDelayAndIncrementAttemptCount() {
        const auto delay = getBackoffDelay();
        ++_attemptCount;
        return delay;
    }

    void incrementAttemptCount() {
        ++_attemptCount;
    }

    void reset() {
        _attemptCount = 0;
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
