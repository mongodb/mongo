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

#include "mongo/db/query/query_planner.h"

#include <fmt/format.h>

namespace mongo::optimizer {

StatusWith<SingleTableAccessPlansResult> singleTableAccessPlans(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    const std::vector<std::unique_ptr<CanonicalQuery>>& queries,
    const SamplingEstimatorMap& samplingEstimators) {
    QuerySolutionMap solns;
    cost_based_ranker::EstimateMap estimates;

    for (auto&& query : queries) {
        QueryPlannerParams params(QueryPlannerParams::ArgsForSingleCollectionQuery{
            .opCtx = opCtx,
            .canonicalQuery = *query,
            .collections = collections,
            .planRankerMode = QueryPlanRankerModeEnum::kSamplingCE,
        });
        auto& nss = query->nss();
        auto swSolns = QueryPlanner::plan(*query, params);
        if (!swSolns.isOK()) {
            return swSolns.getStatus();
        }
        auto swCbrResult = QueryPlanner::planWithCostBasedRanking(*query,
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
        solns[query.get()] = std::move(cbrResult.solutions.front());
        estimates.insert(cbrResult.estimates.begin(), cbrResult.estimates.end());
    }

    return SingleTableAccessPlansResult{
        .solns = std::move(solns),
        .estimate = std::move(estimates),
    };
}

}  // namespace mongo::optimizer
