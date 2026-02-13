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

#include "mongo/db/query/compiler/optimizer/join/single_table_access.h"

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"
#include "mongo/db/query/query_planner.h"

#include <fmt/format.h>

namespace mongo::join_ordering {

SamplingEstimatorMap makeSamplingEstimators(const MultipleCollectionAccessor& collections,
                                            const JoinGraph& graph,
                                            PlanYieldPolicy::YieldPolicy yieldPolicy) {
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
                    qkc.getInternalQuerySamplingCEMethod(),
                    qkc.getNumChunksForChunkBasedSampling(),
                    ce::CardinalityEstimate{numRecords,
                                            cost_based_ranker::EstimationSource::Metadata});

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

StatusWith<SingleTableAccessPlansResult> singleTableAccessPlans(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    const JoinGraph& graph,
    const SamplingEstimatorMap& samplingEstimators) {
    QuerySolutionMap solns;
    cost_based_ranker::EstimateMap estimates;

    const auto numNodes = graph.numNodes();
    for (size_t i = 0; i < numNodes; i++) {
        const auto& node = graph.getNode(i);
        auto& nss = node.accessPath->nss();

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

        QueryPlannerParams params(QueryPlannerParams::ArgsForSingleCollectionQuery{
            .opCtx = opCtx,
            .canonicalQuery = *node.accessPath,
            .collections = singleMca,
            .plannerOptions = options,
            .cbrEnabled = true,
            .planRankerMode = QueryPlanRankerModeEnum::kSamplingCE,
        });

        auto swSolns = QueryPlanner::plan(*node.accessPath, params);
        if (!swSolns.isOK()) {
            return swSolns.getStatus();
        }
        auto swCbrResult = QueryPlanner::planWithCostBasedRanking(*node.accessPath,
                                                                  params,
                                                                  samplingEstimators.at(nss).get(),
                                                                  nullptr /*exactCardinality*/,
                                                                  std::move(swSolns.getValue()));
        // Return bad status if CBR is unable to produce a plan
        if (!swCbrResult.isOK()) {
            return swCbrResult.getStatus();
        }
        auto& cbrResult = swCbrResult.getValue();
        if (cbrResult.solutions.size() != 1) {
            return Status(
                ErrorCodes::NoQueryExecutionPlans,
                fmt::format("CBR failed to find best plan for nss: {}", nss.toStringForErrorMsg()));
        }
        // Save solution and corresponding estimates for the best plan
        solns[node.accessPath.get()] = std::move(cbrResult.solutions.front());
        tassert(11540201,
                "Expected to have estimation data for single table access plan",
                cbrResult.maybeExplainData.has_value());
        for (auto& [k, v] : cbrResult.maybeExplainData->estimates) {
            // Take care to use 'insert_or_assign' which will override existing entries in
            // estimates. It is possible that a QSN for a rejected plan of a previous table which
            // has been destroyed contains an entry in this map. The allocator may reuse the same
            // address for a QSN for the current table. In that case, we want to override the value
            // in the map.
            estimates.insert_or_assign(k, std::move(v));
        }
    }

    return SingleTableAccessPlansResult{
        .solns = std::move(solns),
        .estimate = std::move(estimates),
    };
}

}  // namespace mongo::join_ordering
