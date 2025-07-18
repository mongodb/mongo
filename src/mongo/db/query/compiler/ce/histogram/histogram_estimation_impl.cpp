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

#include "mongo/db/query/compiler/ce/histogram/histogram_estimation_impl.h"

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/query/compiler/stats/value_utils.h"

namespace mongo::ce {

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
    auto&& [min, inclusive] = stats::getMinBound(tag);
    if (compareValues(min.getTag(), min.getValue(), tag, val) == 0) {
        return {resultCard, resultNDV};
    }

    // For $lt and $lte operations use linear interpolation to take a fraction of the bucket
    // cardinality and NDV if there is a preceeding bucket with bound of the same type. Use half of
    // the bucket estimates otherwise.
    double ratio = 0.5;
    if (bucketIndex > 0) {
        const auto [lowBoundTag, lowBoundVal] = h.getBounds().getAt(bucketIndex - 1);
        if (sameTypeBracket(lowBoundTag, boundTag) &&
            !mongo::sbe::value::isInfinity(lowBoundTag, lowBoundVal)) {
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

boost::optional<EstimationResult> estimateCardinalityEqViaTypeCounts(
    const stats::CEHistogram& ceHist, sbe::value::TypeTags tag, sbe::value::Value val) {
    EstimationResult estimation = {0.0 /*card*/, 0.0 /*ndv*/};
    if (tag == sbe::value::TypeTags::Boolean) {
        if (stats::isTrueBool(tag, val)) {
            estimation.card = ceHist.getTrueCount();
        } else {
            estimation.card = ceHist.getFalseCount();
        }
        return estimation;
    } else if (tag == sbe::value::TypeTags::Array && stats::isEmptyArray(tag, val)) {
        estimation.card = ceHist.getEmptyArrayCount();
        return estimation;
    } else if (tag == sbe::value::TypeTags::Null) {
        // An interval [null, null] should match both values from null and nothing.
        estimation.card = ceHist.getTypeCount(tag);
        estimation.card += ceHist.getTypeCount(sbe::value::TypeTags::Nothing);
        return estimation;
    } else if ((tag == sbe::value::TypeTags::MinKey) || (tag == sbe::value::TypeTags::MaxKey) ||
               (tag == sbe::value::TypeTags::bsonUndefined)) {
        // These types have a single possible value. We can estimate the cardinality from the type
        // counts.
        auto nTypeCounts = ceHist.getTypeCounts().find(tag);
        if (nTypeCounts != ceHist.getTypeCounts().end()) {
            estimation.card = nTypeCounts->second;
        }
        // If the type was not found in the typeCounts, means that there are no instances;
        return estimation;
    } else if (sbe::value::isNumber(tag) && sbe::value::isNaN(tag, val)) {
        estimation.card = ceHist.getNanCount();
        return estimation;
    }
    return boost::none;
}

boost::optional<EstimationResult> estimateCardinalityRangeViaTypeCounts(
    const stats::CEHistogram& ceHist,
    bool lowInclusive,
    sbe::value::TypeTags tagLow,
    sbe::value::Value valLow,
    bool highInclusive,
    sbe::value::TypeTags tagHigh,
    sbe::value::Value valHigh) {

    // The values must differ and the values have to be in order.
    tassert(9163902,
            "Order of values invalid for type count estimation",
            stats::compareValues(tagLow, valLow, tagHigh, valHigh) < 0);

    EstimationResult estimation = {0.0 /*card*/, 0.0 /*ndv*/};

    if (tagLow == sbe::value::TypeTags::Boolean &&
        stats::sameTypeBracketInterval(tagLow, highInclusive, tagHigh, valHigh)) {

        // If the types of the bounds are both Boolean, check the inclusivity fo the bounds to
        // accumulate the correct counts of true/false values. We assume that the two values are
        // different and that when sorted False < True.
        // E.g., [False, True], [False, True)

        if (lowInclusive) {
            estimation.card += ceHist.getFalseCount();
        }

        if (highInclusive) {
            estimation.card += ceHist.getTrueCount();
        }

        return estimation;
    } else {

        // For all other types, the interval has to fully cover a full type.
        // E.g., an interval covering all strings: ["", {}), the lower bound is inclusive whereas
        // the uppper bound is non inclusive.

        if (stats::isFullBracketInterval(
                tagLow, valLow, lowInclusive, tagHigh, valHigh, highInclusive)) {

            // Responding to cardinality estimations using TypeCounts, StringSmall and StringBig are
            // considered to be the same type.
            std::vector<sbe::value::TypeTags> toAdd;
            switch (tagLow) {
                case sbe::value::TypeTags::StringSmall:
                case sbe::value::TypeTags::StringBig:
                    toAdd.push_back(sbe::value::TypeTags::StringSmall);
                    toAdd.push_back(sbe::value::TypeTags::StringBig);
                    break;
                case sbe::value::TypeTags::Null:
                    // An interval [null, null] should match both values from null and nothing.
                    toAdd.push_back(sbe::value::TypeTags::Null);
                    toAdd.push_back(sbe::value::TypeTags::Nothing);
                    break;
                default:
                    toAdd.push_back(tagLow);
                    break;
            }

            bool set = false;
            for (auto tag : toAdd) {
                auto typeCounts = ceHist.getTypeCounts().find(tag);
                if (typeCounts != ceHist.getTypeCounts().end()) {
                    estimation.card += typeCounts->second;
                    set = true;
                }
            }

            if (set) {
                return estimation;
            }
        }
    }
    return boost::none;
}

EstimationResult estimateCardinalityEq(const stats::CEHistogram& ceHist,
                                       sbe::value::TypeTags tag,
                                       sbe::value::Value val,
                                       bool includeScalar) {
    // Try to estimate via type counts.
    auto typeCountEstimation = estimateCardinalityEqViaTypeCounts(ceHist, tag, val);
    if (typeCountEstimation) {
        return typeCountEstimation.get();
    }

    EstimationResult estimation = {0.0 /*card*/, 0.0 /*ndv*/};

    // Estimate cardinality for fields containing scalar values if includeScalar is true.
    if (includeScalar) {
        estimation = estimateCardinality(ceHist.getScalar(), tag, val, EstimationType::kEqual);
    }

    // If histogram includes array data points, calculate cardinality for fields containing array
    // values.
    if (ceHist.isArray()) {
        estimation +=
            estimateCardinality(ceHist.getArrayUnique(), tag, val, EstimationType::kEqual);
    }
    return estimation;
}

EstimationResult estimateCardinalityRange(const stats::CEHistogram& ceHist,
                                          bool lowInclusive,
                                          sbe::value::TypeTags tagLow,
                                          sbe::value::Value valLow,
                                          bool highInclusive,
                                          sbe::value::TypeTags tagHigh,
                                          sbe::value::Value valHigh,
                                          bool includeScalar,
                                          ArrayRangeEstimationAlgo arrayRangeEstimationAlgo,
                                          ArrayExactRangeEstimationAlgo estimationAlgo) {
    tassert(8870502,
            "The interval must be in ascending order",
            !reversedInterval(tagLow, valLow, tagHigh, valHigh));

    // Try to estimate via type counts.
    auto typeCountEstimation = estimateCardinalityRangeViaTypeCounts(
        ceHist, lowInclusive, tagLow, valLow, highInclusive, tagHigh, valHigh);
    if (typeCountEstimation) {
        return typeCountEstimation.get();
    }

    // Helper lambda to shorten code for legibility.
    auto estRange = [&](const stats::ScalarHistogram& h) {
        return estimateCardinalityRange(
            h, lowInclusive, tagLow, valLow, highInclusive, tagHigh, valHigh);
    };

    EstimationResult estimation = {0.0 /*card*/, 0.0 /*ndv*/};

    if (ceHist.isArray()) {

        // Queries refering to array fields fall to these two categories:
        // i. kExactArrayCE. For $elemMatch queries we estimate the number of arrays that contain at
        // least one value within the interval.
        // ii. kConjunctArrayCE. For other queries, the intervals {{$gt: a} OR {$lt: b}} intervals
        // are traslated by IndexBoundBuilder to {-inf, a} AND {b, +inf}, thus similarly to (i) we
        // estimate the arrays that contain at least one value within the interval.
        switch (arrayRangeEstimationAlgo) {
            case ArrayRangeEstimationAlgo::kExactArrayCE: {
                const auto arrayMinEst = estRange(ceHist.getArrayMin());
                const auto arrayMaxEst = estRange(ceHist.getArrayMax());
                const auto arrayUniqueEst = estRange(ceHist.getArrayUnique());

                const double totalArrayCount = ceHist.getArrayCount() - ceHist.getEmptyArrayCount();

                uassert(9160701,
                        "Array histograms should contain at least one array",
                        totalArrayCount > 0);
                switch (estimationAlgo) {
                    case ArrayExactRangeEstimationAlgo::HistogramV1: {
                        const double arrayUniqueDensity = (arrayUniqueEst.ndv == 0.0)
                            ? 0.0
                            : (arrayUniqueEst.card / std::sqrt(arrayUniqueEst.ndv));
                        estimation = {std::max(std::max(arrayMinEst.card, arrayMaxEst.card),
                                               arrayUniqueDensity),
                                      0.0};
                        break;
                    }
                    case ArrayExactRangeEstimationAlgo::HistogramV2: {
                        const double avgArraySize =
                            getTotals(ceHist.getArrayUnique()).card / totalArrayCount;
                        const double adjustedUniqueCard = (avgArraySize == 0.0)
                            ? 0.0
                            : std::min(arrayUniqueEst.card / pow(avgArraySize, 0.2),
                                       totalArrayCount);
                        estimation = {std::max(std::max(arrayMinEst.card, arrayMaxEst.card),
                                               adjustedUniqueCard),
                                      0.0};
                        break;
                    }
                    case ArrayExactRangeEstimationAlgo::HistogramV3: {
                        const double adjustedUniqueCard =
                            0.85 * std::min(arrayUniqueEst.card, totalArrayCount);
                        estimation = {std::max(std::max(arrayMinEst.card, arrayMaxEst.card),
                                               adjustedUniqueCard),
                                      0.0};
                        break;
                    }
                    default:
                        MONGO_UNREACHABLE;
                }
                break;
            }
            case ArrayRangeEstimationAlgo::kConjunctArrayCE: {
                const EstimationType highType =
                    highInclusive ? EstimationType::kLessOrEqual : EstimationType::kLess;
                const EstimationResult highEstimate =
                    estimateCardinality(ceHist.getArrayMin(), tagHigh, valHigh, highType);

                const EstimationType lowType =
                    lowInclusive ? EstimationType::kLess : EstimationType::kLessOrEqual;
                const EstimationResult lowEstimate =
                    estimateCardinality(ceHist.getArrayMax(), tagLow, valLow, lowType);

                estimation += (highEstimate - lowEstimate);
                break;
            }
            default:
                MONGO_UNREACHABLE;
        }
    }

    if (includeScalar) {
        estimation += estRange(ceHist.getScalar());
    }

    return estimation;
}

/**
 * Estimates the cardinality of a given interval using either histograms or type counts.
 *
 * If the interval is a point interval, the function estimates the cardinality
 * for that single value. Otherwise, it estimates the cardinality for the range between the
 * start and end values.
 *
 * Assumptions:
 * a) The interval must be in ascending direction, meaning the start value is less than or equal to
 *    the end value.
 * b) The interval must satisfy sameTypeInterval() and the type must be estimable.
 */
CardinalityEstimate estimateIntervalCardinality(const stats::CEHistogram& ceHist,
                                                bool startInclusive,
                                                sbe::value::TypeTags startTag,
                                                sbe::value::Value startVal,
                                                bool endInclusive,
                                                sbe::value::TypeTags endTag,
                                                sbe::value::Value endVal,
                                                ArrayRangeEstimationAlgo arrayRangeEstimationAlgo,
                                                bool includeScalar) {
    // The following conditions should have been satisfied before reaching this point. We only
    // enable the following checks in a debug build.
    if constexpr (kDebugBuild) {
        tassert(
            9485500,
            str::stream() << "Interval cannot be empty: "
                          << stats::printInterval(
                                 startInclusive, startTag, startVal, endInclusive, endTag, endVal),
            !stats::isEmptyInterval(
                startTag, startVal, startInclusive, endTag, endVal, endInclusive));

        // If 'startTag' and 'endTag' are either in the same type or type-bracketed, they
        // are estimable directly via either histograms or type counts.
        tassert(
            9485501,
            str::stream() << "Interval must be bracketized: "
                          << stats::printInterval(
                                 startInclusive, startTag, startVal, endInclusive, endTag, endVal),
            stats::sameTypeBracketInterval(startTag, endInclusive, endTag, endVal));

        tassert(
            9485502,
            str::stream() << "Interval must be estimable using either histogram or type counts: "
                          << stats::printInterval(
                                 startInclusive, startTag, startVal, endInclusive, endTag, endVal),
            stats::canEstimateTypeViaHistogram(startTag) ||
                stats::canEstimateIntervalViaTypeCounts(
                    startTag, startVal, startInclusive, endTag, endVal, endInclusive));
    }

    bool isPointInterval = stats::compareValues(startTag, startVal, endTag, endVal) == 0;
    if (isPointInterval) {
        return CardinalityEstimate{
            CardinalityType{estimateCardinalityEq(ceHist, startTag, startVal, includeScalar).card},
            EstimationSource::Histogram};
    }

    return CardinalityEstimate{CardinalityType{estimateCardinalityRange(ceHist,
                                                                        startInclusive,
                                                                        startTag,
                                                                        startVal,
                                                                        endInclusive,
                                                                        endTag,
                                                                        endVal,
                                                                        includeScalar,
                                                                        arrayRangeEstimationAlgo)
                                                   .card},
                               EstimationSource::Histogram};
}

bool canEstimateInterval(bool isArray,
                         bool startInclusive,
                         sbe::value::TypeTags startTag,
                         sbe::value::Value startVal,
                         bool endInclusive,
                         sbe::value::TypeTags endTag,
                         sbe::value::Value endVal) {
    tassert(9744900,
            "The interval must be in ascending order",
            !reversedInterval(startTag, startVal, endTag, endVal));

    // If 'startTag' and 'endTag' are either in the same type or type-bracketed, they are estimable
    // directly via either histograms or type counts.
    if (stats::sameTypeBracketInterval(startTag, endInclusive, endTag, endVal) &&
        stats::canEstimateTypeViaHistogram(startTag)) {
        return true;
    }

    // For fields with array data, we skip type count estimation and bracketization because we lack
    // statistics for some types in arrays. For instance, scalar booleans are estimable with
    // counters for true/false values, but these are unavailable in arrays.
    if (isArray) {
        return false;
    }

    auto viaTypeCounts = stats::canEstimateIntervalViaTypeCounts(
        startTag, startVal, startInclusive, endTag, endVal, endInclusive);

    // For a mixed-type interval, if both bounds are of estimable types, we can divide the interval
    // into multiple sub-intervals. The first and last sub-intervals can be estimated using either
    // histograms or type counts, while the intermediate sub-intervals, which are fully bracketed,
    // can be estimated using type counts.
    auto viaBracketization = stats::canEstimateType(startTag) && stats::canEstimateType(endTag);

    return viaTypeCounts || viaBracketization;
}

CardinalityEstimate estimateIntervalCardinality(const stats::CEHistogram& ceHist,
                                                const mongo::Interval& interval,
                                                bool includeScalar,
                                                ArrayRangeEstimationAlgo arrayRangeEstimationAlgo) {
    if (interval.isFullyOpen()) {
        return CardinalityEstimate{
            CardinalityType{static_cast<CardinalityType>(ceHist.getSampleSize())},
            EstimationSource::Histogram};
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

    if (canEstimateInterval(
            ceHist.isArray(), startInclusive, startTag, startVal, endInclusive, endTag, endVal)) {
        CardinalityEstimate ce{cost_based_ranker::zeroCE};
        for (const auto& interval : stats::bracketizeInterval(
                 startTag, startVal, startInclusive, endTag, endVal, endInclusive)) {
            ce += estimateIntervalCardinality(ceHist,
                                              interval.first.second,
                                              interval.first.first.getTag(),
                                              interval.first.first.getValue(),
                                              interval.second.second,
                                              interval.second.first.getTag(),
                                              interval.second.first.getValue(),
                                              arrayRangeEstimationAlgo,
                                              includeScalar);
        }
        return ce;
    }

    // Cardinality estimation for an interval should only be called if interval bounds are
    // histogrammable or estimation by type counts is possible (as implemented in
    // canEstimateInterval). If this is the case, code above should have returned cardinality
    // estimate. Otherwise, tassert is triggered since cardinality estimation was mistakenly invoked
    // on invalid TypeTags.
    MONGO_UNREACHABLE_TASSERT(8870500);
}

}  // namespace mongo::ce
