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

#include <absl/random/zipf_distribution.h>
#include <sstream>

#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/query/ce/cbp_histogram_ce/accuracy_test_helpers.h"
#include "mongo/db/query/ce/cbp_histogram_ce/array_histogram_helpers.h"
#include "mongo/db/query/ce/cbp_histogram_ce/histogram_predicate_estimation.h"
#include "mongo/db/query/interval.h"
#include "mongo/logv2/log.h"

namespace mongo::optimizer::cbp::ce {

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

/**
 * Compute the 50th, 90th, 95th, and 99th percentile values of a given vector.
 * This function creates a copy of the given vector which it sorts and over which it calculates the
 * percentiles.
 */
std::tuple<double, double, double, double> percentiles(std::vector<double> arr) {
    // sort array before calculating the cummulative stats.
    std::sort(arr.begin(), arr.end());

    return {arr[(size_t)(arr.size() * 0.50)],
            arr[(size_t)(arr.size() * 0.90)],
            arr[(size_t)(arr.size() * 0.95)],
            arr[(size_t)(arr.size() * 0.99)]};
}

static size_t calculateFrequencyFromDataVectorEq(const std::vector<stats::SBEValue>& data,
                                                 sbe::value::TypeTags type,
                                                 stats::SBEValue valueToCalculate) {
    int actualCard = 0;
    for (const auto& value : data) {
        if (mongo::stats::compareValues(
                type, value.getValue(), type, valueToCalculate.getValue()) == 0) {
            actualCard++;
        }
    }
    return actualCard;
}

static size_t calculateFrequencyFromDataVectorRange(const std::vector<stats::SBEValue>& data,
                                                    sbe::value::TypeTags type,
                                                    stats::SBEValue valueToCalculateLow,
                                                    stats::SBEValue valueToCalculateHigh) {
    int actualCard = 0;
    for (const auto& value : data) {
        // higher OR equal to low AND lower OR equal to high
        if (((mongo::stats::compareValues(
                  type, value.getValue(), type, valueToCalculateLow.getValue()) > 0) ||
             (mongo::stats::compareValues(
                  type, value.getValue(), type, valueToCalculateLow.getValue()) == 0)) &&
            ((mongo::stats::compareValues(
                  type, value.getValue(), type, valueToCalculateHigh.getValue()) < 0) ||
             (mongo::stats::compareValues(
                  type, value.getValue(), type, valueToCalculateHigh.getValue()) == 0))) {
            actualCard++;
        }
    }
    return actualCard;
}

static std::pair<boost::optional<double>, boost::optional<double>> computeErrors(
    size_t actualCard, double estimatedCard) {
    double error = estimatedCard - actualCard;
    boost::optional<double> relError = (actualCard == 0)
        ? (estimatedCard == 0 ? boost::optional<double>(0.0) : boost::none)
        : boost::optional<double>(abs(error / actualCard));
    boost::optional<double> qError =
        std::max((actualCard != 0.0)          ? boost::optional<double>(estimatedCard / actualCard)
                     : (estimatedCard == 0.0) ? boost::optional<double>(0.0)
                                              : boost::none,
                 (estimatedCard != 0.0)    ? boost::optional<double>(actualCard / estimatedCard)
                     : (actualCard == 0.0) ? boost::optional<double>(0.0)
                                           : boost::none);
    return std::make_pair(qError, relError);
}

void printHeader() {
    std::stringstream ss;
    ss << "Data distribution, Number of histogram buckets, Data type, Data size, "
       << "Query type, Query data type, Number of Queries, "
       << "Data interval start, Data interval end, "
       << "relative error (Avg), relative error (Max), relative error "
          "(Median), relative error (90th), relative error (95%), relative error (99%), Q-Error "
          "(Median), Q-Error (90%), Q-Error (95%), Q-Error (99%)";
    LOGV2(8871201, "Accuracy experiment header", ""_attr = ss.str());
}

void printResult(const DataDistributionEnum dataDistribution,
                 const std::vector<std::pair<sbe::value::TypeTags, size_t>>& typeCombination,
                 const int size,
                 const int numberOfBuckets,
                 const std::pair<sbe::value::TypeTags, size_t>& typeCombinationQuery,
                 const int numberOfQueries,
                 QueryType queryType,
                 const std::pair<size_t, size_t>& dataInterval,
                 ErrorCalculationSummary error) {

    std::string distribution;
    switch (dataDistribution) {
        case kUniform:
            distribution = "Uniform";
            break;
        case kNormal:
            distribution = "Normal";
            break;
        case kZipfian:
            distribution = "Zipfian";
            break;
    }

    std::stringstream ss;

    // Distribution
    ss << distribution << ", ";

    // Number of buckets
    ss << numberOfBuckets << ", ";

    // Data types
    for (auto type : typeCombination) {
        ss << type.first << "." << type.second << " ";
    }
    ss << ", ";

    // Data size
    ss << size << ", ";

    // Query data types
    std::string queryTypeStr;
    switch (queryType) {
        case kPoint:
            queryTypeStr = "Point";
            break;
        case kRange:
            queryTypeStr = "Range";
            break;
    }
    ss << queryTypeStr << ", " << typeCombinationQuery.first << ", ";

    // Number of Queries:
    ss << numberOfQueries << ", ";

    // Data interval
    ss << dataInterval.first << ", " << dataInterval.second << ", ";

    // Relative error
    ss << error.relativeErrorAvg << ", " << error.relativeErrorMax << ", "
       << error.relativeErrorMedian << ", " << error.relativeError90thPercentile << ", "
       << error.relativeError95thPercentile << ", " << error.relativeError99thPercentile << ", ";

    // Q-error
    ss << error.qErrorMedian << ", " << error.qError90thPercentile << ", "
       << error.qError95thPercentile << ", " << error.qError99thPercentile;

    LOGV2(8871202, "Accuracy experiment", ""_attr = ss.str());
}

void generateDataUniform(
    size_t size,
    const std::pair<size_t, size_t>& interval,
    const std::vector<std::pair<sbe::value::TypeTags, size_t>>& typeCombination,
    const size_t seed,
    std::vector<stats::SBEValue>& data) {

    // Generator for type selection.
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::mt19937::result_type> distTypes(0, 100);

    // Random value generator for actual data in histogram.
    std::mt19937 gen(seed);
    std::uniform_int_distribution<> distData(interval.first, interval.second);

    // Create one by one the values
    for (size_t i = 0; i < size; i++) {

        // Decide which value type should create.
        for (auto type : typeCombination) {

            // Create values based on probabilities.
            if (distTypes(rng) > type.second) {
                continue;
            }

            if (type.first == sbe::value::TypeTags::Array) {

                std::vector<stats::SBEValue> randArray;
                for (size_t j = 0; j < kPredefinedArraySize; j++) {
                    stats::SBEValue newValue(sbe::value::TypeTags::NumberInt64,
                                             sbe::value::bitcastFrom<int64_t>(distData(gen)));
                    randArray.emplace_back(newValue);
                }

                auto [arrayTag, arrayVal] = sbe::value::makeNewArray();
                sbe::value::Array* arr = sbe::value::getArrayView(arrayVal);
                for (const auto& elem : randArray) {
                    const auto [copyTag, copyVal] =
                        sbe::value::copyValue(elem.getTag(), elem.getValue());
                    arr->push_back(copyTag, copyVal);
                }

                data.emplace_back(arrayTag, arrayVal);
            } else {
                stats::SBEValue newValue(type.first,
                                         sbe::value::bitcastFrom<int64_t>(distData(gen)));
                data.emplace_back(newValue);
            }
        }
    }
}

void generateDataNormal(size_t size,
                        const std::pair<size_t, size_t>& interval,
                        const std::vector<std::pair<sbe::value::TypeTags, size_t>>& typeCombination,
                        const size_t seed,
                        std::vector<stats::SBEValue>& data) {

    // Generator for type selection.
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::mt19937::result_type> distTypes(0, 100);

    // Random value generator for actual data in histogram.
    std::mt19937 gen(seed);
    std::normal_distribution<> distData((float)interval.first, (float)interval.second);

    // Create one by one the values
    for (size_t i = 0; i < size; i++) {

        // Decide which value type should create.
        for (auto type : typeCombination) {

            // Create values based on probabilities.
            if (distTypes(rng) > type.second) {
                continue;
            }

            if (type.first == sbe::value::TypeTags::Array) {

                std::vector<stats::SBEValue> randArray;
                for (size_t j = 0; j < kPredefinedArraySize; j++) {
                    stats::SBEValue newValue(sbe::value::TypeTags::NumberInt64,
                                             sbe::value::bitcastFrom<int64_t>(distData(gen)));
                    randArray.emplace_back(newValue);
                }

                auto [arrayTag, arrayVal] = sbe::value::makeNewArray();
                sbe::value::Array* arr = sbe::value::getArrayView(arrayVal);
                for (const auto& elem : randArray) {
                    const auto [copyTag, copyVal] =
                        sbe::value::copyValue(elem.getTag(), elem.getValue());
                    arr->push_back(copyTag, copyVal);
                }

                data.emplace_back(arrayTag, arrayVal);
            } else {
                stats::SBEValue newValue(type.first,
                                         sbe::value::bitcastFrom<int64_t>(distData(gen)));
                data.emplace_back(newValue);
            }
        }
    }
}

void generateDataZipfian(
    const size_t size,
    const std::pair<size_t, size_t>& interval,
    const std::vector<std::pair<sbe::value::TypeTags, size_t>>& typeCombination,
    const size_t seed,
    std::vector<stats::SBEValue>& data) {

    // Generator for type selection.
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::mt19937::result_type> distTypes(0, 100);

    // Random value generator for actual data in histogram.
    std::default_random_engine gen(seed);
    absl::zipf_distribution<int> distData(interval.second);

    // Create one by one the values
    for (size_t i = 0; i < size; i++) {

        // Decide which value type should create.
        for (auto type : typeCombination) {

            // Create values based on probabilities.
            if (distTypes(rng) > type.second) {
                continue;
            }

            if (type.first == sbe::value::TypeTags::Array) {

                std::vector<stats::SBEValue> randArray;
                for (size_t j = 0; j < kPredefinedArraySize; j++) {
                    stats::SBEValue newValue(sbe::value::TypeTags::NumberInt64,
                                             sbe::value::bitcastFrom<int64_t>(distData(gen)));
                    randArray.emplace_back(newValue);
                }

                auto [arrayTag, arrayVal] = sbe::value::makeNewArray();
                sbe::value::Array* arr = sbe::value::getArrayView(arrayVal);
                for (const auto& elem : randArray) {
                    const auto [copyTag, copyVal] =
                        sbe::value::copyValue(elem.getTag(), elem.getValue());
                    arr->push_back(copyTag, copyVal);
                }

                data.emplace_back(arrayTag, arrayVal);
            } else {
                stats::SBEValue newValue(type.first,
                                         sbe::value::bitcastFrom<int64_t>(distData(gen)));
                data.emplace_back(newValue);
            }
        }
    }
}

ErrorCalculationSummary runQueries(size_t size,
                                   size_t numberOfQueries,
                                   QueryType queryType,
                                   const std::pair<size_t, size_t> interval,
                                   const std::pair<sbe::value::TypeTags, size_t> queryTypeInfo,
                                   const std::vector<stats::SBEValue>& data,
                                   const std::shared_ptr<const stats::ArrayHistogram> arrHist,
                                   bool includeScalar,
                                   bool useE2EAPI,
                                   const size_t seed) {

    // Generator for type selection.
    std::mt19937 rng1(seed);
    std::uniform_int_distribution<std::mt19937::result_type> distData(interval.first,
                                                                      interval.second);

    double relativeErrorSum = 0;
    double relativeErrorMax = 0;

    ErrorCalculationSummary finalResults;

    for (size_t i = 0; i < numberOfQueries; i++) {

        size_t actualCard;
        EstimationResult estimatedCard;

        switch (queryType) {
            case kPoint: {
                stats::SBEValue sbeValEq(queryTypeInfo.first,
                                         sbe::value::bitcastFrom<int64_t>(distData(rng1)));

                // Find actual frequency.
                actualCard =
                    calculateFrequencyFromDataVectorEq(data, queryTypeInfo.first, sbeValEq);

                // Estimate result.
                if (useE2EAPI) {

                    auto bsonobjLow = sbeValueToBSON(sbeValEq, "low");
                    auto bsonobjHigh = sbeValueToBSON(sbeValEq, "high");
                    bsonobjLow = bsonobjLow.addFields(bsonobjHigh);

                    mongo::Interval interval(bsonobjLow, true, true);

                    estimatedCard.card = HistogramCardinalityEstimator::estimateCardinality(
                        *arrHist, size, interval, includeScalar);
                } else {

                    estimatedCard = estimateCardinalityEq(
                        *arrHist, queryTypeInfo.first, sbeValEq.getValue(), includeScalar);
                }
                break;
            }
            case kRange: {
                auto rawLow = distData(rng1);
                stats::SBEValue sbeValLow(queryTypeInfo.first,
                                          sbe::value::bitcastFrom<int64_t>(rawLow));

                std::uniform_int_distribution<std::mt19937::result_type> distDataHigh(
                    rawLow, interval.second);

                auto rawHigh = distDataHigh(rng1);
                stats::SBEValue sbeValHigh(queryTypeInfo.first,
                                           sbe::value::bitcastFrom<int64_t>(rawHigh));

                // Find actual frequency.
                actualCard = calculateFrequencyFromDataVectorRange(
                    data, queryTypeInfo.first, sbeValLow, sbeValHigh);

                // Estimate result.
                if (useE2EAPI) {

                    auto bsonobjLow = sbeValueToBSON(sbeValLow, "low");
                    auto bsonobjHigh = sbeValueToBSON(sbeValHigh, "high");
                    bsonobjLow = bsonobjLow.addFields(bsonobjHigh);

                    mongo::Interval interval(bsonobjLow, true, true);

                    estimatedCard.card = HistogramCardinalityEstimator::estimateCardinality(
                        *arrHist, size, interval, includeScalar);
                } else {

                    estimatedCard = estimateCardinalityRange(*arrHist,
                                                             true /*lowInclusive*/,
                                                             queryTypeInfo.first,
                                                             sbeValLow.getValue(),
                                                             true /*highInclusive*/,
                                                             queryTypeInfo.first,
                                                             sbeValHigh.getValue(),
                                                             includeScalar);
                }
                break;
            }
        }

        std::pair<boost::optional<double>, boost::optional<double>> errors =
            computeErrors(actualCard, estimatedCard.card);

        // If error calculation was successful.
        if (errors.first.has_value()) {

            // Store results to final structure.
            finalResults.queryResults.push_back({actualCard, estimatedCard});

            finalResults.qErrors.push_back(abs(errors.first.get()));
            finalResults.relativeErrors.push_back(abs(errors.second.get()));

            relativeErrorSum += abs(errors.second.get());
            relativeErrorMax = fmax(relativeErrorMax, abs(errors.second.get()));
        }
    }

    // Store results over the whole dataset to final structure.
    finalResults.relativeErrorAvg = relativeErrorSum / numberOfQueries;
    finalResults.relativeErrorMax = relativeErrorMax;

    auto relativeErrorPercentiles = percentiles(finalResults.relativeErrors);
    finalResults.relativeErrorMedian = std::get<0>(relativeErrorPercentiles);
    finalResults.relativeError90thPercentile = std::get<1>(relativeErrorPercentiles);
    finalResults.relativeError95thPercentile = std::get<2>(relativeErrorPercentiles);
    finalResults.relativeError99thPercentile = std::get<3>(relativeErrorPercentiles);

    auto qErrorPercentiles = percentiles(finalResults.qErrors);
    finalResults.qErrorMedian = std::get<0>(qErrorPercentiles);
    finalResults.qError90thPercentile = std::get<1>(qErrorPercentiles);
    finalResults.qError95thPercentile = std::get<2>(qErrorPercentiles);
    finalResults.qError99thPercentile = std::get<3>(qErrorPercentiles);

    return finalResults;
}

bool checkTypeExistence(const TypeProbability& typeCombinationQuery,
                        const TypeCombination& typeCombinationsData) {

    bool typeExists = false;
    for (const auto& typeCombinationData : typeCombinationsData) {
        if (typeCombinationQuery.first == typeCombinationData.first) {
            typeExists = true;
        }
    }

    return typeExists;
}

void runAccuracyTestConfiguration(const DataDistributionEnum dataDistribution,
                                  const TypeCombinations& typeCombinationsData,
                                  const TypeCombination& typeCombinationsQueries,
                                  const std::vector<int>& numberOfBucketsVector,
                                  const size_t size,
                                  const std::pair<size_t, size_t>& dataInterval,
                                  const std::pair<size_t, size_t>& queryInterval,
                                  const int numberOfQueries,
                                  QueryType queryType,
                                  bool includeScalar,
                                  bool useE2EAPI,
                                  const size_t seed,
                                  bool printResults) {


    for (auto numberOfBuckets : numberOfBucketsVector) {
        for (const auto& typeCombinationData : typeCombinationsData) {
            // Random value generator for actual data in histogram.
            std::vector<stats::SBEValue> data;
            std::map<stats::SBEValue, double> insertedData;

            // Create one by one the values.
            switch (dataDistribution) {
                case kUniform:
                    generateDataUniform(size, dataInterval, typeCombinationData, seed, data);
                    break;
                case kNormal:
                    generateDataNormal(size, dataInterval, typeCombinationData, seed, data);
                    break;
                case kZipfian:
                    generateDataZipfian(size, dataInterval, typeCombinationData, seed, data);
                    break;
            }

            // Build histogram.
            auto arrHist = stats::createArrayEstimator(data, numberOfBuckets);

            // Run queries.
            for (const auto& typeCombinationQuery : typeCombinationsQueries) {

                if (!checkTypeExistence(typeCombinationQuery, typeCombinationData)) {
                    continue;
                }

                auto error = runQueries(size,
                                        numberOfQueries,
                                        queryType,
                                        queryInterval,
                                        typeCombinationQuery,
                                        data,
                                        arrHist,
                                        includeScalar,
                                        useE2EAPI,
                                        seed);
                if (printResults) {
                    printResult(dataDistribution,
                                typeCombinationData,
                                size,
                                numberOfBuckets,
                                typeCombinationQuery,
                                numberOfQueries,
                                queryType,
                                dataInterval,
                                error);
                }
            }
        }
    }
}

}  // namespace mongo::optimizer::cbp::ce
