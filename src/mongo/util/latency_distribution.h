/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <map>

#include "mongo/util/duration.h"

namespace mongo {
/**
 * A class containing the latency distribution of operations performed. To avoid memory explosion
 * the distribution is computed using buckets of a user-provided resolution.
 */
class LatencyPercentileDistribution {
public:
    LatencyPercentileDistribution(Microseconds resolution) : _resolution(resolution){};

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
