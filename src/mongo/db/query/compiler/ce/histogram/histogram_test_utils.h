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

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/query/compiler/ce/ce_test_utils.h"
#include "mongo/db/query/compiler/ce/histogram/histogram_common.h"
#include "mongo/db/query/compiler/ce/histogram/histogram_estimator.h"
#include "mongo/db/query/compiler/stats/ce_histogram.h"
#include "mongo/db/query/compiler/stats/scalar_histogram.h"
#include "mongo/db/query/compiler/stats/value_utils.h"

namespace mongo::ce {

using stats::makeInt64Value;

/**
 * Test utility for helping with creation of manual histograms in the unit tests.
 */
struct BucketData {
    Value _v;
    double _equalFreq;
    double _rangeFreq;
    double _ndv;

    BucketData(Value v, double equalFreq, double rangeFreq, double ndv)
        : _v(v), _equalFreq(equalFreq), _rangeFreq(rangeFreq), _ndv(ndv) {}
    BucketData(const std::string& v, double equalFreq, double rangeFreq, double ndv)
        : BucketData(Value(v), equalFreq, rangeFreq, ndv) {}
    BucketData(int v, double equalFreq, double rangeFreq, double ndv)
        : BucketData(Value(v), equalFreq, rangeFreq, ndv) {}
};

double estimateCardinalityScalarHistogramInteger(const stats::ScalarHistogram& hist,
                                                 int v,
                                                 EstimationType type);

static double estimateCard(const stats::ScalarHistogram& hist,
                           const int v,
                           const EstimationType type) {
    const auto [tag, val] = makeInt64Value(v);
    return estimateCardinality(hist, tag, val, type).card;
};

stats::ScalarHistogram makeHistogram(std::vector<stats::SBEValue>& randData, size_t nBuckets);

stats::ScalarHistogram createHistogram(const std::vector<BucketData>& data);

void printResult(const stats::DistrType& dataDistribution,
                 const TypeCombination& typeCombination,
                 int size,
                 int numberOfBuckets,
                 const TypeProbability& typeCombinationQuery,
                 int numberOfQueries,
                 QueryType queryType,
                 const std::pair<size_t, size_t>& dataInterval,
                 bool includeScalar,
                 size_t seedData,
                 size_t seedQueriesLow,
                 size_t seedQueriesHigh,
                 const std::vector<std::pair<TypeTags, sbe::value::Value>>& bounds,
                 ErrorCalculationSummary error);

/**
 * Executes a single query estimation based on the specified query type and parameters.
 *
 * @param queryType The type of query intervals. It can be either kPoint or kRange.
 * @param sbeValLow The lower bound value for the query interval. For kPoint queries, this is the
 *                  single value to estimate.
 * @param sbeValHigh The upper bound value for the query interval. This is used only for kRange
 *                   queries.
 * @param ceHist The CEHistogram used for cardinality estimation.
 * @param includeScalar The flag indicating whether scalar values should be included in the
 *                      estimation.
 * @param arrayRangeEstimationAlgo The estimation algorithms for range queries over array values.
 * @param useE2EAPI A flag indicating whether to use the end-to-end API for estimation.
 * @param size The data size.
 * @return The estimated cardinality for the query interval.
 */
EstimationResult runSingleQuery(QueryType queryType,
                                const stats::SBEValue& sbeValLow,
                                const stats::SBEValue& sbeValHigh,
                                const std::shared_ptr<const stats::CEHistogram>& ceHist,
                                bool includeScalar,
                                ArrayRangeEstimationAlgo arrayRangeEstimationAlgo,
                                bool useE2EAPI,
                                size_t size);

/**
 * Generates query intervals and executes query estimation. Calculates the overall accuracy and
 * returns the summary in ErrorCalculationSummary.
 */
ErrorCalculationSummary runQueries(size_t size,
                                   size_t numberOfQueries,
                                   QueryType queryType,
                                   std::pair<size_t, size_t> interval,
                                   TypeProbability queryTypeInfo,
                                   const std::vector<stats::SBEValue>& data,
                                   std::shared_ptr<const stats::CEHistogram> ceHist,
                                   bool includeScalar,
                                   ArrayRangeEstimationAlgo arrayRangeEstimationAlgo,
                                   bool useE2EAPI,
                                   size_t seedQueriesLow,
                                   size_t seedQueriesHigh);

void runAccuracyTestConfiguration(stats::DistrType dataDistribution,
                                  const TypeCombinations& typeCombinationsData,
                                  const TypeCombination& typeCombinationsQueries,
                                  const std::vector<int>& numberOfBucketsVector,
                                  size_t size,
                                  const std::pair<size_t, size_t>& dataInterval,
                                  const std::pair<size_t, size_t>& queryInterval,
                                  int numberOfQueries,
                                  QueryType queryType,
                                  bool includeScalar,
                                  ArrayRangeEstimationAlgo arrayRangeEstimationAlgo,
                                  bool useE2EAPI,
                                  size_t seedData,
                                  size_t seedQueriesLow,
                                  size_t seedQueriesHigh,
                                  bool printResults,
                                  int arrayTypeLength = 100);

}  // namespace mongo::ce
