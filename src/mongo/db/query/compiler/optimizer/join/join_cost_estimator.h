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

#include "mongo/db/query/compiler/optimizer/join/join_estimates.h"
#include "mongo/db/query/compiler/optimizer/join/join_plan.h"
#include "mongo/db/query/compiler/optimizer/join/logical_defs.h"

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
     */
    virtual JoinCostEstimate costCollScanFragment(NodeId) = 0;

    /**
     * Estimate the cost of a single table index scan plan fragment. This function assumes that the
     * given 'NodeId' corresponds to a node in the JoinGraph whose single table access path is an
     * index scan followed by a fetch; it is caller's responsibility to ensure this is the case.
     *
     * TODO SERVER-117506: Once we support projections and start producing covered plans, we will
     * need to modify this function.
     */
    virtual JoinCostEstimate costIndexScanFragment(NodeId) = 0;

    /**
     * Estimate the cost of a hash join plan fragment.
     */
    virtual JoinCostEstimate costHashJoinFragment(const JoinPlanNode& left,
                                                  const JoinPlanNode& right) = 0;

    /**
     * Estimate the cost of an indexed nested loop join plan fragment.
     */
    virtual JoinCostEstimate costINLJFragment(
        const JoinPlanNode& left,
        NodeId right,
        std::shared_ptr<const IndexCatalogEntry> indexProbe) = 0;

    /**
     * Estiamte the cost of a nested loop join plan fragment.
     */
    virtual JoinCostEstimate costNLJFragment(const JoinPlanNode& left,
                                             const JoinPlanNode& right) = 0;

    /**
     * Estimate the cost of a single table access path.
     */
    virtual JoinCostEstimate costBaseCollectionAccess(NodeId nodeId) = 0;
};

}  // namespace mongo::join_ordering
