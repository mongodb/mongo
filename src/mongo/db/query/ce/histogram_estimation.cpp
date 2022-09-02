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

#include "mongo/db/query/ce/histogram_estimation.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/query/optimizer/syntax/expr.h"

namespace mongo::ce {
using namespace sbe;
namespace {

bool sameTypeBracket(value::TypeTags tag1, value::TypeTags tag2) {
    if (tag1 == tag2) {
        return true;
    }
    return ((value::isNumber(tag1) && value::isNumber(tag2)) ||
            (value::isString(tag1) && value::isString(tag2)));
}

double valueToDouble(value::TypeTags tag, value::Value val) {
    double result = 0;
    if (value::isNumber(tag)) {
        result = value::numericCast<double>(tag, val);
    } else if (value::isString(tag)) {
        const StringData sd = value::getStringView(tag, val);

        // Convert a prefix of the string to a double.
        const size_t maxPrecision = std::min(sd.size(), sizeof(double));
        for (size_t i = 0; i < maxPrecision; ++i) {
            const char ch = sd[i];
            const double charToDbl = ch / std::pow(2, i * 8);
            result += charToDbl;
        }
    } else {
        uassert(6844500, "Unexpected value type", false);
    }

    return result;
}

int32_t compareValues3w(value::TypeTags tag1,
                        value::Value val1,
                        value::TypeTags tag2,
                        value::Value val2) {
    const auto [compareTag, compareVal] = value::compareValue(tag1, val1, tag2, val2);
    uassert(6695716, "Invalid comparison result", compareTag == value::TypeTags::NumberInt32);
    return value::bitcastTo<int32_t>(compareVal);
}
}  // namespace

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

    resultCard += bucket._rangeFreq * ratio;
    resultNDV += bucket._ndv * ratio;

    if (type == EstimationType::kLess) {
        // Subtract from the estimate the cardinality and ndv corresponding to the equality
        // operation.
        const double innerEqNdv = (bucket._ndv * ratio <= 1.0) ? 0.0 : 1.0;
        resultCard -= innerEqFreq;
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

            if (compareValues3w(boundTag, boundVal, tag, val) < 0) {
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
    const bool isEndpoint = compareValues3w(boundTag, boundVal, tag, val) == 0;

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

double estimateCardEq(const ArrayHistogram& ah, value::TypeTags tag, value::Value val) {
    if (tag != value::TypeTags::Null) {
        double card = estimate(ah.getScalar(), tag, val, EstimationType::kEqual).card;
        if (ah.isArray()) {
            return card + estimate(ah.getArrayUnique(), tag, val, EstimationType::kEqual).card;
        }
        return card;
    } else {
        // Predicate: {field: null}
        // Count the values that are either null or that do not contain the field.
        // TODO:
        // This prototype doesn't have the concept of missing values. It can be added easily
        // by adding a cardinality estimate that is >= the number of values.
        // Estimation of $exists can be built on top of this estimate:
        // {$exists: true} matches the documents that contain the field, including those where the
        // field value is null.
        // {$exists: false} matches only the documents that do not contain the field.
        auto findNull = ah.getTypeCounts().find(value::TypeTags::Null);
        if (findNull != ah.getTypeCounts().end()) {
            return findNull->second;
        }
        return 0.0;
    }
}

static EstimationResult estimateRange(const ScalarHistogram& histogram,
                                      bool lowInclusive,
                                      value::TypeTags tagLow,
                                      value::Value valLow,
                                      bool highInclusive,
                                      value::TypeTags tagHigh,
                                      value::Value valHigh) {
    const EstimationType highType =
        highInclusive ? EstimationType::kLessOrEqual : EstimationType::kLess;
    const EstimationResult highEstimate = estimate(histogram, tagHigh, valHigh, highType);

    const EstimationType lowType =
        lowInclusive ? EstimationType::kLess : EstimationType::kLessOrEqual;
    const EstimationResult lowEstimate = estimate(histogram, tagLow, valLow, lowType);

    return highEstimate - lowEstimate;
}

double estimateCardRange(const ArrayHistogram& ah,
                         bool includeScalar,
                         /* Define lower bound. */
                         bool lowInclusive,
                         value::TypeTags tagLow,
                         value::Value valLow,
                         /* Define upper bound. */
                         bool highInclusive,
                         value::TypeTags tagHigh,
                         value::Value valHigh) {
    uassert(6695701,
            "Low bound must not be higher than high",
            compareValues3w(tagLow, valLow, tagHigh, valHigh) <= 0);

    // Helper lambda to shorten code for legibility.
    auto estRange = [&](const ScalarHistogram& h) {
        return estimateRange(h, lowInclusive, tagLow, valLow, highInclusive, tagHigh, valHigh);
    };

    double result = 0.0;
    if (ah.isArray()) {
        const auto arrayMinEst = estRange(ah.getArrayMin());
        const auto arrayMaxEst = estRange(ah.getArrayMax());
        const auto arrayUniqueEst = estRange(ah.getArrayUnique());

        // TODO: should we consider diving by sqrt(ndv) or just by ndv?
        const double arrayUniqueDensity = (arrayUniqueEst.ndv == 0.0)
            ? 0.0
            : (arrayUniqueEst.card / std::sqrt(arrayUniqueEst.ndv));

        result = std::max(std::max(arrayMinEst.card, arrayMaxEst.card), arrayUniqueDensity);
    }

    if (includeScalar) {
        const auto scalarEst = estRange(ah.getScalar());
        result += scalarEst.card;
    }

    return result;
}

double estimateIntervalCardinality(const ce::ArrayHistogram& ah,
                                   const optimizer::IntervalRequirement& interval,
                                   optimizer::CEType childResult) {
    auto getBound = [](const optimizer::BoundRequirement& boundReq) {
        return boundReq.getBound().cast<optimizer::Constant>()->get();
    };

    if (interval.isFullyOpen()) {
        return childResult;
    } else if (interval.isEquality()) {
        auto [tag, val] = getBound(interval.getLowBound());
        return estimateCardEq(ah, tag, val);
    }

    // Otherwise, we have a range.
    auto lowBound = interval.getLowBound();
    auto [lowTag, lowVal] = getBound(lowBound);

    auto highBound = interval.getHighBound();
    auto [highTag, highVal] = getBound(highBound);

    return estimateCardRange(ah,
                             true /*includeScalar*/,
                             lowBound.isInclusive(),
                             lowTag,
                             lowVal,
                             highBound.isInclusive(),
                             highTag,
                             highVal);
}

}  // namespace mongo::ce
