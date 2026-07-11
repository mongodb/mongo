// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <mutex>
#include <random>

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
    Atomic<RWPair> _optimalConcurrency;
    Atomic<RWPair> _throughputAtOptimalConcurrency;
    double _jitterDev = 0.0;
    mutable std::mutex _mutex;
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
