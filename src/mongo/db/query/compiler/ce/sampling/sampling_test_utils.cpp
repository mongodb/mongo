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

#include "mongo/db/query/compiler/ce/sampling/sampling_test_utils.h"

#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::ce {

bool dataConfigurationCoversQueryWorkload(DataConfiguration& dataConfig,
                                          WorkloadConfiguration& workloadConfig) {
    for (const auto& queryField : workloadConfig.queryConfig.queryFields) {
        bool exists = false;
        for (const auto& dataField : dataConfig.collectionFieldsConfiguration) {
            if (queryField.fieldName == dataField.fieldName) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            return false;
        }
    }
    return true;
}

void initializeSamplingEstimator(DataConfiguration& configuration,
                                 SamplingEstimatorTest& samplingEstimatorTest) {
    // Generate data according to the provided configuration
    std::vector<std::vector<stats::SBEValue>> allData;
    generateDataBasedOnConfig(configuration, allData);

    samplingEstimatorTest.setUp();

    // Create vector of BSONObj according to the generated data
    // Number of fields dictates the number of columns the collection will have.
    auto dataBSON = SamplingEstimatorTest::createDocumentsFromSBEValue(
        allData, configuration.collectionFieldsConfiguration);

    // Populate collection
    samplingEstimatorTest.insertDocuments(samplingEstimatorTest._kTestNss, dataBSON);
}

void SamplingEstimatorTest::insertDocuments(const NamespaceString& nss,
                                            const std::vector<BSONObj> docs,
                                            int batchSize) {
    std::vector<InsertStatement> inserts{docs.begin(), docs.end()};

    AutoGetCollection agc(operationContext(), nss, LockMode::MODE_IX);
    {
        size_t currentInsertion = 0;
        while (currentInsertion < inserts.size()) {
            WriteUnitOfWork wuow{operationContext()};

            int insertionsBeforeCommit = 0;
            while (true) {
                ASSERT_OK(collection_internal::insertDocument(
                    operationContext(), *agc, inserts[currentInsertion], nullptr /* opDebug */));
                insertionsBeforeCommit++;
                currentInsertion++;

                if (insertionsBeforeCommit > batchSize || currentInsertion == inserts.size()) {
                    insertionsBeforeCommit = 0;
                    break;
                }
            }
            wuow.commit();
        }
    }
}

std::vector<BSONObj> SamplingEstimatorTest::createDocuments(int num) {
    std::vector<BSONObj> docs;
    for (int i = 0; i < num; i++) {
        BSONObj obj = BSON("_id" << i << "a" << i % 100 << "b" << i % 10 << "arr"
                                 << BSON_ARRAY(10 << 20 << 30 << 40 << 50) << "nil" << BSONNULL
                                 << "obj" << BSON("nil" << BSONNULL));
        docs.push_back(obj);
    }
    return docs;
}

void SamplingEstimatorTest::createIndex(const BSONObj& spec) {
    WriteUnitOfWork wuow(operationContext());
    AutoGetCollection autoColl(operationContext(), _kTestNss, MODE_X);
    CollectionWriter collection(operationContext(), _kTestNss);
    IndexBuildsCoordinator::createIndexesOnEmptyCollection(
        operationContext(), collection, {spec}, /*fromMigrate=*/false);
    wuow.commit();
}

std::vector<BSONObj> SamplingEstimatorTest::createDocumentsFromSBEValue(
    std::vector<std::vector<stats::SBEValue>> data,
    std::vector<CollectionFieldConfiguration> fieldConfig) {
    std::vector<BSONObj> docs;
    size_t dataSize = data[0].size();
    for (size_t i = 0; i < dataSize; i++) {
        BSONObjBuilder builder;
        stats::addSbeValueToBSONBuilder(stats::makeInt64Value(i), "_id", builder);

        int curIdx = -1;
        for (size_t instance = 0; instance < fieldConfig.size(); instance++) {
            if (fieldConfig[instance].fieldPositionInCollection != (curIdx + 1)) {
                // Add any in-between fields required
                while (curIdx < fieldConfig[instance].fieldPositionInCollection) {
                    // Each in-between field will be named as the next field followed by an
                    // underscore and a number e.g., if the first user defined field is 'a' in
                    // position 3, then this config will automatically add field 'a_0', 'a_1',
                    // 'a_2', and then 'a'.
                    std::string extendedName =
                        fieldConfig[instance].fieldName + "_" + std::to_string(curIdx++);
                    stats::addSbeValueToBSONBuilder(data[instance][i], extendedName, builder);
                }
            }
            stats::addSbeValueToBSONBuilder(
                data[instance][i], fieldConfig[instance].fieldName, builder);
            curIdx++;
        }

        docs.push_back(builder.obj());
    }
    return docs;
}

size_t translateSampleDefToActualSampleSize(SampleSizeDef sampleSizeDef) {
    // Translate the sample size definition to corresponding sample size.
    switch (sampleSizeDef) {
        case SampleSizeDef::ErrorSetting1: {
            return SamplingEstimatorForTesting::calculateSampleSize(
                SamplingConfidenceIntervalEnum::k95, 1.0);
        }
        case SampleSizeDef::ErrorSetting2: {
            return SamplingEstimatorForTesting::calculateSampleSize(
                SamplingConfidenceIntervalEnum::k95, 2.0);
        }
        case SampleSizeDef::ErrorSetting5: {
            return SamplingEstimatorForTesting::calculateSampleSize(
                SamplingConfidenceIntervalEnum::k95, 5.0);
        }
    }
    MONGO_UNREACHABLE;
}

std::pair<SamplingEstimatorImpl::SamplingStyle, boost::optional<int>>
iniitalizeSamplingAlgoBasedOnChunks(int numOfChunks) {
    if (numOfChunks <= 0) {
        return {SamplingEstimatorImpl::SamplingStyle::kRandom, boost::none};
    } else {
        return {SamplingEstimatorImpl::SamplingStyle::kChunk, numOfChunks};
    }
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

int evaluateMatchExpressionAgainstDataWithLimit(const std::unique_ptr<MatchExpression> expr,
                                                const std::vector<BSONObj>& data,
                                                boost::optional<size_t> limit) {
    size_t resultCnt = 0, cnt = 0;
    try {
        for (const auto& doc : data) {
            if (exec::matcher::matchesBSON(expr.get(), doc, nullptr)) {
                resultCnt++;
            }
            cnt++;
            if (limit && cnt >= limit) {
                break;
            }
        }
    } catch (const DBException&) {
        std::cout << "EVALUATION FAILED" << std::endl;
    }

    return resultCnt;
}

std::vector<std::vector<std::string>> getIndexCombinations(
    const std::vector<std::string>& candidateIndexFields, IndexCombinationTestSettings setting) {
    switch (setting) {
        case IndexCombinationTestSettings::kNone: {
            return {};
        }
        case IndexCombinationTestSettings::kSingleFieldIdx: {
            std::vector<std::vector<std::string>> result;
            for (const auto& lala : candidateIndexFields) {
                result.push_back({lala});
            }
            return result;
        }
        case IndexCombinationTestSettings::kAllIdxes: {
            int n = candidateIndexFields.size();
            int total = 1 << n;  // 2^n combinations

            std::vector<std::vector<std::string>> result;

            for (int mask = 0; mask < total; ++mask) {
                std::vector<std::string> combination;
                for (int i = 0; i < n; ++i) {
                    if (mask & (1 << i)) {
                        combination.push_back(candidateIndexFields[i]);
                    }
                }

                if (combination.size() > 0) {
                    result.push_back(combination);
                }
            }
            return result;
        }
    }
    return {};
}

std::string createIndexesAccordingToConfiguration(
    SamplingEstimatorTest& planRankingTest,
    std::vector<std::vector<std::string>>& indexCombinations) {

    // Build any indexes.
    std::stringstream indexCombinationString;
    for (const auto& indexCombination : indexCombinations) {
        BSONObjBuilder temp;
        std::string indexName = "";
        for (const auto& dataField : indexCombination) {
            temp << dataField << 1;
            if (indexName.length() > 0) {
                indexName += "_";
            }
            indexName += dataField;
        }
        BSONObj indexSpec =
            BSON("v" << 2 << "name" << indexName << "key" << temp.obj() << "unique" << false);

        if (indexCombinationString.gcount() > 0) {
            indexCombinationString << "-";
        }
        indexCombinationString << "(" << indexName << ")";

        planRankingTest.createIndex(indexSpec);
    }
    return indexCombinationString.str();
}

std::unique_ptr<CanonicalQuery> createCanonicalQueryFromMatchExpression(
    SamplingEstimatorTest& planRankingTest, std::unique_ptr<MatchExpression> matchExpression) {
    auto findCommand = std::make_unique<FindCommandRequest>(planRankingTest._kTestNss);
    auto expCtx = ExpressionContextBuilder{}
                      .fromRequest(planRankingTest.getOperationContext(), *findCommand)
                      .build();

    auto parsedFindCmd = ParsedFindCommand::withExistingFilter(
        expCtx,
        expCtx->getCollator() ? expCtx->getCollator()->clone() : nullptr,
        std::move(matchExpression),
        std::move(findCommand),
        ProjectionPolicies::aggregateProjectionPolicies());
    return std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = expCtx, .parsedFind = std::move(parsedFindCmd.getValue())});
}

ErrorCalculationSummary runQueries(WorkloadConfiguration queryConfig,
                                   std::vector<BSONObj>& bsonData,
                                   const SamplingEstimatorImpl* ceSample) {
    ErrorCalculationSummary finalResults;

    // Generate queries.
    std::vector<std::vector<std::pair<stats::SBEValue, stats::SBEValue>>> queryFieldsIntervals =
        generateMultiFieldIntervals(queryConfig);

    std::vector<std::unique_ptr<MatchExpression>> allMatchExpressionQueries =
        createQueryMatchExpressionOnMultipleFields(queryConfig, queryFieldsIntervals);

    for (const auto& expr : allMatchExpressionQueries) {
        size_t actualCard = calculateCardinality(expr.get(), bsonData);
        CardinalityEstimate estimatedCard = ceSample->estimateCardinality(expr.get());
        // Store results to final structure.
        QueryInfoAndResults queryInfoResults;
        queryInfoResults.matchExpression = expr->toString();
        // We store results to calculate Q-error:
        // Q-error = max(true/est, est/true)
        // where "est" is the estimated cardinality and "true" is the true cardinality.
        // In practice we replace est = max(est, 1) and true = max(est, 1) to avoid
        // divide-by-zero. Q-error = 1 indicates a perfect prediction.
        queryInfoResults.actualCardinality = fmax(actualCard, 1.0);
        queryInfoResults.estimatedCardinality = fmax(estimatedCard.toDouble(), 1.0);
        finalResults.queryResults.push_back(queryInfoResults);
        // Increment the number of executed queries.
        ++finalResults.executedQueries;
    }
    return finalResults;
}

void printResult(DataConfiguration dataConfig,
                 int sampleSize,
                 WorkloadConfiguration queryConfig,
                 const std::pair<SamplingEstimatorImpl::SamplingStyle, boost::optional<int>>&
                     samplingAlgoAndChunks,
                 ErrorCalculationSummary error) {
    BSONObjBuilder builder;

    dataConfig.addToBSONObjBuilder(builder);
    builder << "sampleSize" << sampleSize;
    queryConfig.addToBSONObjBuilder(builder);

    std::vector<std::string> queryValuesLow;
    std::vector<std::string> queryValuesHigh;
    std::string matchExpression = "";
    std::vector<double> actualCardinality;
    std::vector<double> Estimation;
    for (auto values : error.queryResults) {
        if (values.low.has_value()) {
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
                sslow << values.low.get().getValue();
                queryValuesLow.push_back(sslow.str());
                std::stringstream sshigh;
                sshigh << values.high.get().getValue();
                queryValuesHigh.push_back(sshigh.str());
            }
        } else if (values.matchExpression.has_value()) {
            matchExpression = values.matchExpression.get();
        }
        actualCardinality.push_back(values.actualCardinality);
        Estimation.push_back(values.estimatedCardinality);
    }

    builder << "QueryLow" << queryValuesLow;
    builder << "QueryHigh" << queryValuesHigh;
    builder << "QueryMatchExpression" << matchExpression;

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
    DataConfiguration dataConfig,
    WorkloadConfiguration queryConfig,
    const std::vector<SampleSizeDef> sampleSizes,
    const std::vector<std::pair<SamplingEstimatorImpl::SamplingStyle, boost::optional<int>>>
        samplingAlgoAndChunks,
    bool printResults) {
    // Generate data according to the provided configuration
    std::vector<std::vector<mongo::stats::SBEValue>> allData;
    generateDataBasedOnConfig(dataConfig, allData);

    auto nss =
        NamespaceString::createNamespaceString_forTest("SamplingCeAccuracyTest.TestCollection");

    auto dataBSON = SamplingEstimatorTest::createDocumentsFromSBEValue(
        allData, dataConfig.collectionFieldsConfiguration);

    createCollAndInsertDocuments(operationContext(), nss, dataBSON);

    auto acquisition =
        acquireCollectionOrView(operationContext(),
                                CollectionOrViewAcquisitionRequest::fromOpCtx(
                                    operationContext(), nss, AcquisitionPrerequisites::kWrite),
                                LockMode::MODE_IX);
    auto collection = MultipleCollectionAccessor(
        acquisition, {}, false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */);

    for (auto samplingAlgoAndChunk : samplingAlgoAndChunks) {
        for (auto sampleSize : sampleSizes) {
            double actualSampleSize = translateSampleDefToActualSampleSize(sampleSize);

            // Create sample from the provided collection
            SamplingEstimatorImpl samplingEstimator(
                operationContext(),
                collection,
                PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                actualSampleSize,
                samplingAlgoAndChunk.first,
                samplingAlgoAndChunk.second,
                SamplingEstimatorTest::makeCardinalityEstimate(dataConfig.size));
            samplingEstimator.generateSample(ce::NoProjection{});

            auto error = runQueries(queryConfig, dataBSON, &samplingEstimator);

            if (printResults) {
                printResult(dataConfig, actualSampleSize, queryConfig, samplingAlgoAndChunk, error);
            }
        }
    }
}

SamplingEstimatorForTesting SamplingEstimatorTest::createSamplingEstimatorForTesting(
    size_t collCard, size_t sampleSize, ce::ProjectionParams projectionParams) {
    insertDocuments(_kTestNss, createDocuments(collCard));

    auto collection =
        acquireCollectionOrView(operationContext(),
                                CollectionOrViewAcquisitionRequest::fromOpCtx(
                                    operationContext(), _kTestNss, AcquisitionPrerequisites::kRead),
                                MODE_IS);
    auto colls = MultipleCollectionAccessor(
        collection, {}, false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */);

    SamplingEstimatorForTesting samplingEstimator(
        operationContext(),
        colls,
        PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
        sampleSize,
        SamplingEstimatorForTesting::SamplingStyle::kRandom,
        boost::none,
        makeCardinalityEstimate(collCard));
    samplingEstimator.generateSample(projectionParams);

    return samplingEstimator;
}

IndexBounds getIndexBounds(const QueryConfiguration& queryConfig,
                           std::vector<std::pair<stats::SBEValue, stats::SBEValue>>& intervals) {
    ASSERT_EQUALS(queryConfig.queryFields.size(), intervals.size());
    IndexBounds bounds;

    for (size_t fieldIdx = 0; fieldIdx < queryConfig.queryFields.size(); fieldIdx++) {
        OrderedIntervalList oil(queryConfig.queryFields[fieldIdx].fieldName);

        switch (queryConfig.queryTypes[fieldIdx]) {
            case kPoint: {
                auto point = stats::sbeValueToBSON(intervals[fieldIdx].first, "");
                oil.intervals.emplace_back(IndexBoundsBuilder::makePointInterval(point));
                break;
            }
            case kRange: {
                auto range = stats::sbeValuesToInterval(
                    intervals[fieldIdx].first, "", intervals[fieldIdx].second, "");
                oil.intervals.emplace_back(IndexBoundsBuilder::makeRangeInterval(
                    range, BoundInclusion::kIncludeBothStartAndEndKeys));
                break;
            }
        }
        bounds.fields.push_back(oil);
    }
    return bounds;
}
}  // namespace mongo::ce
