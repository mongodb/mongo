// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

    JoinCostEstimate costCollScanFragment(NodeId nodeId, CostEstimate singleTableCpuCost) override;
    JoinCostEstimate costIndexScanFragment(NodeId nodeId, CostEstimate singleTableCpuCost) override;
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

/**
 * Result of 'estimateSortedSparseIO', describing the additional I/Os to charge.
 */
struct SortedSparseIO {
    double numSeqIOs;
    double numRandIOs;
};

/**
 * Models the I/O cost of the page accesses within each RID-ordered fetch group that follow the
 * first random seek. A "group" is one index probe for INLJ or one distinct key for an IXSCAN.
 * Within each group, the index returns RIDs in sorted order. Fetching documents by sorted RID is a
 * "sorted-sparse" scan over the collection — not fully sequential, but not fully random either. The
 * extra page accesses beyond the first random seek per probe are the sorted-sparse I/Os.
 *
 * 'numPagesAccessedColl' is the estimated number of distinct collection pages touched.
 * 'numLogicalPageRequests' is the number of RID-ordered groups.
 * 'mlCase' is the Mackert-Lohman formula branch taken for the same inputs.
 * 'numPagesInStorageEngineCache' is the number of collection pages that can fit in the WT cache.
 */
SortedSparseIO estimateSortedSparseIO(double numPagesAccessedColl,
                                      double numLogicalPageRequests,
                                      MackertLohmanCase mlCase,
                                      double numPagesInStorageEngineCache);

}  // namespace mongo::join_ordering
