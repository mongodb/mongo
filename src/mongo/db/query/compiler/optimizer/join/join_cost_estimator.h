// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/optimizer/join/join_estimates.h"
#include "mongo/db/query/compiler/optimizer/join/join_plan.h"
#include "mongo/db/query/compiler/optimizer/join/logical_defs.h"
#include "mongo/util/modules.h"

namespace mongo::join_ordering {

/**
 * Pure virtual interface representing the cost estimator for join plans. This is implemented as a
 * separate module from the CBR cost estimator because the callers has different structures. Whereas
 * the CBR cost estimator has `QuerySolutionNode` trees as input, the join plan enumeration does not
 * enumerate QSN trees for efficiency and instead works over internal structures (see 'join_plan.h'
 * and 'plan_enumerator.h' for details).
 */
class JoinCostEstimator {
public:
    virtual ~JoinCostEstimator() {}

    /**
     * Estimate the cost of a single table collection scan plan fragment. This function assumes that
     * the given 'NodeId' corresponds to a node in the JoinGraph whose single table access path is a
     * collection scan; it is the caller's responsibility to ensure this is the case.
     *
     * The given cost is the CPU cost of the single-table plan from CBR; IO cost is modeled by the
     * fragment method itself.
     */
    virtual JoinCostEstimate costCollScanFragment(NodeId id, CostEstimate singleTableCpuCost) = 0;

    /**
     * Estimate the cost of a single table index scan plan fragment. This function assumes that the
     * given 'NodeId' corresponds to a node in the JoinGraph whose single table access path is an
     * index scan followed by a fetch; it is caller's responsibility to ensure this is the case.
     *
     * The given cost is the CPU cost of the single-table plan from CBR; IO cost is modeled by the
     * fragment method itself.
     *
     * TODO SERVER-117506: Once we support projections and start producing covered plans, we will
     * need to modify this function.
     */
    virtual JoinCostEstimate costIndexScanFragment(NodeId id, CostEstimate singleTableCpuCost) = 0;

    /**
     * Estimate the cost of a hash join plan fragment.
     */
    virtual JoinCostEstimate costHashJoinFragment(const JoinPlanNode& left,
                                                  const JoinPlanNode& right) = 0;

    /**
     * Estimate the cost of an indexed nested loop join plan fragment.
     */
    virtual JoinCostEstimate costINLJFragment(const JoinPlanNode& left,
                                              NodeId right,
                                              std::shared_ptr<const IndexCatalogEntry> indexProbe,
                                              EdgeId edgeId) = 0;

    /**
     * Estimate the cost of a nested loop join plan fragment.
     */
    virtual JoinCostEstimate costNLJFragment(const JoinPlanNode& left,
                                             const JoinPlanNode& right) = 0;

    /**
     * Estimate the cost of a single table access path.
     */
    virtual JoinCostEstimate costBaseCollectionAccess(NodeId nodeId) = 0;
};

}  // namespace mongo::join_ordering
