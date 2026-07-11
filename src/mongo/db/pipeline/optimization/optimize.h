// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace pipeline_optimization {
/**
 * Enabling the disablePipelineOptimization fail point will stop the aggregate command from
 * attempting to optimize the pipeline or the pipeline stages. Neither DocumentSource::optimizeAt()
 * nor DocumentSource::optimize() will be attempted.
 */
extern FailPoint disablePipelineOptimization;

/**
 * Modifies the pipeline, optimizing it by combining and swapping stages.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void optimizePipeline(Pipeline& pipeline);

/**
 * Modifies the container, optimizes each stage individually.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void optimizeEachStage(ExpressionContext&,
                                                       DocumentSourceContainer* container);

/**
 * Modifies the container, optimizing it by combining, swapping, dropping and/or inserting
 * stages. If 'itr' is given, optimizes the pipeline starting from the stage that 'itr' points to.
 */
void optimizeContainer(ExpressionContext&,
                       DocumentSourceContainer* container,
                       boost::optional<DocumentSourceContainer::iterator> itr = {});

/**
 * Optimize the given pipeline after the stage that 'itr' points to.
 *
 * Returns a valid iterator that points to the new "end of the pipeline": i.e., the stage that
 * comes after 'itr' in the newly optimized pipeline.
 */
DocumentSourceContainer::iterator optimizeEndOfPipeline(ExpressionContext&,
                                                        DocumentSourceContainer::iterator itr,
                                                        DocumentSourceContainer* container);

/*
 * Helper to optimize and validate pipelines. This helper is used by stages that execute
 * subpipelines (lookup, graphLookup, unionWith), and **must** be called before we execute the
 * subpipeline.
 */
inline void optimizeAndValidatePipeline(Pipeline* pipeline) {
    tassert(10313300, "Expected pipeline to optimize", pipeline);
    optimizePipeline(*pipeline);
    pipeline->validateCommon(true /* alreadyOptimized */);
}
}  // namespace pipeline_optimization
}  // namespace mongo
