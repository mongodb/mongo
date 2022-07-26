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

    switch (type) {
        case EstimationType::kEqual: {
            if (isEndpoint) {
                return {bucket._equalFreq, 1.0};
            }
            return {(bucket._ndv == 0.0) ? 0.0 : bucket._rangeFreq / bucket._ndv, 1.0};
        }

        case EstimationType::kLess: {
            double resultCard = bucket._cumulativeFreq - bucket._equalFreq;
            double resultNDV = bucket._cumulativeNDV - 1.0;

            if (!isEndpoint) {
                // TODO: consider value interpolation instead of assigning 50% of the weight.
                resultCard -= bucket._rangeFreq / 2.0;
                resultNDV -= bucket._ndv / 2.0;
            }
            return {resultCard, resultNDV};
        }

        case EstimationType::kLessOrEqual: {
            double resultCard = bucket._cumulativeFreq;
            double resultNDV = bucket._cumulativeNDV;

            if (!isEndpoint) {
                // TODO: consider value interpolation instead of assigning 50% of the weight.
                resultCard -= bucket._equalFreq + bucket._rangeFreq / 2.0;
                resultNDV -= 1.0 + bucket._ndv / 2.0;
            }
            return {resultCard, resultNDV};
        }

        default:
            MONGO_UNREACHABLE;
    }
}

/**
 * Estimates the cardinality of an equality predicate given an ArrayHistogram and an SBE value and
 * type tag pair.
 */
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

/**
 * Estimates the cardinality of a range predicate given an ArrayHistogram and a range predicate.
 * Set 'includeScalar' to true to indicate whether or not the provided range should include no-array
 * values. The other fields define the range of the estimation.
 */
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
