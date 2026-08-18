// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/internode_validation_metrics.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/server_status_options.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;
using otel::metrics::ServerStatusOptions;

// Incremented whenever a non-primary recomputes a document hash that disagrees with the one the
// primary recorded on the oplog entry. These count every detected divergence, independent of
// whether 'continuousInternodeValidationFatalOnMismatch' makes the node fassert.
auto& insertHashMismatchCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kInternodeConsistencyHashMismatchInsert,
    "Number of internode document-hash mismatches detected while applying insert oplog entries",
    MetricUnit::kEvents,
    {.serverStatusOptions =
         ServerStatusOptions{.dottedPath = "repl.internodeConsistency.hashMismatch.insert"}});

auto& updateHashMismatchCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kInternodeConsistencyHashMismatchUpdate,
    "Number of internode document-hash mismatches detected while applying update oplog entries",
    MetricUnit::kEvents,
    {.serverStatusOptions =
         ServerStatusOptions{.dottedPath = "repl.internodeConsistency.hashMismatch.update"}});

auto& deleteHashMismatchCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kInternodeConsistencyHashMismatchDelete,
    "Number of internode document-hash mismatches detected while applying delete oplog entries",
    MetricUnit::kEvents,
    {.serverStatusOptions =
         ServerStatusOptions{.dottedPath = "repl.internodeConsistency.hashMismatch.delete"}});

}  // namespace

void incrementDocumentHashMismatchCount(OpTypeEnum opType) {
    switch (opType) {
        case OpTypeEnum::kInsert:
            insertHashMismatchCounter.add(1);
            return;
        case OpTypeEnum::kUpdate:
            updateHashMismatchCounter.add(1);
            return;
        case OpTypeEnum::kDelete:
            deleteHashMismatchCounter.add(1);
            return;
        default:
            // Only replicated-recordId CRUD oplog entries carry a per-document validation hash, so
            // no other op type can reach a mismatch.
            MONGO_UNREACHABLE_TASSERT(12882000);
    }
}

}  // namespace repl
}  // namespace mongo
