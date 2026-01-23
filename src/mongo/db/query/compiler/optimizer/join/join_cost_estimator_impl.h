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

#pragma once

#include "mongo/db/query/compiler/optimizer/join/cardinality_estimator.h"
#include "mongo/db/query/compiler/optimizer/join/catalog_stats.h"
#include "mongo/db/query/compiler/optimizer/join/join_cost_estimator.h"
#include "mongo/db/query/compiler/optimizer/join/join_estimates.h"
#include "mongo/db/query/compiler/optimizer/join/join_plan.h"

namespace mongo::join_ordering {

class JoinCostEstimatorImpl : public JoinCostEstimator {
public:
    JoinCostEstimatorImpl(const JoinReorderingContext& jCtx,
                          JoinCardinalityEstimator& cardinalityEstimator,
                          const CatalogStats& catalogStats);

    // Delete copy and move operations to prevent issues with copying reference members.
    JoinCostEstimatorImpl(const JoinCostEstimatorImpl&) = delete;
    JoinCostEstimatorImpl& operator=(const JoinCostEstimatorImpl&) = delete;
    JoinCostEstimatorImpl(JoinCostEstimatorImpl&&) = delete;
    JoinCostEstimatorImpl& operator=(JoinCostEstimatorImpl&&) = delete;

    JoinCostEstimate costCollScanFragment(NodeId nodeId) override;
    JoinCostEstimate costIndexScanFragment(NodeId nodeId) override;
    JoinCostEstimate costHashJoinFragment(const JoinPlanNode& left,
                                          const JoinPlanNode& right) override;
    JoinCostEstimate costINLJFragment(const JoinPlanNode& left,
                                      NodeId right,
                                      std::shared_ptr<const IndexCatalogEntry> indexProbe) override;

private:
    double estimateDocSize(NodeSet subset) const;

    const JoinReorderingContext& _jCtx;
    JoinCardinalityEstimator& _cardinalityEstimator;
    const CatalogStats& _catalogStats;
};

}  // namespace mongo::join_ordering
