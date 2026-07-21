// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_executable_pipeline.h"

#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/util/assert_util.h"

#include <utility>

namespace mongo::resharding {

ReshardingExecutablePipeline::ReshardingExecutablePipeline()
    : ReshardingExecutablePipeline(
          [](const Pipeline& pipeline) { return exec::agg::buildPipeline(pipeline); }) {}

ReshardingExecutablePipeline::ReshardingExecutablePipeline(ExecPipelineFactory execPipelineFactory)
    : _execPipelineFactory(std::move(execPipelineFactory)) {}

void ReshardingExecutablePipeline::reinitialize(std::unique_ptr<Pipeline> pipeline) {
    tassert(13159800, "reinitialize() called on an already-initialized pipeline", !isInitialized());

    // Build the executable pipeline into a local and only commit both halves to the member state
    // once it has been constructed successfully. Otherwise, if building the executable pipeline
    // throws, '_pipeline' would be left non-null while '_execPipeline' is null, breaking the
    // invariant the rest of this class relies on.
    auto execPipeline = _execPipelineFactory(pipeline->freeze());
    _pipeline = std::move(pipeline);
    _execPipeline = std::move(execPipeline);
}

void ReshardingExecutablePipeline::reattachToOpCtx(OperationContext* opCtx) {
    tassert(13159801, "reattachToOpCtx() called on an uninitialized pipeline", isInitialized());

    _execPipeline->reattachToOperationContext(opCtx);
    _pipeline->reattachToOperationContext(opCtx);

    if (_memoryUsageTracker) {
        OperationMemoryUsageTracker::moveToOpCtxIfAvailable(opCtx, std::move(_memoryUsageTracker));
    }
}

void ReshardingExecutablePipeline::detachFromOpCtx() {
    tassert(13159802, "detachFromOpCtx() called on an uninitialized pipeline", isInitialized());

    auto* opCtx = _pipeline->getContext()->getOperationContext();
    _execPipeline->detachFromOperationContext();
    _pipeline->detachFromOperationContext();
    _memoryUsageTracker = OperationMemoryUsageTracker::moveFromOpCtxIfAvailable(opCtx);
}

void ReshardingExecutablePipeline::dispose(OperationContext* opCtx) {
    if (!isInitialized()) {
        return;
    }

    _execPipeline->reattachToOperationContext(opCtx);

    if (_memoryUsageTracker) {
        OperationMemoryUsageTracker::moveToOpCtxIfAvailable(opCtx, std::move(_memoryUsageTracker));
    }

    _execPipeline->dispose();
    _pipeline.reset();
    _execPipeline.reset();
}

exec::agg::Pipeline& ReshardingExecutablePipeline::get() {
    tassert(13159803, "get() called on an uninitialized pipeline", isInitialized());
    return *_execPipeline;
}

}  // namespace mongo::resharding
