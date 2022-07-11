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

#include "mongo/db/query/ce/ce_estimation.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/query/optimizer/syntax/expr.h"

namespace mongo::ce {
using namespace sbe;

/**
 * Estimates the cardinality of an equality predicate given an ArrayHistogram and an SBE value and
 * type tag pair.
 */
double estimateCardEq(const ArrayHistogram& ah, value::TypeTags tag, value::Value val) {
    if (tag != value::TypeTags::Null) {
        double card = ah.getScalar().estimate(tag, val, EstimationType::kEqual)._card;
        if (ah.isArray()) {
            return card + ah.getArrayUnique().estimate(tag, val, EstimationType::kEqual)._card;
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

static EstimationResult estimateRange(const Histogram& histogram,
                                      bool lowInclusive,
                                      value::TypeTags tagLow,
                                      value::Value valLow,
                                      bool highInclusive,
                                      value::TypeTags tagHigh,
                                      value::Value valHigh) {
    const EstimationType highType =
        highInclusive ? EstimationType::kLessOrEqual : EstimationType::kLess;
    const EstimationResult highEstimate = histogram.estimate(tagHigh, valHigh, highType);

    const EstimationType lowType =
        lowInclusive ? EstimationType::kLess : EstimationType::kLessOrEqual;
    const EstimationResult lowEstimate = histogram.estimate(tagLow, valLow, lowType);

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
    auto estRange = [&](const Histogram& h) {
        return estimateRange(h, lowInclusive, tagLow, valLow, highInclusive, tagHigh, valHigh);
    };

    double result = 0.0;
    if (ah.isArray()) {
        const auto arrayMinEst = estRange(ah.getArrayMin());
        const auto arrayMaxEst = estRange(ah.getArrayMax());
        const auto arrayUniqueEst = estRange(ah.getArrayUnique());

        // TODO: should we consider diving by sqrt(ndv) or just by ndv?
        const double arrayUniqueDensity = (arrayUniqueEst._ndv == 0.0)
            ? 0.0
            : (arrayUniqueEst._card / std::sqrt(arrayUniqueEst._ndv));

        result = std::max(std::max(arrayMinEst._card, arrayMaxEst._card), arrayUniqueDensity);
    }

    if (includeScalar) {
        const auto scalarEst = estRange(ah.getScalar());
        result += scalarEst._card;
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
        ce::SBEValue sbeValue = getBound(interval.getLowBound());
        auto cardinality = estimateCardEq(ah, sbeValue.getTag(), sbeValue.getValue());
        return cardinality;
    }

    // Otherwise, we have a range.
    ce::SBEValue lowSBEValue(sbe::value::TypeTags::MinKey, 0);
    auto lowBound = interval.getLowBound();
    if (!lowBound.isInfinite()) {
        lowSBEValue = getBound(lowBound);
    }

    ce::SBEValue highSBEValue(sbe::value::TypeTags::MaxKey, 0);
    auto highBound = interval.getHighBound();
    if (!highBound.isInfinite()) {
        highSBEValue = getBound(highBound);
    }

    return estimateCardRange(ah,
                             true /*includeScalar*/,
                             lowBound.isInclusive(),
                             lowSBEValue.getTag(),
                             lowSBEValue.getValue(),
                             highBound.isInclusive(),
                             highSBEValue.getTag(),
                             highSBEValue.getValue());
}

}  // namespace mongo::ce
