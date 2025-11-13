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

#include "mongo/db/pipeline/document_source_redact.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::rule_based_rewrites::pipeline {
namespace {
bool canSwapWithSkippingOrLimitingStage(const PipelineRewriteContext& ctx) {
    return !ctx.atFirstStage() && ctx.prevStage()->constraints().canSwapWithSkippingOrLimitingStage;
}

bool canSwapWithSingleDocTransformOrRedact(const PipelineRewriteContext& ctx) {
    const bool canSwap =
        !ctx.atFirstStage() && ctx.prevStage()->constraints().canSwapWithSingleDocTransformOrRedact;
    if (canSwap) {
        LOGV2_DEBUG(11010402,
                    5,
                    "Pushing a single document transform stage or a redact stage in ahead of "
                    "the previous stage: ",
                    "singleDocTransformOrRedactStage"_attr =
                        redact(ctx.current().serializeToBSONForDebug()),
                    "previousStage"_attr = redact(ctx.prevStage()->serializeToBSONForDebug()));
    }
    return canSwap;
}
}  // namespace

REGISTER_RULES(DocumentSourceSample,
               {
                   .name = "PUSHDOWN_SAMPLE",
                   .precondition = canSwapWithSkippingOrLimitingStage,
                   .transform = Transforms::swapStageWithPrev,
                   .priority = kDefaultPushdownPriority,
                   .tags = PipelineRewriteContext::Tags::Reordering,
               });
REGISTER_RULES(DocumentSourceSingleDocumentTransformation,
               {
                   .name = "PUSHDOWN_SINGLE_DOC_TRANSFORMATION",
                   .precondition = canSwapWithSingleDocTransformOrRedact,
                   .transform = Transforms::swapStageWithPrev,
                   .priority = kDefaultPushdownPriority,
                   .tags = PipelineRewriteContext::Tags::Reordering,
               },
               OPTIMIZE_AT_RULE(DocumentSourceSingleDocumentTransformation));
REGISTER_RULES(DocumentSourceRedact,
               {
                   .name = "PUSHDOWN_REDACT",
                   .precondition = canSwapWithSingleDocTransformOrRedact,
                   .transform = Transforms::swapStageWithPrev,
                   .priority = kDefaultPushdownPriority,
                   .tags = PipelineRewriteContext::Tags::Reordering,
               },
               OPTIMIZE_AT_RULE(DocumentSourceRedact));
}  // namespace mongo::rule_based_rewrites::pipeline
