// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/join_reordering_context.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/util/modules.h"

#pragma once

namespace mongo::join_ordering {

/**
 * Constructor for sampling estimators per collection access. 'joinExpCtx' carries non-array path
 * learnings for all fields checked during join optimization, enabling the PathArraynessChecker to
 * detect arrayness changes during sampling yields.
 */
SamplingEstimatorMap makeSamplingEstimators(
    const MultipleCollectionAccessor& collections,
    const JoinGraph& model,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const boost::intrusive_ptr<ExpressionContext>& joinExpCtx);

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
    const SamplingEstimatorMap& samplingEstimators);

}  // namespace mongo::join_ordering
