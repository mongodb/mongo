/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/pipeline/sbe_pushdown.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstdlib>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_internal_projection.h"
#include "mongo/db/pipeline/document_source_internal_replace_root.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/query_decorations.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {
/**
 * Helper for findSbeCompatibleStagesForPushdown() that checks whether 'stage' is a $project or
 * $addFields that can be pushed down to SBE as a 'DocumentSourceInternalProjection' stage. If so,
 * this returns a pointer to a constructed object of the latter type, else it returns nullptr.
 */
boost::intrusive_ptr<DocumentSource> sbeCompatibleProjectionFromSingleDocumentTransformation(
    const DocumentSourceSingleDocumentTransformation& transformStage,
    SbeCompatibility minRequiredCompatibility) {
    InternalProjectionPolicyEnum policies;
    switch (transformStage.getType()) {
        case TransformerInterface::TransformerType::kExclusionProjection:
        case TransformerInterface::TransformerType::kInclusionProjection:
            policies = InternalProjectionPolicyEnum::kAggregate;
            break;
        case TransformerInterface::TransformerType::kComputedProjection:
            policies = InternalProjectionPolicyEnum::kAddFields;
            break;
        default:
            return nullptr;
    }

    const boost::intrusive_ptr<ExpressionContext>& expCtx = transformStage.getContext();
    SbeCompatibility originalSbeCompatibility =
        std::exchange(expCtx->sbeCompatibility, SbeCompatibility::fullyCompatible);
    ON_BLOCK_EXIT([&] { expCtx->sbeCompatibility = originalSbeCompatibility; });

    boost::intrusive_ptr<DocumentSource> projectionStage =
        make_intrusive<DocumentSourceInternalProjection>(
            expCtx,
            transformStage.getTransformer().serializeTransformation(boost::none).toBson(),
            policies);

    if (expCtx->sbeCompatibility < minRequiredCompatibility) {
        return nullptr;
    }

    return projectionStage;
}

/**
 * Helper for findSbeCompatibleStagesForPushdown() that creates a
 * 'DocumentSourceInternalReplaceRoot' from 'stage' if 'stage' is a '$replaceRoot' that can be
 * pushed down to SBE or returns nullptr otherwise.
 */
boost::intrusive_ptr<DocumentSource> sbeCompatibleReplaceRootStage(
    DocumentSourceSingleDocumentTransformation* replaceRootStage,
    SbeCompatibility minRequiredCompatibility) {
    if (replaceRootStage->getType() != TransformerInterface::TransformerType::kReplaceRoot) {
        return nullptr;
    }

    const auto& replaceRootTransformation =
        dynamic_cast<const ReplaceRootTransformation&>(replaceRootStage->getTransformer());
    if (replaceRootTransformation.sbeCompatibility() < minRequiredCompatibility) {
        return nullptr;
    }

    return make_intrusive<DocumentSourceInternalReplaceRoot>(
        replaceRootStage->getContext(), replaceRootTransformation.getExpression());
}

// A bit field with a bool flag for each aggregation pipeline stage that can be translated to SBE.
// The flags can be used to indicate which translations are enabled and/or supported in a particular
// context.
struct CompatiblePipelineStages {
    bool group : 1;
    bool lookup : 1;

    // The $project and $addField stages are considered the same for the purposes of SBE
    // translation.
    bool transform : 1;

    bool match : 1;
    bool unwind : 1;
    bool sort : 1;
    bool limitSkip : 1;
    bool search : 1;
    bool window : 1;
    bool unpackBucket : 1;
};

// Determine if 'stage' is eligible for SBE, and if it is add it to the 'stagesForPushdown' list and
// return true. Return false if 'stage' is ineligible, either because it is disallowed by
// 'allowedStages' or because it requires functionality that cannot be translated to SBE.
bool pushDownPipelineStageIfCompatible(
    const OperationContext* opCtx,
    const boost::intrusive_ptr<DocumentSource>& stage,
    SbeCompatibility minRequiredCompatibility,
    const CompatiblePipelineStages& allowedStages,
    std::vector<boost::intrusive_ptr<DocumentSource>>& stagesForPushdown) {
    if (auto matchStage = dynamic_cast<DocumentSourceMatch*>(stage.get())) {
        if (!allowedStages.match || matchStage->sbeCompatibility() < minRequiredCompatibility) {
            return false;
        }

        stagesForPushdown.emplace_back(std::move(stage));
        return true;
    } else if (auto groupStage = dynamic_cast<DocumentSourceGroup*>(stage.get())) {
        if (!allowedStages.group || groupStage->doingMerge() ||
            groupStage->sbeCompatibility() < minRequiredCompatibility) {
            return false;
        }

        stagesForPushdown.emplace_back(std::move(stage));
        return true;
    } else if (auto lookupStage = dynamic_cast<DocumentSourceLookUp*>(stage.get())) {
        if (!allowedStages.lookup || lookupStage->sbeCompatibility() < minRequiredCompatibility) {
            return false;
        }

        stagesForPushdown.emplace_back(std::move(stage));
        return true;
    } else if (auto unwindStage = dynamic_cast<DocumentSourceUnwind*>(stage.get())) {
        if (!allowedStages.unwind || unwindStage->sbeCompatibility() < minRequiredCompatibility) {
            return false;
        }

        stagesForPushdown.emplace_back(std::move(stage));
        return true;
    } else if (auto transformStage =
                   dynamic_cast<DocumentSourceSingleDocumentTransformation*>(stage.get())) {
        if (!allowedStages.transform) {
            return false;
        }
        if (auto replaceRoot =
                sbeCompatibleReplaceRootStage(transformStage, minRequiredCompatibility)) {
            stagesForPushdown.emplace_back(std::move(replaceRoot));
            return true;
        } else if (auto projectionStage = sbeCompatibleProjectionFromSingleDocumentTransformation(
                       *transformStage, minRequiredCompatibility)) {
            stagesForPushdown.emplace_back(std::move(projectionStage));
            return true;
        }
        return false;
    } else if (auto sortStage = dynamic_cast<DocumentSourceSort*>(stage.get())) {
        if (!allowedStages.sort || !isSortSbeCompatible(sortStage->getSortKeyPattern())) {
            return false;
        }

        stagesForPushdown.emplace_back(std::move(stage));
        return true;
    } else if (dynamic_cast<DocumentSourceLimit*>(stage.get()) ||
               dynamic_cast<DocumentSourceSkip*>(stage.get())) {
        if (!allowedStages.limitSkip) {
            return false;
        }

        stagesForPushdown.emplace_back(std::move(stage));
        return true;
    } else if (search_helpers::isSearchStage(stage.get()) ||
               search_helpers::isSearchMetaStage(stage.get())) {
        if (!allowedStages.search) {
            return false;
        }

        stagesForPushdown.emplace_back(std::move(stage));
        return true;
    } else if (auto windowStage =
                   dynamic_cast<DocumentSourceInternalSetWindowFields*>(stage.get())) {
        if (!allowedStages.window || windowStage->sbeCompatibility() < minRequiredCompatibility) {
            return false;
        }
        stagesForPushdown.emplace_back(std::move(stage));
        return true;
    } else if (dynamic_cast<DocumentSourceInternalUnpackBucket*>(stage.get())) {
        if (!allowedStages.unpackBucket) {
            return false;
        }

        stagesForPushdown.emplace_back(std::move(stage));
        return true;
    }

    return false;
}

/**
 * Prunes $addFields from 'stagesForPushdown' if it is the last stage, subject to additional
 * conditions. (Must be called repeatedly until it returns false.) When splitting a pipeline between
 * SBE and Classic DocumentSource stages, there is often a performance penalty for executing an
 * $addFields in SBE only to immediately translate its output to MutableDocument form for the
 * Classic DocumentSource execution phase. Instead, we keep the $addFields as a DocumentSource.
 *
 * 'containsEntirePipeline' indicates that 'stagesForPushdown' contains the entire aggregation
 * pipeline for the query, meaning that execution will use SBE exclusively, skipping the
 * MutableDocument translation step and follow-on Classic DocumentSource processing.
 *
 * Returns true iff it pruned a stage.
 */
bool pruneTrailingAddFields(std::vector<boost::intrusive_ptr<DocumentSource>>& stagesForPushdown,
                            bool containsEntirePipeline) {
    // If we are able to push down the entire pipeline, we prefer to do that, rather than pruning
    // this $addFields stage and requiring split SBE/Classic execution.
    if (containsEntirePipeline || stagesForPushdown.empty()) {
        return false;
    }

    auto projectionStage =
        dynamic_cast<DocumentSourceInternalProjection*>(stagesForPushdown.back().get());
    if (projectionStage &&
        projectionStage->projection().type() == projection_ast::ProjectType::kAddition) {
        stagesForPushdown.pop_back();
        return true;
    }
    return false;
}

/**
 * Prunes $unwind from 'stagesForPushdown' if it is the last stage. (Must be called repeatedly until
 * it returns false.) This pruning is done because $unwind performance is bottlenecked by processing
 * of EExpressions for sbe::ProjectStages in the SBE VM, which is slower than Classic's native C++
 * projection implementation. Pushing $unwind down only has a performance benefit when doing so
 * allows additional non-$unwind stages to be pushed down after it.
 *
 * Returns true iff it pruned a stage.
 */
bool pruneTrailingUnwind(std::vector<boost::intrusive_ptr<DocumentSource>>& stagesForPushdown) {
    if (!stagesForPushdown.empty() &&
        dynamic_cast<DocumentSourceUnwind*>(stagesForPushdown.back().get())) {
        stagesForPushdown.pop_back();
        return true;
    }
    return false;
}

/**
 * After copying as many pipeline stages as possible into the 'stagesForPushdown' pipeline, this
 * second pass takes off any stages that may not benefit from execution in SBE.
 */
void prunePushdownStages(std::vector<boost::intrusive_ptr<DocumentSource>>& stagesForPushdown,
                         SbeCompatibility minRequiredCompatibility,
                         bool allStagesPushedDown) {
    bool pruned = false;       // have any stages been pruned?
    bool prunedThisIteration;  // were any stages pruned in the current loop iteration?
    do {
        prunedThisIteration = false;
        if (SbeCompatibility::flagGuarded >= minRequiredCompatibility) {
            // When 'minRequiredCompatibility' is permissive enough (because featureFlagSbeFull is
            // enabled), do not remove trailing $addFields stages.
        } else {
            // Otherwise, remove trailing $addFields stages that we don't expect to improve
            // performance when they execute in SBE.
            if (pruneTrailingAddFields(stagesForPushdown, allStagesPushedDown && !pruned)) {
                prunedThisIteration = true;
                pruned = true;
            }
        }

        // $unwind should not be the last stage pushed down as it is more expensive in SBE.
        if (pruneTrailingUnwind(stagesForPushdown)) {
            prunedThisIteration = true;
            pruned = true;
        };
    } while (prunedThisIteration);
}

// Limit the number of aggregation pipeline stages that can be "pushed down" to the SBE stage
// builders. Compiling too many pipeline stages during stage building would overflow the call stack.
// The limit is higher for optimized builds, because optimization reduces the size of stack frames.
#ifdef MONGO_CONFIG_OPTIMIZED_BUILD
constexpr size_t kSbeMaxPipelineStages = 400;
#else
constexpr size_t kSbeMaxPipelineStages = 100;
#endif

/**
 * Finds a prefix of stages from the given pipeline to prepare for pushdown into the inner query
 * layer so that it can be executed using SBE. Populates 'stagesForPushdown' with the result and
 * returns true iff _all_ stages were included in pushdown.
 *
 * Unless pushdown is completely disabled by
 * {'internalQueryFrameworkControl': 'forceClassicEngine'}, a stage can be extracted from the
 * pipeline if and only if all the stages before it are extracted and it meets the criteria for its
 * stage type. When 'internalQueryFrameworkControl' is set to 'trySbeRestricted', only '$group',
 * '$lookup', '$_internalUnpackBucket', and search can be extracted. Criteria by stage type:
 *
 * $group via 'DocumentSourceGroup':
 *   - The 'internalQuerySlotBasedExecutionDisableGroupPushdown' knob is false and
 *   - the $group is not a merging operation that aggregates partial groups
 *     (DocumentSourceGroupBase::doingMerge()).
 *
 * $lookup via 'DocumentSourceLookUp':
 *   - The 'internalQuerySlotBasedExecutionDisableLookupPushdown' query knob is false,
 *   - the $lookup uses only the 'localField'/'foreignField' syntax (no pipelines), and
 *   - the foreign collection is neither sharded nor a view.
 *
 * $project via 'DocumentSourceInternalProjection':
 *   - No additional criteria.
 *
 * $addFields via 'DocumentSourceInternalProjection':
 *   - The stage that _follows_ the $addFields is also pushed down _or_
 *   - the 'featureFlagSbeFull' flag is enabled.
 *
 * $replaceRoot/$replaceWith via 'DocumentSourceSingleDocumentTransformation':
 *   - No additional criteria.
 *
 * $sort via 'DocumentSourceSort':
 *   - The sort operation does not produce sort key "meta" fields need by a later merging operation
 *     (i.e., 'needsMerge' is false).
 *
 * $match via 'DocumentSourceMatch':
 *   - No additional criteria.
 *
 * $limit via 'DocumentSourceLimit':
 *   - No additional criteria.
 *
 * $skip via 'DocumentSourceSkip':
 *   - No additional criteria.
 *
 * 'DocumentSourceUnpackBucket':
 *   - The 'featureFlagSbeFull' flag is enabled.
 *
 * 'DocumentSourceSearch':
 *   - The 'featureFlagSearchInSbe' flag is enabled.
 *
 * $_internalUnpackBucket via 'DocumentSourceInternalUnpackBucket':
 *   - The 'featureFlagTimeSeriesInSbe' flag is enabled and
 *   - the 'internalQuerySlotBasedExecutionDisableTimeSeriesPushdown', is _not_ enabled,
 */
bool findSbeCompatibleStagesForPushdown(
    const MultipleCollectionAccessor& collections,
    const CanonicalQuery* cq,
    bool needsMerge,
    const Pipeline* pipeline,
    std::vector<boost::intrusive_ptr<DocumentSource>>& stagesForPushdown) {
    // No pushdown if we're using the classic engine.
    if (cq->getForceClassicEngine()) {
        return false;
    }

    const auto& sources = pipeline->getSources();

    bool isMainCollectionSharded = false;
    if (const auto& mainColl = collections.getMainCollection()) {
        isMainCollectionSharded = mainColl.isSharded_DEPRECATED();
    }

    // SERVER-78998: Refactor these checks so that they do not load their values multiple times
    // during the same query.
    const bool sbeFullEnabled = feature_flags::gFeatureFlagSbeFull.isEnabled(
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot());

    SbeCompatibility minRequiredCompatibility =
        sbeFullEnabled ? SbeCompatibility::flagGuarded : SbeCompatibility::fullyCompatible;

    auto& queryKnob = QueryKnobConfiguration::decoration(cq->getExpCtxRaw()->opCtx);

    // We do a pushdown of all eligible stages when one of 3 conditions are met: the query knob is
    // set to 'trySbeEngine'; featureFlagSbeFull is enabled; the given query is a search query.
    const bool doFullPushdown =
        queryKnob.canPushDownFullyCompatibleStages() || sbeFullEnabled || cq->isSearchQuery();

    CompatiblePipelineStages allowedStages = {
        .group = !queryKnob.getSbeDisableGroupPushdownForOp(),

        // If lookup pushdown isn't enabled or the main collection is sharded or any of the
        // secondary namespaces are sharded or are a view, then no $lookup stage will be eligible
        // for pushdown.
        //
        // When acquiring locks for multiple collections, it is the case that we can only determine
        // whether any secondary collection is a view or is sharded, not which ones are a view or
        // are sharded and which ones aren't. As such, if any secondary collection is a view or is
        // sharded, no $lookup will be eligible for pushdown.
        .lookup = !queryKnob.getSbeDisableLookupPushdownForOp() && !isMainCollectionSharded &&
            !collections.isAnySecondaryNamespaceAViewOrSharded(),

        .transform =
            doFullPushdown && SbeCompatibility::fullyCompatible >= minRequiredCompatibility,

        .match = doFullPushdown && SbeCompatibility::fullyCompatible >= minRequiredCompatibility,

        // TODO (SERVER-80226): SBE execution of 'unwind' stages requires 'featureFlagSbeFull' to be
        // enabled.
        .unwind = doFullPushdown && SbeCompatibility::flagGuarded >= minRequiredCompatibility,

        // Note: even if its sort pattern is SBE compatible, we cannot push down a $sort stage when
        // the pipeline is the shard part of a sorted-merge query on a sharded collection. It is
        // possible that the merge operation will need a $sortKey field from the sort, and SBE plans
        // do not yet support metadata fields.
        .sort = doFullPushdown && SbeCompatibility::fullyCompatible >= minRequiredCompatibility &&
            !needsMerge,

        .limitSkip =
            doFullPushdown && SbeCompatibility::fullyCompatible >= minRequiredCompatibility,

        // TODO (SERVER-77229): SBE execution of $search requires 'featureFlagSearchInSbe' to be
        // enabled.
        .search = feature_flags::gFeatureFlagSearchInSbe.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()),

        .window = doFullPushdown && SbeCompatibility::fullyCompatible >= minRequiredCompatibility,

        // TODO (SERVER-80243): Remove 'featureFlagTimeSeriesInSbe' check.
        .unpackBucket = feature_flags::gFeatureFlagTimeSeriesInSbe.isEnabled(
                            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
            !queryKnob.getSbeDisableTimeSeriesForOp() &&
            cq->getExpCtx()->sbePipelineCompatibility == SbeCompatibility::fullyCompatible,
    };

    bool allStagesPushedDown = true;
    for (auto itr = sources.begin(); itr != sources.end(); ++itr) {
        // Push down at most kMaxPipelineStages stages for execution in SBE.
        if (stagesForPushdown.size() >= kSbeMaxPipelineStages) {
            break;
        }

        if (!pushDownPipelineStageIfCompatible(pipeline->getContext()->opCtx,
                                               *itr,
                                               minRequiredCompatibility,
                                               allowedStages,
                                               stagesForPushdown)) {
            // Stop pushing stages down once we hit an incompatible stage.
            allStagesPushedDown = false;
            break;
        }
    }

    // Remove stage patterns where pushing down may degrade performance.
    prunePushdownStages(stagesForPushdown, minRequiredCompatibility, allStagesPushedDown);

    return allStagesPushedDown;
}  // findSbeCompatibleStagesForPushdown
}  // namespace

void finalizePipelineStages(Pipeline* pipeline,
                            QueryMetadataBitSet unavailableMetadata,
                            CanonicalQuery* canonicalQuery) {
    if (!pipeline || pipeline->getSources().empty()) {
        return;
    }

    auto& sources = pipeline->getSources();
    size_t stagesToRemove = canonicalQuery->cqPipeline().size();
    tassert(7087104,
            "stagesToRemove must be <= number of pipeline sources",
            stagesToRemove <= sources.size());
    for (size_t i = 0; i < stagesToRemove; ++i) {
        sources.erase(sources.begin());
    }

    canonicalQuery->setRemainingSearchMetadata(
        pipeline->getDependencies(unavailableMetadata).searchMetadataDeps());
}

void attachPipelineStages(const MultipleCollectionAccessor& collections,
                          const Pipeline* pipeline,
                          bool needsMerge,
                          CanonicalQuery* canonicalQuery) {
    if (!pipeline || pipeline->getSources().empty()) {
        return;
    }

    std::vector<boost::intrusive_ptr<DocumentSource>> stagesForPushdown;
    bool allStagesPushedDown = findSbeCompatibleStagesForPushdown(
        collections, canonicalQuery, needsMerge, pipeline, stagesForPushdown);
    canonicalQuery->setCqPipeline(std::move(stagesForPushdown), allStagesPushedDown);
};

}  // namespace mongo
