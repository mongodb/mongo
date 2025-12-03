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

#include "mongo/db/query/compiler/ce/sampling/sampling_estimator.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates_storage.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/solution_storage.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/util/modules.h"

namespace mongo::join_ordering {

using SamplingEstimatorMap =
    stdx::unordered_map<NamespaceString, std::unique_ptr<ce::SamplingEstimator>>;

/**
 * Struct containing results from 'singleTableAccessPlans()' function.
 */
struct SingleTableAccessPlansResult {
    QuerySolutionMap solns;
    cost_based_ranker::EstimateMap estimate;
};

/**
 * Constructor for sampling estimators per collection access.
 */
SamplingEstimatorMap makeSamplingEstimators(const MultipleCollectionAccessor& collections,
                                            const JoinGraph& model,
                                            PlanYieldPolicy::YieldPolicy yieldPolicy);

/**
 * Given a JoinGraph 'model' where each node links to a CanonicalQuery and a map of
 * 'SamplingEstimators' keyed by namespace, for each query, this function invokes the plan
 * enumerator and uses cost-based ranking (CBR) with sampling-based cardinality estimation. This
 * function returns a QuerySolution representing the best plan for each query along with an
 * 'EstimateMap' which contains cardinality and cost estimates for every QSN.
 */
StatusWith<SingleTableAccessPlansResult> singleTableAccessPlans(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    const JoinGraph& model,
    const SamplingEstimatorMap& samplingEstimators);

}  // namespace mongo::join_ordering
