// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/util/duration.h"

#include <cstdint>
#include <iterator>
#include <map>
#include <ratio>
#include <utility>

namespace mongo {
/**
 * A class containing the latency distribution of operations performed. To avoid memory explosion
 * the distribution is computed using buckets of a user-provided resolution.
 */
class LatencyPercentileDistribution {
public:
    LatencyPercentileDistribution(Microseconds resolution) : _resolution(resolution) {};

    // Adds the provided duration entry into the distribution
    void addEntry(Microseconds duration) {
        // As the entries are stored in each bucket we need to acquire the correct key for the
        // provided duration. In this case we want to bucket rounding up to the resolution
        // desired. This can be accomplished by performing integer division that might round down
        // and checking if it needs to be rounded up instead. That is to say if remainder of the
        // division was larger than 0.
        auto key = (duration + _resolution - Microseconds{1}) / _resolution.count();
        _orderedBuckets[key]++;
        _totalEntries++;
    }

    // Merges this distribution with another one, giving the combined result of the two.
    //
    // This is useful in multithreaded scenarios as each thread can update a local copy and then
    // merge all of them at the end, avoiding all concurrency synchronisation to the bare minimum.
    LatencyPercentileDistribution mergeWith(const LatencyPercentileDistribution& other) {
        LatencyPercentileDistribution result(_resolution);
        for (const auto& [key, count] : this->_orderedBuckets) {
            result._orderedBuckets[key] += count;
        }
        for (const auto& [key, count] : other._orderedBuckets) {
            result._orderedBuckets[key] += count;
        }
        result._totalEntries = this->_totalEntries + other._totalEntries;
        return result;
    }

    // Obtain the provided percentile of latency. The returned value will be an approximation at
    // most off by one resolution unit.
    Microseconds getPercentile(float percentile) const {
        int64_t scannedEntries = 0;
        auto targetEntries = static_cast<int64_t>(_totalEntries * percentile);
        // The buckets are sorted so we iteratively add the percentile values until we surpass the
        // target.
        auto iter = _orderedBuckets.begin();
        Microseconds previousMicrosecondsKey{0};
        while (scannedEntries < targetEntries && iter != _orderedBuckets.end()) {
            auto [key, value] = *iter;
            auto newMicrosecondsKey = key;
            if (scannedEntries + value >= targetEntries) {
                // We need to perform the inverse operation on the key for the actual value. As we
                // divide by resolution we now multiply by it in order to get the original value
                // back.
                auto newMicros = newMicrosecondsKey * _resolution.count();
                auto previousMicros = previousMicrosecondsKey * _resolution.count();
                auto interpolationRate =
                    (targetEntries - scannedEntries) / static_cast<float>(value);
                auto differenceMicroseconds = (newMicros - previousMicros).count();
                auto interpolatedMicros = interpolationRate * differenceMicroseconds;
                return previousMicros + Microseconds{static_cast<int64_t>(interpolatedMicros)};
            } else {
                previousMicrosecondsKey = newMicrosecondsKey;
                scannedEntries += value;
                iter++;
            }
        }
        return Microseconds{0};
    }

    Microseconds getMax() const {
        auto end = *_orderedBuckets.rbegin();
        return end.first * _resolution.count();
    }

    int64_t numEntries() const {
        return _totalEntries;
    }

private:
    Microseconds _resolution;
    // We use a std::map as it guarantees preserving the ordering based on the key provided.
    std::map<Microseconds, int32_t> _orderedBuckets;
    int64_t _totalEntries = 0;
};
}  // namespace mongo
