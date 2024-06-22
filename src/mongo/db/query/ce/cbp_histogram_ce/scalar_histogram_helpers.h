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

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/pipeline/abt/utils.h"  // TODO: remove this somehow!!!
#include "mongo/db/query/ce/bound_utils.h"
#include "mongo/db/query/ce/cbp_histogram_ce/histogram_common.h"
#include "mongo/db/query/stats/value_utils.h"

namespace mongo::optimizer::cbp::ce {

/**
 * Computes an estimate for a value and estimation type. Uses linear interpolation to
 * calculate the frequency of a value in a bucket.
 */
EstimationResult estimateCardinalityEq(const stats::ScalarHistogram& h,
                                       sbe::value::TypeTags tag,
                                       sbe::value::Value val,
                                       EstimationType type);

/**
 * Computes an estimate for a range (low, high) and estimation type. Uses linear
 * interpolation to estimate the parts of buckets that fall in the range.
 */
EstimationResult estimateCardinalityRange(const stats::ScalarHistogram& histogram,
                                          bool lowInclusive,
                                          sbe::value::TypeTags tagLow,
                                          sbe::value::Value valLow,
                                          bool highInclusive,
                                          sbe::value::TypeTags tagHigh,
                                          sbe::value::Value valHigh);

/**
 * Returns cumulative total statistics for a histogram.
 */
EstimationResult getTotals(const stats::ScalarHistogram& h);

/**
 * Uses linear interpolation to estimate the cardinality and number of distinct
 * values (NDV) for a value that falls inside of a histogram bucket.
 */
EstimationResult interpolateEstimateInBucket(const stats::ScalarHistogram& h,
                                             sbe::value::TypeTags tag,
                                             sbe::value::Value val,
                                             EstimationType type,
                                             size_t bucketIndex);

/**
 * Computes an estimate for range query on array data with formula:
 * Card(ArrayMin(a < valHigh)) - Card(ArrayMax(a < valLow))
 */
EstimationResult estimateRangeQueryOnArray(const stats::ScalarHistogram& histogramAmin,
                                           const stats::ScalarHistogram& histogramAmax,
                                           bool lowInclusive,
                                           sbe::value::TypeTags tagLow,
                                           sbe::value::Value valLow,
                                           bool highInclusive,
                                           sbe::value::TypeTags tagHigh,
                                           sbe::value::Value valHigh);


}  // namespace mongo::optimizer::cbp::ce
