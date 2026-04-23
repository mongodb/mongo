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

#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/join_reordering_context.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/util/modules.h"

#pragma once

namespace mongo::join_ordering {

/**
 * Constructor for sampling estimators per collection access.
 */
SamplingEstimatorMap makeSamplingEstimators(const MultipleCollectionAccessor& collections,
                                            const JoinGraph& model,
                                            PlanYieldPolicy::YieldPolicy yieldPolicy);

/**
 * Given a JoinGraph 'model' where each node links to a CanonicalQuery and a map of
 * 'SamplingEstimators' keyed by namespace, for each query this function invokes the plan
 * enumerator and uses cost-based ranking (CBR) with sampling-based cardinality estimation. It
 * returns a 'SingleTableAccessPlansResult' containing the winning QuerySolution for each query,
 * an 'EstimateMap' with cardinality and cost estimates for every QSN in the winning plans, and
 * per-NodeId summaries of each winning plan (root output cardinality and CBR CPU cost) plus the
 * catalog-reported cardinality of each base collection.
 */
StatusWith<SingleTableAccessPlansResult> singleTableAccessPlans(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    const JoinGraph& model,
    const SamplingEstimatorMap& samplingEstimators,
    bool isExplain);

}  // namespace mongo::join_ordering
