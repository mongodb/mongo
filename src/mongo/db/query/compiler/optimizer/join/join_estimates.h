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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/util/modules.h"

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

StringData toStringData(MackertLohmanCase c);

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
        return _totalCost <=> other._totalCost;
    }

    JoinCostEstimate operator*(const CardinalityEstimate& cardEst) const {
        return JoinCostEstimate(_totalCost * cardEst.toDouble());
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
