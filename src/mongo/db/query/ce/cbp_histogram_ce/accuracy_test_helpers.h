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

#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/ce/cbp_histogram_ce/histogram_common.h"
#include "mongo/db/query/ce/cbp_histogram_ce/histogram_predicate_estimation.h"
#include "mongo/db/query/stats/max_diff.h"

namespace mongo::optimizer::cbp::ce {

using stats::TypeCounts;
using TypeTags = sbe::value::TypeTags;
using TypeProbability = std::pair<sbe::value::TypeTags, size_t>;
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
};

static size_t calculateFrequencyFromDataVectorEq(const std::vector<stats::SBEValue>& data,
                                                 sbe::value::TypeTags type,
                                                 stats::SBEValue valueToCalculate);

/**
 * Calculate the frequency of a range in a given vector of values.
 * The range is always inclusive of the bounds.
 */
static size_t calculateFrequencyFromDataVectorRange(const std::vector<stats::SBEValue>& data,
                                                    sbe::value::TypeTags type,
                                                    stats::SBEValue valueToCalculateLow,
                                                    stats::SBEValue valueToCalculateHigh);

static std::pair<boost::optional<double>, boost::optional<double>> computeErrors(
    size_t actualCard, double estimatedCard);

void printHeader();

void printResult(DataDistributionEnum dataDistribution,
                 const std::vector<std::pair<sbe::value::TypeTags, size_t>>& typeCombination,
                 int size,
                 int numberOfBuckets,
                 const std::pair<sbe::value::TypeTags, size_t>& typeCombinationQuery,
                 int numberOfQueries,
                 QueryType queryType,
                 const std::pair<size_t, size_t>& dataInterval,
                 ErrorCalculationSummary error);

void generateDataUniform(size_t size,
                         const std::pair<size_t, size_t>& interval,
                         const TypeCombination& typeCombination,
                         size_t seed,
                         size_t ndv,
                         std::vector<stats::SBEValue>& data);

void generateDataNormal(size_t size,
                        const std::pair<size_t, size_t>& interval,
                        const TypeCombination& typeCombination,
                        size_t seed,
                        size_t ndv,
                        std::vector<stats::SBEValue>& data);

void generateDataZipfian(size_t size,
                         const std::pair<size_t, size_t>& interval,
                         const TypeCombination& typeCombination,
                         size_t seed,
                         size_t ndv,
                         std::vector<stats::SBEValue>& data);

ErrorCalculationSummary runQueries(size_t size,
                                   size_t numberOfQueries,
                                   QueryType queryType,
                                   std::pair<size_t, size_t> interval,
                                   std::pair<sbe::value::TypeTags, size_t> queryTypeInfo,
                                   const std::vector<stats::SBEValue>& data,
                                   std::shared_ptr<const stats::ArrayHistogram> arrHist,
                                   bool includeScalar,
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
                                  bool useE2EAPI,
                                  size_t seed,
                                  bool printResults);

}  // namespace mongo::optimizer::cbp::ce
