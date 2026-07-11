// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/catalog_helper_ddl.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class CatalogHelperTest : public CatalogTestFixture {
public:
    NamespaceString _mainNss = NamespaceString::createNamespaceString_forTest("db1", "coll");
    NamespaceString _otherNss = NamespaceString::createNamespaceString_forTest("db1", "other");
    TimeseriesOptions _tsOptions{"timeField"};
};

/**
 * acquireCollectionOrViewForCatalogWrites
 *
 * These tests verify that when a view is acquired, system.views is also acquired to guarantee that
 * DDLs over views serialize with each other.
 **/

TEST_F(CatalogHelperTest, catalogWritesRegularCollectionDoesNotAcquireSystemViews) {
    auto opCtx = operationContext();

    CreateCommand cmd(_mainNss);
    ASSERT_OK(createCollection(opCtx, cmd));

    auto request = CollectionOrViewAcquisitionRequest::fromOpCtx(
        opCtx, _mainNss, AcquisitionPrerequisites::kWrite);
    auto acquisitions =
        catalog_helper_ddl::acquireCollectionOrViewForCatalogWrites(opCtx, {request});

    ASSERT_TRUE(acquisitions.contains(_mainNss));
    ASSERT_FALSE(acquisitions.getSystemViews().has_value());
}

TEST_F(CatalogHelperTest, catalogWritesPlainViewAcquiresSystemViews) {
    auto opCtx = operationContext();

    CreateCommand cmd(_otherNss);
    ASSERT_OK(createCollection(opCtx, cmd));

    {
        CreateCommand viewCmd(_mainNss);
        auto& req = viewCmd.getCreateCollectionRequest();
        req.setViewOn(_otherNss.coll());
        req.setPipeline(std::vector<mongo::BSONObj>());
        ASSERT_OK(createCollection(opCtx, viewCmd));
    }

    auto request = CollectionOrViewAcquisitionRequest::fromOpCtx(
        opCtx, _mainNss, AcquisitionPrerequisites::kWrite);
    auto acquisitions =
        catalog_helper_ddl::acquireCollectionOrViewForCatalogWrites(opCtx, {request});

    auto systemViewsNss = NamespaceString::makeSystemDotViewsNamespace(_mainNss.dbName());
    ASSERT_TRUE(acquisitions.contains(_mainNss));
    ASSERT_TRUE(acquisitions.getSystemViews()->exists());
}

TEST_F(CatalogHelperTest, catalogWritesViewfulTimeseriesAcquiresSystemViews) {
    auto opCtx = operationContext();

    unittest::ServerParameterGuard featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", false);

    CreateCommand cmd(_mainNss);
    cmd.getCreateCollectionRequest().setTimeseries(_tsOptions);
    ASSERT_OK(createCollection(opCtx, cmd));

    auto request = CollectionOrViewAcquisitionRequest::fromOpCtx(
        opCtx, _mainNss, AcquisitionPrerequisites::kWrite);
    auto acquisitions =
        catalog_helper_ddl::acquireCollectionOrViewForCatalogWrites(opCtx, {request});

    auto systemViewsNss = NamespaceString::makeSystemDotViewsNamespace(_mainNss.dbName());
    ASSERT_TRUE(acquisitions.contains(_mainNss));
    ASSERT_TRUE(acquisitions.contains(systemViewsNss));
    ASSERT_TRUE(acquisitions.at(systemViewsNss).collectionExists());
}

TEST_F(CatalogHelperTest, catalogWritesViewlessTimeseriesDoesNotAcquireSystemViews) {
    auto opCtx = operationContext();

    unittest::ServerParameterGuard featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", true);

    CreateCommand cmd(_mainNss);
    cmd.getCreateCollectionRequest().setTimeseries(_tsOptions);
    ASSERT_OK(createCollection(opCtx, cmd));

    auto request = CollectionOrViewAcquisitionRequest::fromOpCtx(
        opCtx, _mainNss, AcquisitionPrerequisites::kWrite);
    auto acquisitions =
        catalog_helper_ddl::acquireCollectionOrViewForCatalogWrites(opCtx, {request});

    ASSERT_TRUE(acquisitions.contains(_mainNss));
    ASSERT_FALSE(acquisitions.getSystemViews().has_value());
}

TEST_F(CatalogHelperTest, catalogWritesNonExistingNamespaceDoesNotAcquireSystemViews) {
    auto opCtx = operationContext();

    auto request = CollectionOrViewAcquisitionRequest::fromOpCtx(
        opCtx, _mainNss, AcquisitionPrerequisites::kWrite);
    auto acquisitions =
        catalog_helper_ddl::acquireCollectionOrViewForCatalogWrites(opCtx, {request});

    ASSERT_TRUE(acquisitions.contains(_mainNss));
    ASSERT_FALSE(acquisitions.getSystemViews().has_value());
}

TEST_F(CatalogHelperTest, catalogWritesOnSystemViewsOnlyAcquiresSystemViews) {
    auto opCtx = operationContext();

    auto systemViewsNss = NamespaceString::makeSystemDotViewsNamespace(_mainNss.dbName());
    auto request = CollectionOrViewAcquisitionRequest::fromOpCtx(
        opCtx, systemViewsNss, AcquisitionPrerequisites::kWrite);
    auto acquisitions =
        catalog_helper_ddl::acquireCollectionOrViewForCatalogWrites(opCtx, {request});
    ASSERT_TRUE(acquisitions.contains(systemViewsNss));
}


}  // namespace
}  // namespace mongo
