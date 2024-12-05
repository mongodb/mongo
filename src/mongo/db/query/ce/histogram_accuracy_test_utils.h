/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/ce/test_utils.h"

namespace mongo::ce {

using stats::TypeCounts;
using TypeTags = sbe::value::TypeTags;

struct TypeProbability {
    sbe::value::TypeTags typeTag;

    // Type probability [0,100]
    size_t typeProbability;

    // Probability of NaN Value [0,1]
    double nanProb = 0.0;
};

using TypeCombination = std::vector<TypeProbability>;
using TypeCombinations = std::vector<TypeCombination>;

enum DataDistributionEnum { kUniform, kNormal, kZipfian };
enum QueryType { kPoint, kRange };

static constexpr size_t kPredefinedArraySize = 15;

struct ErrorCalculationSummary {
    // actual returned results.
    std::vector<std::pair<size_t, EstimationResult>> queryResults;

    // calculated results.
    std::vector<double> relativeErrors;
    std::vector<double> qErrors;
    double relativeErrorAvg;
    double relativeErrorMax;

    double relativeErrorMedian;
    double relativeError90thPercentile;
    double relativeError95thPercentile;
    double relativeError99thPercentile;

    double qErrorMedian;
    double qError90thPercentile;
    double qError95thPercentile;
    double qError99thPercentile;

    // total executed queries.
    size_t executedQueries = 0;
};

stats::ScalarHistogram createHistogram(const std::vector<BucketData>& data);

/**
 * Calculate the frequency of a specific SBEValue as found in a vector of SBEValues.
 */
size_t calculateFrequencyFromDataVectorEq(const std::vector<stats::SBEValue>& data,
                                          stats::SBEValue valueToCalculate,
                                          bool includeScalar);

/**
 * Calculate the frequency of a specific TypeTag as found in a vector of SBEValues.
 */
size_t calculateTypeFrequencyFromDataVectorEq(const std::vector<stats::SBEValue>& data,
                                              sbe::value::TypeTags type);

/**
 * Calculate the frequency of a range in a given vector of values.
 * The range is always inclusive of the bounds.
 */
static size_t calculateFrequencyFromDataVectorRange(const std::vector<stats::SBEValue>& data,
                                                    stats::SBEValue valueToCalculateLow,
                                                    stats::SBEValue valueToCalculateHigh);

static std::pair<boost::optional<double>, boost::optional<double>> computeErrors(
    size_t actualCard, double estimatedCard);

void printHeader();

void printResult(DataDistributionEnum dataDistribution,
                 const TypeCombination& typeCombination,
                 int size,
                 int numberOfBuckets,
                 const TypeProbability& typeCombinationQuery,
                 int numberOfQueries,
                 QueryType queryType,
                 const std::pair<size_t, size_t>& dataInterval,
                 bool includeScalar,
                 ErrorCalculationSummary error);

void generateDataUniform(size_t size,
                         const std::pair<size_t, size_t>& interval,
                         const TypeCombination& typeCombination,
                         size_t seed,
                         size_t ndv,
                         std::vector<stats::SBEValue>& data,
                         int arrayLength = 0);

void generateDataNormal(size_t size,
                        const std::pair<size_t, size_t>& interval,
                        const TypeCombination& typeCombination,
                        size_t seed,
                        size_t ndv,
                        std::vector<stats::SBEValue>& data,
                        int arrayLength = 0);

void generateDataZipfian(size_t size,
                         const std::pair<size_t, size_t>& interval,
                         const TypeCombination& typeCombination,
                         size_t seed,
                         size_t ndv,
                         std::vector<stats::SBEValue>& data,
                         int arrayLength = 0);
/**
 * Generates query intervals randomly according to testing configuration.
 *
 * @param queryType The type of query intervals. It can be either kPoint or kRange.
 * @param interval A pair representing the overall range [min, max] within which all generated
 *                 query intervals' bounds will fall. Both the low and high bounds of each query
 *                 interval will be within this specified range.
 * @param numberOfQueries The number of query intervals to generate.
 * @param queryTypeInfo The type probability information used for generating query interval bounds.
 * @param seed A seed value for random number generation.
 * @return A vector of pairs, where each pair consists of two SBEValue representing the low and high
 *         bounds of an interval.
 */
std::vector<std::pair<stats::SBEValue, stats::SBEValue>> generateIntervals(
    QueryType queryType,
    const std::pair<size_t, size_t>& interval,
    size_t numberOfQueries,
    const TypeProbability& queryTypeInfo,
    size_t seed);

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
                                   size_t seed);

bool checkTypeExistence(const TypeProbability& typeCombinationQuery,
                        const TypeCombination& typeCombinationsData);

void runAccuracyTestConfiguration(DataDistributionEnum dataDistribution,
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
                                  size_t seed,
                                  bool printResults,
                                  int arrayTypeLength = 1000);

}  // namespace mongo::ce
