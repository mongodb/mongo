// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/util/modules.h"

#include <functional>
#include <memory>

namespace mongo::resharding {

/**
 * Manage a pipeline together with its executable form and ensures that both are either
 * initialized or not (i.e. guarantees that there will be never a case where one is initialized and
 * the other is not).
 *
 * The stages in the executable pipeline report memory usage into the OperationMemoryUsageTracker
 * owned by the operation context they were built with. This class stashes that tracker while
 * detached so it outlives the operation context it came from, and restores it on reattach.
 *
 * Instances of this class are not thread-safe.
 */
class ReshardingExecutablePipeline {
public:
    using ExecPipelineFactory =
        std::function<std::unique_ptr<exec::agg::Pipeline>(const Pipeline&)>;

    /**
     * Builds executable pipelines with exec::agg::buildPipeline().
     */
    ReshardingExecutablePipeline();

    /**
     * Builds executable pipelines with the given factory. Intended for tests that need to exercise
     * failure paths (a throw while building the executable half).
     */
    explicit ReshardingExecutablePipeline(ExecPipelineFactory execPipelineFactory);

    /**
     * Returns whether a pipeline pair is currently held.
     */
    bool isInitialized() const {
        return static_cast<bool>(_pipeline);
    }

    /**
     * Builds the executable pipeline from 'pipeline' and stores both. This provides a strong
     * exception guarantee: if building the executable pipeline throws, this holder is left
     * unchanged (uninitialized) rather than holding a base pipeline with no executable pipeline.
     *
     * The pair is left attached to the operation context that 'pipeline' was built with; call
     * detachFromOpCtx() to detach it. Must not be called while already initialized.
     */
    void reinitialize(std::unique_ptr<Pipeline> pipeline);

    /**
     * Reattaches the pipeline pair to 'opCtx' and restores any stashed memory-usage tracker onto
     * it. Requires the holder to be initialized.
     */
    void reattachToOpCtx(OperationContext* opCtx);

    /**
     * Detaches the pipeline pair from the operation context it is currently attached to, stashing
     * any memory-usage tracker so it outlives that operation context. Requires the holder to be
     * initialized.
     */
    void detachFromOpCtx();

    /**
     * Disposes the pipeline pair - reattaching to 'opCtx' first so disposal runs with a live
     * operation context - and resets this holder to the uninitialized state. Safe no-op if the
     * holder is not initialized.
     */
    void dispose(OperationContext* opCtx);

    /**
     * Returns the executable pipeline. Requires the holder to be initialized. The returned
     * reference is valid until the next call to reinitialize() or dispose().
     */
    exec::agg::Pipeline& get();

private:
    // The stages in '_execPipeline' report memory usage into the OperationMemoryUsageTracker owned
    // by the operation context they were built with. We stash that tracker here while detached so
    // it outlives the operation context it came from, and restore it onto the new operation context
    // on reattach.
    std::unique_ptr<OperationMemoryUsageTracker> _memoryUsageTracker;

    // The raw pipeline. This by itself is not executable but is kept alive for '_execPipeline'.
    std::unique_ptr<Pipeline> _pipeline;

    // The pipeline that is for fetching the oplog entries and is fully executable.
    std::unique_ptr<exec::agg::Pipeline> _execPipeline;

    ExecPipelineFactory _execPipelineFactory;
};

}  // namespace mongo::resharding
