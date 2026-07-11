// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>

[[MONGO_MOD_PUBLIC]];

namespace mongo::otel::metrics {

// Used to denote the unit of measurement in a metric.
enum class MetricUnit {
    // Generic
    kEvents,
    kCount,
    kId,
    kState,
    kBoolean,
    kRatio,

    // Time
    kMicroseconds,
    kMilliseconds,
    kSeconds,

    // Space
    kBytes,

    // Database
    kOperations,
    kQueries,
    kCursors,

    // Networking
    kConnections,
};

// Converts any of the above units to a string.
std::string_view toString(MetricUnit unit);

}  // namespace mongo::otel::metrics
