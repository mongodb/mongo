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

#include "mongo/db/query/ce/histogram_predicate_estimation.h"

#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/pipeline/abt/utils.h"

#include "mongo/db/query/ce/bound_utils.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/utils/ce_math.h"
#include "mongo/db/query/stats/value_utils.h"

namespace mongo::optimizer::ce {
namespace value = sbe::value;

using stats::ArrayHistogram;
using stats::Bucket;
using stats::compareValues;
using stats::sameTypeBracket;
using stats::ScalarHistogram;
using stats::valueToDouble;

SelectivityType getSelectivity(const ArrayHistogram& ah, CEType cardinality) {
    const CEType sampleSize = {ah.getSampleSize()};
    if (sampleSize == 0.0) {
        return {0.0};
    }
    return cardinality / sampleSize;
}

SelectivityType getArraySelectivity(const ArrayHistogram& ah) {
    return getSelectivity(ah, {ah.getArrayCount()});
}

EstimationResult getTotals(const ScalarHistogram& h) {
    if (h.empty()) {
        return {0.0, 0.0};
    }

    const Bucket& last = h.getBuckets().back();
    return {last._cumulativeFreq, last._cumulativeNDV};
}

/**
 * Helper function that uses linear interpolation to estimate the cardinality and NDV for a value
 * that falls inside of a histogram bucket.
 */
EstimationResult interpolateEstimateInBucket(const ScalarHistogram& h,
                                             value::TypeTags tag,
                                             value::Value val,
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
    // For example, let bound 1 = 1000, bound 2 = "abc". The value 100000000 falls in bucket 2, the
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

EstimationResult estimate(const ScalarHistogram& h,
                          value::TypeTags tag,
                          value::Value val,
                          EstimationType type) {
    switch (type) {
        case EstimationType::kGreater:
            return getTotals(h) - estimate(h, tag, val, EstimationType::kLessOrEqual);

        case EstimationType::kGreaterOrEqual:
            return getTotals(h) - estimate(h, tag, val, EstimationType::kLess);

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

/**
 * Estimates equality to the given tag/value using histograms.
 */
CEType estimateCardEq(const ArrayHistogram& ah,
                      value::TypeTags tag,
                      value::Value val,
                      bool includeScalar) {
    double card = 0.0;
    if (includeScalar) {
        card = estimate(ah.getScalar(), tag, val, EstimationType::kEqual).card;
    }
    if (ah.isArray()) {
        card += estimate(ah.getArrayUnique(), tag, val, EstimationType::kEqual).card;
    }
    return {card};
}

static EstimationResult estimateRange(const ScalarHistogram& histogram,
                                      bool lowInclusive,
                                      value::TypeTags tagLow,
                                      value::Value valLow,
                                      bool highInclusive,
                                      value::TypeTags tagHigh,
                                      value::Value valHigh) {
    const auto highType = highInclusive ? EstimationType::kLessOrEqual : EstimationType::kLess;
    const auto highEstimate = estimate(histogram, tagHigh, valHigh, highType);

    const auto lowType = lowInclusive ? EstimationType::kLess : EstimationType::kLessOrEqual;
    const auto lowEstimate = estimate(histogram, tagLow, valLow, lowType);

    const auto est = highEstimate - lowEstimate;

    // There is a case where we estimate an interval (l, u) that falls entirely in a bucket. In this
    // case, we estimate it as: card(<u) - card(<=l) = card(<=u) - card(=u) - card(<=l)
    // where our estimate for equality within the bucket, card(=u) = rangeFreq/ndv, is larger than
    // card(<=high) - card(<=low).
    // This is problematic, because we will obtain a negative value for 'est'. For now, we solve
    // this by clamping this result to a minimum of 0.0.
    return {std::max(0.0, est.card), std::max(0.0, est.ndv)};
}

/**
 * Compute an estimate for range query on array data with formula:
 * Card(ArrayMin(a < valHigh)) - Card(ArrayMax(a < valLow))
 */
static EstimationResult estimateRangeQueryOnArray(const ScalarHistogram& histogramAmin,
                                                  const ScalarHistogram& histogramAmax,
                                                  bool lowInclusive,
                                                  value::TypeTags tagLow,
                                                  value::Value valLow,
                                                  bool highInclusive,
                                                  value::TypeTags tagHigh,
                                                  value::Value valHigh) {
    const EstimationType highType =
        highInclusive ? EstimationType::kLessOrEqual : EstimationType::kLess;
    const EstimationResult highEstimate = estimate(histogramAmin, tagHigh, valHigh, highType);

    const EstimationType lowType =
        lowInclusive ? EstimationType::kLess : EstimationType::kLessOrEqual;
    const EstimationResult lowEstimate = estimate(histogramAmax, tagLow, valLow, lowType);

    return highEstimate - lowEstimate;
}

CEType estimateCardRange(const ArrayHistogram& ah,
                         /* Define lower bound. */
                         bool lowInclusive,
                         value::TypeTags tagLow,
                         value::Value valLow,
                         /* Define upper bound. */
                         bool highInclusive,
                         value::TypeTags tagHigh,
                         value::Value valHigh,
                         bool includeScalar,
                         EstimationAlgo estimationAlgo) {
    uassert(6695701,
            "Low bound must not be higher than high",
            compareValues(tagLow, valLow, tagHigh, valHigh) <= 0);

    // Helper lambda to shorten code for legibility.
    auto estRange = [&](const ScalarHistogram& h) {
        return estimateRange(h, lowInclusive, tagLow, valLow, highInclusive, tagHigh, valHigh);
    };

    double result = 0.0;
    if (ah.isArray()) {

        if (includeScalar) {
            // Range query on array data.
            const EstimationResult rangeCardOnArray = estimateRangeQueryOnArray(ah.getArrayMin(),
                                                                                ah.getArrayMax(),
                                                                                lowInclusive,
                                                                                tagLow,
                                                                                valLow,
                                                                                highInclusive,
                                                                                tagHigh,
                                                                                valHigh);
            result += rangeCardOnArray.card;
        } else {
            // $elemMatch query on array data.
            const auto arrayMinEst = estRange(ah.getArrayMin());
            const auto arrayMaxEst = estRange(ah.getArrayMax());
            const auto arrayUniqueEst = estRange(ah.getArrayUnique());

            // ToDo: try using ah.getArrayCount() - ah.getEmptyArrayCount();
            // when the number of empty arrays is provided by the statistics.
            const double totalArrayCount = ah.getArrayCount();

            uassert(
                6715101, "Array histograms should contain at least one array", totalArrayCount > 0);
            switch (estimationAlgo) {
                case EstimationAlgo::HistogramV1: {
                    const double arrayUniqueDensity = (arrayUniqueEst.ndv == 0.0)
                        ? 0.0
                        : (arrayUniqueEst.card / std::sqrt(arrayUniqueEst.ndv));
                    result =
                        std::max(std::max(arrayMinEst.card, arrayMaxEst.card), arrayUniqueDensity);
                    break;
                }
                case EstimationAlgo::HistogramV2: {
                    const double avgArraySize =
                        getTotals(ah.getArrayUnique()).card / totalArrayCount;
                    const double adjustedUniqueCard = (avgArraySize == 0.0)
                        ? 0.0
                        : std::min(arrayUniqueEst.card / pow(avgArraySize, 0.2), totalArrayCount);
                    result =
                        std::max(std::max(arrayMinEst.card, arrayMaxEst.card), adjustedUniqueCard);
                    break;
                }
                case EstimationAlgo::HistogramV3: {
                    const double adjustedUniqueCard =
                        0.85 * std::min(arrayUniqueEst.card, totalArrayCount);
                    result =
                        std::max(std::max(arrayMinEst.card, arrayMaxEst.card), adjustedUniqueCard);
                    break;
                }
                default:
                    MONGO_UNREACHABLE;
            }
        }
    }

    if (includeScalar) {
        const auto scalarEst = estRange(ah.getScalar());
        result += scalarEst.card;
    }

    return {result};
}

SelectivityType estimateSelEq(const stats::ArrayHistogram& ah,
                              sbe::value::TypeTags tag,
                              sbe::value::Value val,
                              bool includeScalar) {
    const auto ce = estimateCardEq(ah, tag, val, includeScalar);
    return getSelectivity(ah, ce);
}

SelectivityType estimateSelRange(const stats::ArrayHistogram& ah,
                                 bool lowInclusive,
                                 sbe::value::TypeTags tagLow,
                                 sbe::value::Value valLow,
                                 bool highInclusive,
                                 sbe::value::TypeTags tagHigh,
                                 sbe::value::Value valHigh,
                                 bool includeScalar,
                                 EstimationAlgo estAlgo) {
    const auto ce = estimateCardRange(
        ah, lowInclusive, tagLow, valLow, highInclusive, tagHigh, valHigh, includeScalar, estAlgo);
    return getSelectivity(ah, ce);
}

}  // namespace mongo::optimizer::ce
