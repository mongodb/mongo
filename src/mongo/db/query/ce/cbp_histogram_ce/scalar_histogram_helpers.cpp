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

#include "mongo/db/query/ce/cbp_histogram_ce/scalar_histogram_helpers.h"
#include "mongo/db/query/stats/value_utils.h"

namespace mongo::optimizer::cbp::ce {

using stats::Bucket;
using stats::compareValues;
using stats::sameTypeBracket;
using stats::ScalarHistogram;
using stats::valueToDouble;

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
    auto [minTag, minVal] =
        *mongo::optimizer::ce::getConstTypeVal(*minConstant);  // TODO: fix this (clean namespaces)
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

}  // namespace mongo::optimizer::cbp::ce
