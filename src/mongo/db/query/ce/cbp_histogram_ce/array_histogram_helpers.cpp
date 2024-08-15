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

#include "mongo/db/query/ce/cbp_histogram_ce/array_histogram_helpers.h"
#include "mongo/db/query/ce/cbp_histogram_ce/scalar_histogram_helpers.h"

namespace mongo::optimizer::cbp::ce {

using stats::compareValues;

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
                                          /* Define lower bound. */
                                          bool lowInclusive,
                                          sbe::value::TypeTags tagLow,
                                          sbe::value::Value valLow,
                                          /* Define upper bound. */
                                          bool highInclusive,
                                          sbe::value::TypeTags tagHigh,
                                          sbe::value::Value valHigh,
                                          bool includeScalar,
                                          EstimationAlgo estimationAlgo) {
    uassert(9160700,
            "Low bound must not be higher than high",
            compareValues(tagLow, valLow, tagHigh, valHigh) <= 0);

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

EstimationResult getTotals(const stats::ArrayHistogram& ah) {
    // TODO: To implement
    return {};
}

Selectivity estimateSelectivityEq(const stats::ArrayHistogram& ah,
                                  sbe::value::TypeTags tag,
                                  sbe::value::Value val,
                                  bool includeScalar) {
    const auto ce = estimateCardinalityEq(ah, tag, val, includeScalar);
    return getSelectivity(ah, ce.card);
}

Selectivity estimateSelectivityRange(const stats::ArrayHistogram& ah,
                                     bool lowInclusive,
                                     sbe::value::TypeTags tagLow,
                                     sbe::value::Value valLow,
                                     bool highInclusive,
                                     sbe::value::TypeTags tagHigh,
                                     sbe::value::Value valHigh,
                                     bool includeScalar,
                                     EstimationAlgo estAlgo) {
    const auto ce = estimateCardinalityRange(
        ah, lowInclusive, tagLow, valLow, highInclusive, tagHigh, valHigh, includeScalar, estAlgo);
    return getSelectivity(ah, ce.card);
}

Selectivity getSelectivity(const stats::ArrayHistogram& ah, Cardinality cardinality) {
    // TODO: to implement
    return {};
}

}  // namespace mongo::optimizer::cbp::ce
