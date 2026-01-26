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

JoinCostEstimate JoinCostEstimatorImpl::costIndexScanFragment(NodeId nodeId) {
    // For simplicity we assume there are no non-sargable filters applied after the index scan. This
    // means that we assume the number of documents processed and output are both equal to the
    // cardinality estimate of that node.
    CardinalityEstimate numDocsProcessed =
        _cardinalityEstimator.getOrEstimateSubsetCardinality(makeNodeSet(nodeId));
    CardinalityEstimate numDocsOutput = numDocsProcessed;
    // Assume that the sequential IO performed by scanning the index itself is negilible.
    CardinalityEstimate numSeqIOs = zeroCE;
    // Model the random IO performed by fetching documents from the collection. For simplicity,
    // assume that every index entry causes us to read a new page from the collection.
    CardinalityEstimate numRandIOs = numDocsOutput;
    return JoinCostEstimate(numDocsProcessed, numDocsOutput, numSeqIOs, numRandIOs);
}

// Use catalog information to return an estimate of the size of a document from the "relation"
// containing all fields from all nodes in 'NodeSet'.
double JoinCostEstimatorImpl::estimateDocSize(NodeSet subset) const {
    // Calculate average document size of each "relation" and sum them together.
    double result = 0;
    for (auto nodeId : iterable(subset)) {
        auto& collStats =
            _catalogStats.collStats.at(_jCtx.joinGraph.getNode(nodeId).collectionName);
        auto avgDocSize = collStats.allocatedDataPageBytes /
            _cardinalityEstimator.getCollCardinality(nodeId).toDouble();
        result += avgDocSize;
    }
    return result;
}

JoinCostEstimate JoinCostEstimatorImpl::costHashJoinFragment(const JoinPlanNode& left,
                                                             const JoinPlanNode& right) {
    NodeSet leftSubset = getNodeBitset(left);
    NodeSet rightSubset = getNodeBitset(right);
    CardinalityEstimate buildDocs =
        _cardinalityEstimator.getOrEstimateSubsetCardinality(leftSubset);
    CardinalityEstimate probeDocs =
        _cardinalityEstimator.getOrEstimateSubsetCardinality(rightSubset);
    CardinalityEstimate numDocsOutput =
        _cardinalityEstimator.getOrEstimateSubsetCardinality(leftSubset | rightSubset);

    // Note: We currently don't support block/memory estimate types in our estimates algebra (the
    // type safe library with CardinalityEstimate, CostEstimate and other related types), so we are
    // forced to fallback to using doubles for estimation.

    // Represents the fraction of the dataset that exceeds the available memory buffer. A value of 0
    // means the join is fully in memory, while a value of 1 means the join behaves like a Grace
    // Hash Join.
    double overflowFactor = 0;
    // Use catalog information to estimate of one build and probe entry in bytes.
    const double buildDocSize = estimateDocSize(leftSubset);
    const double probeDocSize = estimateDocSize(rightSubset);
    // Assume that spilling for the hash table using 32Kib pages.
    constexpr double blockSize = 32 * 1024;
    // Assume the default spilling threshold is used 100MiB.
    constexpr double spillingThreshold = 100 * 1024 * 1024;

    double buildSideBytesEstimate = buildDocs.toDouble() * buildDocSize;

    if (buildSideBytesEstimate > spillingThreshold) {
        overflowFactor = 1 - (spillingThreshold / buildSideBytesEstimate);
    }

    CardinalityEstimate numDocsProcessed =
        buildDocs + probeDocs + overflowFactor * (buildDocs + probeDocs);

    double buildBlocks = buildDocs.toDouble() * buildDocSize / blockSize;
    double probeBlocks = probeDocs.toDouble() * probeDocSize / blockSize;

    // Writing and reading of overflow partitions, will be 0 if no overflow.
    CardinalityEstimate ioSeq{CardinalityType{2 * overflowFactor * (buildBlocks + probeBlocks)},
                              EstimationSource::Sampling};

    JoinCostEstimate leftCost = getNodeCost(left);
    JoinCostEstimate rightCost = getNodeCost(right);

    return JoinCostEstimate(
        numDocsProcessed, numDocsOutput, ioSeq, zeroCE /*numRandIOs*/, leftCost, rightCost);
}

// TODO SERVER-117583: Consider the number of components of the index in the cost. For now, the
// given index pointer is unused and as a result we return the same cost for all INLJs regardless of
// the index used.
JoinCostEstimate JoinCostEstimatorImpl::costINLJFragment(const JoinPlanNode& left,
                                                         NodeId right,
                                                         std::shared_ptr<const IndexCatalogEntry>) {
    NodeSet leftSubset = getNodeBitset(left);
    NodeSet rightSubset = makeNodeSet(right);
    CardinalityEstimate numDocsOutput =
        _cardinalityEstimator.getOrEstimateSubsetCardinality(leftSubset | rightSubset);
    CardinalityEstimate leftDocs = _cardinalityEstimator.getOrEstimateSubsetCardinality(leftSubset);

    // The INLJ will produce the join key for each document. The index probe, across all
    // invocations, will produce the number of documents this join outputs.
    CardinalityEstimate numDocsProcessed = leftDocs * 2 + numDocsOutput;
    // Assume that sequential IO done by the index scan is neglible.
    CardinalityEstimate numSeqIOs = zeroCE;
    // Model the random IO performed by doing the index probe and fetch. We perform a random IO for
    // each document we fetch.
    // TODO SERVER-117523: Integrate the height of the B-tree into the formula.
    CardinalityEstimate numRandIOs = numDocsProcessed;

    return JoinCostEstimate(numDocsProcessed,
                            numDocsOutput,
                            numSeqIOs,
                            numRandIOs,
                            getNodeCost(left),
                            JoinCostEstimate(zeroCE, zeroCE, zeroCE, zeroCE));
}

JoinCostEstimate JoinCostEstimatorImpl::costNLJFragment(const JoinPlanNode& left,
                                                        const JoinPlanNode& right) {
    NodeSet leftSubset = getNodeBitset(left);
    NodeSet rightSubset = getNodeBitset(right);

    CardinalityEstimate leftDocs = _cardinalityEstimator.getOrEstimateSubsetCardinality(leftSubset);
    CardinalityEstimate rightDocs =
        _cardinalityEstimator.getOrEstimateSubsetCardinality(rightSubset);

    CardinalityEstimate numDocsProcessed = leftDocs * rightDocs.toDouble();
    CardinalityEstimate numDocsOutput =
        _cardinalityEstimator.getOrEstimateSubsetCardinality(leftSubset | rightSubset);

    // NLJ itself does not perform any IO.
    CardinalityEstimate numSeqIOs = zeroCE;
    CardinalityEstimate numRandIOs = zeroCE;

    return JoinCostEstimate(numDocsProcessed,
                            numDocsOutput,
                            numSeqIOs,
                            numRandIOs,
                            getNodeCost(left),
                            // The right side will be executed 'leftDocs' number of times.
                            getNodeCost(right) * leftDocs);
}


}  // namespace mongo::join_ordering
