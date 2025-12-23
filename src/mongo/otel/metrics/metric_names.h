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
#include "mongo/base/string_data.h"
#include "mongo/util/modules.h"

MONGO_MOD_PUBLIC;

namespace mongo::otel::metrics {

/**
 * Wrapper class around a string to ensure `MetricName`s are only constructed in the class
 * definition of `MetricNames`.
 */
class MetricName {
public:
    constexpr StringData getName() const {
        return _name;
    };

    bool operator==(const MetricName& other) const {
        return getName() == other.getName();
    }

private:
    friend class MetricNames;
    constexpr MetricName(StringData name) : _name(name) {};
    StringData _name;
};

/**
 * Central registry of OpenTelemetry metric names used in the server. When adding a new metric to
 * the server, please add an entry to MetricNames grouped under your team name.
 *
 * This ensures that the N&O team has full ownership over new OTel metrics in the server for
 * centralized collaboration with downstream OTel consumers. OTel metrics are stored in time-series
 * DBs by the SRE team, and a sudden increase in metrics will result in operational costs ballooning
 * for the SRE team, which is why N&O owns this registry.
 */
class MetricNames {
public:
    // Networking & Observability Team Metrics
    static constexpr MetricName kConnectionsProcessed = {"connections_processed"};
    // Test-only
    static constexpr MetricName kTest1 = {"test_only.metric1"};
    static constexpr MetricName kTest2 = {"test_only.metric2"};
};

}  // namespace mongo::otel::metrics
