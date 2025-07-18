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

#include "mongo/db/query/compiler/ce/histogram/histogram_test_utils.h"

#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/query/compiler/ce/histogram/histogram_estimation_impl.h"
#include "mongo/db/query/compiler/stats/max_diff.h"
#include "mongo/db/query/compiler/stats/value_utils.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::ce {
namespace value = sbe::value;

using stats::CEHistogram;
using stats::ScalarHistogram;
using stats::TypeCounts;

auto NumberInt64 = sbe::value::TypeTags::NumberInt64;
auto kEqual = EstimationType::kEqual;
auto kLess = EstimationType::kLess;
auto kLessOrEqual = EstimationType::kLessOrEqual;
auto kGreater = EstimationType::kGreater;
auto kGreaterOrEqual = EstimationType::kGreaterOrEqual;
auto Date = sbe::value::TypeTags::Date;
auto TimeStamp = sbe::value::TypeTags::Timestamp;

stats::ScalarHistogram createHistogram(const std::vector<BucketData>& data) {
    value::Array bounds;
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

double estimateCardinalityScalarHistogramInteger(const stats::ScalarHistogram& hist,
                                                 const int v,
                                                 const EstimationType type) {
    const auto [tag, val] =
        std::make_pair(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(v));
    return estimateCardinality(hist, tag, val, type).card;
};

/**
    Given a vector of values, create a histogram reflection the distribution of the vector
    with the supplied number of buckets.
*/
ScalarHistogram makeHistogram(std::vector<stats::SBEValue>& randData, size_t nBuckets) {
    sortValueVector(randData);
    const stats::DataDistribution& dataDistrib = getDataDistribution(randData);
    return genMaxDiffHistogram(dataDistrib, nBuckets);
}

void printResult(const stats::DistrType& dataDistribution,
                 const TypeCombination& typeCombination,
                 const int size,
                 const int numberOfBuckets,
                 const TypeProbability& typeCombinationQuery,
                 const int numberOfQueries,
                 QueryType queryType,
                 const std::pair<size_t, size_t>& dataInterval,
                 bool includeScalar,
                 const size_t seedData,
                 const size_t seedQueriesLow,
                 const size_t seedQueriesHigh,
                 const std::vector<std::pair<TypeTags, sbe::value::Value>>& bounds,
                 ErrorCalculationSummary error) {

    BSONObjBuilder builder;

    switch (dataDistribution) {
        case stats::DistrType::kUniform:
            builder << "DataDistribution"
                    << "Uniform";
            break;
        case stats::DistrType::kNormal:
            builder << "DataDistribution"
                    << "Normal";
            break;
        case stats::DistrType::kZipfian:
            builder << "DataDistribution"
                    << "Zipfian";
            break;
    }

    builder << "NumberOfHistogramBuckets" << numberOfBuckets;

    std::stringstream ss;
    for (auto type : typeCombination) {
        ss << type.typeTag << "." << type.typeProbability << "." << type.nanProb << " ";
    }
    builder << "DataTypes" << ss.str();
    builder << "IncludeScalar" << includeScalar;
    builder << "DataSize" << size;

    std::stringstream ssSeedData, ssSeedQueryLow, ssSeedQueryHigh;
    ssSeedData << seedData;
    builder << "DataSeed" << ssSeedData.str();

    ssSeedQueryLow << seedQueriesLow;
    builder << "QueriesSeedLow" << ssSeedQueryLow.str();

    ssSeedQueryHigh << seedQueriesHigh;
    builder << "QueriesSeedHigh" << ssSeedQueryHigh.str();

    switch (queryType) {
        case kPoint:
            builder << "QueryType"
                    << "Point";
            break;
        case kRange:
            builder << "QueryType"
                    << "Range";
            break;
    }

    std::stringstream ssDataType;
    ssDataType << typeCombinationQuery.typeTag;
    builder << "QueryDataType" << ssDataType.str();

    builder << "NumberOfQueries" << numberOfQueries;

    std::vector<std::string> queryValuesLow;
    std::vector<std::string> queryValuesHigh;
    std::vector<double> ActualCadinality;
    std::vector<double> Estimation;
    for (auto values : error.queryResults) {
        if (values.low->getTag() == sbe::value::TypeTags::StringBig ||
            values.low->getTag() == sbe::value::TypeTags::StringSmall) {
            std::stringstream sslow;
            sslow << values.low.get().getValue();
            queryValuesLow.push_back(sslow.str());
            std::stringstream sshigh;
            sshigh << values.high.get().getValue();
            queryValuesHigh.push_back(sshigh.str());
        } else {
            std::stringstream sslow;
            sslow << values.low.get().get();
            queryValuesLow.push_back(sslow.str());
            std::stringstream sshigh;
            sshigh << values.high.get().get();
            queryValuesHigh.push_back(sshigh.str());
        }
        ActualCadinality.push_back(values.actualCardinality);
        Estimation.push_back(values.estimatedCardinality);
    }

    std::vector<std::string> bucketBounds;
    for (const auto& bound : bounds) {
        if (bound.first == sbe::value::TypeTags::StringBig ||
            bound.first == sbe::value::TypeTags::StringSmall) {
            std::stringstream bd;
            bd << bound.second;
            bucketBounds.push_back(bd.str());
        } else {
            std::stringstream bd;
            stats::SBEValue val(bound.first, bound.second);
            bd << val.get();
            bucketBounds.push_back(bd.str());
        }
    }

    builder << "QueryLow" << queryValuesLow;
    builder << "QueryHigh" << queryValuesHigh;
    builder << "HistogramBounds" << bucketBounds;
    builder << "ActualCadinality" << ActualCadinality;
    builder << "Estimation" << Estimation;

    LOGV2(8871202, "Accuracy experiment", ""_attr = builder.obj());
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
                                   const size_t seedQueriesLow,
                                   const size_t seedQueriesHigh) {
    ErrorCalculationSummary finalResults;

    auto queryIntervals = generateIntervals(
        queryType, interval, numberOfQueries, {queryTypeInfo}, seedQueriesLow, seedQueriesHigh);

    // Transform input data to vector of BSONObj to simplify calculation of actual cardinality.
    std::vector<BSONObj> bsonData = transformSBEValueVectorToBSONObjVector(data);

    for (size_t i = 0; i < queryIntervals.size(); i++) {

        auto expr = createQueryMatchExpression(
            queryType, queryIntervals[i].first, queryIntervals[i].second);

        size_t actualCard = calculateCardinality(expr.get(), bsonData);

        EstimationResult estimatedCard = runSingleQuery(queryType,
                                                        queryIntervals[i].first,
                                                        queryIntervals[i].second,
                                                        ceHist,
                                                        includeScalar,
                                                        arrayRangeEstimationAlgo,
                                                        useE2EAPI,
                                                        size);

        // Store results to final structure.
        QueryInfoAndResults queryInfoResults;
        queryInfoResults.low = queryIntervals[i].first;
        queryInfoResults.high = queryIntervals[i].second;

        // We store results to calculate Q-error:
        // Q-error = max(true/est, est/true)
        // where "est" is the estimated cardinality and "true" is the true cardinality.
        // In practice we replace est = max(est, 1) and true = max(est, 1) to avoid divide-by-zero.
        // Q-error = 1 indicates a perfect prediction.
        queryInfoResults.actualCardinality = fmax(actualCard, 1.0);
        queryInfoResults.estimatedCardinality = fmax(estimatedCard.card, 1.0);

        finalResults.queryResults.push_back(queryInfoResults);

        // Increment the number of executed queries.
        ++finalResults.executedQueries;
    }

    return finalResults;
}

void runAccuracyTestConfiguration(const stats::DistrType dataDistribution,
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
                                  const size_t seedData,
                                  const size_t seedQueriesLow,
                                  const size_t seedQueriesHigh,
                                  bool printResults,
                                  int arrayTypeLength) {

    auto ndv = std::max((size_t)1, (size_t)(dataInterval.second - dataInterval.first));
    for (auto numberOfBuckets : numberOfBucketsVector) {
        for (const auto& typeCombinationData : typeCombinationsData) {
            // Random value generator for actual data in histogram.
            std::vector<stats::SBEValue> data;
            std::map<stats::SBEValue, double> insertedData;

            // Create one by one the values.
            generateDataOneField(ndv,
                                 size,
                                 typeCombinationData,
                                 dataDistribution,
                                 dataInterval,
                                 seedData,
                                 arrayTypeLength,
                                 data);

            // Build histogram.
            auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

            // Run queries.
            for (const auto& typeCombinationQuery : typeCombinationsQueries) {

                if (!checkTypeExistence(typeCombinationQuery.typeTag, typeCombinationData)) {
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
                                        seedQueriesLow,
                                        seedQueriesHigh);
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
                                seedData,
                                seedQueriesLow,
                                seedQueriesHigh,
                                ceHist->getScalar().getBounds().values(),
                                error);
                }
            }
        }
    }
}

}  // namespace mongo::ce
