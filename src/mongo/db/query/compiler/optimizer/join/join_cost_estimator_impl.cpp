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

#include "mongo/db/query/compiler/optimizer/join/join_cost_estimator_impl.h"

namespace mongo::join_ordering {

JoinCostEstimatorImpl::JoinCostEstimatorImpl(const JoinReorderingContext& jCtx,
                                             JoinCardinalityEstimator& cardinalityEstimator,
                                             const CatalogStats& catalogStats)
    : _jCtx(jCtx), _cardinalityEstimator(cardinalityEstimator), _catalogStats(catalogStats) {}

JoinCostEstimate JoinCostEstimatorImpl::costCollScanFragment(NodeId nodeId) {
    // CollScan processes all documents in the collection
    CardinalityEstimate numDocsProcessed = _cardinalityEstimator.getCollCardinality(nodeId);
    // CollScan outputs documenst after applying single table predicates
    CardinalityEstimate numDocsOutput =
        _cardinalityEstimator.getOrEstimateSubsetCardinality(makeNodeSet(nodeId));

    auto& collStats = _catalogStats.collStats.at(_jCtx.joinGraph.getNode(nodeId).collectionName);
    // CollScan performs roughly sequential reads from disk as it is stored in a WT b-tree. We
    // estimate the number of disk read by estimating the number of pages the collscan will read.
    // This is done by dividing the data size by the page size.
    CardinalityEstimate numSeqIOs = CardinalityEstimate{
        CardinalityType{collStats.allocatedDataPageBytes / collStats.pageSizeBytes},
        EstimationSource::Metadata};
    // CollScan does no random read from disk.
    CardinalityEstimate numRandIOs = zeroCE;

    return JoinCostEstimate(numDocsProcessed, numDocsOutput, numSeqIOs, numRandIOs);
}

}  // namespace mongo::join_ordering
