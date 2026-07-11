// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/repl_network_traffic_stats.h"

#include "mongo/base/error_codes.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/server_status_options.h"
#include "mongo/util/assert_util.h"

#include <cstdint>

namespace mongo::repl {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;
using otel::metrics::ServerStatusOptions;

auto& oplogBytesReceivedMetric = MetricsService::instance().createInt64Counter(
    MetricNames::kReplNetworkBytes,
    "Total number of oplog bytes received over the network from a sync source.",
    MetricUnit::kBytes,
    {.serverStatusOptions = ServerStatusOptions({.dottedPath = "repl.network.bytes"})});

auto& oplogBytesSentMetric = MetricsService::instance().createInt64Counter(
    MetricNames::kReplNetworkBytesSent,
    "Total number of oplog bytes sent over the network to syncing nodes.",
    MetricUnit::kBytes,
    {.serverStatusOptions = ServerStatusOptions({.dottedPath = "repl.network.bytesSent"})});

}  // namespace

void recordOplogBytesReceived(int64_t bytes) {
    iassert(ErrorCodes::BadValue, "oplog bytes received must be non-negative", bytes >= 0);
    oplogBytesReceivedMetric.add(bytes);
}

void recordOplogBytesSent(int64_t bytes) {
    iassert(ErrorCodes::BadValue, "oplog bytes sent must be non-negative", bytes >= 0);
    oplogBytesSentMetric.add(bytes);
}

}  // namespace mongo::repl
