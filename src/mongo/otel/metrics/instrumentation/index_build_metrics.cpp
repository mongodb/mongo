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

#include "mongo/otel/metrics/instrumentation/index_build_metrics.h"

#include "mongo/db/index/index_bulk_builder_metrics.h"
#include "mongo/db/service_context.h"
#include "mongo/util/duration.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {
namespace {
struct IndexBuildOtelMetricsState {
    std::unique_ptr<IndexBuildOtelMetrics> metrics;
    PeriodicJobAnchor job;
};

const auto getIndexBuildOtelMetricsState =
    ServiceContext::declareDecoration<IndexBuildOtelMetricsState>();

}  // namespace

class IndexBuildOtelMetrics::Impl {
public:
    Impl() {}

    void update(const IndexBulkBuilderMetricsSnapshot& currSnapshot) {
        updateIndexBulkBuilderOtelMetrics(_prevSnapshot, currSnapshot);
        _prevSnapshot = currSnapshot;
    }

private:
    IndexBulkBuilderMetricsSnapshot _prevSnapshot{};
};

IndexBuildOtelMetrics::IndexBuildOtelMetrics() : _impl(std::make_unique<Impl>()) {}

IndexBuildOtelMetrics::~IndexBuildOtelMetrics() = default;

void IndexBuildOtelMetrics::update(const IndexBulkBuilderMetricsSnapshot& snapshot) {
    _impl->update(snapshot);
}

void installIndexBuildOtelMetrics(ServiceContext* svcCtx) {
    auto& state = getIndexBuildOtelMetricsState(svcCtx);
    state.metrics = std::make_unique<IndexBuildOtelMetrics>();
    state.job = svcCtx->getPeriodicRunner()->makeJob(PeriodicRunner::PeriodicJob{
        "IndexBuildOtelMetrics",
        [&state, svcCtx](Client*) { state.metrics->update(indexBulkBuilderMetrics().snapshot()); },
        Seconds(1),
        false /*isKillableByStepdown*/});
    state.job.start();
}

}  // namespace mongo
