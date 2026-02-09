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

    if (lookup.hasPipeline() && ctx.getExpCtx().queryKnobIsInitialized()) {
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
