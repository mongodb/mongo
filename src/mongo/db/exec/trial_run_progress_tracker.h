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
#include <type_traits>

namespace mongo {
/**
 * During the runtime planning phase this tracker is used to track the progress of the work done
 * so far. A plan stage in a candidate execution tree may supply the number of documents it has
 * processed, or the number of physical reads performed, and the tracker will use it to check if
 * the execution plan has progressed enough.
 */
class TrialRunProgressTracker final {
public:
    /**
     * The type of metric which can be collected and tracked during a trial run.
     */
    enum TrialRunMetric : uint8_t {
        // Number of documents returned during a trial run.
        kNumResults,
        // Number of physical reads performed during a trial run. Once a storage cursor advances,
        // it counts as a single physical read.
        kNumReads,
        // Must always be the last element to hold the number of element in the enum.
        kLastElem
    };

    template <typename... MaxMetrics,
              std::enable_if_t<sizeof...(MaxMetrics) == TrialRunMetric::kLastElem, int> = 0>
    TrialRunProgressTracker(MaxMetrics... maxMetrics) : _maxMetrics{maxMetrics...} {}

    /**
     * Increments the trial run metric specified as a template parameter 'metric' by the
     * 'metricIncrement' value and returns 'true' if the updated metric value has reached
     * its maximum.
     *
     * If the metric has already reached its maximum value before this call, this method
     * returns 'true' immediately without incrementing the metric.
     */
    template <TrialRunMetric metric>
    bool trackProgress(size_t metricIncrement) {
        static_assert(metric >= 0 && metric < sizeof(_metrics));

        if (_done) {
            return true;
        }

        _metrics[metric] += metricIncrement;
        if (_metrics[metric] >= _maxMetrics[metric]) {
            _done = true;
        }
        return _done;
    }

private:
    const size_t _maxMetrics[TrialRunMetric::kLastElem];
    size_t _metrics[TrialRunMetric::kLastElem]{0};
    bool _done{false};
};
}  // namespace mongo
