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
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/timeseries/timeseries_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

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
                                           PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                           repl::ReadConcernArgs::get(_opCtx),
                                           AcquisitionPrerequisites::kRead),
                                       MODE_IS);
    }

    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("test.timeseries_translator");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx;
};

TEST_F(TimeseriesRewritesTest, EmptyPipelineRewriteTest) {
    auto pipeline = Pipeline::parse(std::vector<BSONObj>{}, expCtx);
    translateStagesHelper(*pipeline);

    const auto translatedSources = pipeline->getSources();
    ASSERT_EQ(translatedSources.size(), 1);
    ASSERT_EQ(translatedSources.front()->getSourceName(),
              DocumentSourceInternalUnpackBucket::kStageNameInternal);
}

TEST_F(TimeseriesRewritesTest, InternalUnpackBucketRewriteTest) {
    auto pipeline = Pipeline::parse(std::vector{BSON("$match" << BSON("a" << "1"))}, expCtx);
    const size_t originalPipelineSize = pipeline->size();

    translateStagesHelper(*pipeline);
    const auto translatedSources = pipeline->getSources();
    ASSERT_EQ(translatedSources.size(), originalPipelineSize + 1);
    ASSERT_EQ(translatedSources.front()->getSourceName(),
              DocumentSourceInternalUnpackBucket::kStageNameInternal);
}

TEST_F(TimeseriesRewritesTest, InsertIndexStatsConversionStage) {
    const auto indexStatsStage = BSON("$indexStats" << BSONObj());
    const auto originalSources = std::vector{indexStatsStage};
    auto pipeline = Pipeline::parse(originalSources, expCtx);

    translateStagesHelper(*pipeline);
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
    auto pipeline = Pipeline::parse(originalSources, expCtx);

    translateStagesHelper(*pipeline);
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
        BSON("$_internalChangeStreamAddPreImage" << BSON("fullDocumentBeforeChange" << "required")),
        BSON("$_internalUnpackBucket" << BSON("exclude" << BSONArray() << "timeField"
                                                        << "time"
                                                        << "bucketMaxSpanSeconds" << 3600))};

    for (const auto& initialSource : testCases) {
        const auto matchStage = BSON("$match" << BSON("b" << 2));
        const auto originalSources = std::vector{initialSource, matchStage};
        auto pipeline = Pipeline::parse(originalSources, expCtx);

        translateStagesHelper(*pipeline);
        const auto translatedSources = pipeline->getSources();
        ASSERT_EQ(translatedSources.size(), originalSources.size());

        const auto translatedSerialized = pipeline->serializeToBson();
        ASSERT_BSONOBJ_EQ(translatedSerialized[0], initialSource);
        ASSERT_BSONOBJ_EQ(translatedSerialized[1], matchStage);
    }
}

TEST_F(TimeseriesRewritesTest, TranslateIndexHint) {
    auto matchStage = BSON("$match" << BSON("b" << 2));
    AggregateCommandRequest request(nss, std::vector<mongo::BSONObj>{matchStage});
    request.setHint(BSON(_timeField << 1));

    // The index hint must be rewritten to match the buckets.
    timeseries::translateIndexHintIfRequired(expCtx, getTimeseriesCollAcquisition(), request);

    ASSERT(request.getHint().has_value());
    const auto rewrittenIndex =
        BSON(timeseries::kControlMinFieldNamePrefix + std::string{_timeField}
             << 1 << timeseries::kControlMaxFieldNamePrefix + std::string{_timeField} << 1);
    ASSERT_BSONOBJ_EQ(request.getHint().get(), rewrittenIndex);
}

}  // namespace
}  // namespace mongo
