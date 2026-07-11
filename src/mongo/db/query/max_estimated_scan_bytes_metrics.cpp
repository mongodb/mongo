// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/max_estimated_scan_bytes_metrics.h"

namespace mongo::maxEstimatedScanBytesMetrics {

Counter64& maxEstimatedScanRejected =
    *MetricBuilder<Counter64>{"query.maxEstimatedScanBytes.rejected"};

Counter64& maxEstimatedScanRejectedAndOverridden =
    *MetricBuilder<Counter64>{"query.maxEstimatedScanBytes.rejectedAndOverridden"};

}  // namespace mongo::maxEstimatedScanBytesMetrics
