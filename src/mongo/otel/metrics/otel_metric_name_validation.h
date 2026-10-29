// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <string_view>

[[MONGO_MOD_PUBLIC]];

namespace mongo::otel::metrics {

/**
 * Validates `name` for use as an OpenTelemetry metric instrument name (e.g. MetricName registry
 * strings). A valid name has size less than kMaxOtelMetricNameLength and is made up of one or more
 * dot-separated segments. Each segment is nonempty and may be either snake_case or camelCase.
 * Numbers are allowed but not to initiate a segment. E.g.
 *
 * Valid: "network.open_connections.count", "ingress.ingressTLSLatency", "foo",
 *   "serverStatus.my_metric.latency3"
 * Invalid: "", ".", "network..open_connections", "network.OpenConnections",
 *   "network.Open_connections", "network.2connections"
 */
Status validateOtelMetricName(std::string_view name);

inline constexpr size_t kMaxOtelMetricNameLength = 255;

}  // namespace mongo::otel::metrics
