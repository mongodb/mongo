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
                                             JoinCardinalityEstimator& cardinalityEstimator)
    : _jCtx(jCtx), _cardinalityEstimator(cardinalityEstimator) {}

JoinCostEstimate JoinCostEstimatorImpl::costCollScanFragment(NodeId nodeId) {
    // CollScan processes all documents in the collection
    CardinalityEstimate numDocsProcessed = _cardinalityEstimator.getCollCardinality(nodeId);
    // CollScan outputs documenst after applying single table predicates
    CardinalityEstimate numDocsOutput =
        _cardinalityEstimator.getOrEstimateSubsetCardinality(makeNodeSet(nodeId));

    auto& collStats = _jCtx.catStats.collStats.at(_jCtx.joinGraph.getNode(nodeId).collectionName);
    // CollScan performs roughly sequential reads from disk as it is stored in a WT b-tree. We
    // estimate the number of disk read by estimating the number of pages the collscan will read.
    CardinalityEstimate numSeqIOs =
        CardinalityEstimate{CardinalityType{collStats.numPages()}, EstimationSource::Metadata};
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

    const auto& nss = _jCtx.joinGraph.getNode(nodeId).collectionName;
    auto& collStats = _jCtx.catStats.collStats.at(nss);
    double numPagesColl = collStats.numPages();

    // Estimate the number of distinct pages fetched from the collection using Yao's formula.
    // This represents the 'T' parameter in the Mackert-Lohman formula.
    double numPagesAccessedColl = estimateYaoDistinctPages(numPagesColl, numDocsOutput.toDouble());

    // By default (e.g. for multikey indexes or when no sampling estimator is available), treat
    // every fetched document as a logical page request.
    double numLogicalPageRequests = numDocsOutput.toDouble();

    // For non-multikey indexes, entries sharing the same index key are stored in RID order, so
    // fetches for the same key perform sort-sparse IO rather than independent random IOs. We
    // therefore use the NDV of the index key fields (scaled by the scan's selectivity) as the
    // number of logical page requests instead of numDocsOutput. The NDV from estimateNDV() is for
    // the entire collection; multiplying by (numDocsOutput / collCard) gives the number of distinct
    // key groups actually accessed by this scan.
    // TODO SERVER-122379: extend this to multikey indexes once NDV estimation supports them.
    if (_jCtx.samplingEstimators) {
        const auto* cq = _jCtx.joinGraph.accessPathAt(nodeId);
        const auto& qsn = _jCtx.cbrCqQsns.at(cq);

        auto [ixScanNodePtr, _] = qsn->getFirstNodeByType(STAGE_IXSCAN);
        tassert(12291601, "expected plan fragment to contain IndexScan QSN", ixScanNodePtr);
        const auto* ixNode = static_cast<const IndexScanNode*>(ixScanNodePtr);
        if (!ixNode->index.multikey) {
            std::vector<ce::FieldPathAndEqSemantics> fields;
            for (auto&& elt : ixNode->index.keyPattern) {
                fields.push_back({.path = FieldPath(elt.fieldName())});
            }
            const auto& samplingEstimator = _jCtx.samplingEstimators->at(nss);
            auto ndv = samplingEstimator->estimateNDV(fields);
            double collCard = _cardinalityEstimator.getCollCardinality(nodeId).toDouble();
            // Scale NDV by selectivity of the scan.
            // Guard against division by 0 and 0 NDV, in both cases fallback to estimating a random
            // IO per output document.
            if (ndv.toDouble() > 0 && collCard > 0) {
                numLogicalPageRequests = ndv.toDouble() * numDocsOutput.toDouble() / collCard;
            }
        }
    }

    // Model the random IO performed by fetching documents from the collection.
    CardinalityEstimate numRandIOs =
        CardinalityEstimate{CardinalityType{estimateMackertLohmanRandIO(
                                                numPagesAccessedColl,
                                                _jCtx.catStats.numPagesInStorageEngineCache(nss),
                                                numLogicalPageRequests)
                                                .randIOPages},
                            EstimationSource::Sampling};
    return JoinCostEstimate(numDocsProcessed, numDocsOutput, numSeqIOs, numRandIOs);
}

// Use catalog information to return an estimate of the size of a document from the "relation"
// containing all fields from all nodes in 'NodeSet'.
double JoinCostEstimatorImpl::estimateDocSize(NodeSet subset) const {
    // Calculate average document size of each "relation" and sum them together.
    double result = 0;
    for (auto nodeId : iterable(subset)) {
        auto& collStats =
            _jCtx.catStats.collStats.at(_jCtx.joinGraph.getNode(nodeId).collectionName);
        auto collSize = _cardinalityEstimator.getCollCardinality(nodeId).toDouble();
        if (collSize == 0) {
            continue;
        }
        auto avgDocSize = collStats.logicalDataSizeBytes / collSize;
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

// The costing formula currently makes two assumptions:
// 1. The index being used to satisfy the join predicate fully covers the predicate (there is no
// residual filter for the join predicate). In reality, this is not true, the join predicate is
// reapplied to handle null/missing equality semantics properly.
// 2. The index probe does not apply any single table predicates (all single table predicates are
// residual). This is currently always true because we don't support a probe with any other bounds.
// TODO SERVER-114883: Remove assumption #2.
// TODO SERVER-117583: Consider the number of components of the index in the cost. For now, the
// given index pointer is unused and as a result we return the same cost for all INLJs regardless of
// the index used.
JoinCostEstimate JoinCostEstimatorImpl::costINLJFragment(const JoinPlanNode& left,
                                                         NodeId right,
                                                         std::shared_ptr<const IndexCatalogEntry>,
                                                         EdgeId edgeId) {
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

    // Model the random IO performed by doing the index probe and fetch.
    // TODO SERVER-117523: Integrate the height of the B-tree into the formula.
    const auto& nss = _jCtx.joinGraph.getNode(right).collectionName;
    auto& rightCollStats = _jCtx.catStats.collStats.at(nss);
    double numPagesColl = rightCollStats.numPages();

    // The cardinality of the outer side is the number of probes we will perform.
    double numProbes = leftDocs.toDouble();
    double rightBaseCard = _cardinalityEstimator.getCollCardinality(right).toDouble();
    double joinPredSel = _cardinalityEstimator.getEdgeSelectivity(edgeId).toDouble();
    // The number of documents that the INLJ probes for:
    // numProbes * (rightBaseCard * joinPredSel)
    // The latter term, (rightBaseCard * joinPredSel), corresponds to the number of documents that a
    // single probe will return.
    double numDocsReturnedFromProbe = numProbes * rightBaseCard * joinPredSel;
    double numPagesAccessedColl = estimateYaoDistinctPages(numPagesColl, numDocsReturnedFromProbe);
    auto [numRandIOsCollection, mlCase] = estimateMackertLohmanRandIO(
        numPagesAccessedColl,
        _jCtx.catStats.numPagesInStorageEngineCache(nss),
        // In a MongoDB index, we append the RecordId (RID) to the index key. This means that a
        // single index probe will read index keys for the same join key in RID order. Because
        // MongoDB collections are clustered on RID, each fetch is not performing a truely
        // random I/O but rather a sorted-sparse access pattern over the collection. For now, we
        // ignore the I/O cost of this sorted-sparse and assume that each index probe only
        // performs a single random I/O.
        numProbes);

    return JoinCostEstimate(
        numDocsProcessed,
        numDocsOutput,
        numSeqIOs,
        CardinalityEstimate{CardinalityType{numRandIOsCollection}, EstimationSource::Sampling},
        getNodeCost(left),
        JoinCostEstimate(zeroCE, zeroCE, zeroCE, zeroCE),
        mlCase);
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

JoinCostEstimate JoinCostEstimatorImpl::costBaseCollectionAccess(NodeId baseNode) {
    const auto* cq = _jCtx.joinGraph.accessPathAt(baseNode);
    tassert(11729100, "Expected an access path to exist", cq);
    auto it = _jCtx.cbrCqQsns.find(cq);
    tassert(11729101, "Expected a QSN to exist for this access path", it != _jCtx.cbrCqQsns.end());
    // TODO SERVER-117618: Stricter tree-shape validation.
    if (it->second->hasNode(STAGE_COLLSCAN)) {
        return costCollScanFragment(baseNode);
    } else if (it->second->hasNode(STAGE_IXSCAN)) {
        return costIndexScanFragment(baseNode);
    }
    MONGO_UNIMPLEMENTED_TASSERT(11729102);
}

double estimateYaoDistinctPages(double numLeafPages, double numEntriesRequested) {
    if (numLeafPages <= 0 || numEntriesRequested <= 0) {
        return 0;
    }
    numLeafPages = std::max(1.0, numLeafPages);
    // Intuition:
    // * The probability a specific page is never hit across all 'numEntriesRequested' fetches is:
    //   (1 - (1 / numLeafPages)) ^ numEntriesRequested
    // * The probability that a specific page is hit at least once is:
    //   1 - ((1 - (1 / numLeafPages)) ^ numEntriesRequested)
    // * Multiply by 'numLeafPages' to get the number of expected distinct pages
    //   numLeafPages * (1 - ((1 - (1 / numLeafPages)) ^ numEntriesRequested))
    //
    // This formula has nice properties:
    // * When numEntriesRequested << numLeafPages, formula ~= numEntriesRequested, indicating that
    // every lookup requests a new page.
    // * When numEntriesRequested >> numLeafPages, formula ~= numLeafPages, indicates that every
    // page is eventually requested.
    return numLeafPages * (1 - std::pow(1 - (1 / numLeafPages), numEntriesRequested));
}

MackertLohmanResult estimateMackertLohmanRandIO(double numDistinctPagesNeededFromBtree,
                                                double numPagesInStorageEngineCache,
                                                double numLogicalPageRequests) {
    tassert(11943801,
            "estimateMackertLohmanRandIO() expected numDistinctPagesNeededFromBtree >= 0",
            numDistinctPagesNeededFromBtree >= 0);
    tassert(11943802,
            "estimateMackertLohmanRandIO() expected numPagesInStorageEngineCache > 0",
            numPagesInStorageEngineCache > 0);
    tassert(11943803,
            "estimateMackertLohmanRandIO() expected numLogicalPageRequests >= 0",
            numLogicalPageRequests >= 0);

    // For ease of reference, the formula in the paper is:
    //         / min(Dx, T)                 when T <= b
    // Y_wap = | Dx                         when T > b  AND Dx <= b
    //         \ b + (Dx - b) * (T-b)/T     when T > b  AND Dx > b
    // T = numDistinctPagesNeededFromBtree
    // b = numPagesInStorageEngineCache
    // D*x = numLogicalPageRequests

    // Case 1: The entire collection fits in the WT cache.
    if (numDistinctPagesNeededFromBtree <= numPagesInStorageEngineCache) {
        return {std::min(numLogicalPageRequests, numDistinctPagesNeededFromBtree),
                MackertLohmanCase::kCollectionFitsCache};
    }

    // Case 2: The collection is bigger than the cache, but all the returned documents fit in the WT
    // cache. We assume that have a totally unclustered index, meaning that every key returned from
    // the index scan results in a random I/O.
    if (numLogicalPageRequests <= numPagesInStorageEngineCache) {
        return {numLogicalPageRequests, MackertLohmanCase::kReturnedDocsFitCache};
    }

    // Case 3: The collection is bigger than the cache and the sum of pages that are fetched are
    // also bigger than the cache. This results in cache eviction and means that fetching the same
    // page multiple times may result multiple random I/Os (whereas in the case of a smaller result
    // set, that page would be cached).
    return {numPagesInStorageEngineCache +
                (numLogicalPageRequests - numPagesInStorageEngineCache) *
                    (numDistinctPagesNeededFromBtree - numPagesInStorageEngineCache) /
                    numDistinctPagesNeededFromBtree,
            MackertLohmanCase::kPartialEviction};
}

}  // namespace mongo::join_ordering
