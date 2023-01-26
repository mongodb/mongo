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

#pragma once

#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/index_bounds.h"
#include "mongo/db/query/stats/array_histogram.h"

namespace mongo::optimizer::ce {

enum class EstimationType { kEqual, kLess, kLessOrEqual, kGreater, kGreaterOrEqual };
enum class EstimationAlgo { HistogramV1, HistogramV2, HistogramV3 };

const stdx::unordered_map<EstimationType, std::string> estimationTypeName = {
    {EstimationType::kEqual, "eq"},
    {EstimationType::kLess, "lt"},
    {EstimationType::kLessOrEqual, "lte"},
    {EstimationType::kGreater, "gt"},
    {EstimationType::kGreaterOrEqual, "gte"}};

struct EstimationResult {
    double card;
    double ndv;

    EstimationResult operator-(const EstimationResult& other) const {
        return {card - other.card, ndv - other.ndv};
    }
};

/**
 * Converts an input cardinality to a selectivity based on the histogram's sample size.
 */
SelectivityType getSelectivity(const stats::ArrayHistogram& ah, CEType cardinality);

/**
 * Returns the selectivity of arrays according to this histogram.
 */
SelectivityType getArraySelectivity(const stats::ArrayHistogram& ah);

/**
 * Returns cumulative total statistics for a histogram.
 */
EstimationResult getTotals(const stats::ScalarHistogram& h);

/**
 * Compute an estimate for a given value and estimation type. Use linear interpolation for values
 * that fall inside of histogram buckets.
 */
EstimationResult estimate(const stats::ScalarHistogram& h,
                          sbe::value::TypeTags tag,
                          sbe::value::Value val,
                          EstimationType type);

/**
 * Estimates the cardinality of an equality predicate given an ArrayHistogram and an SBE value and
 * type tag pair.
 */
CEType estimateCardEq(const stats::ArrayHistogram& ah,
                      sbe::value::TypeTags tag,
                      sbe::value::Value val,
                      bool includeScalar);

/**
 * Estimates the cardinality of a range predicate given an ArrayHistogram and a range predicate.
 * Set 'includeScalar' to true to indicate whether or not the provided range should include no-array
 * values. The other fields define the range of the estimation.
 */
CEType estimateCardRange(const stats::ArrayHistogram& ah,
                         bool lowInclusive,
                         sbe::value::TypeTags tagLow,
                         sbe::value::Value valLow,
                         bool highInclusive,
                         sbe::value::TypeTags tagHigh,
                         sbe::value::Value valHigh,
                         bool includeScalar,
                         EstimationAlgo estAlgo = EstimationAlgo::HistogramV2);

/**
 * Estimates the selectivity of an equality predicate given an ArrayHistogram and an SBE value and
 * type tag pair.
 */
SelectivityType estimateSelEq(const stats::ArrayHistogram& ah,
                              sbe::value::TypeTags tag,
                              sbe::value::Value val,
                              bool includeScalar);

/**
 * Estimates the selectivity of a range predicate given an ArrayHistogram and a range predicate.
 * Set 'includeScalar' to true to indicate whether or not the provided range should include no-array
 * values. The other fields define the range of the estimation.
 */
SelectivityType estimateSelRange(const stats::ArrayHistogram& ah,
                                 bool lowInclusive,
                                 sbe::value::TypeTags tagLow,
                                 sbe::value::Value valLow,
                                 bool highInclusive,
                                 sbe::value::TypeTags tagHigh,
                                 sbe::value::Value valHigh,
                                 bool includeScalar,
                                 EstimationAlgo estAlgo = EstimationAlgo::HistogramV2);

}  // namespace mongo::optimizer::ce
