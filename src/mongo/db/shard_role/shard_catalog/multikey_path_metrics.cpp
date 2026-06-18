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
    }
    multikeyBuilder.append("sideTransactions", sideTransactionsCounter.valueForLegacyUse());
}

void recordOrdinaryMultikeyPathChanges(OperationContext* opCtx, int64_t count) {
    invariant(count >= 0);
    (opCtx->inMultiDocumentTransaction() ? ordinaryInTransactionCounter
                                         : ordinaryOutsideTransactionCounter)
        .add(count);
}

void recordSideTransaction() {
    sideTransactionsCounter.add(1);
}

}  // namespace mongo::catalog_metrics
