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

#include "mongo/db/query/compiler/ce/histogram/histogram_estimator.h"

#include "mongo/db/exec/sbe/values/bson.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryCE

namespace mongo::ce {

CardinalityEstimate HistogramEstimator::estimateCardinality(
    const stats::CEHistogram& hist,
    const CardinalityEstimate collectionSize,
    const mongo::Interval& interval,
    bool includeScalar,
    ArrayRangeEstimationAlgo arrayEstimationAlgo) {

    // Empty histogram.
    if (hist.getSampleSize() <= 0) {
        LOGV2_DEBUG(9756602,
                    5,
                    "HistogramCE returning 0-estimate due to empty histogram",
                    "interval"_attr = interval.toString(false));
        return CardinalityEstimate{CardinalityType{0.0}, EstimationSource::Histogram};
    }

    // Rescales the cardinality according to the current collection size.
    auto scaleFactor = collectionSize.toDouble() / hist.getSampleSize();
    CardinalityEstimate card =
        estimateIntervalCardinality(hist, interval, includeScalar, arrayEstimationAlgo);
    auto ret = card * scaleFactor;
    LOGV2_DEBUG(9756603,
                5,
                "HistogramCE cardinality",
                "interval"_attr = interval.toString(false),
                "estimate"_attr = ret);
    return ret;
}

bool HistogramEstimator::canEstimateInterval(const stats::CEHistogram& hist,
                                             const mongo::Interval& interval) {
    bool startInclusive = interval.startInclusive;
    bool endInclusive = interval.endInclusive;
    auto [startTag, startVal] = sbe::bson::convertFrom<false>(interval.start);
    auto [endTag, endVal] = sbe::bson::convertFrom<false>(interval.end);
    sbe::value::ValueGuard startGuard{startTag, startVal};
    sbe::value::ValueGuard endGuard{endTag, endVal};

    // If the interval is not in the ascending order, then reverse it.
    if (reversedInterval(startTag, startVal, endTag, endVal)) {
        std::swap(startInclusive, endInclusive);
        std::swap(startTag, endTag);
        std::swap(startVal, endVal);
    }

    return ::mongo::ce::canEstimateInterval(
        hist.isArray(), startInclusive, startTag, startVal, endInclusive, endTag, endVal);
}

}  // namespace mongo::ce
