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

#include "mongo/db/query/ce/histogram_accuracy_test_utils.h"

using namespace mongo::ce;

/**
 * This program generates accuracy calculations for histogram frequency estimates.
 * The program creates varying distribution datasets (Uniform, Normal, Zipfian), populates a
 * histogram with varying number of buckets, and executes a sequence of point and range queries.
 * The program compares the estimated output with the correct results and outputs the error stats.
 * The collected stats are outputed in CSV format as part of the mongodb log.
 */
int main(int argc, char* argv[]) {

    // the TypeCombination struct defines the probability of this specific type appearing in the
    // dataset and the probability of NaN value (only applicable to numerical types).
    const TypeCombinations typeCombinationsData{
        // Single types
        TypeCombination{{TypeTags::Nothing, 100}},
        TypeCombination{{TypeTags::Null, 100}},
        TypeCombination{{TypeTags::NumberDouble,
                         100 /*Type probability [0,100]*/,
                         1 /*Probability of NaN Value [0,1]*/}},  // NaN
        TypeCombination{{TypeTags::Boolean, 100}},
        TypeCombination{{TypeTags::NumberInt64, 100}},
        TypeCombination{{TypeTags::NumberDouble}},
        TypeCombination{{TypeTags::StringSmall, 100}},
        TypeCombination{{TypeTags::StringBig, 100}},
        TypeCombination{{TypeTags::Array, 100}},
        // Type combinations
        TypeCombination{{TypeTags::NumberInt64, 50}, {TypeTags::NumberDouble, 50}},
        TypeCombination{{TypeTags::NumberInt64, 30},
                        {TypeTags::NumberDouble, 40, 0.3},
                        {TypeTags::Nothing, 15},
                        {TypeTags::Null, 15}},
        TypeCombination{{TypeTags::StringSmall, 30}, {TypeTags::StringBig, 70}},
        TypeCombination{
            {TypeTags::NumberInt64, 30}, {TypeTags::NumberDouble, 40}, {TypeTags::Array, 30}},
        // All types
        TypeCombination{{TypeTags::Nothing, 5},
                        {TypeTags::Null, 5},
                        {TypeTags::NumberDouble, 5, 1},  // NaN
                        {TypeTags::Boolean, 5},
                        {TypeTags::NumberInt64, 20},
                        {TypeTags::NumberDouble, 20},
                        {TypeTags::StringSmall, 10},
                        {TypeTags::StringBig, 20},
                        {TypeTags::Array, 10}}};

    const std::vector<int> numberOfBuckets{10, 50, 100, 200, 400};
    const size_t size = 50000;
    const int numberOfQueries = 1000;
    const int arrayTypeLength = 10;
    bool printResults = true;
    const size_t seed = 1724178214;

    if (printResults) {
        printHeader();
    }

    auto dataDistributions = {kUniform, kNormal, kZipfian};
    auto queryTypes = {kPoint, kRange};

    for (auto queryType : queryTypes) {
        for (auto dataDistribution : dataDistributions) {

            // Query on "Nothing"
            {
                const TypeCombination typeCombinationsQueries{{TypeTags::Nothing, 100}};
                const std::pair<size_t, size_t> dataInterval({0, 1000});
                const std::pair<size_t, size_t> queryInterval({0, 1000});

                runAccuracyTestConfiguration(dataDistribution,
                                             typeCombinationsData,
                                             typeCombinationsQueries,
                                             numberOfBuckets,
                                             size,
                                             dataInterval,
                                             queryInterval,
                                             numberOfQueries,
                                             queryType,
                                             true /*includeScalar*/,
                                             false /*useE2EAPI*/,
                                             seed,
                                             printResults,
                                             arrayTypeLength);
            }

            // Query on "Null"
            {
                const TypeCombination typeCombinationsQueries{{TypeTags::Null, 100}};
                const std::pair<size_t, size_t> dataInterval({0, 1000});
                const std::pair<size_t, size_t> queryInterval({0, 1000});

                runAccuracyTestConfiguration(dataDistribution,
                                             typeCombinationsData,
                                             typeCombinationsQueries,
                                             numberOfBuckets,
                                             size,
                                             dataInterval,
                                             queryInterval,
                                             numberOfQueries,
                                             queryType,
                                             true /*includeScalar*/,
                                             false /*useE2EAPI*/,
                                             seed,
                                             printResults);
            }

            // Query on "NaN"
            {
                const TypeCombination typeCombinationsQueries{{TypeTags::NumberDouble, 100, 1.0}};
                const std::pair<size_t, size_t> dataInterval({0, 1000});
                const std::pair<size_t, size_t> queryInterval({0, 1000});

                runAccuracyTestConfiguration(dataDistribution,
                                             typeCombinationsData,
                                             typeCombinationsQueries,
                                             numberOfBuckets,
                                             size,
                                             dataInterval,
                                             queryInterval,
                                             numberOfQueries,
                                             queryType,
                                             true /*includeScalar*/,
                                             false /*useE2EAPI*/,
                                             seed,
                                             printResults);
            }

            // Query on "Boolean"
            {
                const TypeCombination typeCombinationsQueries{{TypeTags::Boolean, 100}};
                const std::pair<size_t, size_t> dataInterval({0, 2});
                const std::pair<size_t, size_t> queryInterval({0, 2});
                QueryType queryType = kPoint;

                runAccuracyTestConfiguration(dataDistribution,
                                             typeCombinationsData,
                                             typeCombinationsQueries,
                                             numberOfBuckets,
                                             size,
                                             dataInterval,
                                             queryInterval,
                                             numberOfQueries,
                                             queryType,
                                             true /*includeScalar*/,
                                             false /*useE2EAPI*/,
                                             seed,
                                             printResults);
            }

            // Query on "NumberInt64"
            {
                const TypeCombination typeCombinationsQueries{{TypeTags::NumberInt64, 100}};
                const std::pair<size_t, size_t> dataInterval({0, 1000});
                const std::pair<size_t, size_t> queryInterval({0, 1000});

                runAccuracyTestConfiguration(dataDistribution,
                                             typeCombinationsData,
                                             typeCombinationsQueries,
                                             numberOfBuckets,
                                             size,
                                             dataInterval,
                                             queryInterval,
                                             numberOfQueries,
                                             queryType,
                                             true /*includeScalar*/,
                                             false /*useE2EAPI*/,
                                             seed,
                                             printResults);
            }

            // Query on "NumberDouble"
            {
                const TypeCombination typeCombinationsQueries{{TypeTags::NumberDouble, 100}};
                const std::pair<size_t, size_t> dataInterval({0, 1000});
                const std::pair<size_t, size_t> queryInterval({0, 1000});

                runAccuracyTestConfiguration(dataDistribution,
                                             typeCombinationsData,
                                             typeCombinationsQueries,
                                             numberOfBuckets,
                                             size,
                                             dataInterval,
                                             queryInterval,
                                             numberOfQueries,
                                             queryType,
                                             true /*includeScalar*/,
                                             false /*useE2EAPI*/,
                                             seed,
                                             printResults);
            }

            // Query on "StringSmall"
            {
                const TypeCombination typeCombinationsQueries{{TypeTags::StringSmall, 100}};
                const std::pair<size_t, size_t> dataInterval({0, 1000});
                const std::pair<size_t, size_t> queryInterval({0, 1000});

                runAccuracyTestConfiguration(dataDistribution,
                                             typeCombinationsData,
                                             typeCombinationsQueries,
                                             numberOfBuckets,
                                             size,
                                             dataInterval,
                                             queryInterval,
                                             numberOfQueries,
                                             queryType,
                                             true /*includeScalar*/,
                                             false /*useE2EAPI*/,
                                             seed,
                                             printResults);
            }

            // Query on "StringBig"
            {
                const TypeCombination typeCombinationsQueries{{TypeTags::StringBig, 100}};
                const std::pair<size_t, size_t> dataInterval({0, 1000});
                const std::pair<size_t, size_t> queryInterval({0, 1000});

                runAccuracyTestConfiguration(dataDistribution,
                                             typeCombinationsData,
                                             typeCombinationsQueries,
                                             numberOfBuckets,
                                             size,
                                             dataInterval,
                                             queryInterval,
                                             numberOfQueries,
                                             queryType,
                                             true /*includeScalar*/,
                                             false /*useE2EAPI*/,
                                             seed,
                                             printResults);
            }

            // Query on variety of types
            {
                const TypeCombination typeCombinationsQueries{{TypeTags::NumberInt64, 100},
                                                              {TypeTags::StringBig, 100},
                                                              {TypeTags::NumberDouble, 100}};
                const std::pair<size_t, size_t> dataInterval({0, 1000});
                const std::pair<size_t, size_t> queryInterval({0, 1000});

                runAccuracyTestConfiguration(dataDistribution,
                                             typeCombinationsData,
                                             typeCombinationsQueries,
                                             numberOfBuckets,
                                             size,
                                             dataInterval,
                                             queryInterval,
                                             numberOfQueries,
                                             queryType,
                                             false /*includeScalar*/,
                                             false /*useE2EAPI*/,
                                             seed,
                                             printResults);
            }
        }
    }
}
