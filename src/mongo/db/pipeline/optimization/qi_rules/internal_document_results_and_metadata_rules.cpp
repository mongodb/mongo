// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/checked_cast.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/pipeline/wrapped_extension_source_hooks.h"
#include "mongo/db/query/query_feature_flags_gen.h"

namespace mongo::rule_based_rewrites::pipeline {
namespace {

// Drives the wrapped source's suffix-dependency and in-place rules from
// DocumentSourceInternalDocumentResultsAndMetadata's rewrite context via
// WrappedExtensionSourceHooks. Gated on featureFlagExtensionsOptimizations.

const WrappedExtensionSourceHooks* getWrappedHooks(PipelineRewriteContext& ctx) {
    const auto* stage =
        dynamic_cast<const DocumentSourceInternalDocumentResultsAndMetadata*>(&ctx.current());
    // Skipped on a split shard, where the pipeline suffix is incomplete.
    if (!stage || ctx.getExpCtx().getNeedsMerge()) {
        return nullptr;
    }
    return dynamic_cast<const WrappedExtensionSourceHooks*>(stage->getSourceStage().get());
}

bool applySuffixDependenciesPrecondition(PipelineRewriteContext& ctx) {
    return getWrappedHooks(ctx) && ctx.hasAtLeastNNextStages(1);
}

bool applySuffixDependenciesTransform(PipelineRewriteContext& ctx) {
    const auto& stage =
        checked_cast<const DocumentSourceInternalDocumentResultsAndMetadata&>(ctx.current());
    auto* hooks = dynamic_cast<WrappedExtensionSourceHooks*>(stage.getSourceStage().get());
    tassert(13133700,
            "Expected DocumentSourceInternalDocumentResultsAndMetadata's wrapped source to "
            "implement WrappedExtensionSourceHooks",
            hooks != nullptr);
    hooks->applyPipelineSuffixDependencies(ctx.getPipelineSuffixDependencies(),
                                           ctx.getBuiltInVariableRefsInPipelineSuffix());
    return false;
}

bool dispatchWrappedSourceInPlacePrecondition(PipelineRewriteContext& ctx) {
    if (const auto* hooks = getWrappedHooks(ctx)) {
        hooks->dispatchInPlaceRules(ctx);
    }
    return false;
}

}  // namespace

REGISTER_RULES_WITH_FEATURE_FLAG(
    DocumentSourceInternalDocumentResultsAndMetadata,
    &feature_flags::gFeatureFlagExtensionsOptimizations,
    {
        .name = "INTERNAL_DOCUMENT_RESULTS_AND_METADATA_APPLY_SUFFIX_DEPENDENCIES_TO_SOURCE",
        .precondition = applySuffixDependenciesPrecondition,
        .transform = applySuffixDependenciesTransform,
        .priority = kDefaultOptimizeInPlacePriority + 1,
        .tags = PipelineRewriteContext::Tags::InPlace,
    },
    {
        .name = "INTERNAL_DOCUMENT_RESULTS_AND_METADATA_DISPATCH_WRAPPED_SOURCE_IN_PLACE",
        .precondition = dispatchWrappedSourceInPlacePrecondition,
        .transform = Transforms::noop,
        .priority = kDefaultOptimizeInPlacePriority,
        .tags = PipelineRewriteContext::Tags::InPlace,
    });

}  // namespace mongo::rule_based_rewrites::pipeline
