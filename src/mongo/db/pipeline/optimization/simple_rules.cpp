// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
               OPTIMIZE_AT_RULE(DocumentSourceSingleDocumentTransformation),
               OPTIMIZE_IN_PLACE_RULE(DocumentSourceSingleDocumentTransformation));
REGISTER_RULES(DocumentSourceRedact,
               {
                   .name = "PUSHDOWN_REDACT",
                   .precondition = canSwapWithSingleDocTransformOrRedact,
                   .transform = Transforms::swapStageWithPrev,
                   .priority = kDefaultPushdownPriority,
                   .tags = PipelineRewriteContext::Tags::Reordering,
               },
               OPTIMIZE_AT_RULE(DocumentSourceRedact),
               OPTIMIZE_IN_PLACE_RULE(DocumentSourceRedact));
}  // namespace mongo::rule_based_rewrites::pipeline
