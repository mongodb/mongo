// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/otel_metric_server_status_adapter.h"

#include "mongo/base/error_codes.h"
#include "mongo/otel/metrics/metrics_metric.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo::otel::metrics {

OtelMetricServerStatusAdapter::OtelMetricServerStatusAdapter(Metric* metric) : _metric(metric) {
    invariant(_metric);
}

void OtelMetricServerStatusAdapter::appendTo(BSONObjBuilder& b, std::string_view leafName) const {
    if (!isEnabled()) {
        return;
    }
    const std::string key(leafName);
    const BSONObj obj = _metric->serializeToBson(key);
    const BSONElement el = obj.getField(key);
    massert(ErrorCodes::KeyNotFound,
            fmt::format("Provided key {} not found in serialized BSONObj", key),
            !el.eoo());
    b.append(el);
}

}  // namespace mongo::otel::metrics
