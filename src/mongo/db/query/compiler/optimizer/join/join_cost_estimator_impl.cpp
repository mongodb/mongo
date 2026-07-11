// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/join_cost_estimator_impl.h"

namespace mongo::join_ordering {

JoinCostEstimatorImpl::JoinCostEstimatorImpl(const JoinReorderingContext& jCtx,
                                             JoinCardinalityEstimator& cardinalityEstimator)
    : _jCtx(jCtx), _cardinalityEstimator(cardinalityEstimator) {}

JoinCostEstimate JoinCostEstimatorImpl::costCollScanFragment(NodeId nodeId,
                                                             CostEstimate singleTableCpuCost) {
    // CollScan outputs documents after applying single table predicates
    CardinalityEstimate numDocsOutput =
        _cardinalityEstimator.getOrEstimateSubsetCardinality(makeNodeSet(nodeId));

    auto& collStats = _jCtx.catStats.collStats.at(_jCtx.joinGraph.getNode(nodeId).collectionName);
    // CollScan performs roughly sequential reads from disk as it is stored in a WT b-tree. We
    // estimate the number of disk reads by estimating the number of pages the collscan will read.
    CardinalityEstimate numSeqIOs =
        CardinalityEstimate{CardinalityType{collStats.numPages()}, EstimationSource::Metadata};
    // CollScan does no random read from disk.
    CardinalityEstimate numRandIOs = zeroCE;

    return JoinCostEstimate(
        numDocsProcessedFromCpuCost(singleTableCpuCost), numDocsOutput, numSeqIOs, numRandIOs);
}

JoinCostEstimate JoinCostEstimatorImpl::costIndexScanFragment(NodeId nodeId,
                                                              CostEstimate singleTableCpuCost) {
    // For simplicity we assume there are no non-sargable filters applied after the index scan. This
    // means that we assume the number of output documents is equal to the cardinality estimate of
    // that node.
    CardinalityEstimate numDocsOutput =
        _cardinalityEstimator.getOrEstimateSubsetCardinality(makeNodeSet(nodeId));
    // Assume that the sequential IO performed by scanning the index itself is negligible.
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
    // TODO SERVER-123532: extend this to multikey indexes once NDV estimation supports them.
    if (_jCtx.samplingEstimators) {
        const auto* cq = _jCtx.joinGraph.accessPathAt(nodeId);
        const auto& qsn = _jCtx.singleTableAccess.cbrCqQsns.at(cq);

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
            double collCard = _jCtx.singleTableAccess.collCardinalities[nodeId].toDouble();
            // Scale NDV by selectivity of the scan.
            // Guard against division by 0 and 0 NDV, in both cases fallback to estimating a random
            // IO per output document.
            if (cost_based_ranker::exactGt(ndv, cost_based_ranker::zeroCE) && collCard > 0) {
                numLogicalPageRequests = ndv.toDouble() * numDocsOutput.toDouble() / collCard;
            }
        }
    }

    double numPagesInStorageEngineCache = _jCtx.catStats.numPagesInStorageEngineCache(nss);
    // Model the random IO performed by fetching documents from the collection.
    auto [numRandIOsCollection, mlCase] = estimateMackertLohmanRandIO(
        numPagesAccessedColl, numPagesInStorageEngineCache, numLogicalPageRequests);
    const auto sortedSparse = estimateSortedSparseIO(
        numPagesAccessedColl, numLogicalPageRequests, mlCase, numPagesInStorageEngineCache);

    numRandIOsCollection += sortedSparse.numRandIOs;
    numSeqIOs =
        CardinalityEstimate{CardinalityType{sortedSparse.numSeqIOs}, EstimationSource::Sampling};
    CardinalityEstimate numRandIOs{CardinalityType{numRandIOsCollection},
                                   EstimationSource::Sampling};

    return JoinCostEstimate(
        numDocsProcessedFromCpuCost(singleTableCpuCost), numDocsOutput, numSeqIOs, numRandIOs);
}

// Use catalog information to return an estimate of the size of a document from the "relation"
// containing all fields from all nodes in 'NodeSet'.
double JoinCostEstimatorImpl::estimateDocSize(NodeSet subset) const {
    // Calculate average document size of each "relation" and sum them together.
    double result = 0;
    for (auto nodeId : iterable(subset)) {
        auto& collStats =
            _jCtx.catStats.collStats.at(_jCtx.joinGraph.getNode(nodeId).collectionName);
        auto collSize = _jCtx.singleTableAccess.collCardinalities[nodeId].toDouble();
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

    // We attribute a double cost for overflow because those documents have to be both written away
    // and read again.
    CardinalityEstimate numDocsProcessed =
        buildDocs + probeDocs + 2.0 * overflowFactor * (buildDocs + probeDocs);

    double buildBlocks = buildDocs.toDouble() * buildDocSize / blockSize;
    double probeBlocks = probeDocs.toDouble() * probeDocSize / blockSize;

    // Writing and reading of overflow partitions, will be 0 if no overflow.
    CardinalityEstimate ioSeq{CardinalityType{2.0 * overflowFactor * (buildBlocks + probeBlocks)},
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

    // Model the random IO performed by doing the index probe and fetch.
    // TODO SERVER-117523: Integrate the height of the B-tree into the formula.
    const auto& nss = _jCtx.joinGraph.getNode(right).collectionName;
    auto& rightCollStats = _jCtx.catStats.collStats.at(nss);
    double numPagesColl = rightCollStats.numPages();

    // The cardinality of the outer side is the number of probes we will perform.
    double numProbes = leftDocs.toDouble();
    double rightBaseCard = _jCtx.singleTableAccess.collCardinalities[right].toDouble();
    double joinPredSel = _cardinalityEstimator.getEdgeSelectivity(edgeId).toDouble();
    // The number of documents that the INLJ probes for:
    // numProbes * (rightBaseCard * joinPredSel)
    // The latter term, (rightBaseCard * joinPredSel), corresponds to the number of documents that a
    // single probe will return.
    double numDocsReturnedFromProbe = numProbes * rightBaseCard * joinPredSel;

    // The INLJ processes each left-side document twice: once when it receives the document and once
    // when it sends the join key to the right side for probing.
    //
    // Use 'numDocsReturnedFromProbe' rather than 'numDocsOutput' because 'numDocsOutput' already
    // includes right-side filters that are applied after the join.
    CardinalityEstimate numDocsProcessed = leftDocs * 2.0 +
        CardinalityEstimate{CardinalityType{numDocsReturnedFromProbe}, EstimationSource::Sampling};

    // Assume that sequential IO done by the index scan is negligible.
    CardinalityEstimate numSeqIOs = zeroCE;

    double numPagesAccessedColl = estimateYaoDistinctPages(numPagesColl, numDocsReturnedFromProbe);
    double numPagesInStorageEngineCache = _jCtx.catStats.numPagesInStorageEngineCache(nss);
    auto [numRandIOsCollection, mlCase] = estimateMackertLohmanRandIO(
        numPagesAccessedColl,
        numPagesInStorageEngineCache,
        // In a MongoDB index, we append the RecordId (RID) to the index key. This means that a
        // single index probe will read index keys for the same join key in RID order. Because
        // MongoDB collections are clustered on RID, each fetch is not performing a truly
        // random I/O but rather a sorted-sparse access pattern over the collection. For this
        // calculation, we assume that each index probe performs a single random I/O and we model
        // the sorted-sparse I/O cost separately.
        numProbes);

    const auto sortedSparse = estimateSortedSparseIO(
        numPagesAccessedColl, numProbes, mlCase, numPagesInStorageEngineCache);
    numRandIOsCollection += sortedSparse.numRandIOs;
    numSeqIOs =
        CardinalityEstimate{CardinalityType{sortedSparse.numSeqIOs}, EstimationSource::Sampling};

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

    CardinalityEstimate numDocsProcessed = cost_based_ranker::product(leftDocs, rightDocs);
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
    auto it = _jCtx.singleTableAccess.cbrCqQsns.find(cq);
    tassert(11729101,
            "Expected a QSN to exist for this access path",
            it != _jCtx.singleTableAccess.cbrCqQsns.end());

    if (it->second->root()->getType() == STAGE_EOF) {
        return JoinCostEstimate(zeroCost);
    }

    // The full CPU cost of the single-table plan comes from CBR and is passed to the fragment
    // methods, which fold it into the join cost formula alongside the output and IO costs they
    // model themselves.
    CostEstimate singleTableCost = _jCtx.singleTableAccess.nodeCBRCosts[baseNode];

    // TODO SERVER-117618: Stricter tree-shape validation.
    if (it->second->hasNode(STAGE_COLLSCAN)) {
        return costCollScanFragment(baseNode, singleTableCost);
    } else if (it->second->hasNode(STAGE_IXSCAN)) {
        return costIndexScanFragment(baseNode, singleTableCost);
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

SortedSparseIO estimateSortedSparseIO(double numPagesAccessedColl,
                                      double numLogicalPageRequests,
                                      MackertLohmanCase mlCase,
                                      double numPagesInStorageEngineCache) {
    tassert(12226500,
            "estimateSortedSparseIO() expected numPagesAccessedColl >= 0",
            numPagesAccessedColl >= 0);
    tassert(12226501,
            "estimateSortedSparseIO() expected numLogicalPageRequests >= 0",
            numLogicalPageRequests >= 0);
    tassert(12226502,
            "estimateSortedSparseIO() expected numPagesInStorageEngineCache > 0",
            numPagesInStorageEngineCache > 0);

    // M-L charges one random I/O per group (probe or distinct key); the remaining
    // (numPagesAccessedColl - numLogicalPageRequests) accesses within each group follow RID order
    // and are sorted-sparse: cheaper than random access, but costlier than a purely sequential
    // scan.
    double numSortedSparseIOs = std::max(0.0, numPagesAccessedColl - numLogicalPageRequests);

    switch (mlCase) {
        case MackertLohmanCase::kCollectionFitsCache:
            // For case 1 of M-L, do not charge sorted-sparse I/O. The accessed pages fit in cache,
            // and the random I/O cost is already accounted for in the M-L formula.
            return {.numSeqIOs = 0.0, .numRandIOs = 0.0};
        case MackertLohmanCase::kReturnedDocsFitCache:
        case MackertLohmanCase::kPartialEviction: {
            // The overflowFactor (0.0 to 1.0): what fraction of these pages overflow the buffer
            // pool?
            // Case 2 and 3 of M-L, where the number of pages accessed exceeds the cache size.
            double overflowFactor = 1 - (numPagesInStorageEngineCache / numPagesAccessedColl);

            // Apply the sorted spatial locality dampening curve. The square root function models
            // diminishing returns from caching: it rises quickly for small overflow factors and
            // flattens for larger overflow factors. The 0.5 factor caps the penalty because
            // sorted-sparse accesses are partially cached and should not incur the full random I/O
            // cost.
            double dampedOverflowFactor = std::sqrt(overflowFactor) * 0.5;

            // For cases 2 and 3, apply the dampening factor to avoid a separate case-specific set
            // of arbitrary constants.
            return {.numSeqIOs = 0.0, .numRandIOs = numSortedSparseIOs * dampedOverflowFactor};
        }
    }
    MONGO_UNREACHABLE;
}

}  // namespace mongo::join_ordering
