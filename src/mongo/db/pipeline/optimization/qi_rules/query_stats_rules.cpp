// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_query_stats.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_stats/query_stats_top_k_metrics.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace rule_based_rewrites::pipeline {
namespace {

bool isProjectStage(const DocumentSource* stage) {
    const auto* singleDocTransform =
        dynamic_cast<const DocumentSourceSingleDocumentTransformation*>(stage);
    if (!singleDocTransform) {
        return false;
    }

    // Only an inclusion-only $project is eligible.
    return singleDocTransform->getTransformerType() ==
        TransformerInterface::TransformerType::kInclusionProjection;
}

bool canOptimizeQueryStatsTopK(const PipelineRewriteContext& ctx) {
    if (!internalQueryStatsTopKSortOptimizationEnabled.load()) {
        return false;
    }

    if (!ctx.hasAtLeastNNextStages(1)) {
        return false;
    }

    // Check for an optional inclusion-only $project stage, capturing its modified-paths so the
    // sort-field pass-through can be verified below without recomputing them.
    size_t offset = 1;
    boost::optional<DocumentSource::GetModPathsReturn> modPaths;
    if (isProjectStage(ctx.nthNextStage(offset).get())) {
        auto modifiedPaths = ctx.nthNextStage(offset)->getModifiedPaths();
        if (modifiedPaths.type != DocumentSource::GetModPathsReturn::Type::kAllExcept) {
            return false;
        }
        modPaths = std::move(modifiedPaths);
        ++offset;
        // Make sure we still have room for a possible $sort ahead of us.
        if (!ctx.hasAtLeastNNextStages(offset)) {
            return false;
        }
    }

    // Must be followed by a $sort.
    auto* sortStage = dynamic_cast<DocumentSourceSort*>(ctx.nthNextStage(offset).get());
    if (!sortStage) {
        return false;
    }

    // The $sort must have absorbed the following $limit. A standalone $limit is never found here:
    // $sort absorbs an adjacent $limit in its optimize-at rule, and the engine applies
    // Reordering rules before InPlace rules.
    if (!sortStage->hasLimit() ||
        sortStage->getLimit().value() > query_stats::kTopKOptimizationMaxLimit) {
        return false;
    }

    // Must be a single-field sort on a plain field path (no $meta, no compound key).
    const SortPattern& pattern = sortStage->getSortPattern();
    if (!pattern.isSingleElementKey()) {
        return false;
    }
    const auto& part = pattern[0];
    if (part.expression || !part.fieldPath) {
        return false;
    }

    // The sort field must pass through the optional $project unchanged, otherwise the sort below it
    // may be ordering documents on a path the projection renames or removes.
    std::string sortPath = part.fieldPath->fullPath();
    if (modPaths && modPaths->canModify(FieldPath{sortPath})) {
        return false;
    }

    // The sort path must map to a supported accessor.
    if (!query_stats::getCheapMetricAccessor(sortPath)) {
        return false;
    }

    return true;
}

/**
 * Sets a TopKSortSpec hint on $queryStats so the exec stage can perform a bounded heap scan
 * instead of materializing every entry.
 *
 * Returns false: we never restructure the pipeline, only set a hint on the current stage.
 */
bool optimizeQueryStatsTopK(PipelineRewriteContext& ctx) {
    // Re-derive the $project/$sort shape established by the precondition.
    size_t offset = 1;
    if (isProjectStage(ctx.nthNextStage(offset).get())) {
        ++offset;
    }

    auto* sortStage = dynamic_cast<DocumentSourceSort*>(ctx.nthNextStage(offset).get());
    tassert(12938703,
            "Expected a $sort at the offset established by canOptimizeQueryStatsTopK",
            sortStage);
    const SortPattern& pattern = sortStage->getSortPattern();
    tassert(12938704,
            "Expected a single-element sort pattern established by canOptimizeQueryStatsTopK",
            pattern.isSingleElementKey());
    const auto& part = pattern[0];
    tassert(12938705,
            "Expected a plain field path established by canOptimizeQueryStatsTopK",
            part.fieldPath);
    std::string sortPath = part.fieldPath->fullPath();

    // The $sort (and the $limit it absorbed) deliberately stay in the pipeline. The top-K hint
    // only narrows which entries the stage materializes; the retained $sort still performs the
    // user visible ordering, and the retained $limit caps how many documents are returned.
    ctx.currentAs<DocumentSourceQueryStats>().setTopKSortSpec(
        query_stats::TopKSortSpec{std::move(sortPath), part.isAscending, *sortStage->getLimit()});
    return false;
}

}  // namespace

REGISTER_RULES(DocumentSourceQueryStats,
               {
                   .name = "CAPTURE_TOPK_SORT_SPEC",
                   .precondition = canOptimizeQueryStatsTopK,
                   .transform = optimizeQueryStatsTopK,
                   .priority = rbr::kDefaultOptimizeInPlacePriority,
                   .tags = rbr::PipelineRewriteContext::Tags::InPlace,
               });

}  // namespace rule_based_rewrites::pipeline
}  // namespace mongo
