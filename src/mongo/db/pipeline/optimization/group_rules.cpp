// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/pipeline/sbe_pushdown_util.h"
#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph.h"
#include "mongo/db/query/query_feature_flags_gen.h"

namespace mongo::rule_based_rewrites::pipeline {
namespace {

bool mayRunInSbe(PipelineRewriteContext& ctx) {
    const auto& group = ctx.currentAs<DocumentSourceGroup>();
    const auto& knobs = ctx.getExpCtx().getQueryKnobConfiguration();
    return !knobs.isForceClassicEngineEnabled() && !knobs.getSbeDisableGroupPushdownForOp() &&
        isGroupSbeCompatible(
            group,
            getMinRequiredSbeCompatibility(knobs.getInternalQueryFrameworkControlForOp(),
                                           feature_flags::gFeatureFlagSbeFull.isEnabled()));
}

bool annotateNonArrayIdFields(PipelineRewriteContext& ctx) {
    auto& group = ctx.currentAs<DocumentSourceGroup>();
    const auto& graph = ctx.getDependencyGraph();
    OrderedPathSet nonArrayIdFields;
    for (auto&& [field, expr] : group.getIdFields()) {
        auto path = dynamic_cast<const ExpressionFieldPath*>(expr.get());
        if (path && !path->isVariableReference() && !path->isROOT() &&
            !graph.canPathBeArray(&group, path->getFieldPathWithoutCurrentPrefix().fullPath())) {
            nonArrayIdFields.insert(field);
        }
    }
    group.setNonArrayIdFields(std::move(nonArrayIdFields));
    return false;
}

}  // namespace

// Runs only to count plans eligible for streaming group; no streaming group stage uses it yet.
REGISTER_RULES_WITH_FEATURE_FLAG(DocumentSourceGroup,
                                 &feature_flags::gFeatureFlagStreamingGroupEligibilityCounter,
                                 {
                                     .name = "ANNOTATE_GROUP_NON_ARRAY_ID_FIELDS",
                                     .precondition = mayRunInSbe,
                                     .transform = annotateNonArrayIdFields,
                                     .priority = kDefaultOptimizeInPlacePriority,
                                     .tags = PipelineRewriteContext::Tags::Annotation,
                                 });

}  // namespace mongo::rule_based_rewrites::pipeline
