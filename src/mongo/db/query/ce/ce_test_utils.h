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
#include "mongo/db/query/ce/ce_common.h"
#include "mongo/db/query/stats/rand_utils_new.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::ce {

// Enable this flag to log all estimates, and let all tests pass.
constexpr bool kCETestLogOnly = false;
const double kMaxCEError = 0.01;
constexpr double kErrorBound = 0.01;
static constexpr size_t kPredefinedArraySize = 15;

enum DataDistributionEnum { kUniform, kNormal, kZipfian };
enum QueryType { kPoint, kRange };
enum DataType { kInt, kStringSmall, kString, kDouble, kBoolean, kNull, kNan, kArray };

struct TypeProbability {
    sbe::value::TypeTags typeTag;

    // Type probability [0,100]
    size_t typeProbability;

    // Probability of NaN Value [0,1]
    double nanProb = 0.0;
};

using TypeTags = sbe::value::TypeTags;
using TypeCombination = std::vector<TypeProbability>;
using TypeCombinations = std::vector<TypeCombination>;

struct QueryInfoAndResults {

    boost::optional<stats::SBEValue> low;
    boost::optional<stats::SBEValue> high;

    size_t actualCardinality;
    double estimatedCardinality;
};

struct ErrorCalculationSummary {
    // query information and results.
    std::vector<QueryInfoAndResults> queryResults;

    // total executed queries.
    size_t executedQueries = 0;
};

struct BenchmarkConfiguration {
    size_t size;
    DataDistributionEnum dataDistribution;
    DataType dataType;
    QueryType queryType;

    // Inclusive minimum and maximum bounds for randomly generated data, ensuring each data falls
    // within these limits.
    std::pair<size_t, size_t> dataInterval;
    sbe::value::TypeTags sbeDataType;
    double nanProb = 0;
    size_t arrayTypeLength = 0;

    BenchmarkConfiguration(size_t size,
                           DataDistributionEnum dataDistribution,
                           DataType dataType,
                           QueryType queryType)
        : size(size), dataDistribution(dataDistribution), dataType(dataType), queryType(queryType) {
        switch (dataType) {
            case kInt:
                sbeDataType = sbe::value::TypeTags::NumberInt64;
                dataInterval = {0, 1000};
                break;
            case kStringSmall:
                sbeDataType = sbe::value::TypeTags::StringSmall;
                dataInterval = {1, 8};
                break;
            case kString:
                sbeDataType = sbe::value::TypeTags::StringBig;
                dataInterval = {16, 32};
                break;
            case kDouble:
                sbeDataType = sbe::value::TypeTags::NumberDouble;
                dataInterval = {0, 1000};
                break;
            case kBoolean:
                sbeDataType = sbe::value::TypeTags::Boolean;
                dataInterval = {0, 2};
                break;
            case kNull:
                sbeDataType = sbe::value::TypeTags::Null;
                dataInterval = {0, 1000};
                break;
            case kNan:
                sbeDataType = sbe::value::TypeTags::NumberDouble;
                dataInterval = {0, 1000};
                nanProb = 1;
                break;
            case kArray:
                sbeDataType = sbe::value::TypeTags::Array;
                dataInterval = {0, 1000};
                arrayTypeLength = 10;
                break;
        }
    }
};

template <class T1, class T2>
constexpr double absCEDiff(const T1 v1, const T2 v2) {
    return std::abs(static_cast<double>(v1) - static_cast<double>(v2));
}

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
size_t calculateFrequencyFromDataVectorRange(const std::vector<stats::SBEValue>& data,
                                             stats::SBEValue valueToCalculateLow,
                                             stats::SBEValue valueToCalculateHigh);

/**
 * Populates TypeDistrVector 'td' based on the input configuration.
 *
 * This function iterates over a given type combination and populates the provided 'td' with various
 * statistical distributions according to the specified types and their probabilities.
 *
 * This function supports data types: nothing, null, boolean, integer, string, and array. Note that
 * currently, arrays are only generated with integer elements.
 *
 * @param td The TypeDistrVector that will be populated.
 * @param interval A pair representing the inclusive minimum and maximum bounds for the data.
 * @param typeCombination The types and their associated probabilities presenting the distribution.
 * @param ndv The number of distinct values to generate.
 * @param seedArray A random number seed for generating array. Used only by TypeTags::Array.
 * @param mdd The distribution descriptor.
 * @param arrayLength The maximum length for array distributions, defaulting to 0.
 */
void populateTypeDistrVectorAccordingToInputConfig(stats::TypeDistrVector& td,
                                                   const std::pair<size_t, size_t>& interval,
                                                   const TypeCombination& typeCombination,
                                                   size_t ndv,
                                                   std::mt19937_64& seedArray,
                                                   stats::MixedDistributionDescriptor& mdd,
                                                   int arrayLength = 0);

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
    size_t seedQueriesLow,
    size_t seedQueriesHigh);

bool checkTypeExistence(const TypeProbability& typeCombinationQuery,
                        const TypeCombination& typeCombinationsData);

/**
 * Helpful macros for asserting that the CE of a $match predicate is approximately what we were
 * expecting.
 */

#define _ASSERT_CE(estimatedCE, expectedCE)                             \
    if constexpr (kCETestLogOnly) {                                     \
        if (absCEDiff(estimatedCE, expectedCE) > kMaxCEError) {         \
            std::cout << "ERROR: expected " << expectedCE << std::endl; \
        }                                                               \
        ASSERT_APPROX_EQUAL(1.0, 1.0, kMaxCEError);                     \
    } else {                                                            \
        ASSERT_APPROX_EQUAL(estimatedCE, expectedCE, kMaxCEError);      \
    }
#define _PREDICATE(field, predicate) (str::stream() << "{" << field << ": " << predicate "}")
#define _ELEMMATCH_PREDICATE(field, predicate) \
    (str::stream() << "{" << field << ": {$elemMatch: " << predicate << "}}")

// This macro verifies the cardinality of a pipeline or an input ABT.
#define ASSERT_CE(ce, pipeline, expectedCE) _ASSERT_CE(ce.getCE(pipeline), (expectedCE))

// This macro does the same as above but also sets the collection cardinality.
#define ASSERT_CE_CARD(ce, pipeline, expectedCE, collCard) \
    ce.setCollCard({collCard});                            \
    ASSERT_CE(ce, pipeline, expectedCE)

// This macro verifies the cardinality of a pipeline with a single $match predicate.
#define ASSERT_MATCH_CE(ce, predicate, expectedCE) \
    _ASSERT_CE(ce.getMatchCE(predicate), (expectedCE))

#define ASSERT_MATCH_CE_NODE(ce, queryPredicate, expectedCE, nodePredicate) \
    _ASSERT_CE(ce.getMatchCE(queryPredicate, nodePredicate), (expectedCE))

// This macro does the same as above but also sets the collection cardinality.
#define ASSERT_MATCH_CE_CARD(ce, predicate, expectedCE, collCard) \
    ce.setCollCard({collCard});                                   \
    ASSERT_MATCH_CE(ce, predicate, expectedCE)

// This macro tests cardinality of two versions of the predicate; with and without $elemMatch.
#define ASSERT_EQ_ELEMMATCH_CE(tester, expectedCE, elemMatchExpectedCE, field, predicate) \
    ASSERT_MATCH_CE(tester, _PREDICATE(field, predicate), expectedCE);                    \
    ASSERT_MATCH_CE(tester, _ELEMMATCH_PREDICATE(field, predicate), elemMatchExpectedCE)

#define ASSERT_EQ_ELEMMATCH_CE_NODE(tester, expectedCE, elemMatchExpectedCE, field, predicate, n) \
    ASSERT_MATCH_CE_NODE(tester, _PREDICATE(field, predicate), expectedCE, n);                    \
    ASSERT_MATCH_CE_NODE(tester, _ELEMMATCH_PREDICATE(field, predicate), elemMatchExpectedCE, n)

}  // namespace mongo::ce
