// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cmath>

#include <boost/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * `MovingAverage` is an atomically updated [exponential moving average][1].
 *
 * A `MovingAverage` initially has no value. The `addSample(double)` member
 * function atomically contributes a value to the moving average. The first
 * call to `addSample` sets the initial value. Subsequent calls to `addSample`
 * combine the argument with the existing moving average. `addSample` returns
 * the new moving average value.
 *
 * The `get()` member function returns a snapshot of the value of the moving
 * average, or `boost::none` if `addSample` has never been called.
 *
 * `MovingAverage` has one required constructor parameter, the smoothing factor
 * `double alpha`, which determines the relative weight of new data versus the
 * previous average. `alpha` must satisfy `alpha > 0 && alpha < 1`. The member
 * function `alpha()` returns `alpha`.
 *
 * A typical choice for the smoothing factor is `0.2`. `0.2` is chosen per the
 * precedent set by the round-trip-time average calculation in [server
 * selection][2]. That specification notes:
 *
 * > A weighting factor of 0.2 was chosen to put about 85% of the weight of the
 * > average RTT on the 9 most recent observations.
 *
 * For use of `MovingAverage` as a server status metric, see
 * `moving_average_metric.h`.
 *
 * [1]: https://en.wikipedia.org/wiki/Exponential_smoothing
 * [2]:
 * https://github.com/mongodb/specifications/blob/master/source/server-selection/server-selection.md
 */

class MovingAverage {
public:
    /**
     * Creates an exponential moving average with smoothing factor alpha. The moving average
     * initially has no value. The behavior is undefined unless `alpha > 0 && alpha < 1`.
     */
    explicit MovingAverage(double alpha) : _average(std::nan("")), _alpha(alpha) {
        invariant(_alpha > 0);
        invariant(_alpha < 1);
    }

    /** Contributes the specified sample into the moving average. Returns the updated average. */
    double addSample(double sample) {
        double expected = _average.loadRelaxed();
        double desired;
        do {
            if (std::isnan(expected)) {
                desired = sample;  // the very first sample
            } else {
                desired = _alpha * sample + (1 - _alpha) * expected;
            }
        } while (!_average.compareAndSwapWeak(&expected, desired));
        return desired;
    }

    /**
     * Returns the current moving average. If addSample has never been called on this object,
     * then returns `boost::none`.
     */
    boost::optional<double> get() const {
        const double raw = _average.loadRelaxed();
        if (std::isnan(raw)) {
            return boost::none;
        }
        return raw;
    }

    /**
     * Returns the smoothing factor of this moving average. It is the same value as specified in
     * the constructor.
     */
    double alpha() const {
        return _alpha;
    }

    /**
     * Resets the moving average to no value.
     */
    void reset() {
        _average.storeRelaxed(std::nan(""));
    }

private:
    Atomic<double> _average;
    const double _alpha;
};

}  // namespace mongo
