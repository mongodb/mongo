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

#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"

#include <cmath>

#include <boost/optional.hpp>

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
        double expected = _average.load();
        double desired;
        do {
            if (std::isnan(expected)) {
                desired = sample;  // the very first sample
            } else {
                desired = _alpha * sample + (1 - _alpha) * expected;
            }
        } while (!_average.compareAndSwap(&expected, desired));
        return desired;
    }

    /**
     * Returns the current moving average. If addSample has never been called on this object,
     * then returns `boost::none`.
     */
    boost::optional<double> get() const {
        const double raw = _average.load();
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

private:
    AtomicWord<double> _average;
    const double _alpha;
};

}  // namespace mongo
