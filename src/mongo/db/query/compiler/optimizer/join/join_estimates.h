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

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"

namespace mongo::join_ordering {

using namespace cost_based_ranker;

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

    CostEstimate getTotalCost() const {
        return _totalCost;
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

    // Final estimate for the cost of this join. This value of derived from all the other components
    // in this class.
    CostEstimate _totalCost;
};
}  // namespace mongo::join_ordering
