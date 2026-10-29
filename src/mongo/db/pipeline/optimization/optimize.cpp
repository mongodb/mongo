// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/optimization/optimize.h"

#include "mongo/db/pipeline/optimization/graph_validation_rules.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"

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

    // Remove previously inserted validation stages if the dependency graph validation knob is
    // enabled.
    // TODO SERVER-127305: Move removal of validation stages into a separate optimization rule.
    if (internalEnableDependencyGraphValidation.loadRelaxed()) {
        removeArraynessValidationStages(pipeline);
    }

    if (QueryKnobConfiguration::get(pipeline.getContext()->getOperationContext())
            .getEnablePipelineOptimizationAdditionalTestingRules()) {
        applyRuleBasedRewrites(rbr::PipelineRewriteContext(pipeline), Tags::Testing);
    }
    applyRuleBasedRewrites(rbr::PipelineRewriteContext(pipeline), Tags::Reordering);
    applyRuleBasedRewrites(rbr::PipelineRewriteContext(pipeline), Tags::InPlace);

    // Insert validation stages if the dependency graph validation knob is enabled.
    // TODO SERVER-127305: Move insertion of validation stages into a separate optimization rule.
    if (internalEnableDependencyGraphValidation.loadRelaxed()) {
        insertArraynessValidationStages(pipeline);
    }
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
