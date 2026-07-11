// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional.hpp>

namespace mongo::join_ordering {

using namespace cost_based_ranker;

/**
 * Represents which branch of the Mackert-Lohman Y_wap formula a particular estimate used.
 */
enum class MackertLohmanCase {
    // The entire collection fits into the WT cache.
    kCollectionFitsCache,
    // The estimated fetched pages all fit into the WT cache.
    kReturnedDocsFitCache,
    // The estimated fetched pages do not fit into the WT cache and thus cause WT cache eviction
    // during the scan.
    kPartialEviction,
};

std::string_view toStringData(MackertLohmanCase c);

/**
 * Convert a CPU cost into an equivalent 'numDocsProcessed' cardinality based on the per-document
 * processing coefficient used by the join cost model. Used for base table accesses where the CPU
 * cost comes from CBR, allowing the result to be plugged into the 'numDocsProcessed' component.
 */
CardinalityEstimate numDocsProcessedFromCpuCost(CostEstimate cpuCost);

/**
 * Represents the cost estimate for a single join operation. It stores all of its inputs for
 * debugging purposes, as it may be useful to see how individual components contribute to the cost
 * estimate. See `getTotalCost()` for the calculation of the cost of the operation.
 */
class JoinCostEstimate {
public:
    JoinCostEstimate(CardinalityEstimate numDocsProcessed,
                     CardinalityEstimate numDocsOutput,
                     CardinalityEstimate numSeqIOs,
                     CardinalityEstimate numRandIOs);

    JoinCostEstimate(CardinalityEstimate numDocsProcessed,
                     CardinalityEstimate numDocsOutput,
                     CardinalityEstimate numSeqIOs,
                     CardinalityEstimate numRandIOs,
                     JoinCostEstimate leftCost,
                     JoinCostEstimate rightCost);

    // Overload for INLJ nodes that also records which branch of the Mackert-Lohman formula was
    // taken when estimating random I/Os.
    JoinCostEstimate(CardinalityEstimate numDocsProcessed,
                     CardinalityEstimate numDocsOutput,
                     CardinalityEstimate numSeqIOs,
                     CardinalityEstimate numRandIOs,
                     JoinCostEstimate leftCost,
                     JoinCostEstimate rightCost,
                     MackertLohmanCase mackertLohmanCase);

    JoinCostEstimate(CostEstimate totalCost);

    CardinalityEstimate getNumDocsProcessed() const {
        return _numDocsProcessed;
    }

    CardinalityEstimate getNumDocsOutput() const {
        return _numDocsOutput;
    }

    CardinalityEstimate getIoSeqPages() const {
        return _ioSeqNumPages;
    }

    CardinalityEstimate getIoRandPages() const {
        return _ioRandNumPages;
    }

    CardinalityEstimate getNumDocsTransmitted() const {
        return _numDocsTransmitted;
    }

    CostEstimate getLocalOpCost() const {
        return _localOpCost;
    }

    CostEstimate getTotalCost() const {
        return _totalCost;
    }

    boost::optional<MackertLohmanCase> getMackertLohmanCase() const {
        return _mackertLohmanCase;
    }

    std::string toString() const;
    BSONObj toBSON() const;

    auto operator<=>(const JoinCostEstimate& other) const {
        return approxCompare(_totalCost, other._totalCost);
    }

    JoinCostEstimate operator*(const CardinalityEstimate& cardEst) const {
        return JoinCostEstimate(_totalCost * cardEst);
    }

private:
    // Estimate of the number of documents this join operation will process.
    CardinalityEstimate _numDocsProcessed;

    // Estimate of the output cardinality of this join operation.
    CardinalityEstimate _numDocsOutput;

    // Estimate of number of pages of sequential/random IO this operation performs.
    CardinalityEstimate _ioSeqNumPages;
    CardinalityEstimate _ioRandNumPages;

    // Estimate the number of documents transmitted. This is used to distinguish between regular and
    // broadcast hash joins. Note this is currently 0 as we don't support broadcast joins.
    CardinalityEstimate _numDocsTransmitted{zeroCE};

    // Final estimate for the cost of this operation, ignoring the cost of children.
    CostEstimate _localOpCost;

    // Cumulative estimate for the cost of this join including the cost of children. This value of
    // derived from all the other components in this class.
    CostEstimate _totalCost;

    // The branch of the Mackert-Lohman Y_wap formula used to estimate random I/Os. Only set for
    // INDEXED_NESTED_LOOP_JOIN nodes; boost::none for all other join types.
    boost::optional<MackertLohmanCase> _mackertLohmanCase;
};

inline std::ostream& operator<<(std::ostream& os, const JoinCostEstimate& cost) {
    return os << cost.toString();
}

/**
 * A QSNEstimate subclass for join embedding nodes that includes a detailed cost breakdown.
 * When attached to a join node in the EstimateMap, its serialize() emits a 'joinCostComponents'
 * sub-object in addition to the base costEstimate/cardinalityEstimate fields.
 */
class JoinExtraEstimateInfo : public cost_based_ranker::QSNEstimate {
public:
    JoinExtraEstimateInfo(cost_based_ranker::CardinalityEstimate outCE,
                          cost_based_ranker::CostEstimate cost)
        : QSNEstimate(std::move(outCE), std::move(cost)) {}

    double docsProcessed{0};
    double docsOutput{0};
    double sequentialIOPages{0};
    double randomIOPages{0};
    double localOpCost{0};
    // Only set for INDEXED_NESTED_LOOP_JOIN nodes; absent for HJ and NLJ.
    boost::optional<MackertLohmanCase> mackertLohmanCase;

    void serialize(BSONObjBuilder& bob) const override;
};

}  // namespace mongo::join_ordering
