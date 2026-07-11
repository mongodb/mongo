// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/collection_pre_conditions_util.h"

#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/timeseries/timeseries_test_fixture.h"


namespace mongo {
namespace {

class TimeseriesCollectionPreConditionsUtilTest : public timeseries::TimeseriesTestFixture {
protected:
    NamespaceString nonTsNss = NamespaceString::createNamespaceString_forTest("test.nonTsColl");
    NamespaceString viewlessTsNss =
        NamespaceString::createNamespaceString_forTest("test.viewlessTsColl");
    NamespaceString viewfulTsNss =
        NamespaceString::createNamespaceString_forTest("test.viewfulTsColl");
    NamespaceString viewfulTsSystemBucketsNss =
        NamespaceString::createNamespaceString_forTest("test.system.buckets.viewfulTsColl");
};

TEST_F(TimeseriesCollectionPreConditionsUtilTest, NoCollectionNotFound) {
    auto collThatDoesntExist =
        NamespaceString::createNamespaceString_forTest("test.nonexistentColl");
    auto preConditions = timeseries::CollectionPreConditions::getCollectionPreConditions(
        _opCtx, collThatDoesntExist, /*expectedUUID=*/boost::none);
    EXPECT_FALSE(preConditions.exists());
}

TEST_F(TimeseriesCollectionPreConditionsUtilTest, NonTimeseriesCollection) {
    CreateCommand cmd = CreateCommand(nonTsNss);
    uassertStatusOK(createCollection(_opCtx, cmd));
    auto preConditions = timeseries::CollectionPreConditions::getCollectionPreConditions(
        _opCtx, nonTsNss, /*expectedUUID=*/boost::none);
    EXPECT_TRUE(preConditions.exists());
    EXPECT_FALSE(preConditions.isTimeseriesCollection());
    EXPECT_FALSE(preConditions.isViewlessTimeseriesCollection());
}

// TODO SERVER-123350: Remove this test once 9.0 is last LTS.
TEST_F(TimeseriesCollectionPreConditionsUtilTest, LegacyTimeseriesCollection) {
    unittest::ServerParameterGuard featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", false);

    CreateCommand cmd = CreateCommand(viewfulTsNss);
    auto timeseriesOptions = TimeseriesOptions(std::string{_timeField});
    cmd.getCreateCollectionRequest().setTimeseries(std::move(timeseriesOptions));
    uassertStatusOK(createCollection(_opCtx, cmd));

    auto preConditions = timeseries::CollectionPreConditions::getCollectionPreConditions(
        _opCtx, viewfulTsNss, /*expectedUUID=*/boost::none);

    EXPECT_TRUE(preConditions.exists());
    EXPECT_TRUE(preConditions.isTimeseriesCollection());
    EXPECT_FALSE(preConditions.isViewlessTimeseriesCollection());
    EXPECT_TRUE(preConditions.wasNssTranslated());
    EXPECT_EQ(preConditions.getTargetNs(viewfulTsNss), viewfulTsSystemBucketsNss);
}

TEST_F(TimeseriesCollectionPreConditionsUtilTest, ViewlessTimeseriesCollection) {
    unittest::ServerParameterGuard queryKnobController{
        "featureFlagCreateViewlessTimeseriesCollections", true};

    CreateCommand cmd = CreateCommand(viewlessTsNss);
    auto timeseriesOptions = TimeseriesOptions(std::string{_timeField});
    cmd.getCreateCollectionRequest().setTimeseries(std::move(timeseriesOptions));
    uassertStatusOK(createCollection(_opCtx, cmd));

    auto preConditions = timeseries::CollectionPreConditions::getCollectionPreConditions(
        _opCtx, viewlessTsNss, /*expectedUUID=*/boost::none);

    EXPECT_TRUE(preConditions.exists());
    EXPECT_TRUE(preConditions.isTimeseriesCollection());
    EXPECT_TRUE(preConditions.isViewlessTimeseriesCollection());
    EXPECT_FALSE(preConditions.wasNssTranslated());
}

TEST_F(TimeseriesCollectionPreConditionsUtilTest, CollectionCreatedAfterPreConditionsCreated) {
    unittest::ServerParameterGuard queryKnobController{
        "featureFlagCreateViewlessTimeseriesCollections", true};
    auto preConditions = timeseries::CollectionPreConditions::getCollectionPreConditions(
        _opCtx, viewlessTsNss, /*expectedUUID=*/boost::none);

    CreateCommand cmd = CreateCommand(viewlessTsNss);
    auto timeseriesOptions = TimeseriesOptions(std::string{_timeField});
    cmd.getCreateCollectionRequest().setTimeseries(std::move(timeseriesOptions));
    uassertStatusOK(createCollection(_opCtx, cmd));

    const auto collectionAcquisition = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest(viewlessTsNss,
                                     PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
                                     repl::ReadConcernArgs::get(_opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);

    ASSERT_THROWS_CODE(timeseries::CollectionPreConditions::checkAcquisitionAgainstPreConditions(
                           _opCtx, preConditions, collectionAcquisition),
                       DBException,
                       10685100);
}

TEST_F(TimeseriesCollectionPreConditionsUtilTest, DetectWhenCollectionIsDroppedAndReacquired) {
    unittest::ServerParameterGuard queryKnobController{
        "featureFlagCreateViewlessTimeseriesCollections", true};
    CreateCommand cmd = CreateCommand(viewlessTsNss);
    uassertStatusOK(createCollection(_opCtx, cmd));

    auto preConditions = timeseries::CollectionPreConditions::getCollectionPreConditions(
        _opCtx, viewlessTsNss, /*expectedUUID=*/boost::none);

    {
        repl::UnreplicatedWritesBlock uwb(_opCtx);  // Do not use oplog.
        ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(_opCtx, viewlessTsNss));
    }

    CreateCommand secondCmd = CreateCommand(viewlessTsNss);
    auto timeseriesOptions = TimeseriesOptions(std::string{_timeField});
    secondCmd.getCreateCollectionRequest().setTimeseries(std::move(timeseriesOptions));
    uassertStatusOK(createCollection(_opCtx, secondCmd));

    const auto collectionAcquisition = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest(viewlessTsNss,
                                     PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
                                     repl::ReadConcernArgs::get(_opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);

    ASSERT_THROWS_CODE(timeseries::CollectionPreConditions::checkAcquisitionAgainstPreConditions(
                           _opCtx, preConditions, collectionAcquisition),
                       DBException,
                       10685101);
}


}  // namespace
}  // namespace mongo
