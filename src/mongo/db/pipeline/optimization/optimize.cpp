/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/optimization/optimize.h"

#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"

namespace mongo::pipeline_optimization {
namespace rbr = rule_based_rewrites::pipeline;

MONGO_FAIL_POINT_DEFINE(disablePipelineOptimization);

namespace {
using Tags = rbr::PipelineRewriteContext::Tags;

void applyRuleBasedRewrites(rbr::PipelineRewriteContext rewriteContext,
                            rule_based_rewrites::TagSet tags) {
    rbr::PipelineRewriteEngine engine(std::move(rewriteContext),
                                      internalQueryMaxPipelineRewrites.load());

    try {
        engine.applyRules(tags);
    } catch (DBException& ex) {
        ex.addContext("Failed to optimize pipeline");
        throw;
    }
}
}  // namespace

/**
 * Modifies the pipeline, optimizing it by combining and swapping stages.
 */
void optimizePipeline(Pipeline& pipeline) {
    tassert(10706501,
            "unexpected attempt to modify a frozen pipeline in 'optimizePipeline()'",
            !pipeline.isFrozen());
    // If the disablePipelineOptimization failpoint is enabled, the pipeline won't be optimized.
    if (MONGO_unlikely(disablePipelineOptimization.shouldFail())) {
        return;
    }
    applyRuleBasedRewrites(rbr::PipelineRewriteContext(pipeline), Tags::Reordering);
    applyRuleBasedRewrites(rbr::PipelineRewriteContext(pipeline), Tags::InPlace);
}

/**
 * Modifies the container, optimizes each stage individually.
 */
void optimizeEachStage(ExpressionContext& expCtx, DocumentSourceContainer* container) {
    applyRuleBasedRewrites(rbr::PipelineRewriteContext(expCtx, *container), Tags::InPlace);
}

/**
 * Modifies the container, optimizing it by combining, swapping, dropping and/or inserting
 * stages.
 */
void optimizeContainer(ExpressionContext& expCtx,
                       DocumentSourceContainer* container,
                       boost::optional<DocumentSourceContainer::iterator> itr) {
    applyRuleBasedRewrites(rbr::PipelineRewriteContext(expCtx, *container, itr), Tags::Reordering);
}

/**
 * Optimize the given pipeline after the stage that 'itr' points to.
 *
 * Returns a valid iterator that points to the new "end of the pipeline": i.e., the stage that
 * comes after 'itr' in the newly optimized pipeline.
 */
DocumentSourceContainer::iterator optimizeEndOfPipeline(ExpressionContext& expCtx,
                                                        DocumentSourceContainer::iterator itr,
                                                        DocumentSourceContainer* container) {
    // We must create a new DocumentSourceContainer representing the subsection of the pipeline we
    // wish to optimize, since otherwise calls to optimizeAt() will overrun these limits.
    auto endOfPipeline = DocumentSourceContainer(std::next(itr), container->end());
    optimizeContainer(expCtx, &endOfPipeline);
    optimizeEachStage(expCtx, &endOfPipeline);
    container->erase(std::next(itr), container->end());
    container->splice(std::next(itr), endOfPipeline);

    return std::next(itr);
}
}  // namespace mongo::pipeline_optimization
