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

#include <absl/random/zipf_distribution.h>
#include <sstream>

#include "mongo/db/query/ce/cbp_histogram_ce/test_helpers.h"
#include "mongo/db/query/stats/rand_utils_new.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::optimizer::cbp::ce {

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

double estimateCardinalityScalarHistogramInteger(const stats::ScalarHistogram& hist,
                                                 const int v,
                                                 const cbp::ce::EstimationType type) {
    const auto [tag, val] =
        std::make_pair(sbe::value::TypeTags::NumberInt64, sbe::value::bitcastFrom<int64_t>(v));
    auto estimate = estimateCardinality(hist, tag, val, type);
    return estimate.card;
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

static std::pair<double, double> computeErrors(size_t actualCard, double estimatedCard) {
    double error = estimatedCard - actualCard;
    double relError = (actualCard == 0) ? (estimatedCard == 0 ? 0.0 : -1.0) : error / actualCard;
    double qError = std::max((actualCard != 0.0)          ? (estimatedCard / actualCard)
                                 : (estimatedCard == 0.0) ? 0.0
                                                          : -1.0,
                             (estimatedCard != 0.0)    ? (actualCard / estimatedCard)
                                 : (actualCard == 0.0) ? 0.0
                                                       : -1.0);
    return std::make_pair(qError, relError);
}

stats::ScalarHistogram createHistogram(const std::vector<BucketData>& data) {
    sbe::value::Array bounds;
    std::vector<stats::Bucket> buckets;

    double cumulativeFreq = 0.0;
    double cumulativeNDV = 0.0;

    // Create a value vector & sort it.
    std::vector<stats::SBEValue> values;
    for (size_t i = 0; i < data.size(); i++) {
        const auto& item = data[i];
        const auto [tag, val] = sbe::value::makeValue(item._v);
        values.emplace_back(tag, val);
    }
    sortValueVector(values);

    for (size_t i = 0; i < values.size(); i++) {
        const auto& val = values[i];
        const auto [tag, value] = copyValue(val.getTag(), val.getValue());
        bounds.push_back(tag, value);

        const auto& item = data[i];
        cumulativeFreq += item._equalFreq + item._rangeFreq;
        cumulativeNDV += item._ndv + 1.0;
        buckets.emplace_back(
            item._equalFreq, item._rangeFreq, cumulativeFreq, item._ndv, cumulativeNDV);
    }
    return stats::ScalarHistogram::make(std::move(bounds), std::move(buckets));
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

void generateDataUniform(size_t size,
                         const std::pair<size_t, size_t>& interval,
                         const TypeCombination& typeCombination,
                         const size_t seed,
                         const size_t ndv,
                         std::vector<stats::SBEValue>& data) {

    // Generator for type selection.
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::mt19937::result_type> distTypes(0, 100);

    // Random value generator for actual data in histogram.
    std::mt19937_64 seedArray(42);
    std::mt19937_64 seedDataset(seed);

    stats::MixedDistributionDescriptor uniform{{stats::DistrType::kUniform, 1.0}};
    stats::TypeDistrVector td;

    for (auto type : typeCombination) {
        switch (type.first) {
            case sbe::value::TypeTags::NumberInt32:
            case sbe::value::TypeTags::NumberInt64:
                td.push_back(std::make_unique<stats::IntDistribution>(
                    uniform, type.second, ndv, interval.first, interval.second));
                break;
            case sbe::value::TypeTags::NumberDouble:
                td.push_back(std::make_unique<stats::DoubleDistribution>(
                    uniform, type.second, ndv, interval.first, interval.second));
                break;
            case sbe::value::TypeTags::StringSmall:
            case sbe::value::TypeTags::StringBig:
                td.push_back(std::make_unique<stats::StrDistribution>(
                    uniform, type.second, ndv, interval.first, interval.second));
                break;
            case sbe::value::TypeTags::Array: {
                stats::TypeDistrVector arrayData;
                arrayData.push_back(
                    std::make_unique<stats::IntDistribution>(uniform, 1.0, 100, 0, 10000));
                auto arrayDataDesc =
                    std::make_unique<stats::DatasetDescriptorNew>(std::move(arrayData), seedArray);
                td.push_back(std::make_unique<stats::ArrDistribution>(
                    uniform, 1.0, 10, 1, 5, std::move(arrayDataDesc)));
                break;
            }
            default:
                break;
        }
    }

    stats::DatasetDescriptorNew desc{std::move(td), seedDataset};
    data = desc.genRandomDataset(size);
}

void generateDataNormal(size_t size,
                        const std::pair<size_t, size_t>& interval,
                        const TypeCombination& typeCombination,
                        const size_t seed,
                        const size_t ndv,
                        std::vector<stats::SBEValue>& data) {

    // Generator for type selection.
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::mt19937::result_type> distTypes(0, 100);

    // Random value generator for actual data in histogram.
    std::mt19937_64 seedArray(42);
    std::mt19937_64 seedDataset(seed);

    stats::MixedDistributionDescriptor normal{{stats::DistrType::kNormal, 1.0}};
    stats::TypeDistrVector td;

    for (auto type : typeCombination) {
        switch (type.first) {
            case sbe::value::TypeTags::NumberInt32:
            case sbe::value::TypeTags::NumberInt64:
                td.push_back(std::make_unique<stats::IntDistribution>(
                    normal, type.second, ndv, interval.first, interval.second));
                break;
            case sbe::value::TypeTags::NumberDouble:
                td.push_back(std::make_unique<stats::DoubleDistribution>(
                    normal, type.second, ndv, interval.first, interval.second));
                break;
            case sbe::value::TypeTags::StringSmall:
            case sbe::value::TypeTags::StringBig:
                td.push_back(std::make_unique<stats::StrDistribution>(
                    normal, type.second, ndv, interval.first, interval.second));
                break;
            case sbe::value::TypeTags::Array: {
                stats::TypeDistrVector arrayData;
                arrayData.push_back(
                    std::make_unique<stats::IntDistribution>(normal, 1.0, 100, 0, 10000));
                auto arrayDataDesc =
                    std::make_unique<stats::DatasetDescriptorNew>(std::move(arrayData), seedArray);
                td.push_back(std::make_unique<stats::ArrDistribution>(
                    normal, 1.0, 10, 1, 5, std::move(arrayDataDesc)));
                break;
            }
            default:
                break;
        }
    }

    stats::DatasetDescriptorNew desc{std::move(td), seedDataset};
    data = desc.genRandomDataset(size);
}

void generateDataZipfian(const size_t size,
                         const std::pair<size_t, size_t>& interval,
                         const TypeCombination& typeCombination,
                         const size_t seed,
                         const size_t ndv,
                         std::vector<stats::SBEValue>& data) {

    // Generator for type selection.
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::mt19937::result_type> distTypes(0, 100);

    // Random value generator for actual data in histogram.
    std::mt19937_64 seedArray(42);
    std::mt19937_64 seedDataset(seed);

    stats::MixedDistributionDescriptor zipfian{{stats::DistrType::kZipfian, 1.0}};
    stats::TypeDistrVector td;

    for (auto type : typeCombination) {
        switch (type.first) {
            case sbe::value::TypeTags::NumberInt32:
            case sbe::value::TypeTags::NumberInt64:
                td.push_back(std::make_unique<stats::IntDistribution>(
                    zipfian, type.second, ndv, interval.first, interval.second));
                break;
            case sbe::value::TypeTags::NumberDouble:
                td.push_back(std::make_unique<stats::DoubleDistribution>(
                    zipfian, type.second, ndv, interval.first, interval.second));
                break;
            case sbe::value::TypeTags::StringSmall:
            case sbe::value::TypeTags::StringBig:
                td.push_back(std::make_unique<stats::StrDistribution>(
                    zipfian, type.second, ndv, interval.first, interval.second));
                break;
            case sbe::value::TypeTags::Array: {
                stats::TypeDistrVector arrayData;
                arrayData.push_back(
                    std::make_unique<stats::IntDistribution>(zipfian, 1.0, 100, 0, 10000));
                auto arrayDataDesc =
                    std::make_unique<stats::DatasetDescriptorNew>(std::move(arrayData), seedArray);
                td.push_back(std::make_unique<stats::ArrDistribution>(
                    zipfian, 1.0, 10, 1, 5, std::move(arrayDataDesc)));
                break;
            }
            default:
                break;
        }
    }

    stats::DatasetDescriptorNew desc{std::move(td), seedDataset};
    data = desc.genRandomDataset(size);
}

ErrorCalculationSummary runQueries(size_t size,
                                   size_t numberOfQueries,
                                   QueryType queryType,
                                   const std::pair<size_t, size_t> interval,
                                   const std::pair<sbe::value::TypeTags, size_t> queryTypeInfo,
                                   const std::vector<stats::SBEValue>& data,
                                   const std::shared_ptr<const stats::ArrayHistogram> arrHist,
                                   bool includeScalar,
                                   const size_t seed) {
    double relativeErrorSum = 0, relativeErrorMax = 0;
    ErrorCalculationSummary finalResults;

    // sbeValLow stores also the values for the equality comparison.
    std::vector<stats::SBEValue> sbeValLow, sbeValHigh;
    TypeCombination typeCombination{{queryTypeInfo.first, 100}};
    switch (queryType) {
        case kPoint: {
            // For ndv we set the number of values in the provided data interval. This may lead to
            // re-running values the same values if the number of queries is larger than the size of
            // the interval.
            generateDataUniform(numberOfQueries,
                                interval,
                                typeCombination,
                                seed,
                                interval.second - interval.first /*ndv*/,
                                sbeValLow);
            break;
        }
        case kRange: {

            const std::pair<size_t, size_t> intervalLow{
                interval.first, interval.first + (interval.second - interval.first) / 2};

            const std::pair<size_t, size_t> intervalHigh{
                interval.first + (interval.second - interval.first) / 2, interval.second};

            // For ndv we set the number of values in the provided data interval. This may lead to
            // re-running values the same values if the number of queries is larger than the size of
            // the interval.
            generateDataUniform(numberOfQueries,
                                intervalLow,
                                typeCombination,
                                seed,
                                intervalLow.second - intervalLow.first /*ndv*/,
                                sbeValLow);

            generateDataUniform(numberOfQueries,
                                intervalHigh,
                                typeCombination,
                                seed,
                                intervalHigh.second - intervalHigh.first /*ndv*/,
                                sbeValHigh);
            break;
        }
    }

    for (size_t i = 0; i < numberOfQueries; i++) {

        size_t actualCard;
        EstimationResult estimatedCard;

        switch (queryType) {
            case kPoint: {

                // Find actual frequency.
                actualCard =
                    calculateFrequencyFromDataVectorEq(data, queryTypeInfo.first, sbeValLow[i]);

                // Estimate result.
                estimatedCard = mongo::optimizer::cbp::ce::estimateCardinalityEq(
                    *arrHist, queryTypeInfo.first, sbeValLow[i].getValue(), includeScalar);
                break;
            }
            case kRange: {
                if (mongo::stats::compareValues(sbeValLow[i].getTag(),
                                                sbeValLow[i].getValue(),
                                                sbeValHigh[i].getTag(),
                                                sbeValHigh[i].getValue()) >= 0) {
                    continue;
                }

                // Find actual frequency.
                actualCard = calculateFrequencyFromDataVectorRange(
                    data, queryTypeInfo.first, sbeValLow[i], sbeValHigh[i]);

                // Estimate result.
                estimatedCard =
                    mongo::optimizer::cbp::ce::estimateCardinalityRange(*arrHist,
                                                                        true /*lowInclusive*/,
                                                                        queryTypeInfo.first,
                                                                        sbeValLow[i].getValue(),
                                                                        true /*highInclusive*/,
                                                                        queryTypeInfo.first,
                                                                        sbeValHigh[i].getValue(),
                                                                        includeScalar);
                break;
            }
        }

        // store results to final structure.
        finalResults.queryResults.push_back({actualCard, estimatedCard});

        std::pair<double, double> errors = computeErrors(actualCard, estimatedCard.card);

        finalResults.qErrors.push_back(abs(errors.first));
        finalResults.relativeErrors.push_back(abs(errors.second));

        relativeErrorSum += abs(errors.second);
        relativeErrorMax = fmax(relativeErrorMax, abs(errors.second));
    }

    // store results over the whole dataset to final structure.
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

}  // namespace mongo::optimizer::cbp::ce
