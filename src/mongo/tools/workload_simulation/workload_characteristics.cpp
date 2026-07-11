// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/tools/workload_simulation/workload_characteristics.h"

#include <cmath>

namespace mongo::workload_simulation {
namespace {
int32_t independentThroughput(int32_t throughputAtOptimalConcurrency,
                              int32_t optimalConcurrency,
                              int32_t concurrency) {
    if (concurrency == optimalConcurrency) {
        return throughputAtOptimalConcurrency;
    }

    // Modeling throughput as piecewise parabolic functions in the regions of interest, flat
    // past 5x optimal. The 5x threshold is somewhat arbitrary, but is chosen so that the
    // degradation in performance slightly past the optimal is significant, but we do not
    // converge on the minimum immediately.

    if (concurrency < optimalConcurrency) {
        // Fit parabola to (0, 0), (optimalConcurrency, throughputAtOptimalConcurrency),
        // (2 * optimalConcurrency, 0).
        const double a = static_cast<double>(throughputAtOptimalConcurrency) * (-1.0) /
            (optimalConcurrency * optimalConcurrency);
        const double b = a * optimalConcurrency * -2.0;
        return (a * concurrency * concurrency) + (b * concurrency);
    } else if (concurrency < 5 * optimalConcurrency) {
        // Fit parabola to (optimalConcurrency, throughputAtOptimalConcurrency),
        // (5 * optimalConcurrency, 0.1 * throughputAtOptimalConcurrency),
        // (9 * optimalConcurrency, throughputAtOptimalConcurrency).
        const double a = (static_cast<double>(throughputAtOptimalConcurrency) * 9.0) /
            (160.0 * optimalConcurrency * optimalConcurrency);
        const double b = ((static_cast<double>(throughputAtOptimalConcurrency) * -9.0) /
                          (40.0 * optimalConcurrency)) -
            (6.0 * a * optimalConcurrency);
        const double c = static_cast<double>(throughputAtOptimalConcurrency) -
            (a * optimalConcurrency * optimalConcurrency) - (b * optimalConcurrency);
        return static_cast<int32_t>((a * concurrency * concurrency) + (b * concurrency) + c);
    }

    return throughputAtOptimalConcurrency / 10;
}
}  // namespace

MockWorkloadCharacteristics::MockWorkloadCharacteristics(RWPair optimalConcurrency,
                                                         RWPair throughputAtOptimalConcurrency,
                                                         double jitterDev)
    : _optimalConcurrency(optimalConcurrency),
      _throughputAtOptimalConcurrency(throughputAtOptimalConcurrency),
      _jitterDev{jitterDev},
      // If the deviation is 0, we will always return 0, so we don't care about the distribution.
      // However, it's not valid to pass 0 as the sigma for a normal distribution, so we'll need to
      // override it.
      _jitterDist{0.0, _jitterDev == 0.0 ? 1.0 : _jitterDev} {
    invariant(_jitterDev >= 0.0);
    invariant(_jitterDev < 1.0);
}

MockWorkloadCharacteristics::~MockWorkloadCharacteristics() {}

int32_t MockWorkloadCharacteristics::readThroughput(RWPair concurrency) const {
    return _throughput(concurrency).read;
}
int32_t MockWorkloadCharacteristics::writeThroughput(RWPair concurrency) const {
    return _throughput(concurrency).write;
}

Nanoseconds MockWorkloadCharacteristics::readLatency(RWPair concurrency) const {
    return _latency(concurrency.read, readThroughput(concurrency));
}
Nanoseconds MockWorkloadCharacteristics::writeLatency(RWPair concurrency) const {
    return _latency(concurrency.write, writeThroughput(concurrency));
}

RWPair MockWorkloadCharacteristics::optimal() const {
    return _optimalConcurrency.load();
}

void MockWorkloadCharacteristics::reset(RWPair newOptimalConcurrency, RWPair newOptimalThroughput) {
    _optimalConcurrency.store(newOptimalConcurrency);
    _throughputAtOptimalConcurrency.store(newOptimalThroughput);
}

double MockWorkloadCharacteristics::_jitter() const {
    if (_jitterDev == 0.0) {
        return 0.0;
    }
    std::lock_guard lk{_mutex};
    return _jitterDist(_rng);
}

Nanoseconds MockWorkloadCharacteristics::_latency(int32_t concurrency, int32_t throughput) const {
    invariant(throughput != 0);
    invariant(throughput < 1'000'000);
    return Nanoseconds{static_cast<int64_t>(static_cast<double>(1'000'000'000) * concurrency *
                                            std::clamp(1.0 + _jitter(), 0.0, kMaxJitterMultiplier) /
                                            throughput)};
}

ParabolicWorkloadCharacteristics::ParabolicWorkloadCharacteristics(
    RWPair optimalConcurrency, RWPair throughputAtOptimalConcurrency, double jitterDev)
    : MockWorkloadCharacteristics(optimalConcurrency, throughputAtOptimalConcurrency, jitterDev) {}

RWPair ParabolicWorkloadCharacteristics::_throughput(RWPair concurrency) const {
    RWPair throughputAtOptimalConcurrency = _throughputAtOptimalConcurrency.load();
    RWPair optimalConcurrency = _optimalConcurrency.load();
    RWPair independentThroughputs = {
        independentThroughput(
            throughputAtOptimalConcurrency.read, optimalConcurrency.read, concurrency.read),
        independentThroughput(
            throughputAtOptimalConcurrency.write, optimalConcurrency.write, concurrency.write)};

    double optimalRatio =
        static_cast<double>(optimalConcurrency.read) / std::max(1, optimalConcurrency.write);
    double ratio = static_cast<double>(concurrency.read) / std::max(1, concurrency.write);

    RWPair scaledThroughputs = independentThroughputs;
    if (ratio > optimalRatio) {
        // Too many reads, penalize writes
        scaledThroughputs.write *= std::sqrt(optimalRatio / ratio);
    } else {
        // Too many writes, penalize reads
        scaledThroughputs.read *= std::sqrt(ratio / optimalRatio);
    }

    return scaledThroughputs;
}

}  // namespace mongo::workload_simulation
