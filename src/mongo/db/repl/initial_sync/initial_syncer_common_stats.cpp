// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/initial_sync/initial_syncer_common_stats.h"

#include "mongo/logv2/log.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_attributes.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/server_status_options.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplicationInitialSync


namespace mongo {
namespace repl {
namespace initial_sync_common_stats {

using otel::metrics::AttributeDefinition;
using otel::metrics::Counter;
using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;
using otel::metrics::ServerStatusOptions;


static const std::vector initialSyncKindsVector{
    initialSyncKindToStringView(InitialSyncKind::kLogical),
    initialSyncKindToStringView(InitialSyncKind::kFCBIS)};

// The number of initial sync attempts that have failed since server startup. Each instance of
// InitialSyncer may run multiple attempts to fulfill an initial sync request that is triggered
// when InitialSyncer::startup() is called.
Counter<int64_t, std::string_view>& initialSyncFailedAttempts =
    MetricsService::instance().createInt64Counter<std::string_view>(
        MetricNames::kInitialSyncFailedAttempts,
        "Number of initial sync attempts that have failed sync server startup. An initial syncer "
        "may run multiple attempts to fulfill one request.",
        MetricUnit::kEvents,
        AttributeDefinition<std::string_view>{.name = "kind", .values = initialSyncKindsVector},
        {.serverStatusOptions =
             ServerStatusOptions({.dottedPath = "repl.initialSync.failedAttempts"})});

// The number of initial sync requests that have been requested and failed. Each instance of
// InitialSyncer (upon successful startup()) corresponds to a single initial sync request.
// This value does not include the number of times where a InitialSyncer is created successfully
// but failed in startup().
Counter<int64_t, std::string_view>& initialSyncFailures =
    MetricsService::instance().createInt64Counter<std::string_view>(
        MetricNames::kInitialSyncFailures,
        "Number of initial sync requests that have been requested and failed. This does not "
        "include "
        "times where an initial syncer is created successfully but failed in startup.",
        MetricUnit::kEvents,
        AttributeDefinition<std::string_view>{.name = "kind", .values = initialSyncKindsVector},
        {.serverStatusOptions = ServerStatusOptions({.dottedPath = "repl.initialSync.failures"})});

// The number of initial sync requests that have been requested and completed successfully. Each
// instance of InitialSyncer corresponds to a single initial sync request.
Counter<int64_t, std::string_view>& initialSyncCompletes =
    MetricsService::instance().createInt64Counter<std::string_view>(
        MetricNames::kInitialSyncCompleted,
        "Number of initial sync requests that have been completed successfully.",
        MetricUnit::kEvents,
        AttributeDefinition<std::string_view>{.name = "kind", .values = initialSyncKindsVector},
        {.serverStatusOptions = ServerStatusOptions({.dottedPath = "repl.initialSync.completed"})});

size_t initialSyncFailedAttemptCount = 0;
size_t initialSyncFailureCount = 0;
size_t initialSyncCompleteCount = 0;

void incrementInitialSyncFailedAttemptMetric(InitialSyncKind kind) {
    initialSyncFailedAttempts.add(1, initialSyncKindToStringView(kind));
    ++initialSyncFailedAttemptCount;
}

void incrementInitialSyncFailureMetric(InitialSyncKind kind) {
    initialSyncFailures.add(1, initialSyncKindToStringView(kind));
    ++initialSyncFailureCount;
}

void incrementInitialSyncCompleteMetric(InitialSyncKind kind) {
    initialSyncCompletes.add(1, initialSyncKindToStringView(kind));
    ++initialSyncCompleteCount;
}

[[nodiscard]] size_t getInitialSyncFailedAttemptCount() {
    return initialSyncFailedAttemptCount;
}

[[nodiscard]] size_t getInitialSyncFailureCount() {
    return initialSyncFailureCount;
}

[[nodiscard]] size_t getInitialSyncCompleteCount() {
    return initialSyncCompleteCount;
}

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
