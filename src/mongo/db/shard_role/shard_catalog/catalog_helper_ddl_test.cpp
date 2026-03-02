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
#include "mongo/idl/server_parameter_test_controller.h"
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

    RAIIServerParameterControllerForTest featureFlagController(
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

    RAIIServerParameterControllerForTest featureFlagController(
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
