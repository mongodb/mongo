// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/ce/histogram/histogram_estimation_impl.h"
#include "mongo/util/modules.h"

namespace mongo::ce {

class HistogramEstimator {
public:
    /**
     * Estimates the cardinality of an interval based on the provided histogram.
     * 'collectionSize' represents the number of documents in the collection.
     * 'inputScalar' indicates whether or not the provided interval should include non-array values.
     * e.g., $elemMatch should exclude the non-array values when 'includeScalar' is set to false.
     */
    static CardinalityEstimate estimateCardinality(const stats::CEHistogram& hist,
                                                   CardinalityEstimate collectionSize,
                                                   const mongo::Interval& interval,
                                                   bool includeScalar,
                                                   ArrayRangeEstimationAlgo arrayEstimationAlgo);

    /**
     * Checks if given interval can be estimated.
     */
    static bool canEstimateInterval(const stats::CEHistogram& hist,
                                    const mongo::Interval& interval);
};

}  // namespace mongo::ce
