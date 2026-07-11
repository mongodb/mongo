// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/initial_sync/initial_syncer_common_stats.h"

#include "mongo/logv2/log.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/server_status_options.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplicationInitialSync


namespace mongo {
namespace repl {
namespace initial_sync_common_stats {

using otel::metrics::Counter;
using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;
using otel::metrics::ServerStatusOptions;

Counter<int64_t>& initialSyncFailedAttempts = MetricsService::instance().createInt64Counter(
    MetricNames::kInitialSyncFailedAttempts,
    "Number of initial sync attempts that have failed sync server startup. An initial syncer "
    "may run multiple attempts to fulfill one request.",
    MetricUnit::kEvents,
    {.serverStatusOptions =
         ServerStatusOptions({.dottedPath = "repl.initialSync.failedAttempts"})});

Counter<int64_t>& initialSyncFailures = MetricsService::instance().createInt64Counter(
    MetricNames::kInitialSyncFailures,
    "Number of initial sync requests that have been requested and failed. This does not "
    "include "
    "times where an initial syncer is created successfully but failed in startup.",
    MetricUnit::kEvents,
    {.serverStatusOptions = ServerStatusOptions({.dottedPath = "repl.initialSync.failures"})});

Counter<int64_t>& initialSyncCompletes = MetricsService::instance().createInt64Counter(
    MetricNames::kInitialSyncCompleted,
    "Number of initial sync requests that have been completed successfully.",
    MetricUnit::kEvents,
    {.serverStatusOptions = ServerStatusOptions({.dottedPath = "repl.initialSync.completed"})});

void LogInitialSyncAttemptStats(const StatusWith<OpTimeAndWallTime>& attemptResult,
                                bool hasRetries,
                                const BSONObj& stats) {
    // Don't remove or change this log id as it is ingested to Atlas.
    LOGV2(21192,
          "Initial sync status and statistics",
          "status"_attr =
              attemptResult.isOK() ? "successful" : (hasRetries ? "in_progress" : "failed"),
          "statistics"_attr = redact(stats));
}

}  // namespace initial_sync_common_stats
}  // namespace repl
}  // namespace mongo
