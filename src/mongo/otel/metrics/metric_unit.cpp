// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/metric_unit.h"

#include "mongo/logv2/log.h"

#include <string_view>

namespace mongo::otel::metrics {

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

std::string_view toString(MetricUnit unit) {
    switch (unit) {
        // Generic
        case MetricUnit::kEvents:
            return "events";
        case MetricUnit::kCount:
            return "count";
        case MetricUnit::kId:
            return "id";
        case MetricUnit::kState:
            return "state";
        case MetricUnit::kBoolean:
            return "bool";
        case MetricUnit::kRatio:
            return "ratio";

        // Time
        case MetricUnit::kMicroseconds:
            return "microseconds";
        case MetricUnit::kMilliseconds:
            return "milliseconds";
        case MetricUnit::kSeconds:
            return "seconds";

        // Space
        case MetricUnit::kBytes:
            return "bytes";

        // Database
        case MetricUnit::kOperations:
            return "operations";
        case MetricUnit::kQueries:
            return "queries";
        case MetricUnit::kCursors:
            return "cursors";

        // Networking
        case MetricUnit::kConnections:
            return "connections";
    }
    LOGV2_FATAL(11494600, "Unknown MetricUnit value", "value"_attr = static_cast<int>(unit));
}
}  // namespace mongo::otel::metrics
