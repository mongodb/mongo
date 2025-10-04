/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>

namespace mongo {
/**
 * During the runtime planning phase this tracker is used to track the progress of the work done
 * so far. A plan stage in a candidate execution tree may supply the number of documents it has
 * processed, or the number of physical reads performed, and the tracker will use it to check if
 * the execution plan has progressed enough.
 */
class TrialRunTracker final {
public:
    /**
     * The type of metric which can be collected and tracked during a trial run.
     */
    enum TrialRunMetric : uint8_t {
        // Number of documents returned by the part of the plan that participated in the
        // multi-planning.
        kNumResults,
        // Number of physical reads performed during a trial run. Once a storage cursor advances,
        // it counts as a single physical read.
        kNumReads,
        // Must always be the last element to hold the number of elements in the enum.
        kLastElem
    };

    static constexpr size_t numTrialRunMetrics = static_cast<size_t>(TrialRunMetric::kLastElem);

    /**
     * Constructs a 'TrialRunTracker' which indicates that the trial period is over when any
     * 'TrialRunMetric' exceeds the maximum provided at construction.
     *
     * Callers can also pass a value of boost::none to indicate that the given metric should not
     * be tracked.
     */
    TrialRunTracker(boost::optional<size_t> maxNumResults, boost::optional<size_t> maxNumReads)
        : _maxMetrics{maxNumResults, maxNumReads} {}

    /**
     * Constructs a 'TrialRunTracker' that also has an '_onMetricReached' function, which gets
     * called when any 'TrialRunMetric' exceeds its maximum. When an '_onMetricReached' callback is
     * present, it must return true for 'trackProgress' to return true. By returning false,
     * '_onMetricReached' can prevent tracking from halting plan execution, thereby upgrading a
     * trial run to a normal run.
     */
    TrialRunTracker(std::function<bool(TrialRunMetric)> onMetricReached,
                    boost::optional<size_t> maxNumResults,
                    boost::optional<size_t> maxNumReads)
        : _maxMetrics{maxNumResults, maxNumReads}, _onMetricReached(std::move(onMetricReached)) {}

    /**
     * Increments the trial run metric specified as a template parameter 'metric' by the
     * 'metricIncrement' value and, if the updated metric value has exceeded its maximum, calls the
     * '_onMetricReached' if there is one and returns true (unless '_onMetricReached' returned
     * false).
     *
     * This is a no-op, and will return false, if the given metric is not being tracked by this
     * 'TrialRunTracker'.
     *
     * If the metric has already exceeded its maximum value before this call, this method
     * returns 'true' immediately without incrementing the metric.
     */
    template <TrialRunMetric metric>
    bool trackProgress(size_t metricIncrement) {
        static_assert(metric >= 0 && metric < sizeof(_metrics) / sizeof(size_t));

        if (!_maxMetrics[metric].has_value()) {
            // This metric is not being tracked.
            return false;
        }

        if (_done) {
            return true;
        }

        _metrics[metric] += metricIncrement;
        if (metricReached<metric>()) {
            if (_onMetricReached) {
                _done = _onMetricReached(metric);
            } else {
                _done = true;
            }
        }
        return _done;
    }

    template <TrialRunMetric metric>
    size_t getMetric() const {
        static_assert(metric >= 0 && metric < sizeof(_metrics) / sizeof(size_t));
        return _metrics[metric];
    }

    template <TrialRunMetric metric>
    bool metricReached() const {
        static_assert(metric >= 0 && metric < sizeof(_metrics) / sizeof(size_t));
        return _maxMetrics[metric].has_value() && _metrics[metric] > *_maxMetrics[metric];
    }

    template <TrialRunMetric metric>
    bool metricTracked() const {
        static_assert(metric >= 0 && metric < sizeof(_metrics) / sizeof(size_t));
        return _maxMetrics[metric].has_value();
    }

    template <TrialRunMetric metric>
    void updateMaxMetric(boost::optional<size_t> newMaxMetric) {
        static_assert(metric >= 0 && metric < sizeof(_metrics) / sizeof(size_t));
        _maxMetrics[metric] = newMaxMetric;
    }

private:
    boost::optional<size_t> _maxMetrics[numTrialRunMetrics];
    size_t _metrics[numTrialRunMetrics]{0};
    bool _done{false};
    std::function<bool(TrialRunMetric)> _onMetricReached{};
};
}  // namespace mongo
