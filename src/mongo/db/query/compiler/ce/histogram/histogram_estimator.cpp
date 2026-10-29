// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
    auto start = sbe::bson::convertToOwned(interval.start);
    auto end = sbe::bson::convertToOwned(interval.end);

    // If the interval is not in the ascending order, then reverse it.
    if (reversedInterval(start.tag(), start.value(), end.tag(), end.value())) {
        std::swap(startInclusive, endInclusive);
        std::swap(start, end);
    }

    return ::mongo::ce::canEstimateInterval(hist.isArray(),
                                            startInclusive,
                                            start.tag(),
                                            start.value(),
                                            endInclusive,
                                            end.tag(),
                                            end.value());
}

}  // namespace mongo::ce
