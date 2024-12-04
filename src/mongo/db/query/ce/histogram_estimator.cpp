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

#include "mongo/db/query/ce/histogram_estimator.h"
#include "mongo/db/query/ce/histogram_estimation_impl.h"

namespace mongo::ce {


CardinalityEstimate HistogramEstimator::estimateCardinality(
    const stats::CEHistogram& hist,
    const CardinalityEstimate collectionSize,
    const mongo::Interval& interval,
    bool includeScalar,
    ArrayRangeEstimationAlgo arrayEstimationAlgo) {

    // Empty histogram.
    if (hist.getSampleSize() <= 0) {
        return CardinalityEstimate{CardinalityType{0.0}, EstimationSource::Histogram};
    }

    // Rescales the cardinality according to the current collection size.
    auto scaleFactor = collectionSize.toDouble() / hist.getSampleSize();
    CardinalityEstimate card =
        estimateIntervalCardinality(hist, interval, includeScalar, arrayEstimationAlgo);
    return card * scaleFactor;
}

bool HistogramEstimator::canEstimateInterval(const stats::CEHistogram& hist,
                                             const mongo::Interval& interval,
                                             bool includeScalar) {

    auto [startTag, startVal] = sbe::bson::convertFrom<false>(interval.start);
    auto [endTag, endVal] = sbe::bson::convertFrom<false>(interval.end);
    sbe::value::ValueGuard startGuard{startTag, startVal};
    sbe::value::ValueGuard endGuard{endTag, endVal};

    // If 'startTag' and 'endTag' are either in the same type or type-bracketed, they are estimable
    // directly via either histograms or type counts.
    bool viaHistogram =
        (stats::sameTypeBracketInterval(startTag, interval.endInclusive, endTag, endVal) &&
         stats::canEstimateTypeViaHistogram(startTag));

    auto viaTypeCounts = stats::canEstimateIntervalViaTypeCounts(
        startTag, startVal, interval.startInclusive, endTag, endVal, interval.endInclusive);

    // For a mixed-type interval, if both bounds are of estimable types, we can divide the interval
    // into multiple sub-intervals. The first and last sub-intervals can be estimated using either
    // histograms or type counts, while the intermediate sub-intervals, which are fully bracketed,
    // can be estimated using type counts.
    auto viaBracketization = stats::canEstimateType(startTag) && stats::canEstimateType(endTag);

    return viaHistogram || viaTypeCounts || viaBracketization;
}

}  // namespace mongo::ce
