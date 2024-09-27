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

#include "mongo/db/query/ce/histogram_estimation_impl.h"

namespace mongo::ce {

using stats::Bucket;
using stats::compareValues;
using stats::sameTypeBracket;
using stats::ScalarHistogram;
using stats::valueToDouble;

// --------------------- SCALAR HISTOGRAM ESTIMATION METHODS ---------------------

EstimationResult estimateCardinality(const ScalarHistogram& h,
                                     sbe::value::TypeTags tag,
                                     sbe::value::Value val,
                                     EstimationType type) {
    switch (type) {
        case EstimationType::kGreater:
            return getTotals(h) - estimateCardinality(h, tag, val, EstimationType::kLessOrEqual);

        case EstimationType::kGreaterOrEqual:
            return getTotals(h) - estimateCardinality(h, tag, val, EstimationType::kLess);

        default:
            // Continue.
            break;
    }

    size_t bucketIndex = 0;
    {
        size_t len = h.getBuckets().size();
        while (len > 0) {
            const size_t half = len >> 1;
            const auto [boundTag, boundVal] = h.getBounds().getAt(bucketIndex + half);

            if (compareValues(boundTag, boundVal, tag, val) < 0) {
                bucketIndex += half + 1;
                len -= half + 1;
            } else {
                len = half;
            }
        }
    }
    if (bucketIndex == h.getBuckets().size()) {
        // Value beyond the largest endpoint.
        switch (type) {
            case EstimationType::kEqual:
                return {0.0, 0.0};

            case EstimationType::kLess:
            case EstimationType::kLessOrEqual:
                return getTotals(h);

            default:
                MONGO_UNREACHABLE;
        }
    }

    const Bucket& bucket = h.getBuckets().at(bucketIndex);
    const auto [boundTag, boundVal] = h.getBounds().getAt(bucketIndex);
    const bool isEndpoint = compareValues(boundTag, boundVal, tag, val) == 0;

    if (isEndpoint) {
        switch (type) {
            case EstimationType::kEqual: {
                return {bucket._equalFreq, 1.0};
            }

            case EstimationType::kLess: {
                double resultCard = bucket._cumulativeFreq - bucket._equalFreq;
                double resultNDV = bucket._cumulativeNDV - 1.0;
                return {resultCard, resultNDV};
            }

            case EstimationType::kLessOrEqual: {
                double resultCard = bucket._cumulativeFreq;
                double resultNDV = bucket._cumulativeNDV;
                return {resultCard, resultNDV};
            }

            default:
                MONGO_UNREACHABLE;
        }
    } else {
        return interpolateEstimateInBucket(h, tag, val, type, bucketIndex);
    }
}

EstimationResult estimateCardinalityRange(const ScalarHistogram& histogram,
                                          bool lowInclusive,
                                          sbe::value::TypeTags tagLow,
                                          sbe::value::Value valLow,
                                          bool highInclusive,
                                          sbe::value::TypeTags tagHigh,
                                          sbe::value::Value valHigh) {
    tassert(8870503,
            "The interval must be in ascending order",
            !reversedInterval(tagLow, valLow, tagHigh, valHigh));

    const auto highType = highInclusive ? EstimationType::kLessOrEqual : EstimationType::kLess;
    const auto highEstimate = estimateCardinality(histogram, tagHigh, valHigh, highType);

    const auto lowType = lowInclusive ? EstimationType::kLess : EstimationType::kLessOrEqual;
    const auto lowEstimate = estimateCardinality(histogram, tagLow, valLow, lowType);

    const auto est = highEstimate - lowEstimate;

    // There is a case where we estimate an interval (l, u) that falls entirely in a bucket. In this
    // case, we estimate it as: card(<u) - card(<=l) = card(<=u) - card(=u) - card(<=l)
    // where our estimate for equality within the bucket, card(=u) = rangeFreq/ndv, is larger than
    // card(<=high) - card(<=low).
    // This is problematic, because we will obtain a negative value for 'est'. For now, we solve
    // this by clamping this result to a minimum of 0.0.
    return {std::max(0.0, est.card), std::max(0.0, est.ndv)};
}

EstimationResult getTotals(const ScalarHistogram& h) {
    if (h.empty()) {
        return {0.0, 0.0};
    }

    const Bucket& last = h.getBuckets().back();
    return {last._cumulativeFreq, last._cumulativeNDV};
}

EstimationResult interpolateEstimateInBucket(const ScalarHistogram& h,
                                             sbe::value::TypeTags tag,
                                             sbe::value::Value val,
                                             EstimationType type,
                                             size_t bucketIndex) {

    const Bucket& bucket = h.getBuckets().at(bucketIndex);
    const auto [boundTag, boundVal] = h.getBounds().getAt(bucketIndex);

    double resultCard = bucket._cumulativeFreq - bucket._equalFreq - bucket._rangeFreq;
    double resultNDV = bucket._cumulativeNDV - bucket._ndv - 1.0;

    // Check if the estimate is at the point of type brackets switch. If the current bucket is the
    // first bucket of a new type bracket and the value is of another type, estimate cardinality
    // from the current bucket as 0.
    //
    // For example, let bound 1 = 1000, bound 2 = "abc". The value 100000000 falls in bucket 2 the
    // first bucket for strings, but should not get cardinality/ ndv fraction from it.
    if (!sameTypeBracket(tag, boundTag)) {
        if (type == EstimationType::kEqual) {
            return {0.0, 0.0};
        } else {
            return {resultCard, resultNDV};
        }
    }

    // Estimate for equality frequency inside of the bucket.
    const double innerEqFreq = (bucket._ndv == 0.0) ? 0.0 : bucket._rangeFreq / bucket._ndv;

    if (type == EstimationType::kEqual) {
        return {innerEqFreq, 1.0};
    }

    // If the value is minimal for its type, and the operation is $lt or $lte return cardinality up
    // to the previous bucket.
    auto&& [minConstant, inclusive] = getMinMaxBoundForType(true /*isMin*/, tag);
    auto [minTag, minVal] = *getConstTypeVal(*minConstant);
    if (compareValues(minTag, minVal, tag, val) == 0) {
        return {resultCard, resultNDV};
    }

    // For $lt and $lte operations use linear interpolation to take a fraction of the bucket
    // cardinality and NDV if there is a preceeding bucket with bound of the same type. Use half of
    // the bucket estimates otherwise.
    double ratio = 0.5;
    if (bucketIndex > 0) {
        const auto [lowBoundTag, lowBoundVal] = h.getBounds().getAt(bucketIndex - 1);
        if (sameTypeBracket(lowBoundTag, boundTag)) {
            double doubleLowBound = valueToDouble(lowBoundTag, lowBoundVal);
            double doubleUpperBound = valueToDouble(boundTag, boundVal);
            double doubleVal = valueToDouble(tag, val);
            ratio = (doubleVal - doubleLowBound) / (doubleUpperBound - doubleLowBound);
        }
    }

    const double bucketFreqRatio = bucket._rangeFreq * ratio;
    resultCard += bucketFreqRatio;
    resultNDV += bucket._ndv * ratio;

    if (type == EstimationType::kLess) {
        // Subtract from the estimate the cardinality and ndv corresponding to the equality
        // operation, if they are larger than the ratio taken from this bucket.
        const double innerEqFreqCorrection = (bucketFreqRatio < innerEqFreq) ? 0.0 : innerEqFreq;
        const double innerEqNdv = (bucket._ndv * ratio <= 1.0) ? 0.0 : 1.0;
        resultCard -= innerEqFreqCorrection;
        resultNDV -= innerEqNdv;
    }
    return {resultCard, resultNDV};
}

EstimationResult estimateRangeQueryOnArray(const ScalarHistogram& histogramAmin,
                                           const ScalarHistogram& histogramAmax,
                                           bool lowInclusive,
                                           sbe::value::TypeTags tagLow,
                                           sbe::value::Value valLow,
                                           bool highInclusive,
                                           sbe::value::TypeTags tagHigh,
                                           sbe::value::Value valHigh) {
    const EstimationType highType =
        highInclusive ? EstimationType::kLessOrEqual : EstimationType::kLess;
    const EstimationResult highEstimate =
        estimateCardinality(histogramAmin, tagHigh, valHigh, highType);

    const EstimationType lowType =
        lowInclusive ? EstimationType::kLess : EstimationType::kLessOrEqual;
    const EstimationResult lowEstimate =
        estimateCardinality(histogramAmax, tagLow, valLow, lowType);

    return highEstimate - lowEstimate;
}

// --------------------- ARRAY HISTOGRAM ESTIMATION METHODS ---------------------

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

}  // namespace mongo::ce
