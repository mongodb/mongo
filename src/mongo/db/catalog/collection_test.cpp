/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/bson/oid.h"
#include "mongo/db/catalog/capped_utils.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/catalog/collection_validation.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

class CollectionTest : public CatalogTestFixture {
protected:
    void makeCapped(NamespaceString nss, long long cappedSize = 8192);
};

void CollectionTest::makeCapped(NamespaceString nss, long long cappedSize) {
    CollectionOptions options;
    options.capped = true;
    options.cappedSize = cappedSize;  // Maximum size of capped collection in bytes.
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));
}

TEST_F(CollectionTest, CappedNotifierKillAndIsDead) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();
    ASSERT_FALSE(notifier->isDead());
    notifier->kill();
    ASSERT(notifier->isDead());
}

TEST_F(CollectionTest, CappedNotifierTimeouts) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();
    ASSERT_EQ(notifier->getVersion(), 0u);

    auto before = Date_t::now();
    notifier->waitUntil(0u, before + Milliseconds(25));
    auto after = Date_t::now();
    ASSERT_GTE(after - before, Milliseconds(25));
    ASSERT_EQ(notifier->getVersion(), 0u);
}

TEST_F(CollectionTest, CappedNotifierWaitAfterNotifyIsImmediate) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();

    auto prevVersion = notifier->getVersion();
    notifier->notifyAll();
    auto thisVersion = prevVersion + 1;
    ASSERT_EQ(notifier->getVersion(), thisVersion);

    auto before = Date_t::now();
    notifier->waitUntil(prevVersion, before + Seconds(25));
    auto after = Date_t::now();
    ASSERT_LT(after - before, Seconds(25));
}

TEST_F(CollectionTest, CappedNotifierWaitUntilAsynchronousNotifyAll) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();
    auto prevVersion = notifier->getVersion();
    auto thisVersion = prevVersion + 1;

    auto before = Date_t::now();
    stdx::thread thread([before, prevVersion, &notifier] {
        notifier->waitUntil(prevVersion, before + Milliseconds(25));
        auto after = Date_t::now();
        ASSERT_GTE(after - before, Milliseconds(25));
        notifier->notifyAll();
    });
    notifier->waitUntil(prevVersion, before + Seconds(25));
    auto after = Date_t::now();
    ASSERT_LT(after - before, Seconds(25));
    ASSERT_GTE(after - before, Milliseconds(25));
    thread.join();
    ASSERT_EQ(notifier->getVersion(), thisVersion);
}

TEST_F(CollectionTest, CappedNotifierWaitUntilAsynchronousKill) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();
    auto prevVersion = notifier->getVersion();

    auto before = Date_t::now();
    stdx::thread thread([before, prevVersion, &notifier] {
        notifier->waitUntil(prevVersion, before + Milliseconds(25));
        auto after = Date_t::now();
        ASSERT_GTE(after - before, Milliseconds(25));
        notifier->kill();
    });
    notifier->waitUntil(prevVersion, before + Seconds(25));
    auto after = Date_t::now();
    ASSERT_LT(after - before, Seconds(25));
    ASSERT_GTE(after - before, Milliseconds(25));
    thread.join();
    ASSERT_EQ(notifier->getVersion(), prevVersion);
}

TEST_F(CollectionTest, HaveCappedWaiters) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    ASSERT_FALSE(col->getCappedCallback()->haveCappedWaiters());
    {
        auto notifier = col->getCappedInsertNotifier();
        ASSERT(col->getCappedCallback()->haveCappedWaiters());
    }
    ASSERT_FALSE(col->getCappedCallback()->haveCappedWaiters());
}

TEST_F(CollectionTest, NotifyCappedWaitersIfNeeded) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    col->getCappedCallback()->notifyCappedWaitersIfNeeded();
    {
        auto notifier = col->getCappedInsertNotifier();
        ASSERT_EQ(notifier->getVersion(), 0u);
        col->getCappedCallback()->notifyCappedWaitersIfNeeded();
        ASSERT_EQ(notifier->getVersion(), 1u);
    }
}

TEST_F(CollectionTest, AsynchronouslyNotifyCappedWaitersIfNeeded) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();
    auto prevVersion = notifier->getVersion();
    auto thisVersion = prevVersion + 1;

    auto before = Date_t::now();
    notifier->waitUntil(prevVersion, before + Milliseconds(25));
    stdx::thread thread([before, prevVersion, &col] {
        auto after = Date_t::now();
        ASSERT_GTE(after - before, Milliseconds(25));
        col->getCappedCallback()->notifyCappedWaitersIfNeeded();
    });
    notifier->waitUntil(prevVersion, before + Seconds(25));
    auto after = Date_t::now();
    ASSERT_LT(after - before, Seconds(25));
    ASSERT_GTE(after - before, Milliseconds(25));
    thread.join();
    ASSERT_EQ(notifier->getVersion(), thisVersion);
}

TEST_F(CollectionTest, CreateTimeseriesBucketCollection) {
    NamespaceString nss("test.system.buckets.a");
    invariant(nss.isTimeseriesBucketsCollection());

    AutoGetOrCreateDb databaseWriteGuard(operationContext(), nss.db(), MODE_IX);
    auto db = databaseWriteGuard.getDb();
    invariant(db);

    Lock::CollectionLock lk(operationContext(), nss, MODE_IX);

    const BSONObj idxSpec = BSON("v" << IndexDescriptor::getDefaultIndexVersion() << "name"
                                     << "_id_"
                                     << "key" << BSON("_id" << 1));

    CollectionOptions options;
    options.clusteredIndex = ClusteredIndexOptions{};
    {
        WriteUnitOfWork wuow(operationContext());

        // Database::createCollection() ignores the index spec if the _id index is not required on
        // the collection.
        Collection* collection = db->createCollection(operationContext(),
                                                      nss,
                                                      options,
                                                      /*createIdIndex=*/true,
                                                      /*idIndex=*/
                                                      idxSpec);
        ASSERT(collection);
        ASSERT_EQ(0, collection->getIndexCatalog()->numIndexesTotal(operationContext()));

        StatusWith<BSONObj> swSpec = collection->getIndexCatalog()->createIndexOnEmptyCollection(
            operationContext(), idxSpec);
        ASSERT_NOT_OK(swSpec.getStatus());
        ASSERT_EQ(swSpec.getStatus().code(), ErrorCodes::CannotCreateIndex);
        ASSERT_STRING_CONTAINS(swSpec.getStatus().reason(),
                               "cannot have an _id index on a time-series bucket collection");

        // Rollback.
    }

    {
        WriteUnitOfWork wuow(operationContext());
        auto collection =
            db->createCollection(operationContext(), nss, options, /*createIdIndex=*/false);
        ASSERT(collection);
        ASSERT_EQ(0, collection->getIndexCatalog()->numIndexesTotal(operationContext()));
        wuow.commit();
    }
}

TEST_F(CatalogTestFixture, CollectionPtrNoYieldTag) {
    CollectionMock mock(NamespaceString("test.t"));

    CollectionPtr coll(&mock, CollectionPtr::NoYieldTag{});
    ASSERT_TRUE(coll);
    ASSERT_EQ(coll.get(), &mock);

    // Yield should be a no-op
    coll.yield();
    ASSERT_TRUE(coll);
    ASSERT_EQ(coll.get(), &mock);

    // Restore should also be a no-op
    coll.restore();
    ASSERT_TRUE(coll);
    ASSERT_EQ(coll.get(), &mock);

    coll.reset();
    ASSERT_FALSE(coll);
}

TEST_F(CatalogTestFixture, CollectionPtrYieldable) {
    CollectionMock beforeYield(NamespaceString("test.t"));
    CollectionMock afterYield(NamespaceString("test.t"));

    int numRestoreCalls = 0;

    CollectionPtr coll(operationContext(),
                       &beforeYield,
                       [&afterYield, &numRestoreCalls](OperationContext*, CollectionUUID) {
                           ++numRestoreCalls;
                           return &afterYield;
                       });

    ASSERT_TRUE(coll);
    ASSERT_EQ(coll.get(), &beforeYield);

    // Calling yield should invalidate
    coll.yield();
    ASSERT_FALSE(coll);
    ASSERT_EQ(numRestoreCalls, 0);

    // Calling yield when already yielded is a no-op
    coll.yield();
    ASSERT_FALSE(coll);
    ASSERT_EQ(numRestoreCalls, 0);

    // Restore should replace Collection pointer
    coll.restore();
    ASSERT_TRUE(coll);
    ASSERT_EQ(coll.get(), &afterYield);
    ASSERT_NE(coll.get(), &beforeYield);
    ASSERT_EQ(numRestoreCalls, 1);

    // Calling restore when we are valid is a no-op
    coll.restore();
    ASSERT_TRUE(coll);
    ASSERT_EQ(coll.get(), &afterYield);
    ASSERT_NE(coll.get(), &beforeYield);
    ASSERT_EQ(numRestoreCalls, 1);

    coll.reset();
    ASSERT_FALSE(coll);
}

}  // namespace
