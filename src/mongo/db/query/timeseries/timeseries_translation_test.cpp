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

#include "mongo/db/query/timeseries/timeseries_translation.h"

#include "mongo/bson/json.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_test_fixture.h"
#include "mongo/db/timeseries/timeseries_test_util.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
struct DefaultTranslationParams {
    const StringData timeField = "time"_sd;
    const boost::optional<StringData> metaField = boost::none;
    const boost::optional<std::int32_t> bucketMaxSpanSeconds = 3600;
    const bool assumeNoMixedSchemaData = {false};
    const bool timeseriesBucketsAreFixed = {false};
};

class TimeseriesRewritesTest : public timeseries::TimeseriesTestFixture {
protected:
    void setUp() override {
        timeseries::TimeseriesTestFixture::setUp();
        expCtx = make_intrusive<ExpressionContextForTest>(_opCtx, nss);
        RAIIServerParameterControllerForTest featureFlagController(
            "featureFlagCreateViewlessTimeseriesCollections", true);

        // Create a viewless timeseries collection.
        auto timeseriesOptions = TimeseriesOptions(std::string{_timeField});
        CreateCommand cmd = CreateCommand(nss);
        cmd.getCreateCollectionRequest().setTimeseries(std::move(timeseriesOptions));
        uassertStatusOK(createCollection(_opCtx, cmd));
    }

    void translateStagesHelper(Pipeline& pipeline) {
        const auto collOrView = getTimeseriesCollAcquisition();
        timeseries::translateStagesIfRequired(expCtx, pipeline, collOrView);
    }

    CollectionOrViewAcquisition getTimeseriesCollAcquisition() {
        return acquireCollectionOrView(_opCtx,
                                       CollectionOrViewAcquisitionRequest(
                                           nss,
                                           PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
                                           repl::ReadConcernArgs::get(_opCtx),
                                           AcquisitionPrerequisites::kRead),
                                       MODE_IS);
    }

    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("test.timeseries_translator");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx;

    // Helper function to call the factory function and ensure some basic expected truths.
    std::tuple<std::vector<BSONObj>, BSONObj> prependUnpackStageHelper(
        const DefaultTranslationParams& args = {},
        const std::vector<BSONObj>& originalPipeline = std::vector{
            BSON("$match" << BSON("a" << 1))}) {
        auto expCtx = make_intrusive<ExpressionContextForTest>();
        auto pipeline = pipeline_factory::makePipeline(
            originalPipeline, expCtx, pipeline_factory::kOptionsMinimal);
        const size_t originalPipelineSize = pipeline->getSources().size();

        auto options = TimeseriesOptions{};
        options.setTimeField(args.timeField);
        options.setMetaField(args.metaField);
        options.setBucketMaxSpanSeconds(args.bucketMaxSpanSeconds);

        timeseries::prependUnpackStageToPipeline_forTest(
            expCtx,
            *pipeline,
            {options, args.assumeNoMixedSchemaData, args.timeseriesBucketsAreFixed});

        const auto sources = pipeline->getSources();
        ASSERT_EQ(sources.size(),
                  originalPipelineSize + 1);  // One stage should be added.

        // The first stage should be the generated $_internalUnpackBucket stage.
        const auto firstStage = sources.front();
        ASSERT_EQ(firstStage->getSourceName(),
                  DocumentSourceInternalUnpackBucket::kStageNameInternal);

        return {pipeline->serializeToBson(),
                firstStage->serializeToBSONForDebug().firstElement().Obj().getOwned()};
    }
};

TEST_F(TimeseriesRewritesTest, EmptyPipelineRewriteTest) {
    auto pipeline = pipeline_factory::makePipeline(
        std::vector<BSONObj>{}, expCtx, pipeline_factory::kOptionsMinimal);
    translateStagesHelper(*pipeline);

    const auto translatedSources = pipeline->getSources();
    ASSERT(pipeline->isTranslated());
    ASSERT_EQ(translatedSources.size(), 1);
    ASSERT_EQ(translatedSources.front()->getSourceName(),
              DocumentSourceInternalUnpackBucket::kStageNameInternal);
}

TEST_F(TimeseriesRewritesTest, InternalUnpackBucketRewriteTest) {
    auto pipeline = pipeline_factory::makePipeline(
        std::vector{BSON("$match" << BSON("a" << "1"))}, expCtx, pipeline_factory::kOptionsMinimal);
    const size_t originalPipelineSize = pipeline->size();

    translateStagesHelper(*pipeline);
    ASSERT(pipeline->isTranslated());
    const auto translatedSources = pipeline->getSources();
    ASSERT_EQ(translatedSources.size(), originalPipelineSize + 1);
    ASSERT_EQ(translatedSources.front()->getSourceName(),
              DocumentSourceInternalUnpackBucket::kStageNameInternal);
}

TEST_F(TimeseriesRewritesTest, InsertIndexStatsConversionStage) {
    const auto indexStatsStage = BSON("$indexStats" << BSONObj());
    const auto originalSources = std::vector{indexStatsStage};
    auto pipeline =
        pipeline_factory::makePipeline(originalSources, expCtx, pipeline_factory::kOptionsMinimal);

    translateStagesHelper(*pipeline);
    ASSERT(pipeline->isTranslated());
    const auto translatedSources = pipeline->getSources();
    ASSERT_EQ(translatedSources.size(), originalSources.size() + 1);

    const auto translatedSerialized = pipeline->serializeToBson();
    ASSERT_BSONOBJ_EQ(translatedSerialized[0], indexStatsStage);
    ASSERT_BSONOBJ_EQ(translatedSerialized[1],
                      BSON("$_internalConvertBucketIndexStats" << BSON("timeField" << "time")));
}

TEST_F(TimeseriesRewritesTest, InsertIndexStatsConversionStageWithMatch) {
    const auto indexStatsStage = BSON("$indexStats" << BSONObj());
    const auto matchStage = BSON("$match" << BSON("b" << 2));
    const auto originalSources = std::vector{indexStatsStage, matchStage};
    auto pipeline =
        pipeline_factory::makePipeline(originalSources, expCtx, pipeline_factory::kOptionsMinimal);

    translateStagesHelper(*pipeline);
    ASSERT(pipeline->isTranslated());
    const auto translatedSources = pipeline->getSources();
    ASSERT_EQ(translatedSources.size(), originalSources.size() + 1);

    const auto translatedSerialized = pipeline->serializeToBson();
    ASSERT_BSONOBJ_EQ(translatedSerialized[0], indexStatsStage);
    ASSERT_BSONOBJ_EQ(translatedSerialized[1],
                      BSON("$_internalConvertBucketIndexStats" << BSON("timeField" << "time")));
    ASSERT_BSONOBJ_EQ(translatedSerialized[2], matchStage);
}

TEST_F(TimeseriesRewritesTest, DontInsertUnpackStageWhenStagesDontExpectUserDocuments) {
    // These are a few stages that do not expect user-inserted documents as input. Therefore, we
    // should not add the $_internalUnpackBucket stage that transforms bucket documents to user
    // inserted documents.
    const std::vector<BSONObj> testCases{
        BSON("$collStats" << BSON("latencyStats" << BSON("histograms" << true))),
        fromjson(
            R"({$_internalApplyOplogUpdate: {oplogUpdate: {"$v": NumberInt(2), diff: {u: {b: 3}}}}})"),
        BSON("$_internalChangeStreamAddPreImage"
             << BSON("fullDocumentBeforeChange" << "required"))};

    for (const auto& initialSource : testCases) {
        const auto matchStage = BSON("$match" << BSON("b" << 2));
        const auto originalSources = std::vector{initialSource, matchStage};
        auto pipeline = pipeline_factory::makePipeline(
            originalSources, expCtx, pipeline_factory::kOptionsMinimal);

        translateStagesHelper(*pipeline);
        ASSERT(pipeline->isTranslated());
        const auto translatedSources = pipeline->getSources();
        ASSERT_EQ(translatedSources.size(), originalSources.size());

        const auto translatedSerialized = pipeline->serializeToBson();
        ASSERT_BSONOBJ_EQ(translatedSerialized[0], initialSource);
        ASSERT_BSONOBJ_EQ(translatedSerialized[1], matchStage);
    }
}

TEST_F(TimeseriesRewritesTest, ExistingUnpackStageIsAugmentedWithCollectionMetadata) {
    // When the pipeline already has an $_internalUnpackBucket stage, we should not add another
    // one. However, populateUnpackBucketStagesFromCollection augments existing unpack stages
    // with collection-derived metadata (e.g. assumeNoMixedSchemaData).
    const auto initialSource =
        BSON("$_internalUnpackBucket" << BSON("exclude" << BSONArray() << "timeField"
                                                        << "time"
                                                        << "bucketMaxSpanSeconds" << 3600));
    const auto matchStage = BSON("$match" << BSON("b" << 2));
    const auto originalSources = std::vector{initialSource, matchStage};
    auto pipeline =
        pipeline_factory::makePipeline(originalSources, expCtx, pipeline_factory::kOptionsMinimal);

    translateStagesHelper(*pipeline);
    ASSERT(pipeline->isTranslated());
    const auto translatedSources = pipeline->getSources();
    ASSERT_EQ(translatedSources.size(), originalSources.size());

    const auto translatedSerialized = pipeline->serializeToBson();
    const auto expectedUnpackStage =
        BSON("$_internalUnpackBucket"
             << BSON("exclude" << BSONArray() << "timeField"
                               << "time"
                               << "bucketMaxSpanSeconds" << 3600
                               << DocumentSourceInternalUnpackBucket::kAssumeNoMixedSchemaData
                               << true));
    ASSERT_BSONOBJ_EQ(translatedSerialized[0], expectedUnpackStage);
    ASSERT_BSONOBJ_EQ(translatedSerialized[1], matchStage);
}

TEST_F(TimeseriesRewritesTest, TranslateIndexHint) {
    auto matchStage = BSON("$match" << BSON("b" << 2));
    AggregateCommandRequest request(nss, std::vector<mongo::BSONObj>{matchStage});
    request.setHint(BSON(_timeField << 1));

    // The index hint must be rewritten to match the buckets.
    timeseries::translateIndexHintIfRequired(expCtx, getTimeseriesCollAcquisition(), request);

    ASSERT(request.getHint().has_value());
    const auto rewrittenIndex =
        BSONObjBuilder{}
            .append(fmt::format("{}{}", timeseries::kControlMinFieldNamePrefix, _timeField), 1)
            .append(fmt::format("{}{}", timeseries::kControlMaxFieldNamePrefix, _timeField), 1)
            .obj();
    ASSERT_BSONOBJ_EQ(request.getHint().get(), rewrittenIndex);
}

// The following tests just validate the the 'prependUnpackStage' function.
TEST_F(TimeseriesRewritesTest, EnsureStageIsGeneratedInReturnedPipeline) {
    const auto originalPipeline = std::vector{BSON("$match" << BSON("a" << 1))};
    const auto [alteredPipeline, _] = prependUnpackStageHelper({}, originalPipeline);

    // The rest of the stages should be unchanged.
    for (auto oitr = originalPipeline.begin(), aitr = alteredPipeline.begin() + 1;
         oitr != originalPipeline.end() && aitr != alteredPipeline.end();
         ++oitr, ++aitr) {
        ASSERT_BSONOBJ_EQ(*aitr, *oitr);
    }
}

TEST_F(TimeseriesRewritesTest, ValidateFieldCombinations) {
    const auto [alteredPipeline, firstStage] = prependUnpackStageHelper({
        .metaField = "foo"_sd,
        .bucketMaxSpanSeconds = 42,
        .assumeNoMixedSchemaData = true,
        .timeseriesBucketsAreFixed = true,
    });
    ASSERT_BSONOBJ_EQ(BSON(DocumentSourceInternalUnpackBucket::kExclude
                           << BSONArray() << timeseries::kTimeFieldName << "time"_sd
                           << timeseries::kMetaFieldName << "foo"_sd
                           << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 42
                           << DocumentSourceInternalUnpackBucket::kAssumeNoMixedSchemaData << true
                           << DocumentSourceInternalUnpackBucket::kFixedBuckets << true),
                      firstStage);
}

TEST_F(TimeseriesRewritesTest, ValidateTimeField) {
    // Default value for timeField.
    {
        const auto [alteredPipeline, firstStage] = prependUnpackStageHelper();
        ASSERT_BSONOBJ_EQ(BSON(DocumentSourceInternalUnpackBucket::kExclude
                               << BSONArray() << timeseries::kTimeFieldName << "time"_sd
                               << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds
                               << 3600),
                          firstStage);
    }
    // Non-default value for timeField.
    {
        const auto [alteredPipeline, firstStage] = prependUnpackStageHelper({
            .timeField = "readingTimestamp"_sd,
        });
        ASSERT_BSONOBJ_EQ(BSON(DocumentSourceInternalUnpackBucket::kExclude
                               << BSONArray() << timeseries::kTimeFieldName << "readingTimestamp"_sd
                               << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds
                               << 3600),
                          firstStage);
    }
    // Empty string value for timeField.
    {
        const auto [alteredPipeline, firstStage] = prependUnpackStageHelper({
            .timeField = ""_sd,
        });
        ASSERT_BSONOBJ_EQ(BSON(DocumentSourceInternalUnpackBucket::kExclude
                               << BSONArray() << timeseries::kTimeFieldName << ""_sd
                               << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds
                               << 3600),
                          firstStage);
    }
}

TEST_F(TimeseriesRewritesTest, ValidateMetaField) {
    // Meta field should be omitted if not present.
    {
        const auto [alteredPipeline, firstStage] = prependUnpackStageHelper();
        ASSERT_BSONOBJ_EQ(BSON(DocumentSourceInternalUnpackBucket::kExclude
                               << BSONArray() << timeseries::kTimeFieldName << "time"_sd
                               << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds
                               << 3600),
                          firstStage);
    }
    // Meta field should be included if present.
    {
        const auto [alteredPipeline, firstStage] = prependUnpackStageHelper({
            .metaField = "foo"_sd,
        });
        ASSERT_BSONOBJ_EQ(BSON(DocumentSourceInternalUnpackBucket::kExclude
                               << BSONArray() << timeseries::kTimeFieldName << "time"_sd
                               << timeseries::kMetaFieldName << "foo"_sd
                               << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds
                               << 3600),
                          firstStage);
    }
    // Empty string.
    {
        const auto [alteredPipeline, firstStage] = prependUnpackStageHelper({
            .metaField = ""_sd,
        });
        ASSERT_BSONOBJ_EQ(BSON(DocumentSourceInternalUnpackBucket::kExclude
                               << BSONArray() << timeseries::kTimeFieldName << "time"_sd
                               << timeseries::kMetaFieldName << ""_sd
                               << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds
                               << 3600),
                          firstStage);
    }
}

TEST_F(TimeseriesRewritesTest, ValidateAssumeNoMixedSchemaDataField) {
    {
        const auto [alteredPipeline, firstStage] = prependUnpackStageHelper({
            .assumeNoMixedSchemaData = true,
        });
        ASSERT_BSONOBJ_EQ(BSON(DocumentSourceInternalUnpackBucket::kExclude
                               << BSONArray() << timeseries::kTimeFieldName << "time"_sd
                               << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 3600
                               << DocumentSourceInternalUnpackBucket::kAssumeNoMixedSchemaData
                               << true),
                          firstStage);
    }
}

TEST_F(TimeseriesRewritesTest, ValidateBucketMaxSpanSecondsField) {
    {
        const auto [alteredPipeline, firstStage] = prependUnpackStageHelper({});
        ASSERT_BSONOBJ_EQ(BSON(DocumentSourceInternalUnpackBucket::kExclude
                               << BSONArray() << timeseries::kTimeFieldName << "time"_sd
                               << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds
                               << 3600),
                          firstStage);
    }
    {
        const auto [alteredPipeline, firstStage] = prependUnpackStageHelper({
            .bucketMaxSpanSeconds = 43,
        });
        ASSERT_BSONOBJ_EQ(BSON(DocumentSourceInternalUnpackBucket::kExclude
                               << BSONArray() << timeseries::kTimeFieldName << "time"_sd
                               << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 43),
                          firstStage);
    }
}

TEST_F(TimeseriesRewritesTest, BucketsFixedTest) {
    {
        const auto [_, internalUnpackBucketStage] =
            prependUnpackStageHelper({.timeseriesBucketsAreFixed = false});
        // The 'fixedBuckets' field is not serialized if buckets are not fixed.
        ASSERT_TRUE(
            internalUnpackBucketStage[DocumentSourceInternalUnpackBucket::kFixedBuckets].eoo());
    }

    {
        const auto [_, internalUnpackBucketStage] =
            prependUnpackStageHelper({.timeseriesBucketsAreFixed = true});
        ASSERT_TRUE(
            internalUnpackBucketStage[DocumentSourceInternalUnpackBucket::kFixedBuckets].Bool());
    }
}

/**
 * Fixture that sets up a timeseries buckets collection with a 2dsphere_bucket index to verify
 * the bucket spec uses the index's 2dsphereIndexVersion when creating
 * InternalBucketGeoWithinMatchExpression.
 */
class GeoIndexVersionWithCatalogTest : public CatalogTestFixture {
public:
    void setUp() override {
        CatalogTestFixture::setUp();
        auto opCtx = operationContext();

        CreateCommand createCmd(_measurementsNss);
        createCmd.getCreateCollectionRequest().setTimeseries(TimeseriesOptions("time"));
        ASSERT_OK(createCollection(opCtx, createCmd));

        // Add a 2dsphere_bucket index on data.loc with 2dsphereIndexVersion 3.
        const BSONObj indexSpec =
            BSON("v" << 2 << "key" << BSON("data.loc" << "2dsphere_bucket") << "name"
                     << "loc_2dsphere"
                     << "2dsphereIndexVersion" << 3);
        ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
            opCtx, timeseries::test_util::resolveTimeseriesNss(_measurementsNss), {indexSpec}));
    }

    boost::intrusive_ptr<ExpressionContextForTest> getExpCtx() {
        return make_intrusive<ExpressionContextForTest>(operationContext(), _measurementsNss);
    }

    CollectionOrViewAcquisition getCollAcquisition() {
        return acquireCollectionOrView(
            operationContext(),
            CollectionOrViewAcquisitionRequest(
                timeseries::test_util::resolveTimeseriesNss(_measurementsNss),
                PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
                repl::ReadConcernArgs::get(operationContext()),
                AcquisitionPrerequisites::kRead),
            MODE_IS);
    }

    const NamespaceString _measurementsNss =
        NamespaceString::createNamespaceString_forTest("test", "pipeline_test");
};

TEST_F(GeoIndexVersionWithCatalogTest, GeoWithinUses2dsphereBucketIndexVersionFromCatalog) {
    // Simulate a pipeline arriving from the router: it has a pre-translated unpack stage but no
    // 2dsphere index version info (the router lacks index catalog access).
    auto expCtx = getExpCtx();
    auto pipeline = pipeline_factory::makePipeline(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                            "'time', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {loc: {$geoWithin: {$geometry: {type: \"Polygon\", "
                            "coordinates: [ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 ] ] ]}}}}}")),
        expCtx,
        pipeline_factory::kOptionsMinimal);

    auto collAcquisition = getCollAcquisition();
    timeseries::translateStagesIfRequired(expCtx, *pipeline, collAcquisition);

    auto& container = pipeline->getSources();
    ASSERT_EQ(container.size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_TRUE(predicate.loosePredicate);
    auto* ibgw =
        dynamic_cast<InternalBucketGeoWithinMatchExpression*>(predicate.loosePredicate.get());
    ASSERT_NE(ibgw, nullptr)
        << "Expected InternalBucketGeoWithinMatchExpression when $geoWithin is pushed down";
    ASSERT_TRUE(ibgw->getIndexVersion())
        << "Expected index version from catalog when 2dsphere_bucket index exists";
    ASSERT_EQ(*ibgw->getIndexVersion(), S2_INDEX_VERSION_3)
        << "Expected 2dsphereIndexVersion 3 from the index we created";
}
}  // namespace
}  // namespace mongo
