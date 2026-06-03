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

#include "mongo/db/index/index_bulk_builder_metrics.h"

#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/sorter/sorter_stats.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/metrics_updown_counter.h"
#include "mongo/util/static_immortal.h"

#include <algorithm>

namespace mongo {
namespace {
using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;

auto& numSortedMetric =
    MetricsService::instance().createInt64Counter(MetricNames::kIndexBulkBuilderNumSorted,
                                                  "The total number of sorted documents",
                                                  MetricUnit::kCount);
auto& bytesSortedMetric =
    MetricsService::instance().createInt64Counter(MetricNames::kIndexBulkBuilderBytesSorted,
                                                  "The total number of bytes for sorted documents",
                                                  MetricUnit::kBytes);
auto& bytesSpilledMetric = MetricsService::instance().createInt64Counter(
    MetricNames::kIndexBulkBuilderBytesSpilled,
    "The number of bytes written to disk by the external sorter",
    MetricUnit::kBytes);
auto& bytesSpilledUncompressedMetric = MetricsService::instance().createInt64Counter(
    MetricNames::kIndexBulkBuilderBytesSpilledUncompressed,
    "The number of bytes to be written to disk by the external sorter before compression",
    MetricUnit::kBytes);
auto& memUsageMetric = MetricsService::instance().createInt64Gauge(
    MetricNames::kIndexBulkBuilderMemUsage,
    "The current bytes of memory allocated for building indexes",
    MetricUnit::kBytes);
auto& spilledRangesMetric = MetricsService::instance().createInt64UpDownCounter(
    MetricNames::kIndexBulkBuilderSpilledRanges,
    "The number of times the external sorter spilled to disk",
    MetricUnit::kCount);

/**
 * Metrics for index bulk builder operations. Intended to support index build diagnostics
 * during the following scenarios:
 * - createIndex commands;
 * - collection cloning during initial sync; and
 * - resuming index builds at startup.
 *
 * Also includes statistics for disk usage (by the external sorter) for index builds that
 * do not fit in memory.
 */
class IndexBulkBuilderSSS : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const final {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& configElement) const final {
        const auto metrics = indexBulkBuilderMetrics().snapshot();
        BSONObjBuilder builder;
        builder.append("count", metrics.count);
        builder.append("resumed", metrics.resumed);
        builder.append("filesOpenedForExternalSort", metrics.filesOpenedForExternalSort);
        builder.append("filesClosedForExternalSort", metrics.filesClosedForExternalSort);
        builder.append("spilledRanges", metrics.spilledRanges);
        builder.append("mergedSpills", metrics.mergedSpills);
        builder.append("bytesSpilledUncompressed", metrics.bytesSpilledUncompressed);
        builder.append("bytesSpilled", metrics.bytesSpilled);
        builder.append("numSorted", metrics.numSorted);
        builder.append("bytesSorted", metrics.bytesSorted);
        builder.append("memUsage", metrics.memUsage);
        return builder.obj();
    }
};

auto& indexBulkBuilderSSS =
    *ServerStatusSectionBuilder<IndexBulkBuilderSSS>("indexBulkBuilder").forShard();
}  // namespace

IndexBulkBuilderMetricsSnapshot IndexBulkBuilderMetrics::snapshot() const {
    return {count.loadRelaxed(),
            resumed.loadRelaxed(),
            sorterFileStats.opened.loadRelaxed(),
            sorterFileStats.closed.loadRelaxed(),
            sorterTracker.spilledRanges.loadRelaxed(),
            sorterTracker.mergedSpills.loadRelaxed(),
            sorterTracker.bytesSpilledUncompressed.loadRelaxed(),
            sorterTracker.bytesSpilled.loadRelaxed(),
            sorterTracker.numSorted.loadRelaxed(),
            sorterTracker.bytesSorted.loadRelaxed(),
            sorterTracker.memUsage.loadRelaxed()};
}

IndexBulkBuilderMetrics& indexBulkBuilderMetrics() {
    static StaticImmortal<IndexBulkBuilderMetrics> instance;
    return *instance;
}

void updateIndexBulkBuilderOtelMetrics(const IndexBulkBuilderMetricsSnapshot& prev,
                                       const IndexBulkBuilderMetricsSnapshot& curr) {
    const auto safeDelta = [](long long prev, long long curr) {
        return std::max(0LL, curr - prev);
    };

    numSortedMetric.add(safeDelta(prev.numSorted, curr.numSorted));
    bytesSortedMetric.add(safeDelta(prev.bytesSorted, curr.bytesSorted));
    bytesSpilledMetric.add(safeDelta(prev.bytesSpilled, curr.bytesSpilled));
    bytesSpilledUncompressedMetric.add(
        safeDelta(prev.bytesSpilledUncompressed, curr.bytesSpilledUncompressed));
    memUsageMetric.set(curr.memUsage);
    spilledRangesMetric.add(curr.spilledRanges - prev.spilledRanges);
}

}  // namespace mongo
