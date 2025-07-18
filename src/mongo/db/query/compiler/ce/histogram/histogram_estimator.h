/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/ce/histogram/histogram_estimation_impl.h"

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
