/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/engine_selection.h"

#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/engine_selection_plan.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_utils.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {
/**
 * Returns true if 'collection' has an index that contains two fields, one of which is a path prefix
 * of the other, where the prefix field is hashed. Indexes can only contain one hashed field.
 *
 * TODO SERVER-99889: At the time of writing, there is a bug in the SBE stage builders that
 * constructs ExpressionFieldPaths over hashed values. This leads to wrong query results.
 *
 * The bug arises for covered index scans where a path P is a non-hashed path in the index and a
 * strict prefix P' of P is a hashed path in the index.
 */
bool collectionHasIndexWithHashedPathPrefixOfNonHashedPath(const CollectionPtr& collection,
                                                           ExpressionContext* expCtx) {
    const IndexCatalog* indexCatalog = collection->getIndexCatalog();
    tassert(10230200, "'CollectionPtr' does not have an 'IndexCatalog'", indexCatalog);
    OperationContext* opCtx = expCtx->getOperationContext();
    tassert(10230201, "'ExpressionContext' does not have an 'OperationContext'", opCtx);
    std::unique_ptr<IndexCatalog::IndexIterator> indexIter =
        indexCatalog->getIndexIterator(IndexCatalog::InclusionPolicy::kReady);
    while (indexIter->more()) {
        const IndexCatalogEntry* entry = indexIter->next();
        if (indexHasHashedPathPrefixOfNonHashedPath(entry->descriptor()->keyPattern())) {
            return true;
        }
    }
    return false;
}

/**
 * Checks if the given query can be executed with the SBE engine based on the canonical query.
 *
 * This method determines whether the query may be compatible with SBE based only on high-level
 * information from the canonical query, before query planning has taken place (such as ineligible
 * expressions or collections).
 *
 * If this method returns true, query planning should be done, followed by another layer of
 * validation to make sure the query plan can be executed with SBE. If it returns false, SBE query
 * planning can be short-circuited as it is already known that the query is ineligible for SBE.
 */
bool isQuerySbeCompatible(const CollectionPtr& collection,
                          const CanonicalQuery& cq,
                          const QuerySolution* solution) {
    auto expCtx = cq.getExpCtxRaw();

    // If we don't support all expressions used or the query is eligible for IDHack, don't use SBE.
    // Relies on the pre-computed isIdHackQuery() flag being up-to-date: callers must invoke
    // maybeUpgradeIdHackFlag() before calling isQuerySbeCompatible() so that filters which
    // normalize to a simple _id equality after MatchExpression parsing (e.g. {_id:{$in:[v]}})
    // are correctly reflected here.
    if (!expCtx || expCtx->getSbeCompatibility() == SbeCompatibility::notCompatible ||
        expCtx->getSbePipelineCompatibility() == SbeCompatibility::notCompatible ||
        (collection && expCtx->isIdHackQuery())) {
        return false;
    }

    const auto* proj = cq.getProj();
    if (proj && (proj->requiresMatchDetails() || proj->containsElemMatch())) {
        return false;
    }

    // Tailable and resumed scans are not supported either.
    if (expCtx->isTailable() || cq.getFindCommandRequest().getRequestResumeToken()) {
        return false;
    }

    const auto& nss = cq.nss();

    const auto isTimeseriesColl = collection && collection->isTimeseriesCollection();

    auto& queryKnob = cq.getExpCtx()->getQueryKnobConfiguration();
    if ((!feature_flags::gFeatureFlagTimeSeriesInSbe.isEnabled() ||
         queryKnob.getSbeDisableTimeSeriesForOp()) &&
        isTimeseriesColl) {
        return false;
    }

    // Queries against the oplog are not supported. Also queries on the inner side of a $lookup are
    // not considered for SBE except search queries.
    if ((expCtx->getInLookup() && !cq.isSearchQuery()) || nss.isOplog() ||
        !cq.metadataDeps().none()) {
        return false;
    }


    // Queries against collections with a particular shape of compound hashed indexes are not
    // supported.
    if (!feature_flags::gFeatureFlagGetExecutorDeferredEngineChoice.isEnabled() && collection &&
        collectionHasIndexWithHashedPathPrefixOfNonHashedPath(collection, expCtx)) {
        return false;
    }

    // Find and aggregate queries with the $_startAt parameter are not supported in SBE.
    if (!cq.getFindCommandRequest().getStartAt().isEmpty()) {
        return false;
    }

    const auto& sortPattern = cq.getSortPattern();
    if (sortPattern && !isSortSbeCompatible(*sortPattern)) {
        return false;
    }

    if (solution && !isPlanSbeEligible(solution)) {
        return false;
    }

    return true;
}

EngineSelectionResult shouldUseRegularSbeDeferredEngineSelection(
    OperationContext* opCtx,
    const CanonicalQuery& cq,
    const CollectionPtr& mainCollection,
    const bool sbeFull,
    const QuerySolution* solution,
    const std::function<void()>& extendSolutionWithPipelineFn) {
    if (mainCollection && mainCollection->isTimeseriesCollection()) {
        // TODO SERVER-120734 decide engine selection logic for TS collections.
        // TS queries only use SBE when there's a pipeline.
        return {cq.cqPipeline().empty() ? EngineChoice::kClassic : EngineChoice::kSbe, nullptr};
    }

    // Check for SBE compatability.
    const auto& queryKnob = cq.getExpCtx()->getQueryKnobConfiguration();
    SbeCompatibility minRequiredCompatibility =
        getMinRequiredSbeCompatibility(queryKnob.getInternalQueryFrameworkControlForOp(), sbeFull);
    if (cq.getExpCtx()->getSbeCompatibility() < minRequiredCompatibility) {
        return {EngineChoice::kClassic, nullptr};
    }

    // If `trySbeEngine` is set, we'll always use SBE when we can.
    if (queryKnob.getInternalQueryFrameworkControlForOp() ==
        QueryFrameworkControlEnum::kTrySbeEngine) {
        return {EngineChoice::kSbe, nullptr};
    }

    const QuerySolutionNode* dataAccessNode = solution->root();
    extendSolutionWithPipelineFn();
    return engineSelectionForPlan(solution, dataAccessNode);
}

/**
 * Function which returns true if 'cq' uses features that are currently supported in SBE without
 * 'featureFlagSbeFull' being set; false otherwise.
 */
EngineChoice shouldUseRegularSbe(OperationContext* opCtx,
                                 const CanonicalQuery& cq,
                                 const CollectionPtr& mainCollection,
                                 const bool sbeFull) {
    // When featureFlagSbeFull is not enabled, we cannot use SBE unless 'trySbeEngine' is enabled or
    // if 'trySbeRestricted' is enabled, and we have eligible pushed down stages in the cq pipeline.
    auto& queryKnob = cq.getExpCtx()->getQueryKnobConfiguration();
    if (!queryKnob.canPushDownFullyCompatibleStages() && cq.cqPipeline().empty()) {
        return EngineChoice::kClassic;
    }

    if (mainCollection && mainCollection->isTimeseriesCollection() && cq.cqPipeline().empty()) {
        // TS queries only use SBE when there's a pipeline.
        return EngineChoice::kClassic;
    }

    // Return true if all the expressions in the CanonicalQuery's filter and projection are SBE
    // compatible.
    SbeCompatibility minRequiredCompatibility =
        getMinRequiredSbeCompatibility(queryKnob.getInternalQueryFrameworkControlForOp(), sbeFull);
    if (cq.getExpCtx()->getSbeCompatibility() >= minRequiredCompatibility) {
        return EngineChoice::kSbe;
    }

    return EngineChoice::kClassic;
}
}  // namespace

EngineSelectionResult chooseEngine(OperationContext* opCtx,
                                   const MultipleCollectionAccessor& collections,
                                   CanonicalQuery* cq,
                                   const Pipeline* pipeline,
                                   bool needsMerge,
                                   std::unique_ptr<QueryPlannerParams> plannerParams,
                                   const QuerySolution* solution,
                                   const std::function<void()>& extendSolutionWithPipelineFn) {
    const bool hasSolution = solution != nullptr;
    const bool deferredEngineChoice =
        feature_flags::gFeatureFlagGetExecutorDeferredEngineChoice.isEnabled();
    tassert(11742301,
            "Expected to choose engine based on solution only if "
            "featureFlagGetExecutorDeferredEngineChoice is "
            "enabled.",
            hasSolution == deferredEngineChoice);

    const auto& mainColl = collections.getMainCollection();
    const bool forceClassic =
        cq->getExpCtx()->getQueryKnobConfiguration().isForceClassicEngineEnabled();
    if (forceClassic || !isQuerySbeCompatible(mainColl, *cq, solution)) {
        return {EngineChoice::kClassic, nullptr};
    }

    // Add the stages that are candidates for SBE lowering from the 'pipeline' into the
    // 'canonicalQuery'. This must be done _before_ checking shouldUseRegularSbe() or
    // creating the planner.
    attachPipelineStages(collections, pipeline, needsMerge, cq, std::move(plannerParams));

    const bool sbeFull = feature_flags::gFeatureFlagSbeFull.isEnabled();
    if (sbeFull) {
        return {EngineChoice::kSbe, nullptr};
    }
    return deferredEngineChoice
        ? shouldUseRegularSbeDeferredEngineSelection(
              opCtx, *cq, mainColl, sbeFull, solution, extendSolutionWithPipelineFn)
        : EngineSelectionResult{shouldUseRegularSbe(opCtx, *cq, mainColl, sbeFull), nullptr};
}

EngineChoice extendSolutionAndSelectEngine(std::unique_ptr<QuerySolution>& solution,
                                           OperationContext* opCtx,
                                           CanonicalQuery* cq,
                                           const Pipeline* pipeline,
                                           const MultipleCollectionAccessor& collections,
                                           QueryPlannerParams& plannerParams) {
    tassert(12152003,
            "QSN tree shouldn't be extended before engine selection",
            !solution->hasExtension());

    // We use CqPipeline as a staging area for SBE eligible stages, and some callers might call this
    // function twice (i.e. during replanning), so by clearing it we ensure we get a clean run every
    // time, which is a precondition for making this function idempotent.
    cq->clearCqPipeline();

    // If there is an eligible pipeline prefix to attach to the QSN, fills out the planner
    // params for secondary collections and attaches the stages to the QSN.
    auto extendSolutionWithPipelineFn = [&]() {
        if (cq->cqPipeline().empty()) {
            // Nothing to extend if the CQ pipeline is empty.
            return;
        }
        plannerParams.fillOutSecondaryCollectionsPlannerParams(opCtx, *cq, collections);
        solution = QueryPlanner::extendWithAggPipeline(*cq,
                                                       std::move(solution),
                                                       plannerParams.secondaryCollectionsInfo,
                                                       true /* skipOptimization */);
    };

    const auto [engine, planPushdownRoot] = chooseEngine(
        opCtx,
        collections,
        cq,
        pipeline,
        cq->getExpCtx()->getNeedsMerge(),
        std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForPushDownStagesDecision{
            .opCtx = opCtx,
            .canonicalQuery = *cq,
            .collections = collections,
            .plannerOptions = plannerParams.providedOptions,
        }),
        solution.get(),
        extendSolutionWithPipelineFn);

    if (engine == EngineChoice::kClassic && solution->hasExtension()) {
        // If classic was chosen and we extended the QSN to check for SBE eligibility, remove
        // the extension.
        solution->removePathFromExtension(solution->unextendedRootId());
    } else if (engine == EngineChoice::kSbe) {
        // If SBE is chosen, we might still need to call the extension function.
        if (!solution->hasExtension()) {
            extendSolutionWithPipelineFn();
        }

        // We could have to remove extension stages from the QSN because of the plan-based pushdown.
        // After that, we also remove the corresponding DocumentSources from the CQ pipeline, to
        // prevent them from being removed from the classic pipeline.
        //
        // For example, given the data access plan COLLSCAN for the pipeline ($group, $lookup,
        // $unwind), if at a given version engine selection forbids $lu queries from running with a
        // COLLSCAN plan, it'd return the GROUP stage (which implements the $group) as the plan
        // pushdown root.
        //
        // EQ_LOOKUP_UNWIND
        //        |
        //      GROUP (plan pushdown root)
        //        |
        //     COLLSCAN
        //
        // The EQ_LOOKUP_UNWIND would be removed from the QSN.
        //
        // The CQ pipeline is trimmed in lockstep with the QSN pruning. removeRootToNode returns the
        // number of removed extension nodes, and removeSuffixFromCqPipeline removes the same number
        // of DocumentSources from cqPipeline. In this example, CQ pipeline transitions from
        // [$group, $lookup (with absorbed $unwind)] to [$group].
        if (planPushdownRoot) {
            int removedCount = solution->removePathFromExtension(planPushdownRoot->nodeId());
            cq->removeSuffixFromCqPipeline(removedCount);
        }

        // There could be an additional optimization we may be missing.
        if (solution->hasExtension()) {
            solution =
                QueryPlannerAnalysis::removeInclusionProjectionBelowGroup(std::move(solution));
        }
    }
    return engine;
}

}  // namespace mongo
