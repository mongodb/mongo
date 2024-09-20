/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <random>

#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/duration.h"

namespace mongo::workload_simulation {

struct RWPair {
    int32_t read = 0;
    int32_t write = 0;
};

/**
 * Base class to model workload characteristics. The idea behind the approach is to specify an
 * optimal read and write concurrency level, and the throughput that is achieved by the server for
 * the workload at that concurrency level.
 *
 * A derived class must override the '_throughput' method to specify the level of throughput the
 * server will achieve for a given concurrency level. Different implementations will model the
 * throughput of the workload for different values based on some model (e.g. parabolic, piecewise
 * linear, etc.).
 *
 * The base class then derives the typical latency for an operation at a specified concurrency
 * level from the `_throughput' value. In order to make this a bit more realistic, we add some
 * jitter to the latency using a multiplier of '1 + jitter()', clamped to the range of (0.0,
 * kMaxJitterMultiplier), where jitter() returns a normally-distributed value with mean 0.0, and
 * standard deviation 'jitterDev'. If 'jitterDev' is zero, we do not add jitter to the latency.
 */
class MockWorkloadCharacteristics {
    static constexpr double kMaxJitterMultiplier = 5.0;

public:
    MockWorkloadCharacteristics(RWPair optimalConcurrency,
                                RWPair throughputAtOptimalConcurrency,
                                double jitterDev = 0.1);
    virtual ~MockWorkloadCharacteristics();

    /**
     * Returns the read throughput observed at the specified 'concurrency' level.
     */
    int32_t readThroughput(RWPair concurrency) const;

    /**
     * Returns the write throughput observed at the specified 'concurrency' level.
     */
    int32_t writeThroughput(RWPair concurrency) const;

    /**
     * Returns the latency of a read operation at the specified 'concurrency' level (including
     * jitter).
     */
    Nanoseconds readLatency(RWPair concurrency) const;

    /**
     * Returns the latency of a write operation at the specified 'concurrency' level (including
     * jitter).
     */
    Nanoseconds writeLatency(RWPair concurrency) const;

    /**
     * The optimal concurrency level for this workload.
     */
    RWPair optimal() const;

    /**
     * Resets the optimal concurrency and throughput values to the input.
     */
    void reset(RWPair newOptimalConcurrency, RWPair newOptimalThroughput);

protected:
    virtual RWPair _throughput(RWPair concurrency) const = 0;

    double _jitter() const;
    Nanoseconds _latency(int32_t concurrency, int32_t throughput) const;

protected:
    AtomicWord<RWPair> _optimalConcurrency;
    AtomicWord<RWPair> _throughputAtOptimalConcurrency;
    double _jitterDev = 0.0;
    mutable stdx::mutex _mutex;
    mutable std::mt19937 _rng;
    mutable std::normal_distribution<double> _jitterDist;
};

/**
 * This workload uses parabolic modeling, which aligns reasonably well with some of the workload
 * characteristics we've observed in our non-simulated workload testing. The model is defined
 * piecewise, and degrades to a flat function past 5x the optimal concurrency.
 */
class ParabolicWorkloadCharacteristics : public MockWorkloadCharacteristics {
public:
    ParabolicWorkloadCharacteristics(RWPair optimalConcurrency,
                                     RWPair throughputAtOptimalConcurrency,
                                     double jitterDev = 0.1);

protected:
    RWPair _throughput(RWPair concurrency) const override;
};

}  // namespace mongo::workload_simulation
