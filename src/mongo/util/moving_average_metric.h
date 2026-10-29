// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/util/modules.h"
#include "mongo/util/moving_average.h"

#include <string_view>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * This header defines `MovingAverageMetric`, which can be used with
 * `MetricBuilder` to define exponential moving average server status metrics.
 *
 * For example:
 *
 *     #include "mongo/db/commands/server_status/server_status_metric.h"
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

    void appendTo(BSONObjBuilder& b, std::string_view leafName) const {
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
