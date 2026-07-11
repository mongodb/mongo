// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands/server_status/server_status_metric.h"

#include <string_view>

namespace mongo::otel::metrics {

class Metric;

/**
 * Adapts Metric to ServerStatusMetric for registration on the MetricTree. The Metric pointer must
 * remain valid for the lifetime of this adapter.
 */
class OtelMetricServerStatusAdapter final : public ServerStatusMetric {
public:
    explicit OtelMetricServerStatusAdapter(Metric* metric);

    /**
     * If this metric is enabled, appends it as a BSON field named `leafName` to `b`.
     */
    void appendTo(BSONObjBuilder& b, std::string_view leafName) const override;

private:
    Metric* _metric;
};

}  // namespace mongo::otel::metrics
