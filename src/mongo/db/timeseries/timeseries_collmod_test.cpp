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

#include "mongo/db/timeseries/timeseries_collmod.h"

#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/ddl/coll_mod_gen.h"
#include "mongo/db/local_catalog/ddl/create_gen.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/timeseries/timeseries_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

class TimeseriesCollmodTest : public timeseries::TimeseriesTestFixture {
protected:
    void setUp() override {
        // Set up mongod.
        timeseries::TimeseriesTestFixture::setUp();

        // Set up required state for the replication coordinator.
        auto service = _opCtx->getServiceContext();

        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(service, std::move(replCoord));
        repl::createOplog(_opCtx);
    }
    NamespaceString testNss = NamespaceString::createNamespaceString_forTest("test.curColl");
};


// Collmods with timeseries options should be correctly translated to timeseries buckets
TEST_F(TimeseriesCollmodTest, TimeseriesCollModCommandTranslation) {
    auto timeseriesOptions = TimeseriesOptions(std::string{_timeField});
    auto collModTimeseries = CollModTimeseries();

    // Set all of the fields that makeTimeseriesBucketsCollModCommand transfers over to something
    // retrievable in the returned CollMod
    collModTimeseries.setGranularity(BucketGranularityEnum::Seconds);
    CollMod collModCmd(_ns1);
    collModCmd.setValidator(BSON("a" << "1"));
    collModCmd.setValidationLevel(ValidationLevelEnum::strict);
    collModCmd.setValidationAction(ValidationActionEnum::errorAndLog);
    collModCmd.setViewOn("test.view"_sd);
    std::vector<BSONObj> pipeline = {BSON("$match" << BSON("a" << 1))};
    collModCmd.setPipeline(pipeline);
    ChangeStreamPreAndPostImagesOptions changeStreamPreAndPostImagesOptions;
    changeStreamPreAndPostImagesOptions.setEnabled(true);
    collModCmd.setChangeStreamPreAndPostImages(changeStreamPreAndPostImagesOptions);
    collModCmd.setExpireAfterSeconds(
        boost::make_optional<std::variant<std::string, std::int64_t>>(100));
    collModCmd.setTimeseries(collModTimeseries);
    collModCmd.setTimeseriesBucketsMayHaveMixedSchemaData(true);
    collModCmd.setDryRun(true);

    auto collModBuckets =
        timeseries::makeTimeseriesBucketsCollModCommand(timeseriesOptions, collModCmd);

    ASSERT(collModBuckets);
    ASSERT((*collModBuckets->getValidator()).binaryEqual(BSON("a" << "1")));
    ASSERT_EQ(*(collModBuckets->getValidationLevel()), ValidationLevelEnum::strict);
    ASSERT_EQ(*(collModBuckets->getValidationAction()), ValidationActionEnum::errorAndLog);
    ASSERT_EQ(collModBuckets->getViewOn(), "test.view"_sd);
    ASSERT((*collModBuckets->getPipeline())[0].binaryEqual(BSON("$match" << BSON("a" << 1))));
    ASSERT_EQ(collModBuckets->getChangeStreamPreAndPostImages()->getEnabled(), true);
    ASSERT_EQ(std::get<int64_t>(*(collModBuckets->getExpireAfterSeconds())), 100);
    ASSERT_EQ(*(collModBuckets->getTimeseries()->getGranularity()), BucketGranularityEnum::Seconds);
    ASSERT_EQ(*(collModBuckets->getTimeseriesBucketsMayHaveMixedSchemaData()), true);
    ASSERT_EQ(*(collModBuckets->getDryRun()), true);
}

// Collmods that specify an index should have that index correctly translated to timeseries buckets
TEST_F(TimeseriesCollmodTest, TimeseriesCollModIndexTranslation) {
    // {tm: 1} should be converted to {control.min.tm: 1, control.max.tm: 1}
    auto timeseriesIndexSpec = BSON("tm" << 1);
    BSONObj expectedTranslation = BSON("control.min.tm" << 1 << "control.max.tm" << 1);

    CollMod collModCmd(_ns1);
    auto timeseriesOptions = TimeseriesOptions(std::string{_timeField});
    auto collModTimeseries = CollModTimeseries();
    auto collModIndex = CollModIndex();
    collModIndex.setKeyPattern(timeseriesIndexSpec);
    collModCmd.setIndex(collModIndex);

    auto collModBuckets =
        timeseries::makeTimeseriesBucketsCollModCommand(timeseriesOptions, collModCmd);
    ASSERT(collModBuckets);
    ASSERT(collModBuckets->getIndex()->getKeyPattern()->binaryEqual(expectedTranslation));
}

// If the index is not a valid timeseries index, an error should be thrown
TEST_F(TimeseriesCollmodTest, TimeseriesCollModBadIndex) {
    auto badIndex = BSON("$hint" << BSON("field" << 1));

    CollMod collModCmd(testNss);
    auto timeseriesOptions = TimeseriesOptions(std::string{_timeField});
    auto collModTimeseries = CollModTimeseries();
    auto collModIndex = CollModIndex();
    collModIndex.setKeyPattern(badIndex);
    collModCmd.setIndex(collModIndex);

    std::unique_ptr<CollMod> collModBuckets = nullptr;
    // This is only one of many error states that can be thrown after translation, but the main
    // thing tested here is that control flow passes to
    // createBucketsIndexSpecFromTimeseriesIndexSpec() properly.
    ASSERT_THROWS_CODE(
        timeseries::makeTimeseriesBucketsCollModCommand(timeseriesOptions, collModCmd),
        DBException,
        ErrorCodes::IndexNotFound);
}

// If view translation is required, a new Collmod pointer should be returned
TEST_F(TimeseriesCollmodTest, TimeseriesCollModViewTranslation) {
    auto timeseriesOptions = TimeseriesOptions(std::string{_timeField});
    auto collModTimeseries = CollModTimeseries();

    // Modify the timeseries options to trigger a change
    collModTimeseries.setGranularity(BucketGranularityEnum::Minutes);

    CollMod collModCmd(_ns1);
    collModCmd.setTimeseries(collModTimeseries);

    auto collModView = timeseries::makeTimeseriesViewCollModCommand(timeseriesOptions, collModCmd);

    ASSERT(collModView);
    ASSERT((*collModView->getPipeline())[0].binaryEqual(
        BSON("$_internalUnpackBucket"
             << BSON("timeField" << _timeField << "bucketMaxSpanSeconds" << 86400))));
}

// Checks that view translation is skipped if the command does not have a timeseries mod
TEST_F(TimeseriesCollmodTest, TimeseriesCollmodViewTranslationNoTimeseriesMod) {
    auto timeseriesOptions = TimeseriesOptions(std::string{_timeField});
    CollMod collModCmd(testNss);
    auto collModView = timeseries::makeTimeseriesViewCollModCommand(timeseriesOptions, collModCmd);
    ASSERT(!collModView);
}

// If the timeseries options are invalid, a null pointer should be returned
TEST_F(TimeseriesCollmodTest, TimeseriesCollModViewTranslationInvalidMod) {
    auto timeseriesOptions = TimeseriesOptions(std::string{_timeField});
    auto collModTimeseries = CollModTimeseries();

    // There is an internal check that prevents these options from being edited at the same time.
    collModTimeseries.setGranularity(BucketGranularityEnum::Seconds);
    collModTimeseries.setBucketRoundingSeconds(boost::optional<std::int32_t>{10});

    CollMod collModCmd(_ns1);
    collModCmd.setTimeseries(collModTimeseries);

    auto collModView = timeseries::makeTimeseriesViewCollModCommand(timeseriesOptions, collModCmd);
    ASSERT(!collModView);
}

// Check that timeseries options are correctly translated to a new CollMod.
TEST_F(TimeseriesCollmodTest, ProcessCollModCommandWithTimeseriesTranslation) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagTSBucketingParametersUnchanged", true);

    auto collModTimeseries = CollModTimeseries();
    // Create a command that requires timeseries translation.
    collModTimeseries.setGranularity(BucketGranularityEnum::Minutes);
    CollMod collModCmd(testNss);
    collModCmd.setTimeseries(collModTimeseries);

    // Create an existing collection with timeseries options.
    auto timeseriesOptions = TimeseriesOptions(std::string{_timeField});
    timeseriesOptions.setBucketRoundingSeconds(1000);
    timeseriesOptions.setBucketMaxSpanSeconds(1000);
    CreateCommand cmd = CreateCommand(testNss);
    cmd.getCreateCollectionRequest().setTimeseries(std::move(timeseriesOptions));
    uassertStatusOK(createCollection(_opCtx, cmd));

    auto status = timeseries::processCollModCommandWithTimeSeriesTranslation(
        _opCtx, testNss, collModCmd, false, nullptr);

    ASSERT_OK(status);
    // Editing timeseries options sets a flag in the collection that we can check.
    auto bucketsColl =
        NamespaceString::createNamespaceString_forTest("test.system.buckets.curColl");
    {
        const auto collectionAcquisition = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest(bucketsColl,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(_opCtx),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
        // Assert the bucketing parameters have changed on the collection.
        ASSERT_TRUE(collectionAcquisition.exists());
        ASSERT_TRUE(
            *collectionAcquisition.getCollectionPtr()->timeseriesBucketingParametersHaveChanged());
    }
    _addNsToValidate(testNss);
}

// If timeseries translation and view translation are both required, both should be executed.
TEST_F(TimeseriesCollmodTest, ProcessCollModCommandWithTimeseriesTranslationAndView) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagTSBucketingParametersUnchanged", true);

    auto collModTimeseries = CollModTimeseries();
    // Create a command that requires timeseries translation.
    collModTimeseries.setGranularity(BucketGranularityEnum::Minutes);
    CollMod collModCmd(testNss);
    collModCmd.setTimeseries(collModTimeseries);

    // Create an existing collection with timeseries options.
    auto timeseriesOptions = TimeseriesOptions(std::string{_timeField});
    timeseriesOptions.setBucketRoundingSeconds(100);
    timeseriesOptions.setBucketMaxSpanSeconds(100);
    CreateCommand cmd = CreateCommand(testNss);
    cmd.getCreateCollectionRequest().setTimeseries(std::move(timeseriesOptions));
    uassertStatusOK(createCollection(_opCtx, cmd));

    auto viewPrior =
        CollectionCatalog::get(_opCtx)->lookupViewWithoutValidatingDurable(_opCtx, testNss);
    ASSERT(viewPrior);
    ASSERT(viewPrior->pipeline()[0].binaryEqual(
        BSON("$_internalUnpackBucket"
             << BSON("timeField" << _timeField << "bucketMaxSpanSeconds" << 100))));

    auto status = timeseries::processCollModCommandWithTimeSeriesTranslation(
        _opCtx, testNss, collModCmd, true, nullptr);

    auto viewAfter =
        CollectionCatalog::get(_opCtx)->lookupViewWithoutValidatingDurable(_opCtx, testNss);
    ASSERT(viewAfter);
    // A bucket granularity of minutes results in the pipeline being updated to 86400 seconds.
    ASSERT(viewAfter->pipeline()[0].binaryEqual(
        BSON("$_internalUnpackBucket"
             << BSON("timeField" << _timeField << "bucketMaxSpanSeconds" << 86400))));

    // View translation is successful if this function returns OK.
    ASSERT_OK(status);
    // Editing timeseries options sets a flag in the collection that we can check.
    auto bucketsColl =
        NamespaceString::createNamespaceString_forTest("test.system.buckets.curColl");
    {
        const auto collectionAcquisition = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest(bucketsColl,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(_opCtx),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
        // Assert the bucketing parameters have changed on the collection.
        ASSERT_TRUE(collectionAcquisition.exists());
        ASSERT_TRUE(
            *collectionAcquisition.getCollectionPtr()->timeseriesBucketingParametersHaveChanged());
    }
    _addNsToValidate(testNss);
}

// Collmod processing will proceed normally on a non timeseries collection.
TEST_F(TimeseriesCollmodTest, ProcessCollModCommandWithTimeseriesTranslationNotTimeseries) {
    auto timeseriesOptions = TimeseriesOptions(std::string{_timeField});
    auto collModTimeseries = CollModTimeseries();

    // Create a non timeseries collection.
    CollMod collModCmd(testNss);
    CreateCommand cmd = CreateCommand(testNss);
    uassertStatusOK(createCollection(_opCtx, cmd));

    BSONObjBuilder result;
    auto status = timeseries::processCollModCommandWithTimeSeriesTranslation(
        _opCtx, testNss, collModCmd, false, &result);
    ASSERT_OK(status);
    // Enforce no spurious timeseries changes.
    {
        const auto collectionAcquisition = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest(testNss,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(_opCtx),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
        ASSERT_TRUE(collectionAcquisition.exists());
        ASSERT_FALSE(collectionAcquisition.getCollectionPtr()->getTimeseriesOptions());
    }
    _addNsToValidate(testNss);
}

}  // namespace
}  // namespace mongo
