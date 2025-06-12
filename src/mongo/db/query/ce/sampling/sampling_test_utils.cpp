/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/ce/sampling/sampling_test_utils.h"

#include "mongo/db/concurrency/exception_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::ce {

void generateData(size_t ndv,
                  size_t size,
                  TypeCombination typeCombinationData,
                  DataDistributionEnum dataDistribution,
                  const std::pair<size_t, size_t>& dataInterval,
                  size_t seedData,
                  int arrayTypeLength,
                  std::vector<stats::SBEValue>& data) {
    tassert(
        10545500, "For valid data generation number of distinct values (NDV) must be > 0", ndv > 0);
    switch (dataDistribution) {
        case kUniform:
            generateDataUniform(
                size, dataInterval, typeCombinationData, seedData, ndv, data, arrayTypeLength);
            break;
        case kNormal:
            generateDataNormal(
                size, dataInterval, typeCombinationData, seedData, ndv, data, arrayTypeLength);
            break;
        case kZipfian:
            generateDataZipfian(
                size, dataInterval, typeCombinationData, seedData, ndv, data, arrayTypeLength);
            break;
    }
}

void generateData(SamplingEstimationBenchmarkConfiguration& configuration,
                  const size_t seedData,
                  std::vector<stats::SBEValue>& data) {

    const TypeCombination typeCombinationData{
        TypeCombination{{configuration.sbeDataType, 100, configuration.nanProb}}};

    tassert(10472400,
            "For valid data generation number of distinct values (NDV) must be initialized",
            configuration.ndv.has_value());

    generateData(configuration.ndv.value(),
                 configuration.size,
                 typeCombinationData,
                 configuration.dataDistribution,
                 configuration.dataInterval,
                 seedData,
                 configuration.arrayTypeLength,
                 data);
}
ErrorCalculationSummary runQueries(size_t size,
                                   size_t numberOfQueries,
                                   QueryType queryType,
                                   const std::pair<size_t, size_t> interval,
                                   const TypeProbability queryTypeInfo,
                                   const std::vector<stats::SBEValue>& data,
                                   const SamplingEstimatorImpl* ceSample,
                                   const size_t seedQueriesLow,
                                   const size_t seedQueriesHigh) {
    ErrorCalculationSummary finalResults;

    auto queryIntervals = generateIntervals(
        queryType, interval, numberOfQueries, queryTypeInfo, seedQueriesLow, seedQueriesHigh);

    // Transform input data to vector of BSONObj to simplify calculation of actual cardinality.
    std::vector<BSONObj> bsonData = transformSBEValueVectorToBSONObjVector(data);

    for (size_t i = 0; i < queryIntervals.size(); i++) {

        auto expr = createQueryMatchExpression(
            queryType, queryIntervals[i].first, queryIntervals[i].second);

        size_t actualCard = calculateCardinality(expr.get(), bsonData);

        CardinalityEstimate estimatedCard = ceSample->estimateCardinality(expr.get());

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
        queryInfoResults.estimatedCardinality = fmax(estimatedCard.toDouble(), 1.0);

        finalResults.queryResults.push_back(queryInfoResults);

        // Increment the number of executed queries.
        ++finalResults.executedQueries;
    }

    return finalResults;
}

void createCollAndInsertDocuments(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const std::vector<BSONObj>& docs) {
    writeConflictRetry(opCtx, "createColl", nss, [&] {
        shard_role_details::getRecoveryUnit(opCtx)->setTimestampReadSource(
            RecoveryUnit::ReadSource::kNoTimestamp);
        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

        WriteUnitOfWork wunit(opCtx);
        AutoGetCollection collRaii(opCtx, nss, MODE_X);

        auto db = collRaii.ensureDbExists(opCtx);
        invariant(db->createCollection(opCtx, nss, {}));
        wunit.commit();
    });

    std::vector<InsertStatement> inserts{docs.begin(), docs.end()};

    AutoGetCollection agc(opCtx, nss, LockMode::MODE_IX);
    {
        WriteUnitOfWork wuow{opCtx};
        ASSERT_OK(collection_internal::insertDocuments(
            opCtx, *agc, inserts.begin(), inserts.end(), nullptr /* opDebug */));
        wuow.commit();
    }
}

void printResult(const DataDistributionEnum& dataDistribution,
                 const TypeCombination& typeCombination,
                 int size,
                 int sampleSize,
                 const TypeProbability& typeCombinationQuery,
                 int numberOfQueries,
                 QueryType queryType,
                 const std::pair<size_t, size_t>& dataInterval,
                 size_t seedData,
                 size_t seedQueriesLow,
                 size_t seedQueriesHigh,
                 const std::pair<SamplingEstimatorImpl::SamplingStyle, boost::optional<int>>&
                     samplingAlgoAndChunks,
                 ErrorCalculationSummary error) {

    BSONObjBuilder builder;

    switch (dataDistribution) {
        case kUniform:
            builder << "DataDistribution"
                    << "Uniform";
            break;
        case kNormal:
            builder << "DataDistribution"
                    << "Normal";
            break;
        case kZipfian:
            builder << "DataDistribution"
                    << "Zipfian";
            break;
    }

    builder << "sampleSize" << sampleSize;

    std::stringstream ss;
    for (auto type : typeCombination) {
        ss << type.typeTag << "." << type.typeProbability << "." << type.nanProb << " ";
    }
    builder << "DataTypes" << ss.str();
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
    std::vector<double> actualCardinality;
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
        actualCardinality.push_back(values.actualCardinality);
        Estimation.push_back(values.estimatedCardinality);
    }

    builder << "QueryLow" << queryValuesLow;
    builder << "QueryHigh" << queryValuesHigh;

    std::stringstream ssSamplingAlgoChunks;
    ssSamplingAlgoChunks << static_cast<int>(samplingAlgoAndChunks.first) << "-"
                         << samplingAlgoAndChunks.second.value_or(0);

    builder << "samplingAlgoChunks" << ssSamplingAlgoChunks.str();
    builder << "numberOfChunks" << samplingAlgoAndChunks.second.value_or(0);
    builder << "ActualCardinality" << actualCardinality;
    builder << "Estimation" << Estimation;

    LOGV2(10545501, "Accuracy experiment", ""_attr = builder.obj());
}

void SamplingAccuracyTest::runSamplingEstimatorTestConfiguration(
    const DataDistributionEnum dataDistribution,
    const TypeCombination& typeCombinationData,
    const TypeCombination& typeCombinationsQueries,
    const size_t size,
    const std::pair<size_t, size_t>& dataInterval,
    const std::pair<size_t, size_t>& queryInterval,
    const int numberOfQueries,
    const size_t seedData,
    const size_t seedQueriesLow,
    const size_t seedQueriesHigh,
    int arrayTypeLength,
    size_t ndv,
    size_t numberOfFields,
    const std::vector<QueryType> queryTypes,
    const std::vector<SamplingEstimationBenchmarkConfiguration::SampleSizeDef> sampleSizes,
    const std::vector<std::pair<SamplingEstimatorImpl::SamplingStyle, boost::optional<int>>>
        samplingAlgoAndChunks,
    bool printResults) {

    // Generate data according to the provided configuration
    std::vector<mongo::stats::SBEValue> data;
    generateData(ndv,
                 size,
                 typeCombinationData,
                 dataDistribution,
                 dataInterval,
                 seedData,
                 arrayTypeLength,
                 data);

    auto nss =
        NamespaceString::createNamespaceString_forTest("SamplingCeAccuracyTest.TestCollection");

    auto dataBSON = SamplingEstimatorTest::createDocumentsFromSBEValue(data);
    createCollAndInsertDocuments(operationContext(), nss, dataBSON);

    AutoGetCollection collPtr(operationContext(), nss, LockMode::MODE_IX);
    auto collection =
        MultipleCollectionAccessor(operationContext(),
                                   &collPtr.getCollection(),
                                   nss,
                                   false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */,
                                   {});

    for (auto samplingAlgoAndChunk : samplingAlgoAndChunks) {
        for (auto sampleSize : sampleSizes) {

            double actualSampleSize =
                SamplingEstimationBenchmarkConfiguration::translateSampleDefToActualSampleSize(
                    sampleSize);
            // Create sample from the provided collection
            SamplingEstimatorImpl samplingEstimator(
                operationContext(),
                collection,
                actualSampleSize,
                samplingAlgoAndChunk.first,
                samplingAlgoAndChunk.second,
                SamplingEstimatorTest::makeCardinalityEstimate(size));

            // Run queries.
            for (const auto& queryType : queryTypes) {
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
                                            &samplingEstimator,
                                            seedQueriesLow,
                                            seedQueriesHigh);

                    if (printResults) {
                        printResult(dataDistribution,
                                    typeCombinationData,
                                    size,
                                    actualSampleSize,
                                    typeCombinationQuery,
                                    numberOfQueries,
                                    queryType,
                                    dataInterval,
                                    seedData,
                                    seedQueriesLow,
                                    seedQueriesHigh,
                                    samplingAlgoAndChunk,
                                    error);
                    }
                }
            }
        }
    }
}

}  // namespace mongo::ce
