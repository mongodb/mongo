// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_list_sessions.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::rule_based_rewrites::pipeline {
namespace {

bool canOptimizeSubPipeForJoinOpt(PipelineRewriteContext& ctx) {
    auto& lookup = ctx.currentAs<DocumentSourceLookUp>();

    if (lookup.hasPipeline()) {
        // Join optimization currently only supports lookup subpipelines with a depth of 1 eg there
        // cannot be further nested subpipelines.
        auto depth = lookup.getSubpipelineExpCtx()->getSubPipelineDepth();
        return depth == 1 && ctx.getExpCtx().getQueryKnobConfiguration().isJoinOrderingEnabled();
    }
    return false;
}

bool optimizeSubPipeForJoinOpt(PipelineRewriteContext& ctx) {

    auto& lookup = checked_cast<DocumentSourceLookUp&>(ctx.current());
    auto& subpipeline = lookup.getResolvedIntrospectionPipeline();
    pipeline_optimization::optimizePipeline(subpipeline);

    return false;
}


}  // namespace

/**
 * By default, $lookup optimizes its subpipeline during execution. As a consequence, a $lookup
 * subpipeline that contains multiple consecutive $match stages will be considered ineligible for
 * join-opt, which only allows for $lookup + $unwind pipelines where if present, a $lookup
 * subpipeline can only contain a single match stage. This rewrite rule ensures that when the join
 * opt knob is enabled, multiple $match stages in a $lookup subpipeline get merged together before
 * the query is evaluted for join opt eligibility.
 */
REGISTER_RULES(DocumentSourceLookUp,
               OPTIMIZE_AT_RULE(DocumentSourceLookUp),
               {
                   .name = "OPTIMIZE_SUBPIPELINE_FOR_JOIN_OPT",
                   .precondition = canOptimizeSubPipeForJoinOpt,
                   .transform = optimizeSubPipeForJoinOpt,
                   .priority = kDefaultPushdownPriority,
                   .tags = PipelineRewriteContext::Tags::InPlace,
               });

}  // namespace mongo::rule_based_rewrites::pipeline
