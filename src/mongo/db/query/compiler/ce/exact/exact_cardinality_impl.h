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

#pragma once
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/query/compiler/ce/ce_common.h"
#include "mongo/db/query/compiler/ce/exact/exact_cardinality.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates_storage.h"
namespace mongo::ce {

using CardinalityEstimate = mongo::cost_based_ranker::CardinalityEstimate;
using CEResult = StatusWith<CardinalityEstimate>;

/**
 * This class implements bottom-up exact cardinality calculation of QuerySolutionNode plans
 * that consist of QSN nodes, MatchExpression filter nodes, and Intervals. This is meant for use
 * with the "exactCE" mode.
 *
 * The calculation is done by executing the query plan and then using the execution stats to
 * populate the estimates map.
 */
class ExactCardinalityImpl : public ExactCardinalityEstimator {
public:
    ExactCardinalityImpl(const CollectionPtr& collection,
                         const CanonicalQuery& query,
                         OperationContext* opCtx)
        : _cq(query), _opCtx(opCtx), _coll(&collection) {}

    ExactCardinalityImpl(const ExactCardinalityImpl&) = delete;
    ExactCardinalityImpl(ExactCardinalityImpl&&) = delete;
    ExactCardinalityImpl& operator=(const ExactCardinalityImpl&) = delete;
    ExactCardinalityImpl& operator=(ExactCardinalityImpl&&) = delete;

    ~ExactCardinalityImpl() override {};
    /**
     * Entry point for the exact cardinality calculation, performs it for the given plan.
     */
    CEResult calculateExactCardinality(
        const QuerySolution& plan, cost_based_ranker::EstimateMap& cardinalities) const override;

private:
    /**
     * Helper method to populate the cardinalities map given the execution stats.
     * We also pass in the QSN as these are the keys in the estimates map.
     */
    CEResult populateCardinalities(const QuerySolutionNode* node,
                                   const PlanStage* execStage,
                                   cost_based_ranker::EstimateMap& cardinalities) const;
    const CanonicalQuery& _cq;
    OperationContext* _opCtx;
    const CollectionPtr* _coll;
};
}  // namespace mongo::ce
