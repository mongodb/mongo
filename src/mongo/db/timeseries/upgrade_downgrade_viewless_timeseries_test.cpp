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

#include "mongo/db/timeseries/upgrade_downgrade_viewless_timeseries.h"

#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::timeseries {
namespace {

class UpgradeDowngradeViewlessTimeseriesTest : public CatalogTestFixture {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();
    }

    void createTimeseriesCollection(const NamespaceString& nss) {
        CreateCommand cmd = CreateCommand(nss);
        cmd.getCreateCollectionRequest().setTimeseries(TimeseriesOptions("timestamp"));
        ASSERT_OK(createCollection(operationContext(), cmd));
    }

    UUID createViewfulTimeseriesCollection(const NamespaceString& nss) {
        RAIIServerParameterControllerForTest featureFlagController(
            "featureFlagCreateViewlessTimeseriesCollections", false);
        createTimeseriesCollection(nss);
        return CollectionCatalog::get(operationContext())
            ->lookupCollectionByNamespace(
                operationContext(),
                nss.isTimeseriesBucketsCollection() ? nss : nss.makeTimeseriesBucketsNamespace())
            ->uuid();
    }

    UUID createViewlessTimeseriesCollection(const NamespaceString& nss) {
        RAIIServerParameterControllerForTest featureFlagController(
            "featureFlagCreateViewlessTimeseriesCollections", true);
        createTimeseriesCollection(nss);
        return CollectionCatalog::get(operationContext())
            ->lookupCollectionByNamespace(operationContext(), nss)
            ->uuid();
    }

    void assertIsTimeseriesCollection(const NamespaceString& nss, const UUID& uuid) {
        auto opCtx = operationContext();

        auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
        ASSERT(coll && coll->isTimeseriesCollection() && coll->uuid() == uuid);
    }

    void assertIsViewfulTimeseries(const NamespaceString& nss, const UUID& uuid) {
        auto opCtx = operationContext();

        ASSERT(!CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss));
        auto view = CollectionCatalog::get(opCtx)->lookupView(opCtx, nss);
        ASSERT(view && view->timeseries() &&
               view->viewOn() == nss.makeTimeseriesBucketsNamespace());

        assertIsTimeseriesCollection(nss.makeTimeseriesBucketsNamespace(), uuid);
    }

    void assertIsViewlessTimeseries(const NamespaceString& nss, const UUID& uuid) {
        auto opCtx = operationContext();

        assertIsTimeseriesCollection(nss, uuid);

        ASSERT(!CollectionCatalog::get(opCtx)->lookupView(opCtx, nss));
        ASSERT(!CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
            opCtx, nss.makeTimeseriesBucketsNamespace()));
    }

    const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("test", "foo");
    const NamespaceString nss2 = NamespaceString::createNamespaceString_forTest("test", "bar");
};

/**
 * Basic upgrade/downgrade.
 */
TEST_F(UpgradeDowngradeViewlessTimeseriesTest, UpgradeOne) {
    auto uuid1 = createViewfulTimeseriesCollection(nss1);

    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", true);
    upgradeToViewlessTimeseries(operationContext(), nss1);
    assertIsViewlessTimeseries(nss1, uuid1);
}

TEST_F(UpgradeDowngradeViewlessTimeseriesTest, DowngradeOne) {
    auto uuid1 = createViewlessTimeseriesCollection(nss1);

    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", false);
    downgradeFromViewlessTimeseries(operationContext(), nss1);
    assertIsViewfulTimeseries(nss1, uuid1);
}

/**
 * Idempotency.
 */
TEST_F(UpgradeDowngradeViewlessTimeseriesTest, UpgradeOneWithExpectedUUID) {
    auto uuid1 = createViewfulTimeseriesCollection(nss1);

    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", true);
    upgradeToViewlessTimeseries(operationContext(), nss1, uuid1 /* expectedUUID */);
    assertIsViewlessTimeseries(nss1, uuid1);
}

TEST_F(UpgradeDowngradeViewlessTimeseriesTest, UpgradeIdempotency) {
    auto uuid1 = createViewlessTimeseriesCollection(nss1);

    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", true);
    upgradeToViewlessTimeseries(operationContext(), nss1);
    assertIsViewlessTimeseries(nss1, uuid1);
}

TEST_F(UpgradeDowngradeViewlessTimeseriesTest, DowngradeOneWithExpectedUUID) {
    auto uuid1 = createViewlessTimeseriesCollection(nss1);

    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", false);
    downgradeFromViewlessTimeseries(operationContext(), nss1, uuid1 /* expectedUUID */);
    assertIsViewfulTimeseries(nss1, uuid1);
}

TEST_F(UpgradeDowngradeViewlessTimeseriesTest, DowngradeIdempotency) {
    auto uuid = createViewfulTimeseriesCollection(nss1);

    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", false);
    downgradeFromViewlessTimeseries(operationContext(), nss1);
    assertIsViewfulTimeseries(nss1, uuid);
}

/**
 * Check that the collection options resulting from an upgrade/downgrade are consistent with those
 * of a brand new collection.
 *
 * This exercises the metadata changes, e.g. adding/removing the buckets validator.
 */
TEST_F(UpgradeDowngradeViewlessTimeseriesTest, UpgradedOptionsConsistentWithNewViewless) {
    createViewfulTimeseriesCollection(nss1);

    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", true);
    upgradeToViewlessTimeseries(operationContext(), nss1);

    createViewlessTimeseriesCollection(nss2);

    auto catalog = CollectionCatalog::get(operationContext());
    auto options1 =
        catalog->lookupCollectionByNamespace(operationContext(), nss1)->getCollectionOptions();
    auto options2 =
        catalog->lookupCollectionByNamespace(operationContext(), nss2)->getCollectionOptions();
    ASSERT_BSONOBJ_EQ(options1.toBSON(false /* includeUUID */),
                      options2.toBSON(false /* includeUUID */));
}

TEST_F(UpgradeDowngradeViewlessTimeseriesTest, DowngradedOptionsConsistentWithNewViewful) {
    createViewlessTimeseriesCollection(nss1);

    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", false);
    downgradeFromViewlessTimeseries(operationContext(), nss1);

    createViewfulTimeseriesCollection(nss2);

    auto catalog = CollectionCatalog::get(operationContext());
    auto options1 =
        catalog
            ->lookupCollectionByNamespace(operationContext(), nss1.makeTimeseriesBucketsNamespace())
            ->getCollectionOptions();
    auto options2 =
        catalog
            ->lookupCollectionByNamespace(operationContext(), nss2.makeTimeseriesBucketsNamespace())
            ->getCollectionOptions();
    ASSERT_BSONOBJ_EQ(options1.toBSON(false /* includeUUID */),
                      options2.toBSON(false /* includeUUID */));

    auto viewPipeline1 = catalog->lookupView(operationContext(), nss1)->pipeline();
    auto viewPipeline2 = catalog->lookupView(operationContext(), nss2)->pipeline();
    ASSERT_EQ(viewPipeline1.size(), viewPipeline2.size());
    for (size_t i = 0; i < viewPipeline1.size(); i++) {
        ASSERT_BSONOBJ_EQ(viewPipeline1[i], viewPipeline2[i]);
    }
}

/**
 * Upgrade/downgrade all collections in the catalog.
 */
TEST_F(UpgradeDowngradeViewlessTimeseriesTest, UpgradeMany) {
    auto uuid1 = createViewfulTimeseriesCollection(nss1);
    auto uuid2 = createViewfulTimeseriesCollection(nss2);

    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", true);
    upgradeAllTimeseriesToViewless(operationContext());
    assertIsViewlessTimeseries(nss1, uuid1);
    assertIsViewlessTimeseries(nss2, uuid2);
}

TEST_F(UpgradeDowngradeViewlessTimeseriesTest, DowngradeMany) {
    auto uuid1 = createViewlessTimeseriesCollection(nss1);
    auto uuid2 = createViewlessTimeseriesCollection(nss2);

    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", false);
    downgradeAllTimeseriesFromViewless(operationContext());
    assertIsViewfulTimeseries(nss1, uuid1);
    assertIsViewfulTimeseries(nss2, uuid2);
}

/**
 * Handling of inconsistent collections on upgrade.
 */
TEST_F(UpgradeDowngradeViewlessTimeseriesTest, UpgradeWithoutView) {
    auto uuid1 = createViewfulTimeseriesCollection(nss1.makeTimeseriesBucketsNamespace());

    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", true);
    upgradeToViewlessTimeseries(operationContext(), nss1);
    assertIsViewlessTimeseries(nss1, uuid1);
}

TEST_F(UpgradeDowngradeViewlessTimeseriesTest, UpgradeSkippedOnConflictingCollection) {
    auto uuid1 = createViewfulTimeseriesCollection(nss1.makeTimeseriesBucketsNamespace());
    ASSERT_OK(createCollection(operationContext(), CreateCommand(nss1)));

    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", true);
    upgradeToViewlessTimeseries(operationContext(), nss1);
    assertIsTimeseriesCollection(nss1.makeTimeseriesBucketsNamespace(), uuid1);
    ASSERT(CollectionCatalog::get(operationContext())
               ->lookupCollectionByNamespace(operationContext(), nss1));
}

TEST_F(UpgradeDowngradeViewlessTimeseriesTest, UpgradeSkippedOnConflictingView) {
    auto uuid1 = createViewfulTimeseriesCollection(nss1.makeTimeseriesBucketsNamespace());

    CreateCommand createViewCmd(nss1);
    createViewCmd.setViewOn(nss2.coll());
    createViewCmd.setPipeline({{}});
    ASSERT_OK(createCollection(operationContext(), createViewCmd));

    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagCreateViewlessTimeseriesCollections", true);
    upgradeToViewlessTimeseries(operationContext(), nss1);
    assertIsTimeseriesCollection(nss1.makeTimeseriesBucketsNamespace(), uuid1);
    ASSERT(CollectionCatalog::get(operationContext())->lookupView(operationContext(), nss1));
}


}  // namespace
}  // namespace mongo::timeseries
