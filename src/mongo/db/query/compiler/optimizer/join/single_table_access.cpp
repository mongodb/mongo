// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/single_table_access.h"

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cardinality_estimator.h"
#include "mongo/db/query/query_planner.h"

#include <fmt/format.h>

namespace mongo::join_ordering {

SamplingEstimatorMap makeSamplingEstimators(
    const MultipleCollectionAccessor& collections,
    const JoinGraph& graph,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const boost::intrusive_ptr<ExpressionContext>& joinExpCtx) {
    const auto numNodes = graph.numNodes();

    SamplingEstimatorMap samplingEstimators;
    samplingEstimators.reserve(numNodes);

    for (size_t i = 0; i < numNodes; i++) {
        const auto& node = graph.getNode(i);
        const auto& nss = node.accessPath->nss();
        if (samplingEstimators.find(nss) == samplingEstimators.end()) {
            cost_based_ranker::CardinalityType numRecords{static_cast<double>(
                collections.lookupCollection(nss)->getRecordStore()->numRecords())};

            const auto& cq = node.accessPath;
            const auto& qkc = cq->getExpCtx()->getQueryKnobConfiguration();
            std::unique_ptr<ce::SamplingEstimator> estimator =
                std::make_unique<ce::SamplingEstimatorImpl>(
                    cq->getOpCtx(),
                    collections,
                    cq->nss(),
                    yieldPolicy,
                    qkc.getInternalJoinPlanSamplingSize(),
                    qkc.getInternalJoinOptimizationSamplingCEMethod(),
                    qkc.getNumChunksForChunkBasedSampling(),
                    ce::CardinalityEstimate{numRecords,
                                            cost_based_ranker::EstimationSource::Metadata},
                    joinExpCtx,
                    SamplingSourceEnum::kPersistentSample,
                    qkc.getInternalJoinOptimizationSamplingCEMethod());

            // Generate a sample for the fields relevant to this join.
            // TODO SERVER-112233: figure out based on join predicates which fields exactly we need.
            estimator->generateSample(ce::ProjectionParams{ce::NoProjection{}});
            samplingEstimators.emplace(nss, std::move(estimator));

        } else {
            continue;
        }
    }
    return samplingEstimators;
}

// For cardinality estimation, we ignore the selectivies of derived predicates and thus just
// estimate the selectivity of the filter provided in the original user query.
StatusWith<cost_based_ranker::CardinalityEstimate> cardinalityEstimateOnOriginalFilter(
    OperationContext* opCtx,
    const JoinNode& node,
    const MultipleCollectionAccessor& singleMca,
    size_t plannerOptions,
    const ce::SamplingEstimator* samplingEstimator,
    const cost_based_ranker::CardinalityEstimate& collCard) {
    if (collCard.toDouble() == 0.0) {
        // Sampling-based selectivity estimation divides by the collection cardinality, which
        // is undefined for an empty collection. An empty collection trivially has 0 matching
        // documents regardless of the filter.
        return cost_based_ranker::zeroMetadataCE;
    }

    QueryPlannerParams ceParams(QueryPlannerParams::ArgsForSingleCollectionQuery{
        .opCtx = opCtx,
        .canonicalQuery = *node.originalFilter,
        .collections = singleMca,
        .plannerOptions = plannerOptions,
        .planRanker = QueryPlanRankerEnum::kCostBased,
    });

    cost_based_ranker::EstimateMap ceEstimates;
    cost_based_ranker::CardinalityEstimator ce{
        ceParams.mainCollectionInfo,
        samplingEstimator,
        ceEstimates,
        QueryCBRCEModeEnum::kSamplingCE,
    };

    const MatchExpression* filter = node.originalFilter->getPrimaryMatchExpression();
    return ce.estimateFilter(filter);
}

// Holds the winning single-table plan for a filter that includes derived predicates, along with its
// CBR cost and the estimates for every QSN considered while ranking it.
struct SingleTablePlanResult {
    std::unique_ptr<QuerySolution> solution;
    cost_based_ranker::CostEstimate cbrCost;
    cost_based_ranker::EstimateMap estimates;
};

// Plans and cost-based-ranks the access path with derived predicates, returning the
// single winning solution together with its cost and estimates.
StatusWith<SingleTablePlanResult> accessPlanForFilterWithDerivedPredicates(
    OperationContext* opCtx,
    const JoinNode& node,
    const MultipleCollectionAccessor& singleMca,
    size_t plannerOptions,
    ce::SamplingEstimator* samplingEstimator) {
    QueryPlannerParams querySolutionParams(QueryPlannerParams::ArgsForSingleCollectionQuery{
        .opCtx = opCtx,
        .canonicalQuery = *node.accessPath,
        .collections = singleMca,
        .plannerOptions = plannerOptions,
        .planRanker = QueryPlanRankerEnum::kCostBased,
    });

    auto swSolns = QueryPlanner::plan(*node.accessPath, querySolutionParams);
    if (!swSolns.isOK()) {
        return swSolns.getStatus();
    }
    auto swCbrResult = QueryPlanner::planWithCostBasedRanking(querySolutionParams,
                                                              samplingEstimator,
                                                              nullptr /*exactCardinality*/,
                                                              std::move(swSolns.getValue()),
                                                              *node.accessPath,
                                                              QueryCBRCEModeEnum::kSamplingCE);
    // Return bad status if CBR is unable to produce a plan
    if (!swCbrResult.isOK()) {
        return swCbrResult.getStatus();
    }
    auto& cbrResult = swCbrResult.getValue();
    if (cbrResult.solutions.size() != 1) {
        return Status(ErrorCodes::NoQueryExecutionPlans,
                      fmt::format("CBR failed to find best plan for nss: {}",
                                  node.accessPath->nss().toStringForErrorMsg()));
    }
    tassert(11540201,
            "Expected to have estimation data for single table access plan",
            cbrResult.maybeExplainData.has_value());

    auto& winningSolution = cbrResult.solutions.front();
    const auto* rootQsn = winningSolution->root();

    auto rootEstIt = cbrResult.maybeExplainData->estimates.find(rootQsn);
    tassert(11514601,
            "Missing estimate for winning single-table plan's root QSN",
            rootEstIt != cbrResult.maybeExplainData->estimates.end());

    return SingleTablePlanResult{
        .solution = std::move(winningSolution),
        .cbrCost = rootEstIt->second->cost,
        .estimates = std::move(cbrResult.maybeExplainData->estimates),
    };
}

StatusWith<SingleTableAccessPlansResult> singleTableAccessPlans(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    const JoinGraph& graph,
    const SamplingEstimatorMap& samplingEstimators) {
    const auto numNodes = graph.numNodes();
    QuerySolutionMap solns;
    cost_based_ranker::EstimateMap estimates;

    NodeCardinalities nodeCardinalitiesOriginalFilter;
    NodeCardinalities collCardinalities;
    NodeCBRCosts nodeCBRCosts;

    nodeCardinalitiesOriginalFilter.reserve(numNodes);
    collCardinalities.reserve(numNodes);
    nodeCBRCosts.reserve(numNodes);

    for (size_t i = 0; i < numNodes; i++) {
        const auto& node = graph.getNode(i);
        auto& nss = node.accessPath->nss();

        const auto& samplingEstimator = samplingEstimators.at(nss);
        collCardinalities.push_back(
            cost_based_ranker::CardinalityEstimate{samplingEstimator->getCollCard().cardinality(),
                                                   cost_based_ranker::EstimationSource::Metadata});

        // Re-construct MultipleCollectionAccessor so that this collection is treated as the "main"
        // collection during query planning (and CE).
        auto singleAcq = [&nss, &collections]() -> CollectionOrViewAcquisition {
            if (nss == collections.getMainCollectionPtrOrAcquisition().nss()) {
                return collections.getMainCollectionPtrOrAcquisition();
            }

            const auto& secondaries = collections.getSecondaryCollectionAcquisitions();
            auto it = secondaries.find(nss);
            tassert(11434000, "Namespace not found in collections", it != secondaries.end());
            return it->second;
        }();
        MultipleCollectionAccessor singleMca{singleAcq};

        const auto& qkc = node.accessPath->getExpCtx()->getQueryKnobConfiguration();
        size_t options = QueryPlannerParams::DEFAULT;
        // Note this is a different default from the classic 'find()' codepath. This is done to
        // prevent the optimizer from choosing unselective index scans which perform a lot of random
        // IO when a collection scan is better. The impact of this can be multiplied in join plans
        // as the inner side may be executed multiple times.
        // TODO SERVER-13065: Update this comment once 'find()' consider collection scans in the
        // presence of indexes.
        if (qkc.getInternalJoinEnumerateCollScanPlans()) {
            options |= QueryPlannerParams::INCLUDE_COLLSCAN;
        }

        auto nodeCardinalityEstimateOrigFilterRes = cardinalityEstimateOnOriginalFilter(
            opCtx, node, singleMca, options, samplingEstimator.get(), collCardinalities.back());

        if (!nodeCardinalityEstimateOrigFilterRes.isOK()) {
            return nodeCardinalityEstimateOrigFilterRes.getStatus();
        }
        nodeCardinalitiesOriginalFilter.push_back(nodeCardinalityEstimateOrigFilterRes.getValue());


        auto swPlan = accessPlanForFilterWithDerivedPredicates(
            opCtx, node, singleMca, options, samplingEstimator.get());
        if (!swPlan.isOK()) {
            return swPlan.getStatus();
        }
        auto& plan = swPlan.getValue();

        // Save access plan solution and corresponding CBR estimates for the best plan
        solns[node.accessPath.get()] = std::move(plan.solution);
        nodeCBRCosts.push_back(plan.cbrCost);

        for (auto& [k, v] : plan.estimates) {
            // Take care to use 'insert_or_assign' which will override existing entries in
            // estimates. It is possible that a QSN for a rejected plan of a previous table which
            // has been destroyed contains an entry in this map. The allocator may reuse the same
            // address for a QSN for the current table. In that case, we want to override the value
            // in the map.
            estimates.insert_or_assign(k, std::move(v));
        }
    }

    return SingleTableAccessPlansResult{
        .cbrCqQsns = std::move(solns),
        .estimate = std::move(estimates),
        .nodeCardinalitiesOriginalFilter = std::move(nodeCardinalitiesOriginalFilter),
        .collCardinalities = std::move(collCardinalities),
        .nodeCBRCosts = std::move(nodeCBRCosts),
    };
}

}  // namespace mongo::join_ordering
