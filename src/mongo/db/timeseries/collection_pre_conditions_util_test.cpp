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

#include "mongo/db/timeseries/collection_pre_conditions_util.h"

#include "mongo/db/local_catalog/create_collection.h"
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
    ASSERT(!preConditions.exists());
}

TEST_F(TimeseriesCollectionPreConditionsUtilTest, NonTimeseriesCollection) {
    CreateCommand cmd = CreateCommand(nonTsNss);
    uassertStatusOK(createCollection(_opCtx, cmd));
    auto preConditions = timeseries::CollectionPreConditions::getCollectionPreConditions(
        _opCtx, nonTsNss, /*expectedUUID=*/boost::none);
    ASSERT(preConditions.exists());
    ASSERT(!preConditions.isTimeseriesCollection());
    ASSERT(!preConditions.isViewlessTimeseriesCollection());
}

TEST_F(TimeseriesCollectionPreConditionsUtilTest, LegacyTimeseriesCollection) {
    CreateCommand cmd = CreateCommand(viewfulTsNss);
    auto timeseriesOptions = TimeseriesOptions(std::string{_timeField});
    cmd.getCreateCollectionRequest().setTimeseries(std::move(timeseriesOptions));
    uassertStatusOK(createCollection(_opCtx, cmd));

    auto preConditions = timeseries::CollectionPreConditions::getCollectionPreConditions(
        _opCtx, viewfulTsNss, /*expectedUUID=*/boost::none);

    ASSERT(preConditions.exists());
    ASSERT(preConditions.isTimeseriesCollection());
    ASSERT(!preConditions.isViewlessTimeseriesCollection());
    ASSERT(preConditions.wasNssTranslated());
    ASSERT_EQ(preConditions.getTargetNs(viewfulTsNss), viewfulTsSystemBucketsNss);
}

TEST_F(TimeseriesCollectionPreConditionsUtilTest, ViewlessTimeseriesCollection) {
    RAIIServerParameterControllerForTest queryKnobController{
        "featureFlagCreateViewlessTimeseriesCollections", true};

    CreateCommand cmd = CreateCommand(viewlessTsNss);
    auto timeseriesOptions = TimeseriesOptions(std::string{_timeField});
    cmd.getCreateCollectionRequest().setTimeseries(std::move(timeseriesOptions));
    uassertStatusOK(createCollection(_opCtx, cmd));

    auto preConditions = timeseries::CollectionPreConditions::getCollectionPreConditions(
        _opCtx, viewlessTsNss, /*expectedUUID=*/boost::none);

    ASSERT(preConditions.exists());
    ASSERT(preConditions.isTimeseriesCollection());
    ASSERT(preConditions.isViewlessTimeseriesCollection());
    ASSERT(!preConditions.wasNssTranslated());
}

TEST_F(TimeseriesCollectionPreConditionsUtilTest, CollectionCreatedAfterPreConditionsCreated) {
    RAIIServerParameterControllerForTest queryKnobController{
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
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(_opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);

    ASSERT_THROWS_CODE(timeseries::CollectionPreConditions::checkAcquisitionAgainstPreConditions(
                           _opCtx, preConditions, collectionAcquisition),
                       DBException,
                       10685100);
}

TEST_F(TimeseriesCollectionPreConditionsUtilTest, DetectWhenCollectionIsDroppedAndReacquired) {
    RAIIServerParameterControllerForTest queryKnobController{
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
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
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
