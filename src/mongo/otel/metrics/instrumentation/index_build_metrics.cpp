// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
