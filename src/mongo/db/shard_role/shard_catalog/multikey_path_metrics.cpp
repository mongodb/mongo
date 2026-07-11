// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/multikey_path_metrics.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/util/assert_util.h"

namespace mongo::catalog_metrics {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;

auto& ordinaryInTransactionCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kIndexStatsMultikeyNewPathsOrdinaryInTransaction,
    "New ordinary (non-wildcard) multikey path components recorded inside multi-document "
    "transactions.",
    MetricUnit::kCount);

auto& ordinaryOutsideTransactionCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kIndexStatsMultikeyNewPathsOrdinaryOutsideTransaction,
    "New ordinary (non-wildcard) multikey path components recorded outside multi-document "
    "transactions.",
    MetricUnit::kCount);

auto& wildcardInTransactionCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kIndexStatsMultikeyNewPathsWildcardInTransaction,
    "New wildcard multikey metadata paths recorded inside multi-document transactions.",
    MetricUnit::kCount);

auto& wildcardOutsideTransactionCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kIndexStatsMultikeyNewPathsWildcardOutsideTransaction,
    "New wildcard multikey metadata paths recorded outside multi-document transactions.",
    MetricUnit::kCount);

auto& sideTransactionsCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kIndexStatsMultikeySideTransactions,
    "Multikey metadata side transactions committed inside multi-document transactions.",
    MetricUnit::kEvents);

}  // namespace

void appendMultikeyPathStatsToIndexStats(BSONObjBuilder* indexStatsBuilder) {
    BSONObjBuilder multikeyBuilder(indexStatsBuilder->subobjStart("multikey"));
    {
        BSONObjBuilder newPathsBuilder(multikeyBuilder.subobjStart("newPaths"));
        BSONObjBuilder ordinaryBuilder(newPathsBuilder.subobjStart("ordinary"));
        ordinaryBuilder.append("inTransaction", ordinaryInTransactionCounter.valueForLegacyUse());
        ordinaryBuilder.append("outsideTransaction",
                               ordinaryOutsideTransactionCounter.valueForLegacyUse());
        ordinaryBuilder.done();
        BSONObjBuilder wildcardBuilder(newPathsBuilder.subobjStart("wildcard"));
        wildcardBuilder.append("inTransaction", wildcardInTransactionCounter.valueForLegacyUse());
        wildcardBuilder.append("outsideTransaction",
                               wildcardOutsideTransactionCounter.valueForLegacyUse());
        wildcardBuilder.done();
    }
    multikeyBuilder.append("sideTransactions", sideTransactionsCounter.valueForLegacyUse());
}

void recordOrdinaryMultikeyPathChanges(OperationContext* opCtx, int64_t count) {
    invariant(count >= 0);
    (opCtx->inMultiDocumentTransaction() ? ordinaryInTransactionCounter
                                         : ordinaryOutsideTransactionCounter)
        .add(count);
}

void recordWildcardMultikeyPathChanges(OperationContext* opCtx, int64_t count) {
    invariant(count >= 0);
    (opCtx->inMultiDocumentTransaction() ? wildcardInTransactionCounter
                                         : wildcardOutsideTransactionCounter)
        .add(count);
}

void recordSideTransaction() {
    sideTransactionsCounter.add(1);
}

}  // namespace mongo::catalog_metrics
