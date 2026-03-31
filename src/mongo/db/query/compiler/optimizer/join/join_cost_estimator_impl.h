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
#include "mongo/db/query/compiler/optimizer/join/join_cost_estimator.h"
#include "mongo/db/query/compiler/optimizer/join/join_estimates.h"
#include "mongo/db/query/compiler/optimizer/join/join_plan.h"
#include "mongo/util/modules.h"

namespace mongo::join_ordering {

class JoinCostEstimatorImpl final : public JoinCostEstimator {
public:
    JoinCostEstimatorImpl(const JoinReorderingContext& jCtx,
                          JoinCardinalityEstimator& cardinalityEstimator);

    // Delete copy and move operations to prevent issues with copying reference members.
    JoinCostEstimatorImpl(const JoinCostEstimatorImpl&) = delete;
    JoinCostEstimatorImpl& operator=(const JoinCostEstimatorImpl&) = delete;
    JoinCostEstimatorImpl(JoinCostEstimatorImpl&&) = delete;
    JoinCostEstimatorImpl& operator=(JoinCostEstimatorImpl&&) = delete;

    JoinCostEstimate costCollScanFragment(NodeId nodeId) override;
    JoinCostEstimate costIndexScanFragment(NodeId nodeId) override;
    JoinCostEstimate costBaseCollectionAccess(NodeId nodeId) override;
    JoinCostEstimate costHashJoinFragment(const JoinPlanNode& left,
                                          const JoinPlanNode& right) override;
    JoinCostEstimate costINLJFragment(const JoinPlanNode& left,
                                      NodeId right,
                                      std::shared_ptr<const IndexCatalogEntry> indexProbe,
                                      EdgeId edgeId) override;
    JoinCostEstimate costNLJFragment(const JoinPlanNode& left, const JoinPlanNode& right) override;

private:
    double estimateDocSize(NodeSet subset) const;

    const JoinReorderingContext& _jCtx;
    JoinCardinalityEstimator& _cardinalityEstimator;
};

/**
 * Estimate the number of distinct pages accessed when fetching 'numEntriesRequested' records/keys
 * from a b-tree with 'numLeafPages' leaf pages. Assumes that each entry requested from the b-tree
 * is uniformly distributed among the 'numLeafPages' pages.
 * The formula used is presented in "Approximating Block AccessesIn Database Organizations" by Yao
 * in 1977 (https://dl.acm.org/doi/epdf/10.1145/359461.359475).
 */
double estimateYaoDistinctPages(double numLeafPages, double numEntriesRequested);

/*
 * Return type for estimateMackertLohmanRandIO, bundling the estimated page count with the formula
 * branch that was taken—useful for explain output.
 */
struct MackertLohmanResult {
    double randIOPages;
    MackertLohmanCase theCase;
};

/**
 * Estimates the number of random disk I/Os required to read leaf pages from a b-tree, accounting
 * for a finite LRU buffer (the WiredTiger cache). When the number of requested pages exceeds the
 * cache capacity, previously loaded pages may be evicted and subsequently re-read from disk, so the
 * estimated I/O count can exceed the number of distinct pages accessed.
 *
 * Implements the Y_wap formula from:
 *   "Index Scans Using a Finite LRU Buffer", Mackert & Lohman, VLDB 1989
 *   https://dl.acm.org/doi/epdf/10.1145/68012.68016
 *
 * Used to estimate the random I/O cost for:
 *   1. Fetching documents from the collection during an index scan (IXSCAN + FETCH)
 *   2. Fetching probe-side documents in an INLJ plan fragment
 *   3. Accessing the probe-side index in an INLJ plan fragment
 *
 * Assumptions:
 *   - Pages are accessed uniformly at random (totally unclustered index).
 *   - The cache uses LRU eviction.
 *   - Each output document resides on exactly one leaf page (one doc = one page request).
 *   - Only leaf-page I/Os are modeled; internal b-tree node traversals are ignored.
 *   - The entire WT cache is available for this b-tree (no contention from concurrent operations,
 *     other collections, or other indexes sharing the cache).
 *   - All leaf pages are the same size.
 *   - No prefetching or read-ahead by the storage engine.
 *
 * 'numDistinctPagesNeededFromBtree' is the number of distinct leaf pages of the b-tree that the
 * plan fragment will access. See 'estimateYaoDistinctPages' for details of how this is estimated.
 * 'numPagesInStorageEngineCache' is the number of pages of the b-tree that can fit in the WT cache.
 * 'numLogicalPageRequests' is the number of leaf page requests this plan fragment will make to the
 * storage engine.
 *
 * Returns the estimated number of physical random disk reads.
 */
MackertLohmanResult estimateMackertLohmanRandIO(double numDistinctPagesNeededFromBtree,
                                                double numPagesInStorageEngineCache,
                                                double numLogicalPageRequests);

}  // namespace mongo::join_ordering
