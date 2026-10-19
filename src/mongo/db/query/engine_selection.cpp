// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/engine_selection.h"

#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/pipeline/sbe_pushdown_util.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/engine_selection_plan.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_utils.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {
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
    if (!expCtx || expCtx->getSbeCompatibility() == SbeCompatibility::notCompatible ||
        expCtx->getSbePipelineCompatibility() == SbeCompatibility::notCompatible ||
        (collection && isIdHackEligibleQuery(collection, cq))) {
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

    // Find and aggregate queries with the $_startAt parameter are not supported in SBE.
    if (!cq.getFindCommandRequest().getStartAt().isEmpty()) {
        return false;
    }

    const auto& sortPattern = cq.getSortPattern();
    if (sortPattern && !isSortSbeCompatible(*sortPattern)) {
        return false;
    }

    if (solution && !isPlanSbeCompatible(solution)) {
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
    return engineSelectionForPlan(solution, dataAccessNode, *cq.getExpCtx()->getIfrContext());
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
    tassert(11742301, "Expected to choose engine based on the winning solution.", hasSolution);

    const auto& mainColl = collections.getMainCollection();
    const bool forceClassic =
        cq->getExpCtx()->getQueryKnobConfiguration().isForceClassicEngineEnabled();
    if (forceClassic || !isQuerySbeCompatible(mainColl, *cq, solution)) {
        return {EngineChoice::kClassic, nullptr};
    }

    // Add the stages that are candidates for SBE lowering from the 'pipeline' into the
    // 'canonicalQuery'. This must be done _before_ engine selection.
    attachPipelineStages(collections, pipeline, needsMerge, cq, std::move(plannerParams));

    const bool sbeFull = feature_flags::gFeatureFlagSbeFull.isEnabled();
    if (sbeFull) {
        return {EngineChoice::kSbe, nullptr};
    }
    return shouldUseRegularSbeDeferredEngineSelection(
        opCtx, *cq, mainColl, sbeFull, solution, extendSolutionWithPipelineFn);
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
                                                       /*skipProjectGroupOptimization=*/true);
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

        // Increment non-leading pushdown stage counters.
        incrementNonLeadingPushdownCounters(*cq);
    }
    return engine;
}

}  // namespace mongo
