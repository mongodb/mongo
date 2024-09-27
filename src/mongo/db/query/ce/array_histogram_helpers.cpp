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

#include "mongo/db/query/ce/array_histogram_helpers.h"

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/ce/histogram_common.h"
#include "mongo/db/query/ce/scalar_histogram_helpers.h"
#include "mongo/db/query/stats/array_histogram.h"
#include "mongo/db/query/stats/value_utils.h"

namespace mongo::optimizer::cbp::ce {

int compareTypeTags(sbe::value::TypeTags a, sbe::value::TypeTags b) {
    auto orderOfA = canonicalizeBSONTypeUnsafeLookup(tagToType(a));
    auto orderOfB = canonicalizeBSONTypeUnsafeLookup(tagToType(b));
    if (orderOfA < orderOfB) {
        return -1;
    } else if (orderOfA > orderOfB) {
        return 1;
    }
    return 0;
}

EstimationResult estimateCardinalityEq(const stats::ArrayHistogram& ah,
                                       sbe::value::TypeTags tag,
                                       sbe::value::Value val,
                                       bool includeScalar) {
    EstimationResult estimation = {0.0 /*card*/, 0.0 /*ndv*/};
    // Estimate cardinality for fields containing scalar values if includeScalar is true.
    if (includeScalar) {
        estimation = estimateCardinality(ah.getScalar(), tag, val, EstimationType::kEqual);
    }
    // If histogram includes array data points, calculate cardinality for fields containing array
    // values.
    if (ah.isArray()) {
        estimation += estimateCardinality(ah.getArrayUnique(), tag, val, EstimationType::kEqual);
    }
    return estimation;
}

EstimationResult estimateCardinalityRange(const stats::ArrayHistogram& ah,
                                          bool lowInclusive,
                                          sbe::value::TypeTags tagLow,
                                          sbe::value::Value valLow,
                                          bool highInclusive,
                                          sbe::value::TypeTags tagHigh,
                                          sbe::value::Value valHigh,
                                          bool includeScalar,
                                          EstimationAlgo estimationAlgo) {
    tassert(8870502,
            "The interval must be in ascending order",
            !reversedInterval(tagLow, valLow, tagHigh, valHigh));

    // Helper lambda to shorten code for legibility.
    auto estRange = [&](const stats::ScalarHistogram& h) {
        return estimateCardinalityRange(
            h, lowInclusive, tagLow, valLow, highInclusive, tagHigh, valHigh);
    };

    EstimationResult result = {0.0 /*card*/, 0.0 /*ndv*/};
    if (ah.isArray()) {

        if (includeScalar) {
            // Range query on array data.
            result += estimateRangeQueryOnArray(ah.getArrayMin(),
                                                ah.getArrayMax(),
                                                lowInclusive,
                                                tagLow,
                                                valLow,
                                                highInclusive,
                                                tagHigh,
                                                valHigh);
        } else {
            // $elemMatch query on array data.
            const auto arrayMinEst = estRange(ah.getArrayMin());
            const auto arrayMaxEst = estRange(ah.getArrayMax());
            const auto arrayUniqueEst = estRange(ah.getArrayUnique());

            const double totalArrayCount = ah.getArrayCount() - ah.getEmptyArrayCount();

            uassert(
                9160701, "Array histograms should contain at least one array", totalArrayCount > 0);
            switch (estimationAlgo) {
                case EstimationAlgo::HistogramV1: {
                    const double arrayUniqueDensity = (arrayUniqueEst.ndv == 0.0)
                        ? 0.0
                        : (arrayUniqueEst.card / std::sqrt(arrayUniqueEst.ndv));
                    result = {
                        std::max(std::max(arrayMinEst.card, arrayMaxEst.card), arrayUniqueDensity),
                        0.0};
                    break;
                }
                case EstimationAlgo::HistogramV2: {
                    const double avgArraySize =
                        getTotals(ah.getArrayUnique()).card / totalArrayCount;
                    const double adjustedUniqueCard = (avgArraySize == 0.0)
                        ? 0.0
                        : std::min(arrayUniqueEst.card / pow(avgArraySize, 0.2), totalArrayCount);
                    result = {
                        std::max(std::max(arrayMinEst.card, arrayMaxEst.card), adjustedUniqueCard),
                        0.0};
                    break;
                }
                case EstimationAlgo::HistogramV3: {
                    const double adjustedUniqueCard =
                        0.85 * std::min(arrayUniqueEst.card, totalArrayCount);
                    result = {
                        std::max(std::max(arrayMinEst.card, arrayMaxEst.card), adjustedUniqueCard),
                        0.0};
                    break;
                }
                default:
                    MONGO_UNREACHABLE;
            }
        }
    }

    if (includeScalar) {
        result += estRange(ah.getScalar());
    }

    return {result};
}

bool canEstimateBound(const stats::ArrayHistogram& ah,
                      const sbe::value::TypeTags tag,
                      bool includeScalar) {
    // if histogrammable, then it's estimable
    if (stats::canEstimateTypeViaHistogram(tag)) {
        return true;
    }
    // check if it's not one of the type-count-estimable types
    if (includeScalar) {
        if (tag != sbe::value::TypeTags::Boolean && tag != sbe::value::TypeTags::Array &&
            tag != sbe::value::TypeTags::Null) {
            return false;
        }
    }
    if (ah.isArray()) {
        if (tag != sbe::value::TypeTags::Null) {
            return false;
        }
    }
    return true;
}

// TODO: SERVER-94855 Supports mixed type intervals with type counts.
Cardinality estimateIntervalCardinality(const stats::ArrayHistogram& ah,
                                        const mongo::Interval& interval,
                                        bool includeScalar) {
    if (interval.isFullyOpen()) {
        return ah.getSampleSize();
    }

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

    // If 'startTag' and 'endTag' are in the same sort order, they are estimable directly via
    // either histograms or type counts.
    // TODO: SERVER-94856 to support estimating type-bracketed interval here.
    if (compareTypeTags(startTag, endTag) == 0) {
        if (stats::canEstimateTypeViaHistogram(startTag)) {
            if (stats::compareValues(startTag, startVal, endTag, endVal) == 0) {
                return estimateCardinalityEq(ah, startTag, startVal, includeScalar).card;
            }
            return estimateCardinalityRange(ah,
                                            startInclusive,
                                            startTag,
                                            startVal,
                                            endInclusive,
                                            endTag,
                                            endVal,
                                            includeScalar)
                .card;
        }
        // TODO: SERVER-91639 to support estimating via type counts here.
    }

    // Cardinality estimation for an interval should only be called if interval bounds are
    // histogrammable or estimation by type counts is possible (as implemented in
    // canEstimateInterval). If this is the case, code above should have returned cardinality
    // estimate. Otherwise, tassert is triggered since cardinality estimation was mistakenly invoked
    // on invalid TypeTags.
    MONGO_UNREACHABLE_TASSERT(8870500);
}

}  // namespace mongo::optimizer::cbp::ce
