// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_query_stats.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_stats/query_stats_top_k_metrics.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::rule_based_rewrites::pipeline {
namespace {

constexpr std::string_view kKeyHashFieldName = "keyHash";

/**
 * Extracts the user-visible hashes from a keyHash-only $eq/$in leaf, or boost::none if 'expr' is
 * not one. The keyHash index compares exact user-visible hashes, so a non-simple collation (and,
 * for $in, a regex) makes the leaf ineligible.
 */
boost::optional<std::vector<std::string>> extractUserVisibleHashesFromLeaf(
    const MatchExpression* expr) {
    switch (expr->matchType()) {
        case MatchExpression::EQ: {
            const auto* eq = static_cast<const EqualityMatchExpression*>(expr);
            if (eq->path() != kKeyHashFieldName || eq->getCollator()) {
                return boost::none;
            }
            // A non-string element can never equal the keyHash field so it's dropped.
            if (eq->getData().type() != BSONType::string) {
                return std::vector<std::string>{};
            }
            return std::vector<std::string>{eq->getData().str()};
        }
        case MatchExpression::MATCH_IN: {
            const auto* in = static_cast<const InMatchExpression*>(expr);
            if (in->path() != kKeyHashFieldName || in->hasRegex() || in->getCollator()) {
                return boost::none;
            }
            std::vector<std::string> userVisibleHashes;
            for (const auto& elem : in->getEqualities()) {
                // A non-string element can never equal the keyHash field, so drop it.
                if (elem.type() == BSONType::string) {
                    userVisibleHashes.push_back(elem.str());
                }
            }
            return userVisibleHashes;
        }
        default:
            return boost::none;
    }
}

/**
 * The user-visible hashes present in both, in the first's order.
 */
std::vector<std::string> intersectUserVisibleHashes(
    const std::vector<std::string>& userVisibleHashes1,
    const std::vector<std::string>& userVisibleHashes2) {
    std::vector<std::string> intersection;
    for (const auto& location : userVisibleHashes1) {
        if (std::find(userVisibleHashes2.begin(), userVisibleHashes2.end(), location) !=
            userVisibleHashes2.end()) {
            intersection.push_back(location);
        }
    }
    return intersection;
}

boost::optional<std::vector<std::string>> extractKeyHashEqualities(const MatchExpression* expr) {
    if (!expr) {
        return boost::none;
    }
    if (expr->matchType() == MatchExpression::OR) {
        std::vector<std::string> locations;
        for (const auto& child : static_cast<const OrMatchExpression&>(*expr).getChildVector()) {
            auto childLocations = extractKeyHashEqualities(child.get());
            if (!childLocations) {
                return boost::none;
            }
            locations.insert(locations.end(), childLocations->begin(), childLocations->end());
        }
        return locations;
    }
    if (expr->matchType() != MatchExpression::AND) {
        return extractUserVisibleHashesFromLeaf(expr);
    }

    // Capture keyHash $eq/$in conjuncts reachable through AND expressions (compound or nested)
    // only. Anything under a $nor or $not may match a different keyHash, so the index cannot narrow
    // it. Conjuncts intersect, since a document has a single keyHash.
    boost::optional<std::vector<std::string>> locations;
    for (const auto& child : static_cast<const AndMatchExpression&>(*expr).getChildVector()) {
        auto childLocations = extractKeyHashEqualities(child.get());
        if (!childLocations) {
            continue;
        }
        if (!locations) {
            locations = std::move(*childLocations);
            continue;
        }
        locations = intersectUserVisibleHashes(*locations, *childLocations);
        if (locations->empty()) {
            // The keyHash conjuncts are disjoint, so no document can match the $and.
            return std::vector<std::string>{};
        }
    }
    return locations;
}

bool canExtractUserVisibleHashes(PipelineRewriteContext& ctx) {
    if (!internalQueryStatsMatchPushdownOptimizationEnabled.load()) {
        // The optimization is disabled: leave the stage on its default path.
        return false;
    }
    return !ctx.atLastStage() && dynamic_cast<DocumentSourceMatch*>(ctx.nextStage().get());
}

/**
 * Records the keyHash locations from the $match immediately following $queryStats, so execution
 * resolves them through the query stats store's 'KeyHashIndex' instead of scanning the whole store.
 *
 * Writes boost::none when the following $match has no keyHash equal/containment leaf usable by the
 * index (e.g. only non-keyHash predicates, or an $or with a branch the index cannot resolve).
 */
bool captureKeyHashLookupLocations(PipelineRewriteContext& ctx) {
    auto& nextMatch = checked_cast<DocumentSourceMatch&>(*ctx.nextStage());
    ctx.currentAs<DocumentSourceQueryStats>().setKeyHashLookupLocations(
        extractKeyHashEqualities(nextMatch.getMatchExpression()));
    return false;
}

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

REGISTER_RULES(DocumentSourceQueryStats,
               {
                   .name = "PUSH_MATCH_KEYHASH_LOOKUP_LOCATIONS",
                   .precondition = canExtractUserVisibleHashes,
                   .transform = captureKeyHashLookupLocations,
                   .priority = kDefaultPushdownPriority,
                   .tags = PipelineRewriteContext::Tags::InPlace,
               });

REGISTER_RULES(DocumentSourceQueryStats,
               {
                   .name = "CAPTURE_TOPK_SORT_SPEC",
                   .precondition = canOptimizeQueryStatsTopK,
                   .transform = optimizeQueryStatsTopK,
                   .priority = rbr::kDefaultOptimizeInPlacePriority,
                   .tags = rbr::PipelineRewriteContext::Tags::InPlace,
               });

}  // namespace

}  // namespace mongo::rule_based_rewrites::pipeline
