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

#include "mongo/db/query/compiler/ce/histogram/histogram_common.h"

namespace mongo::ce {

enum class ArrayExactRangeEstimationAlgo { HistogramV1, HistogramV2, HistogramV3 };

/**
 * Computes an estimate for a value and estimation type. Uses linear interpolation to
 * calculate the frequency of a value in a bucket.
 */
EstimationResult estimateCardinality(const stats::ScalarHistogram& h,
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

// --------------------- CE HISTOGRAM ESTIMATION METHODS ---------------------

/**
 * Estimates the cardinality of an equality predicate given an CEHistogram and an SBE value and
 * type tag pair.
 */
EstimationResult estimateCardinalityEq(const stats::CEHistogram& ceHist,
                                       sbe::value::TypeTags tag,
                                       sbe::value::Value val,
                                       bool includeScalar);

/**
 * Estimates the cardinality of a range predicate given an CEHistogram and a range predicate.
 * Set 'includeScalar' to true to indicate whether or not the provided range should include no-array
 * values. The other fields define the range of the estimation. This method assumes that the values
 * are in order.
 */
EstimationResult estimateCardinalityRange(
    const stats::CEHistogram& ceHist,
    bool lowInclusive,
    sbe::value::TypeTags tagLow,
    sbe::value::Value valLow,
    bool highInclusive,
    sbe::value::TypeTags tagHigh,
    sbe::value::Value valHigh,
    bool includeScalar,
    ArrayRangeEstimationAlgo arrayRangeEstimationAlgo,
    ArrayExactRangeEstimationAlgo estAlgo = ArrayExactRangeEstimationAlgo::HistogramV2);

/**
 * Estimates the cardinality of a given interval using either histograms or type counts. Otherwise,
 * throw an exception.
 */
CardinalityEstimate estimateIntervalCardinality(const stats::CEHistogram& ceHist,
                                                const mongo::Interval& interval,
                                                bool includeScalar,
                                                ArrayRangeEstimationAlgo arrayRangeEstimationAlgo);

/**
 * Estimate the cardinality of a point query (e.g., {a: {$eq: val}}) via type counts. The value
 * 'val' has to be of specific type/value combination for which we are able to estimate using type
 * counts. Such types are:
 * - Boolean (True/False)
 * - Array (Empty)
 * - Null/Nothing
 * - Int (NaN)
 */
boost::optional<EstimationResult> estimateCardinalityEqViaTypeCounts(
    const stats::CEHistogram& ceHist, sbe::value::TypeTags tag, sbe::value::Value val);

/**
 * Estimate the cardinality of a range query (e.g., [valLow, valHigh]) via type counts. The function
 * will return a result if the values 'valLow', 'valHigh' are i. either Boolean (True/False) or ii.
 * completely enclose a specific data type (i.e., all values belonging to a specific data type).
 * This function assumes that i. The two typetags of the interval boundary values (tagLow, tagHigh)
 * may differ, ii. valLow and valHigh differ, and iii. valLow and valHigh are sorted by value (i.e.,
 * valLow < valHigh).
 */
boost::optional<EstimationResult> estimateCardinalityRangeViaTypeCounts(
    const stats::CEHistogram& ceHist,
    bool lowInclusive,
    sbe::value::TypeTags tagLow,
    sbe::value::Value valLow,
    bool highInclusive,
    sbe::value::TypeTags tagHigh,
    sbe::value::Value valHigh);

/**
 * Checks if given SBEValue interval can be estimated.
 *
 * @param isArray indicates if the field contains array data.
 */
bool canEstimateInterval(bool isArray,
                         bool startInclusive,
                         sbe::value::TypeTags startTag,
                         sbe::value::Value startVal,
                         bool endInclusive,
                         sbe::value::TypeTags endTag,
                         sbe::value::Value endVal);

}  // namespace mongo::ce
