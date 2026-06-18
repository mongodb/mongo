/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <string_view>

MONGO_MOD_PUBLIC;

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
