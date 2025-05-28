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

#include "mongo/db/commands/server_status_metric.h"
#include "mongo/util/moving_average.h"

namespace mongo {

/**
 * This header defines `MovingAverageMetric`, which can be used with
 * `MetricBuilder` to define exponential moving average server status metrics.
 *
 * For example:
 *
 *     #include "mongo/db/commands/server_status_metric.h"
 *     #include "mongo/util/moving_average_metric.h"
 *
 *     auto& someAverageMetricThing =
 *         *MetricBuilder<MovingAverageMetric>("some.averageMetricThing").bind(0.2);
 *
 * Then `someAverageMetricThing` is a `MovingAverageMetric` that you can call
 * `.addSample(double)` on. It has a smoothing factor of `0.2`. The smoothing
 * factor must be specified with `bind`, as above.
 *
 * Once `someAverageMetricThing.addSample` has been called at least once, the
 * metric "averageMetricThing" will appear in the "some" subsection of the
 * server status command response's "metrics" section, as might be seen in
 * `mongosh`:
 *
 *     test> db.runCommand({serverStatus: 1}).metrics.some
 *     {
 *       averageMetricThing: 2.34
 *     }
 *     test>
 */

class MovingAverageMetric {
public:
    explicit MovingAverageMetric(double alpha) : _v(alpha) {}

    double addSample(double sample) {
        return _v.addSample(sample);
    }

    MovingAverage& avg() {
        return _v;
    }
    const MovingAverage& avg() const {
        return _v;
    }

private:
    MovingAverage _v;
};

/**
 * `MovingAverageMetric` is an atomic `double`. If no samples have ever
 * contributed to the average, then it is omitted from metrics.
 */
struct MovingAverageMetricPolicy {
public:
    explicit MovingAverageMetricPolicy(double alpha) : _v(alpha) {}

    MovingAverageMetric& value() {
        return _v;
    }

    void appendTo(BSONObjBuilder& b, StringData leafName) const {
        if (const boost::optional<double> snapshot = _v.avg().get()) {
            b.append(leafName, *snapshot);
        }
    }

private:
    MovingAverageMetric _v;
};

template <>
struct ServerStatusMetricPolicySelection<MovingAverageMetric> {
    using type = MovingAverageMetricPolicy;
};

}  // namespace mongo
