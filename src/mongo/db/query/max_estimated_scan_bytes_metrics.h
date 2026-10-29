// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/commands/server_status/server_status_metric.h"

namespace mongo::maxEstimatedScanBytesMetrics {

// Incremented when a query is rejected due to maxEstimatedScanBytes.
extern Counter64& maxEstimatedScanRejected;

// Incremented when maxEstimatedScanBytes would have rejected a query but a $natural hint
// (command-level or PQS) overrode the restriction.
extern Counter64& maxEstimatedScanRejectedAndOverridden;

// Incremented when maxEstimatedScanBytesDryRun is enabled and a query would have been rejected by
// maxEstimatedScanBytes, but was allowed to proceed because dry-run mode is active.
extern Counter64& maxEstimatedScanDryRunWouldReject;

}  // namespace mongo::maxEstimatedScanBytesMetrics
