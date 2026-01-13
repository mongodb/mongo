/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/timeseries/catalog_helper.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/upgrade_downgrade_viewless_timeseries.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class TimeseriesCatalogHelperTest : public CatalogTestFixture {
public:
    TimeseriesCatalogHelperTest() {
        _tsOptions.setTimeField("timeField");
        _tsOptions.setMetaField(boost::make_optional<std::string>("metaField"));
    }

    void setUp() override {
        CatalogTestFixture::setUp();
    }

    void tearDown() override {
        CatalogTestFixture::tearDown();
    }

    NamespaceString _mainNss = NamespaceString::createNamespaceString_forTest("db1", "coll");
    NamespaceString _bucketsNss = NamespaceString::createNamespaceString_forTest("db1", "coll")
                                      .makeTimeseriesBucketsNamespace();
    NamespaceString _otherNss = NamespaceString::createNamespaceString_forTest("db1", "other");
    TimeseriesOptions _tsOptions;
};

/**
 * Non-existent collection:
 *
 * acquireCollectionWithBucketsLookup should:
 *  - return a non-existent CollectionAcquisition
 *  - not mark wasTranslated
 *  - not throw.
 */
TEST_F(TimeseriesCatalogHelperTest, acquireNonExistingCollThroughMainNss) {
    auto opCtx = operationContext();

    auto [acq, wasTranslated] = timeseries::acquireCollectionWithBucketsLookup(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, _mainNss, AcquisitionPrerequisites::OperationType::kRead),
        LockMode::MODE_IS);

    ASSERT_FALSE(acq.exists());
    ASSERT_EQ(acq.nss(), _mainNss);
    ASSERT_FALSE(wasTranslated);
}

TEST_F(TimeseriesCatalogHelperTest, acquireNonExistingCollThroughBucketsNss) {
    auto opCtx = operationContext();

    auto [acq, wasTranslated] = timeseries::acquireCollectionWithBucketsLookup(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, _bucketsNss, AcquisitionPrerequisites::OperationType::kRead),
        LockMode::MODE_IS);

    ASSERT_FALSE(acq.exists());
    ASSERT_EQ(acq.nss(), _bucketsNss);
    ASSERT_FALSE(wasTranslated);

    // Using expected UUID on non existing collection must raise error
    ASSERT_THROWS_CODE(
        timeseries::acquireCollectionWithBucketsLookup(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(
                opCtx, _bucketsNss, AcquisitionPrerequisites::OperationType::kRead, UUID::gen()),
            LockMode::MODE_IS),
        DBException,
        ErrorCodes::CollectionUUIDMismatch);
}

TEST_F(TimeseriesCatalogHelperTest, acquireCollectionThroughMainNss) {
    auto opCtx = operationContext();

    CreateCommand cmd(_mainNss);
    ASSERT_OK(createCollection(opCtx, cmd));

    auto [acq, wasTranslated] = timeseries::acquireCollectionWithBucketsLookup(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, _mainNss, AcquisitionPrerequisites::OperationType::kRead),
        LockMode::MODE_IS);

    ASSERT_TRUE(acq.exists());
    ASSERT_EQ(acq.nss(), _mainNss);
    ASSERT_FALSE(acq.getCollectionPtr()->isTimeseriesCollection());
    ASSERT_FALSE(wasTranslated);
}

TEST_F(TimeseriesCatalogHelperTest, acquireCollectionThroughBucketsNss) {
    auto opCtx = operationContext();

    CreateCommand cmd(_mainNss);
    ASSERT_OK(createCollection(opCtx, cmd));

    auto [acq, wasTranslated] = timeseries::acquireCollectionWithBucketsLookup(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, _bucketsNss, AcquisitionPrerequisites::OperationType::kRead),
        LockMode::MODE_IS);

    ASSERT_FALSE(acq.exists());
    ASSERT_EQ(acq.nss(), _bucketsNss);
    ASSERT_FALSE(wasTranslated);
}

TEST_F(TimeseriesCatalogHelperTest, acquireLegacyTimeseriesThroughMainNss) {
    auto opCtx = operationContext();

    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", false);

    // Create legacy (viewful) timeseries collection.
    CreateCommand cmd(_mainNss);
    cmd.getCreateCollectionRequest().setTimeseries(_tsOptions);
    ASSERT_OK(createCollection(opCtx, cmd));

    auto [acq, wasTranslated] = timeseries::acquireCollectionWithBucketsLookup(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, _mainNss, AcquisitionPrerequisites::OperationType::kRead),
        LockMode::MODE_IS);

    ASSERT_TRUE(acq.exists());
    ASSERT_EQ(acq.nss(), _bucketsNss);
    ASSERT_TRUE(acq.getCollectionPtr()->isTimeseriesCollection());
    ASSERT_FALSE(acq.getCollectionPtr()->isNewTimeseriesWithoutView());
    ASSERT_TRUE(wasTranslated);
}

TEST_F(TimeseriesCatalogHelperTest, acquireLegacyTimeseriesThroughBucketsNss) {
    auto opCtx = operationContext();

    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", false);

    // Create legacy (viewful) timeseries collection.
    CreateCommand cmd(_mainNss);
    cmd.getCreateCollectionRequest().setTimeseries(_tsOptions);
    ASSERT_OK(createCollection(opCtx, cmd));

    auto [acq, wasTranslated] = timeseries::acquireCollectionWithBucketsLookup(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, _bucketsNss, AcquisitionPrerequisites::OperationType::kRead),
        LockMode::MODE_IS);

    ASSERT_TRUE(acq.exists());
    ASSERT_EQ(acq.nss(), _bucketsNss);
    ASSERT_TRUE(acq.getCollectionPtr()->isTimeseriesCollection());
    ASSERT_FALSE(acq.getCollectionPtr()->isNewTimeseriesWithoutView());
    ASSERT_FALSE(wasTranslated);
}

TEST_F(TimeseriesCatalogHelperTest, acquireBucketsCollWithoutViewThroughMainNss) {
    auto opCtx = operationContext();

    // Create timeseries buckets collection only (no associated view).
    CreateCommand cmd(_bucketsNss);
    cmd.getCreateCollectionRequest().setTimeseries(_tsOptions);
    ASSERT_OK(createCollection(opCtx, cmd));

    auto [acq, wasTranslated] = timeseries::acquireCollectionWithBucketsLookup(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, _mainNss, AcquisitionPrerequisites::OperationType::kRead),
        LockMode::MODE_IS);

    ASSERT_TRUE(acq.exists());
    ASSERT_EQ(acq.nss(), _bucketsNss);
    ASSERT_TRUE(acq.getCollectionPtr()->isTimeseriesCollection());
    ASSERT_FALSE(acq.getCollectionPtr()->isNewTimeseriesWithoutView());
    ASSERT_TRUE(wasTranslated);
}

TEST_F(TimeseriesCatalogHelperTest, acquireBucketsCollWithoutViewThroughBucketsNss) {
    auto opCtx = operationContext();

    // Create timeseries buckets collection only (no associated view).
    CreateCommand cmd(_bucketsNss);
    cmd.getCreateCollectionRequest().setTimeseries(_tsOptions);
    ASSERT_OK(createCollection(opCtx, cmd));

    auto [acq, wasTranslated] = timeseries::acquireCollectionWithBucketsLookup(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, _bucketsNss, AcquisitionPrerequisites::OperationType::kRead),
        LockMode::MODE_IS);

    ASSERT_TRUE(acq.exists());
    ASSERT_EQ(acq.nss(), _bucketsNss);
    ASSERT_TRUE(acq.getCollectionPtr()->isTimeseriesCollection());
    ASSERT_FALSE(acq.getCollectionPtr()->isNewTimeseriesWithoutView());
    ASSERT_FALSE(wasTranslated);
}

TEST_F(TimeseriesCatalogHelperTest, acquireViewlessTimeseriesThroughMainNss) {
    auto opCtx = operationContext();

    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", true);

    CreateCommand cmd(_mainNss);
    cmd.getCreateCollectionRequest().setTimeseries(_tsOptions);
    ASSERT_OK(createCollection(opCtx, cmd));

    auto [acq, wasTranslated] = timeseries::acquireCollectionWithBucketsLookup(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, _mainNss, AcquisitionPrerequisites::OperationType::kRead),
        LockMode::MODE_IS);

    ASSERT_TRUE(acq.exists());
    ASSERT_EQ(acq.nss(), _mainNss);
    ASSERT_TRUE(acq.getCollectionPtr()->isTimeseriesCollection());
    ASSERT_TRUE(acq.getCollectionPtr()->isNewTimeseriesWithoutView());
    ASSERT_FALSE(wasTranslated);
}

TEST_F(TimeseriesCatalogHelperTest, acquireViewlessTimeseriesThroughBucketsNss) {
    auto opCtx = operationContext();

    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", true);

    CreateCommand cmd(_mainNss);
    cmd.getCreateCollectionRequest().setTimeseries(_tsOptions);
    ASSERT_OK(createCollection(opCtx, cmd));

    auto [acq, wasTranslated] = timeseries::acquireCollectionWithBucketsLookup(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, _bucketsNss, AcquisitionPrerequisites::OperationType::kRead),
        LockMode::MODE_IS);

    ASSERT_TRUE(acq.exists());
    ASSERT_EQ(acq.nss(), _mainNss);
    ASSERT_TRUE(acq.getCollectionPtr()->isTimeseriesCollection());
    ASSERT_TRUE(acq.getCollectionPtr()->isNewTimeseriesWithoutView());
    ASSERT_TRUE(wasTranslated);
}

/**
 * Plain (non-timeseries) view:
 *
 * acquireCollectionWithBucketsLookup should resolve the view definition and, seeing it is not a
 * timeseries view, throw CommandNotSupportedOnView when the original viewMode was
 * MustBeCollection.
 */
TEST_F(TimeseriesCatalogHelperTest, acquireViewThroughMainNss) {
    auto opCtx = operationContext();

    // Create backing collection.
    CreateCommand cmd(_otherNss);
    ASSERT_OK(createCollection(opCtx, cmd));

    // Create a simple view on top of the other collection (not a time-series view).
    {
        CreateCommand viewCmd(_mainNss);
        auto& req = viewCmd.getCreateCollectionRequest();
        req.setViewOn(_otherNss.coll());
        req.setPipeline(std::vector<mongo::BSONObj>());  // empty pipeline
        ASSERT_OK(createCollection(opCtx, viewCmd));
    }

    ASSERT_THROWS_CODE(timeseries::acquireCollectionWithBucketsLookup(
                           opCtx,
                           CollectionAcquisitionRequest::fromOpCtx(
                               opCtx, _mainNss, AcquisitionPrerequisites::OperationType::kRead),
                           LockMode::MODE_IS),
                       DBException,
                       ErrorCodes::CommandNotSupportedOnView);
}

TEST_F(TimeseriesCatalogHelperTest, acquireViewThroughBucketsNss) {
    auto opCtx = operationContext();

    // Create backing collection.
    CreateCommand cmd(_otherNss);
    ASSERT_OK(createCollection(opCtx, cmd));

    // Create a simple view on top of the other collection (not a time-series view).
    {
        CreateCommand viewCmd(_mainNss);
        auto& req = viewCmd.getCreateCollectionRequest();
        req.setViewOn(_otherNss.coll());
        req.setPipeline(std::vector<mongo::BSONObj>());  // empty pipeline
        ASSERT_OK(createCollection(opCtx, viewCmd));
    }
    auto [acq, wasTranslated] = timeseries::acquireCollectionWithBucketsLookup(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, _bucketsNss, AcquisitionPrerequisites::OperationType::kRead),
        LockMode::MODE_IS);

    ASSERT_FALSE(acq.exists());
    ASSERT_EQ(acq.nss(), _bucketsNss);
    ASSERT_FALSE(wasTranslated);
}
/**
 * system.buckets.* collection without timeseries options:
 *
 * This simulates an "invalid" buckets collection (e.g. from old versions or catalog bugs) where
 * the name starts with system.buckets. but the collection isn't actually timeseries.
 *
 * acquireCollectionWithBucketsLookup called on the buckets namespace should:
 *  - acquire that same namespace
 *  - not treat it as timeseries
 *  - not mark wasTranslated.
 */
TEST_F(TimeseriesCatalogHelperTest, acquireBucketsCollWithoutTsOptionsThroughBucketsNss) {
    auto opCtx = operationContext();

    {
        // Create backing collection.
        FailPointEnableBlock fp("skipCreateTimeseriesBucketsWithoutOptionsCheck");
        CreateCommand cmd(_bucketsNss);
        ASSERT_OK(createCollection(opCtx, cmd));
    }

    auto [acq, wasTranslated] = timeseries::acquireCollectionWithBucketsLookup(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, _bucketsNss, AcquisitionPrerequisites::OperationType::kRead),
        LockMode::MODE_IS);

    ASSERT_TRUE(acq.exists());
    ASSERT_EQ(acq.nss(), _bucketsNss);
    ASSERT_FALSE(acq.getCollectionPtr()->isTimeseriesCollection());
    ASSERT_FALSE(acq.getCollectionPtr()->isNewTimeseriesWithoutView());
    ASSERT_FALSE(wasTranslated);
}

TEST_F(TimeseriesCatalogHelperTest, acquireBucketsCollWithoutTsOptionsThroughMainNss) {
    auto opCtx = operationContext();

    {
        // Create backing collection.
        FailPointEnableBlock fp("skipCreateTimeseriesBucketsWithoutOptionsCheck");
        CreateCommand cmd(_bucketsNss);
        ASSERT_OK(createCollection(opCtx, cmd));
    }

    auto [acq, wasTranslated] = timeseries::acquireCollectionWithBucketsLookup(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, _mainNss, AcquisitionPrerequisites::OperationType::kRead),
        LockMode::MODE_IS);

    ASSERT_FALSE(acq.exists());
    ASSERT_EQ(acq.nss(), _mainNss);
    ASSERT_FALSE(wasTranslated);
}

TEST_F(TimeseriesCatalogHelperTest, acquireWithUpgradeDowngrade) {
    auto opCtx = operationContext();
    {
        RAIIServerParameterControllerForTest featureFlagController(
            "featureFlagCreateViewlessTimeseriesCollections", false);
        // Create legacy (viewful) timeseries collection.
        CreateCommand cmd(_mainNss);
        cmd.getCreateCollectionRequest().setTimeseries(_tsOptions);
        ASSERT_OK(createCollection(opCtx, cmd));
    }
    Atomic<bool> _upgradeDowngradeInBackground(true);
    unittest::JoinThread modifierThread([&, svcCtx = getServiceContext()] {
        ThreadClient client(svcCtx->getService());
        auto newOpCtx = client->makeOperationContext();
        while (_upgradeDowngradeInBackground.load()) {
            {
                RAIIServerParameterControllerForTest featureFlagController(
                    "featureFlagCreateViewlessTimeseriesCollections", true);
                timeseries::upgradeToViewlessTimeseries(newOpCtx.get(), _mainNss);
            }
            {
                RAIIServerParameterControllerForTest featureFlagController(
                    "featureFlagCreateViewlessTimeseriesCollections", false);
                timeseries::downgradeFromViewlessTimeseries(newOpCtx.get(), _mainNss);
            }
        }
    });
    ON_BLOCK_EXIT([&] { _upgradeDowngradeInBackground.store(false); });
    for (int i = 0; i < 10000; i++) {
        auto [acq, wasTranslated] = timeseries::acquireCollectionWithBucketsLookup(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(
                opCtx, _mainNss, AcquisitionPrerequisites::OperationType::kRead),
            LockMode::MODE_IS);
        ASSERT_TRUE(acq.exists());
        ASSERT_TRUE(acq.getCollectionPtr()->isTimeseriesCollection());
    }
}

}  // namespace
}  // namespace mongo
