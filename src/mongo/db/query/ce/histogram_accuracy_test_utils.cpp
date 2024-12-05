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

#include <sstream>

#include "mongo/db/query/ce/histogram_accuracy_test_utils.h"
#include "mongo/db/query/ce/histogram_estimation_impl.h"

namespace mongo::ce {

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

/**
 * Compute the 50th, 90th, 95th, and 99th percentile values of a given vector.
 * This function creates a copy of the given vector which it sorts and over which it calculates the
 * percentiles.
 */
std::tuple<double, double, double, double> percentiles(std::vector<double> arr) {

    // Check if the simulation has returned at least one error estimation.
    if (arr.size() == 0) {
        return {-1, -1, -1, -1};
    }

    // Sort array before calculating the cummulative stats.
    std::sort(arr.begin(), arr.end());

    return {arr[(size_t)(arr.size() * 0.50)],
            arr[(size_t)(arr.size() * 0.90)],
            arr[(size_t)(arr.size() * 0.95)],
            arr[(size_t)(arr.size() * 0.99)]};
}

size_t calculateFrequencyFromDataVectorEq(const std::vector<stats::SBEValue>& data,
                                          stats::SBEValue valueToCalculate,
                                          bool includeScalar) {
    int actualCard = 0;
    for (const auto& value : data) {
        if (value.getTag() == TypeTags::Array) {
            auto array = sbe::value::getArrayView(value.getValue());

            bool matched = std::any_of(
                array->values().begin(), array->values().end(), [&](const auto& element) {
                    return mongo::stats::compareValues(element.first,
                                                       element.second,
                                                       valueToCalculate.getTag(),
                                                       valueToCalculate.getValue()) == 0;
                });

            if (matched) {
                actualCard++;
            }
        } else {
            if (includeScalar) {
                if (mongo::stats::compareValues(value.getTag(),
                                                value.getValue(),
                                                valueToCalculate.getTag(),
                                                valueToCalculate.getValue()) == 0) {
                    actualCard++;
                }
            }
        }
    }
    return actualCard;
}

size_t calculateTypeFrequencyFromDataVectorEq(const std::vector<stats::SBEValue>& data,
                                              sbe::value::TypeTags type) {
    int actualCard = 0;
    for (const auto& value : data) {
        if (type == value.getTag()) {
            actualCard++;
        }
    }
    return actualCard;
}

static size_t calculateFrequencyFromDataVectorRange(const std::vector<stats::SBEValue>& data,
                                                    stats::SBEValue valueToCalculateLow,
                                                    stats::SBEValue valueToCalculateHigh) {
    int actualCard = 0;
    for (const auto& value : data) {
        // Higher OR equal to low AND lower OR equal to high.
        if (((mongo::stats::compareValues(value.getTag(),
                                          value.getValue(),
                                          valueToCalculateLow.getTag(),
                                          valueToCalculateLow.getValue()) > 0) ||
             (mongo::stats::compareValues(value.getTag(),
                                          value.getValue(),
                                          valueToCalculateLow.getTag(),
                                          valueToCalculateLow.getValue()) == 0)) &&
            ((mongo::stats::compareValues(value.getTag(),
                                          value.getValue(),
                                          valueToCalculateHigh.getTag(),
                                          valueToCalculateHigh.getValue()) < 0) ||
             (mongo::stats::compareValues(value.getTag(),
                                          value.getValue(),
                                          valueToCalculateHigh.getTag(),
                                          valueToCalculateHigh.getValue()) == 0))) {
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
    ss << "Data distribution, Number of histogram buckets, Data type, IncludeScalar, Data size, "
       << "Query type, Query data type, Number of Queries, "
       << "Data interval start, Data interval end, "
       << "relative error (Avg), relative error (Max), relative error "
          "(Median), relative error (90th), relative error (95%), relative error (99%), Q-Error "
          "(Median), Q-Error (90%), Q-Error (95%), Q-Error (99%)";
    LOGV2(8871201, "Accuracy experiment header", ""_attr = ss.str());
}

void printResult(const DataDistributionEnum dataDistribution,
                 const TypeCombination& typeCombination,
                 const int size,
                 const int numberOfBuckets,
                 const TypeProbability& typeCombinationQuery,
                 const int numberOfQueries,
                 QueryType queryType,
                 const std::pair<size_t, size_t>& dataInterval,
                 bool includeScalar,
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
        ss << type.typeTag << "." << type.typeProbability << "." << type.nanProb << " ";
    }
    ss << ", ";

    ss << includeScalar << ", ";

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
    ss << queryTypeStr << ", " << typeCombinationQuery.typeTag << ", ";

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
                                                   const size_t ndv,
                                                   std::mt19937_64& seedArray,
                                                   stats::MixedDistributionDescriptor& mdd,
                                                   int arrayLength = 0) {
    for (auto type : typeCombination) {

        switch (type.typeTag) {
            case sbe::value::TypeTags::Nothing:
            case sbe::value::TypeTags::Null:
                td.push_back(
                    std::make_unique<stats::NullDistribution>(mdd, type.typeProbability, ndv));
                break;
            case sbe::value::TypeTags::Boolean: {
                bool includeFalse = false, includeTrue = false;
                if (!(bool)interval.first || !(bool)interval.second) {
                    includeFalse = true;
                }
                if ((bool)interval.first || (bool)interval.second) {
                    includeTrue = true;
                }
                td.push_back(std::make_unique<stats::BooleanDistribution>(mdd,
                                                                          type.typeProbability,
                                                                          (int)includeFalse +
                                                                              (int)includeTrue,
                                                                          includeFalse,
                                                                          includeTrue));
                break;
            }
            case sbe::value::TypeTags::NumberInt32:
            case sbe::value::TypeTags::NumberInt64:
                td.push_back(std::make_unique<stats::IntDistribution>(mdd,
                                                                      type.typeProbability,
                                                                      ndv,
                                                                      interval.first,
                                                                      interval.second,
                                                                      0 /*nullsRatio*/,
                                                                      type.nanProb));
                break;
            case sbe::value::TypeTags::NumberDouble:
                td.push_back(std::make_unique<stats::DoubleDistribution>(mdd,
                                                                         type.typeProbability,
                                                                         ndv,
                                                                         interval.first,
                                                                         interval.second,
                                                                         0 /*nullsRatio*/,
                                                                         type.nanProb));
                break;
            case sbe::value::TypeTags::StringSmall:
            case sbe::value::TypeTags::StringBig:
                td.push_back(std::make_unique<stats::StrDistribution>(
                    mdd, type.typeProbability, ndv, interval.first, interval.second));
                break;
            case sbe::value::TypeTags::Array: {
                stats::TypeDistrVector arrayData;
                arrayData.push_back(std::make_unique<stats::IntDistribution>(
                    mdd, type.typeProbability, ndv, interval.first, interval.second));
                auto arrayDataDesc =
                    std::make_unique<stats::DatasetDescriptorNew>(std::move(arrayData), seedArray);
                td.push_back(std::make_unique<stats::ArrDistribution>(mdd,
                                                                      1.0 /*weight*/,
                                                                      10 /*ndv*/,
                                                                      0 /*minArraLen*/,
                                                                      arrayLength /*maxArrLen*/,
                                                                      std::move(arrayDataDesc)));
                break;
            }
            default:
                MONGO_UNREACHABLE;
                break;
        }
    }
}

void generateDataUniform(size_t size,
                         const std::pair<size_t, size_t>& interval,
                         const TypeCombination& typeCombination,
                         const size_t seed,
                         const size_t ndv,
                         std::vector<stats::SBEValue>& data,
                         int arrayLength) {

    // Generator for type selection.
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::mt19937::result_type> distTypes(0, 100);

    // Random value generator for actual data in histogram.
    std::mt19937_64 seedArray(42);
    std::mt19937_64 seedDataset(seed);

    stats::MixedDistributionDescriptor uniform{{stats::DistrType::kUniform, 1.0}};
    stats::TypeDistrVector td;

    populateTypeDistrVectorAccordingToInputConfig(
        td, interval, typeCombination, ndv, seedArray, uniform, arrayLength);

    stats::DatasetDescriptorNew desc{std::move(td), seedDataset};
    data = desc.genRandomDataset(size);
}

void generateDataNormal(size_t size,
                        const std::pair<size_t, size_t>& interval,
                        const TypeCombination& typeCombination,
                        const size_t seed,
                        const size_t ndv,
                        std::vector<stats::SBEValue>& data,
                        int arrayLength) {

    // Generator for type selection.
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::mt19937::result_type> distTypes(0, 100);

    // Random value generator for actual data in histogram.
    std::mt19937_64 seedArray(42);
    std::mt19937_64 seedDataset(seed);

    stats::MixedDistributionDescriptor normal{{stats::DistrType::kNormal, 1.0}};
    stats::TypeDistrVector td;

    populateTypeDistrVectorAccordingToInputConfig(
        td, interval, typeCombination, ndv, seedArray, normal, arrayLength);

    stats::DatasetDescriptorNew desc{std::move(td), seedDataset};
    data = desc.genRandomDataset(size);
}

void generateDataZipfian(const size_t size,
                         const std::pair<size_t, size_t>& interval,
                         const TypeCombination& typeCombination,
                         const size_t seed,
                         const size_t ndv,
                         std::vector<stats::SBEValue>& data,
                         int arrayLength) {

    // Generator for type selection.
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::mt19937::result_type> distTypes(0, 100);

    // Random value generator for actual data in histogram.
    std::mt19937_64 seedArray(42);
    std::mt19937_64 seedDataset(seed);

    stats::MixedDistributionDescriptor zipfian{{stats::DistrType::kZipfian, 1.0}};
    stats::TypeDistrVector td;

    populateTypeDistrVectorAccordingToInputConfig(
        td, interval, typeCombination, ndv, seedArray, zipfian, arrayLength);

    stats::DatasetDescriptorNew desc{std::move(td), seedDataset};
    data = desc.genRandomDataset(size);
}

std::vector<std::pair<stats::SBEValue, stats::SBEValue>> generateIntervals(
    QueryType queryType,
    const std::pair<size_t, size_t>& interval,
    size_t numberOfQueries,
    const TypeProbability& queryTypeInfo,
    const size_t seed) {
    std::vector<stats::SBEValue> sbeValLow, sbeValHigh;
    switch (queryType) {
        case kPoint: {
            // For ndv we set the number of values in the provided data interval. This may lead to
            // re-running values the same values if the number of queries is larger than the size of
            // the interval.
            auto ndv = interval.second - interval.first;
            generateDataUniform(numberOfQueries, interval, {queryTypeInfo}, seed, ndv, sbeValLow);
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
            auto ndv = intervalLow.second - intervalLow.first;
            generateDataUniform(
                numberOfQueries, intervalLow, {queryTypeInfo}, seed, ndv, sbeValLow);

            generateDataUniform(
                numberOfQueries, intervalHigh, {queryTypeInfo}, seed, ndv, sbeValHigh);

            for (size_t i = 0; i < sbeValLow.size();) {
                if (mongo::stats::compareValues(sbeValLow[i].getTag(),
                                                sbeValLow[i].getValue(),
                                                sbeValHigh[i].getTag(),
                                                sbeValHigh[i].getValue()) >= 0) {
                    // Remove elements from both vectors
                    sbeValLow.erase(sbeValLow.begin() + i);
                    sbeValHigh.erase(sbeValHigh.begin() + i);
                } else {
                    // Only increment if no removal to avoid skipping elements
                    ++i;
                }
            }
            break;
        }
    }

    std::vector<std::pair<stats::SBEValue, stats::SBEValue>> intervals;
    for (size_t i = 0; i < sbeValLow.size(); ++i) {
        if (queryType == kPoint) {
            // Copy the first argument and move the second argument.
            intervals.emplace_back(sbeValLow[i], std::move(sbeValLow[i]));
        } else {
            intervals.emplace_back(std::move(sbeValLow[i]), std::move(sbeValHigh[i]));
        }
    }
    return intervals;
}

EstimationResult runSingleQuery(QueryType queryType,
                                const stats::SBEValue& sbeValLow,
                                const stats::SBEValue& sbeValHigh,
                                const std::shared_ptr<const stats::CEHistogram>& ceHist,
                                bool includeScalar,
                                ArrayRangeEstimationAlgo arrayRangeEstimationAlgo,
                                bool useE2EAPI,
                                size_t size) {
    EstimationResult estimatedCard;

    switch (queryType) {
        case kPoint: {
            if (useE2EAPI) {
                BSONObj bsonInterval = sbeValuesToInterval(sbeValLow, "", sbeValLow, "");

                Interval interval(bsonInterval, true /*startIncluded*/, true /*endIncluded*/);

                auto sizeCardinality =
                    CardinalityEstimate{CardinalityType{(double)size}, EstimationSource::Histogram};

                estimatedCard.card =
                    HistogramEstimator::estimateCardinality(*ceHist,
                                                            sizeCardinality,
                                                            interval,
                                                            includeScalar,
                                                            ArrayRangeEstimationAlgo::kExactArrayCE)
                        .toDouble();

            } else {
                // Estimate result.
                estimatedCard = estimateCardinalityEq(
                    *ceHist, sbeValLow.getTag(), sbeValLow.getValue(), includeScalar);
            }

            break;
        }
        case kRange: {
            if (useE2EAPI) {
                BSONObj bsonInterval = sbeValuesToInterval(sbeValLow, "", sbeValHigh, "");

                Interval interval(bsonInterval, true /*startIncluded*/, true /*endIncluded*/);

                auto sizeCardinality =
                    CardinalityEstimate{CardinalityType{(double)size}, EstimationSource::Histogram};

                estimatedCard.card =
                    HistogramEstimator::estimateCardinality(*ceHist,
                                                            sizeCardinality,
                                                            interval,
                                                            includeScalar,
                                                            ArrayRangeEstimationAlgo::kExactArrayCE)
                        .toDouble();
            } else {
                // Estimate result.
                estimatedCard = estimateCardinalityRange(*ceHist,
                                                         true /*lowInclusive*/,
                                                         sbeValLow.getTag(),
                                                         sbeValLow.getValue(),
                                                         true /*highInclusive*/,
                                                         sbeValHigh.getTag(),
                                                         sbeValHigh.getValue(),
                                                         includeScalar,
                                                         arrayRangeEstimationAlgo);
            }
            break;
        }
    }
    return estimatedCard;
}

ErrorCalculationSummary runQueries(size_t size,
                                   size_t numberOfQueries,
                                   QueryType queryType,
                                   const std::pair<size_t, size_t> interval,
                                   const TypeProbability queryTypeInfo,
                                   const std::vector<stats::SBEValue>& data,
                                   const std::shared_ptr<const stats::CEHistogram> ceHist,
                                   bool includeScalar,
                                   ArrayRangeEstimationAlgo arrayRangeEstimationAlgo,
                                   bool useE2EAPI,
                                   const size_t seed) {
    double relativeErrorSum = 0, relativeErrorMax = 0;
    ErrorCalculationSummary finalResults;

    auto queryIntervals =
        generateIntervals(queryType, interval, numberOfQueries, queryTypeInfo, seed);

    for (size_t i = 0; i < numberOfQueries; i++) {
        size_t actualCard;
        EstimationResult estimatedCard = runSingleQuery(queryType,
                                                        queryIntervals[i].first,
                                                        queryIntervals[i].second,
                                                        ceHist,
                                                        includeScalar,
                                                        arrayRangeEstimationAlgo,
                                                        useE2EAPI,
                                                        size);

        switch (queryType) {
            case kPoint: {
                // Find actual frequency.
                actualCard = calculateFrequencyFromDataVectorEq(
                    data, queryIntervals[i].first, includeScalar);

                break;
            }
            case kRange: {
                // Find actual frequency.
                actualCard = calculateFrequencyFromDataVectorRange(
                    data, queryIntervals[i].first, queryIntervals[i].second);
                break;
            }
        }

        // Store results to final structure.
        auto errors = computeErrors(actualCard, estimatedCard.card);

        if (errors.first.has_value() && errors.second.has_value()) {
            finalResults.queryResults.push_back({actualCard, estimatedCard});

            finalResults.qErrors.push_back(abs(errors.first.get()));
            finalResults.relativeErrors.push_back(abs(errors.second.get()));

            relativeErrorSum += abs(errors.second.get());
            relativeErrorMax = fmax(relativeErrorMax, abs(errors.second.get()));
        }

        // Increment the number of executed queries.
        ++finalResults.executedQueries;
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
        if (typeCombinationQuery.typeTag == typeCombinationData.typeTag) {
            typeExists = true;
        } else if (typeCombinationData.typeTag == TypeTags::Array &&
                   typeCombinationQuery.typeTag == TypeTags::NumberInt64) {
            // If the data type is array, we accept queries on integers. (the default data type in
            // arrays is integer.)
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
                                  ArrayRangeEstimationAlgo arrayRangeEstimationAlgo,
                                  bool useE2EAPI,
                                  const size_t seed,
                                  bool printResults,
                                  int arrayTypeLength) {

    auto ndv = std::max((size_t)1, (size_t)(dataInterval.second - dataInterval.first));
    for (auto numberOfBuckets : numberOfBucketsVector) {
        for (const auto& typeCombinationData : typeCombinationsData) {
            // Random value generator for actual data in histogram.
            std::vector<stats::SBEValue> data;
            std::map<stats::SBEValue, double> insertedData;

            // Create one by one the values.
            switch (dataDistribution) {
                case kUniform:
                    generateDataUniform(
                        size, dataInterval, typeCombinationData, seed, ndv, data, arrayTypeLength);
                    break;
                case kNormal:
                    generateDataNormal(
                        size, dataInterval, typeCombinationData, seed, ndv, data, arrayTypeLength);
                    break;
                case kZipfian:
                    generateDataZipfian(
                        size, dataInterval, typeCombinationData, seed, ndv, data, arrayTypeLength);
                    break;
            }

            // Build histogram.
            auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

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
                                        ceHist,
                                        includeScalar,
                                        arrayRangeEstimationAlgo,
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
                                includeScalar,
                                error);
                }
            }
        }
    }
}

}  // namespace mongo::ce
