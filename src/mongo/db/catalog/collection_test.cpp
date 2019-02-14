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

#include "mongo/bson/oid.h"
#include "mongo/db/catalog/capped_utils.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

class CollectionTest : public CatalogTestFixture {
protected:
    void makeCapped(NamespaceString nss, long long cappedSize = 8192);
    void makeUncapped(NamespaceString nss);
    void checkValidate(Collection* coll, bool valid, int records, int invalid, int errors);
    std::vector<ValidateCmdLevel> levels{kValidateIndex, kValidateRecordStore, kValidateFull};
};

void CollectionTest::makeCapped(NamespaceString nss, long long cappedSize) {
    CollectionOptions options;
    options.capped = true;
    options.cappedSize = cappedSize;  // Maximum size of capped collection in bytes.
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));
}

void CollectionTest::makeUncapped(NamespaceString nss) {
    CollectionOptions options;
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));
}

// Call validate with different validation levels and verify the results.
void CollectionTest::checkValidate(
    Collection* coll, bool valid, int records, int invalid, int errors) {
    auto opCtx = operationContext();
    auto collLock =
        std::make_unique<Lock::CollectionLock>(opCtx->lockState(), coll->ns().ns(), MODE_X);

    for (auto level : levels) {
        ValidateResults results;
        BSONObjBuilder output;
        auto status = coll->validate(opCtx, level, false, std::move(collLock), &results, &output);
        ASSERT_OK(status);
        ASSERT_EQ(results.valid, valid);
        ASSERT_EQ(results.errors.size(), (long unsigned int)errors);

        BSONObj obj = output.obj();
        ASSERT_EQ(obj.getIntField("nrecords"), records);
        ASSERT_EQ(obj.getIntField("nInvalidDocuments"), invalid);
    }
}

TEST_F(CollectionTest, CappedNotifierKillAndIsDead) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    Collection* col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();
    ASSERT_FALSE(notifier->isDead());
    notifier->kill();
    ASSERT(notifier->isDead());
}

TEST_F(CollectionTest, CappedNotifierTimeouts) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    Collection* col = acfr.getCollection();
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
    Collection* col = acfr.getCollection();
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
    Collection* col = acfr.getCollection();
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
    Collection* col = acfr.getCollection();
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
    Collection* col = acfr.getCollection();
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
    Collection* col = acfr.getCollection();
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
    Collection* col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();
    auto prevVersion = notifier->getVersion();
    auto thisVersion = prevVersion + 1;

    auto before = Date_t::now();
    notifier->waitUntil(prevVersion, before + Milliseconds(25));
    stdx::thread thread([before, prevVersion, col] {
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

// Verify that calling validate() on an empty collection with different validation levels returns an
// OK status.
TEST_F(CollectionTest, ValidateEmpty) {
    NamespaceString nss("test.t");
    makeUncapped(nss);

    auto opCtx = operationContext();
    AutoGetCollection agc(opCtx, nss, MODE_X);
    Collection* coll = agc.getCollection();

    checkValidate(coll, true, 0, 0, 0);
}

// Verify calling validate() on a nonempty collection with different validation levels.
TEST_F(CollectionTest, Validate) {
    NamespaceString nss("test.t");
    makeUncapped(nss);

    auto opCtx = operationContext();
    AutoGetCollection agc(opCtx, nss, MODE_X);
    Collection* coll = agc.getCollection();

    std::vector<InsertStatement> inserts;
    for (int i = 0; i < 5; i++) {
        auto doc = BSON("_id" << i);
        inserts.push_back(InsertStatement(doc));
    }

    auto status = coll->insertDocuments(opCtx, inserts.begin(), inserts.end(), nullptr, false);
    ASSERT_OK(status);
    checkValidate(coll, true, inserts.size(), 0, 0);
}

// Verify calling validate() on a collection with an invalid document.
TEST_F(CollectionTest, ValidateError) {
    NamespaceString nss("test.t");
    makeUncapped(nss);

    auto opCtx = operationContext();
    AutoGetCollection agc(opCtx, nss, MODE_X);
    Collection* coll = agc.getCollection();
    RecordStore* rs = coll->getRecordStore();

    std::string invalidBson = "\0\0\0\0\0";
    const char* recordData = invalidBson.c_str();
    auto statusWithId = rs->insertRecord(opCtx, recordData, 5, Timestamp::min());
    ASSERT_OK(statusWithId.getStatus());
    checkValidate(coll, false, 1, 1, 1);
}

}  // namespace
