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

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

#define ASSERT_ID_EQ(EXPR, ID)                        \
    [](boost::optional<Record> record, RecordId id) { \
        ASSERT(record);                               \
        ASSERT_EQ(record->id, id);                    \
    }((EXPR), (ID));

class CollectionTest : public CatalogTestFixture {
protected:
    void makeCapped(NamespaceString nss, long long cappedSize = 8192);
    void makeTimeseries(NamespaceString nss);
    void makeCollectionForMultikey(NamespaceString nss, StringData indexName);
};

void CollectionTest::makeCapped(NamespaceString nss, long long cappedSize) {
    CollectionOptions options;
    options.capped = true;
    options.cappedSize = cappedSize;  // Maximum size of capped collection in bytes.
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));
}

void CollectionTest::makeTimeseries(NamespaceString nss) {
    CollectionOptions options;
    options.timeseries = TimeseriesOptions(/*timeField=*/"t");
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));
}

TEST_F(CollectionTest, CappedNotifierKillAndIsDead) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getRecordStore()->getCappedInsertNotifier();
    ASSERT_FALSE(notifier->isDead());
    notifier->kill();
    ASSERT(notifier->isDead());
}

TEST_F(CollectionTest, CappedNotifierTimeouts) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getRecordStore()->getCappedInsertNotifier();
    ASSERT_EQ(notifier->getVersion(), 0u);

    auto before = Date_t::now();
    notifier->waitUntil(operationContext(), 0u, before + Milliseconds(25));
    auto after = Date_t::now();
    ASSERT_GTE(after - before, Milliseconds(25));
    ASSERT_EQ(notifier->getVersion(), 0u);
}

TEST_F(CollectionTest, CappedNotifierWaitAfterNotifyIsImmediate) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getRecordStore()->getCappedInsertNotifier();

    auto prevVersion = notifier->getVersion();
    notifier->notifyAll();
    auto thisVersion = prevVersion + 1;
    ASSERT_EQ(notifier->getVersion(), thisVersion);

    auto before = Date_t::now();
    notifier->waitUntil(operationContext(), prevVersion, before + Seconds(25));
    auto after = Date_t::now();
    ASSERT_LT(after - before, Seconds(25));
}

TEST_F(CollectionTest, CappedNotifierWaitUntilAsynchronousNotifyAll) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getRecordStore()->getCappedInsertNotifier();
    auto prevVersion = notifier->getVersion();
    auto thisVersion = prevVersion + 1;

    auto before = Date_t::now();
    stdx::thread thread([this, before, prevVersion, &notifier] {
        ThreadClient client(getServiceContext()->getService());
        auto opCtx = cc().makeOperationContext();
        notifier->waitUntil(opCtx.get(), prevVersion, before + Milliseconds(25));
        auto after = Date_t::now();
        ASSERT_GTE(after - before, Milliseconds(25));
        notifier->notifyAll();
    });
    notifier->waitUntil(operationContext(), prevVersion, before + Seconds(25));
    auto after = Date_t::now();
    ASSERT_LT(after - before, Seconds(25));
    ASSERT_GTE(after - before, Milliseconds(25));
    thread.join();
    ASSERT_EQ(notifier->getVersion(), thisVersion);
}

TEST_F(CollectionTest, CappedNotifierWaitUntilAsynchronousKill) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    makeCapped(nss);
    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getRecordStore()->getCappedInsertNotifier();
    auto prevVersion = notifier->getVersion();

    auto before = Date_t::now();
    stdx::thread thread([this, before, prevVersion, &notifier] {
        ThreadClient client(getServiceContext()->getService());
        auto opCtx = cc().makeOperationContext();
        notifier->waitUntil(opCtx.get(), prevVersion, before + Milliseconds(25));
        auto after = Date_t::now();
        ASSERT_GTE(after - before, Milliseconds(25));
        notifier->kill();
    });
    notifier->waitUntil(operationContext(), prevVersion, before + Seconds(25));
    auto after = Date_t::now();
    ASSERT_LT(after - before, Seconds(25));
    ASSERT_GTE(after - before, Milliseconds(25));
    thread.join();
    ASSERT_EQ(notifier->getVersion(), prevVersion);
}

TEST_F(CollectionTest, CappedNotifierWaitUntilInterrupt) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getRecordStore()->getCappedInsertNotifier();
    auto prevVersion = notifier->getVersion();

    auto& clientToInterrupt = cc();
    auto before = Date_t::now();
    stdx::thread thread([this, before, prevVersion, &notifier, &clientToInterrupt] {
        ThreadClient client(getServiceContext()->getService());
        auto opCtx = cc().makeOperationContext();
        notifier->waitUntil(opCtx.get(), prevVersion, before + Milliseconds(25));
        auto after = Date_t::now();
        ASSERT_GTE(after - before, Milliseconds(25));

        stdx::lock_guard<Client> lk(clientToInterrupt);
        getServiceContext()->killOperation(
            lk, clientToInterrupt.getOperationContext(), ErrorCodes::Interrupted);
    });

    ASSERT_THROWS(notifier->waitUntil(operationContext(), prevVersion, before + Seconds(25)),
                  ExceptionFor<ErrorCodes::Interrupted>);

    auto after = Date_t::now();
    ASSERT_LT(after - before, Seconds(25));
    ASSERT_GTE(after - before, Milliseconds(25));
    thread.join();
    ASSERT_EQ(notifier->getVersion(), prevVersion);
}

TEST_F(CollectionTest, HaveCappedWaiters) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    ASSERT(!col->getRecordStore()->haveCappedWaiters());
    {
        auto notifier = col->getRecordStore()->getCappedInsertNotifier();
        ASSERT(col->getRecordStore()->haveCappedWaiters());
    }
    ASSERT(!col->getRecordStore()->haveCappedWaiters());
}

TEST_F(CollectionTest, NotifyCappedWaitersIfNeeded) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    col->getRecordStore()->notifyCappedWaitersIfNeeded();
    {
        auto notifier = col->getRecordStore()->getCappedInsertNotifier();
        ASSERT_EQ(notifier->getVersion(), 0u);
        col->getRecordStore()->notifyCappedWaitersIfNeeded();
        ASSERT_EQ(notifier->getVersion(), 1u);
    }
}

TEST_F(CollectionTest, AsynchronouslyNotifyCappedWaitersIfNeeded) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getRecordStore()->getCappedInsertNotifier();
    auto prevVersion = notifier->getVersion();
    auto thisVersion = prevVersion + 1;

    auto before = Date_t::now();
    notifier->waitUntil(operationContext(), prevVersion, before + Milliseconds(25));
    stdx::thread thread([before, prevVersion, notifier] {
        auto after = Date_t::now();
        ASSERT_GTE(after - before, Milliseconds(25));
        notifier->notifyAll();
    });
    notifier->waitUntil(operationContext(), prevVersion, before + Seconds(25));
    auto after = Date_t::now();
    ASSERT_LT(after - before, Seconds(25));
    ASSERT_GTE(after - before, Milliseconds(25));
    thread.join();
    ASSERT_EQ(notifier->getVersion(), thisVersion);
}

void CollectionTest::makeCollectionForMultikey(NamespaceString nss, StringData indexName) {
    auto opCtx = operationContext();
    {
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        auto db = autoColl.ensureDbExists(opCtx);
        WriteUnitOfWork wuow(opCtx);
        ASSERT(db->createCollection(opCtx, nss));
        wuow.commit();
    }

    {
        AutoGetCollection autoColl(opCtx, nss, MODE_X);
        WriteUnitOfWork wuow(opCtx);
        auto collWriter = autoColl.getWritableCollection(opCtx);
        ASSERT_OK(collWriter->getIndexCatalog()->createIndexOnEmptyCollection(
            opCtx, collWriter, BSON("v" << 2 << "name" << indexName << "key" << BSON("a" << 1))));
        wuow.commit();
    }
}

TEST_F(CollectionTest, VerifyIndexIsUpdated) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    auto indexName = "myindex"_sd;
    makeCollectionForMultikey(nss, indexName);

    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    const auto& coll = autoColl.getCollection();

    auto oldDoc = BSON("_id" << 1 << "a" << 1);
    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(
            collection_internal::insertDocument(opCtx, coll, InsertStatement(oldDoc), nullptr));
        wuow.commit();
    }
    auto idxCatalog = coll->getIndexCatalog();
    auto idIndex = idxCatalog->findIdIndex(opCtx);
    auto userIdx = idxCatalog->findIndexByName(opCtx, indexName);
    auto oldRecordId = idIndex->getEntry()->accessMethod()->asSortedData()->findSingle(
        opCtx, coll, idIndex->getEntry(), BSON("_id" << 1));
    auto oldIndexRecordID = userIdx->getEntry()->accessMethod()->asSortedData()->findSingle(
        opCtx, coll, userIdx->getEntry(), BSON("a" << 1));
    ASSERT_TRUE(!oldRecordId.isNull());
    ASSERT_EQ(oldRecordId, oldIndexRecordID);
    {
        Snapshotted<BSONObj> result;
        ASSERT_TRUE(coll->findDoc(opCtx, oldRecordId, &result));
        ASSERT_BSONOBJ_EQ(oldDoc, result.value());
    }

    auto newDoc = BSON("_id" << 1 << "a" << 5);
    {
        WriteUnitOfWork wuow(opCtx);
        Snapshotted<BSONObj> oldSnap(shard_role_details::getRecoveryUnit(opCtx)->getSnapshotId(),
                                     oldDoc);
        CollectionUpdateArgs args{oldDoc};
        collection_internal::updateDocument(opCtx,
                                            coll,
                                            oldRecordId,
                                            oldSnap,
                                            newDoc,
                                            collection_internal::kUpdateAllIndexes,
                                            nullptr /* indexesAffected */,
                                            nullptr /* opDebug */,
                                            &args);
        wuow.commit();
    }
    auto indexRecordId = userIdx->getEntry()->accessMethod()->asSortedData()->findSingle(
        opCtx, coll, userIdx->getEntry(), BSON("a" << 1));
    ASSERT_TRUE(indexRecordId.isNull());
    indexRecordId = userIdx->getEntry()->accessMethod()->asSortedData()->findSingle(
        opCtx, coll, userIdx->getEntry(), BSON("a" << 5));
    ASSERT_EQ(indexRecordId, oldRecordId);
}

TEST_F(CollectionTest, VerifyIndexIsUpdatedWithDamages) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    auto indexName = "myindex"_sd;
    makeCollectionForMultikey(nss, indexName);

    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    const auto& coll = autoColl.getCollection();

    auto oldDoc = BSON("_id" << 1 << "a" << 1 << "b"
                             << "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(
            collection_internal::insertDocument(opCtx, coll, InsertStatement(oldDoc), nullptr));
        wuow.commit();
    }
    auto idxCatalog = coll->getIndexCatalog();
    auto idIndex = idxCatalog->findIdIndex(opCtx);
    auto userIdx = idxCatalog->findIndexByName(opCtx, indexName);
    auto oldRecordId = idIndex->getEntry()->accessMethod()->asSortedData()->findSingle(
        opCtx, coll, idIndex->getEntry(), BSON("_id" << 1));
    ASSERT_TRUE(!oldRecordId.isNull());

    auto newDoc = BSON("_id" << 1 << "a" << 5 << "b" << 32);
    auto diff = doc_diff::computeOplogDiff(oldDoc, newDoc, 0);
    ASSERT(diff);
    auto damagesOutput = doc_diff::computeDamages(oldDoc, *diff, false);
    {
        WriteUnitOfWork wuow(opCtx);
        Snapshotted<BSONObj> oldSnap(shard_role_details::getRecoveryUnit(opCtx)->getSnapshotId(),
                                     oldDoc);
        CollectionUpdateArgs args{oldDoc};
        auto newDocStatus =
            collection_internal::updateDocumentWithDamages(opCtx,
                                                           coll,
                                                           oldRecordId,
                                                           oldSnap,
                                                           damagesOutput.damageSource.get(),
                                                           damagesOutput.damages,
                                                           collection_internal::kUpdateAllIndexes,
                                                           nullptr /* indexesAffected */,
                                                           nullptr /* opDebug */,
                                                           &args);
        ASSERT_OK(newDocStatus);
        ASSERT_BSONOBJ_EQ(newDoc, newDocStatus.getValue());
        wuow.commit();
    }
    auto indexRecordId = userIdx->getEntry()->accessMethod()->asSortedData()->findSingle(
        opCtx, coll, userIdx->getEntry(), BSON("a" << 1));
    ASSERT_TRUE(indexRecordId.isNull());
    indexRecordId = userIdx->getEntry()->accessMethod()->asSortedData()->findSingle(
        opCtx, coll, userIdx->getEntry(), BSON("a" << 5));
    ASSERT_EQ(indexRecordId, oldRecordId);
}

TEST_F(CollectionTest, SetIndexIsMultikey) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    auto indexName = "myindex"_sd;
    makeCollectionForMultikey(nss, indexName);

    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    const auto& coll = autoColl.getCollection();
    ASSERT(coll);
    MultikeyPaths paths = {{0}};
    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT(coll->setIndexIsMultikey(opCtx, indexName, paths));
        wuow.commit();
    }
    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_FALSE(coll->setIndexIsMultikey(opCtx, indexName, paths));
        wuow.commit();
    }
}

TEST_F(CollectionTest, SetIndexIsMultikeyRemovesUncommittedChangesOnRollback) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    auto indexName = "myindex"_sd;
    makeCollectionForMultikey(nss, indexName);

    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    const auto& coll = autoColl.getCollection();
    ASSERT(coll);
    MultikeyPaths paths = {{0}};

    {
        FailPointEnableBlock failPoint("WTWriteConflictException");
        WriteUnitOfWork wuow(opCtx);
        ASSERT_THROWS(coll->setIndexIsMultikey(opCtx, indexName, paths), WriteConflictException);
    }

    // After rolling back the above WUOW, we should succeed in retrying setIndexIsMultikey().
    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT(coll->setIndexIsMultikey(opCtx, indexName, paths));
        wuow.commit();
    }
}

TEST_F(CollectionTest, ForceSetIndexIsMultikey) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    auto indexName = "myindex"_sd;
    makeCollectionForMultikey(nss, indexName);

    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    const auto& coll = autoColl.getCollection();
    ASSERT(coll);
    MultikeyPaths paths = {{0}};
    {
        WriteUnitOfWork wuow(opCtx);
        auto desc = coll->getIndexCatalog()->findIndexByName(opCtx, indexName);
        coll->forceSetIndexIsMultikey(opCtx, desc, true, paths);
        wuow.commit();
    }
    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_FALSE(coll->setIndexIsMultikey(opCtx, indexName, paths));
        wuow.commit();
    }
}

TEST_F(CollectionTest, ForceSetIndexIsMultikeyRemovesUncommittedChangesOnRollback) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    auto indexName = "myindex"_sd;
    makeCollectionForMultikey(nss, indexName);

    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    const auto& coll = autoColl.getCollection();
    ASSERT(coll);
    MultikeyPaths paths = {{0}};

    {
        FailPointEnableBlock failPoint("WTWriteConflictException");
        WriteUnitOfWork wuow(opCtx);
        auto desc = coll->getIndexCatalog()->findIndexByName(opCtx, indexName);
        ASSERT_THROWS(coll->forceSetIndexIsMultikey(opCtx, desc, true, paths),
                      WriteConflictException);
    }

    // After rolling back the above WUOW, we should succeed in retrying setIndexIsMultikey().
    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT(coll->setIndexIsMultikey(opCtx, indexName, paths));
        wuow.commit();
    }
}

TEST_F(CollectionTest, CheckTimeseriesBucketDocsForMixedSchemaData) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.system.buckets.ts");
    makeTimeseries(nss);

    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    const auto& coll = autoColl.getCollection();
    ASSERT(coll);
    ASSERT(coll->getTimeseriesOptions());

    // These are the min/max control fields generated prior to the change in SERVER-60565 in order
    // to test the detection of mixed-schema data in time-series buckets from earlier versions.
    std::vector<BSONObj> mixedSchemaControlDocs = {
        // Insert -> {x: NumberLong(1)}, {x: {y: "z"}}, {x: "abc"}
        ::mongo::fromjson(
            R"({ "control" : { "min" : { "x" : NumberLong(1) },
                               "max" : { "x" : { "y" : "z" } } } })"),
        // Insert -> {x: NumberLong(1)}, {x: [1, 2, 3]}, {x: "abc"}
        ::mongo::fromjson(
            R"({ "control" : { "min" : { "x" : NumberLong(1) },
                               "max" : { "x" : [ 1, 2, 3 ] } } })"),
        // Insert -> {x: {y: 1}}, {x: {y: 2}}, {x: {y: [1, 2]}}
        ::mongo::fromjson(
            R"({ "control" : { "min" : { "x" : { "y" : 1 } },
                               "max" : { "x" : { "y" : [ 1, 2 ] } } } })"),
        // Insert -> {x: 1}, {x: {y: 10}}, {x: true}
        ::mongo::fromjson(R"({ "control" : { "min" : { "x" : 1 },
                                             "max" : { "x" : true } } })"),
        // Insert -> {x: {y: 1}}, {x: {y: 2}}, {x: {y: null}}
        ::mongo::fromjson(
            R"({ "control" : { "min" : { "x" : { "y" : null } },
                               "max" : { "x" : { "y" : 2 } } } })"),
        // Insert -> {x: {y: true}}, {x: {y: false}}, {x: {y: null}}
        ::mongo::fromjson(
            R"({ "control" : { "min" : { "x" : { "y" : null } },
                               "max" : { "x" : { "y" : true } } } })"),
        // Insert -> {x: NumberLong(1)}, {x: {y: NumberDecimal(1.5)}}, {x: NumberLong(2)}
        ::mongo::fromjson(
            R"({ "control" : { "min" : { "x" : NumberLong(1) },
                               "max" : { "x" : { "y" : NumberDecimal("1.50000000000000") } } } })"),
        // Insert -> {x: ["abc"]}, {x: [123]}
        ::mongo::fromjson(R"({ "control" : { "min" : { "x" : [ 123 ] },
                                             "max" : { "x" : [ "abc" ] } } })"),
        // Insert -> {x: ["abc", 123]}, {x: [123, "abc"]}
        ::mongo::fromjson(
            R"({ "control" : { "min" : { "x" : [ 123, 123 ] },
                               "max" : { "x" : [ "abc", "abc" ] } } })"),
        // Insert -> {x: {y: 1}}, {x: {y: {z: 5}}}, {x: {y: [1, 2]}}
        ::mongo::fromjson(
            R"({ "control" : { "min" : { "x" : { "y" : 1 } },
                               "max" : { "x" : { "y" : [ 1, 2 ] } } } })"),
        // Insert -> {x: Number(1.0)}, {x: {y: "z"}}, {x: NumberLong(10)}
        ::mongo::fromjson(R"({ "control" : { "min" : { "x" : 1 },
                                             "max" : { "x" : { "y" : "z" } } } })"),
        // Insert -> {x: Number(1.0)}, {x: [Number(2.0), Number(3.0)]}, {x: NumberLong(10)}
        ::mongo::fromjson(R"({ "control" : { "min" : { "x" : 1 },
                                             "max" : { "x" : [ 2, 3 ] } } })")};

    for (const auto& controlDoc : mixedSchemaControlDocs) {
        auto mixedSchema = coll->doesTimeseriesBucketsDocContainMixedSchemaData(controlDoc);
        ASSERT_OK(mixedSchema) << controlDoc;
        ASSERT_TRUE(mixedSchema.getValue()) << controlDoc;
    }

    std::vector<BSONObj> nonMixedSchemaControlDocs = {
        // Insert -> {x: 1}, {x: 2}, {x: 3}
        ::mongo::fromjson(R"({ "control" : { "min" : { "x" : 1 },
                                             "max" : { "x" : 3 } } })"),
        // Insert -> {x: 1}, {x: 1.5}
        ::mongo::fromjson(R"({ "control" : { "min" : { "x" : 1 },
                                             "max" : { "x" : 1.5 } } })"),
        // Insert -> {x: NumberLong(1)}, {x: NumberDecimal(2)}
        ::mongo::fromjson(
            R"({ "control" : { "min" : { "x" : NumberLong(1) },
                               "max" : { "x" : NumberDecimal("2.00000000000000") } } })"),
        // Insert -> {x: NumberInt(1)}, {x: NumberDecimal(1.5)}, {x: NumberLong(2)}
        ::mongo::fromjson(R"({ "control" : { "min" : { "x" : 1 },
                                             "max" : { "x" : NumberLong(2) } } })"),
        // Insert -> {x: NumberLong(1)}, {x: NumberDecimal(1.5)}, {x: NumberLong(2)}
        ::mongo::fromjson(
            R"({ "control" : { "min" : { "x" : NumberLong(1) },
                               "max" : { "x" : NumberLong(2) } } })"),
        // Insert -> {x: {y: true}}, {x: {y: false}}
        ::mongo::fromjson(
            R"({ "control" : { "min" : { "x" : { "y" : false } },
                               "max" : { "x" : { "y" : true } } } })"),
        // Insert -> {x: [1, 2, 3]}, {x: [4, 5, 6]}
        ::mongo::fromjson(
            R"({ "control" : { "min" : { "x" : [ 1, 2, 3 ] },
                               "max" : { "x" : [ 4, 5, 6 ] } } })"),
        // Insert -> {x: [{x: 1}, {z: false}]}, {x: [{x: 5}, {y: "abc"}]}
        ::mongo::fromjson(
            R"({ "control" : { "min" : { "x" : [ { "x" : 1 }, { "y" : "abc", "z" : false } ] },
                               "max" : { "x" : [ { "x" : 5 }, { "y" : "abc", "z" : false } ] } } })"),
        // Insert -> {x: 1}, {y: 1}
        ::mongo::fromjson(R"({ "control" : { "min" : { "x" : 1, "y" : 1 },
                                             "max" : { "x" : 1, "y" : 1 } } })"),
        // Insert -> {x: ["a"]}, {y: [1]}
        ::mongo::fromjson(
            R"({ control : { min : { x : [ "a" ], y : [ 1 ] },
                             max : { x : [ "a" ], y : [ 1 ] } } })"),
        // Insert -> {x: {y: [{a: Number(1.0)}, [{b: NumberLong(10)}]]}},
        //           {x: {y: [{a: Number(5.0)}, [{b: NumberLong(50)}]]}}
        ::mongo::fromjson(
            R"({ "control" : { "min" : { "x" : { "y" : [ { "a" : 1 }, [ { "b" : NumberLong(10) } ] ] } },
                               "max" : { "x" : { "y" : [ { "a" : 5 }, [ { "b" : NumberLong(50) } ] ] } } } })"),
        // Insert -> {x: Number(1.0)}, {x: NumberLong(10)}
        ::mongo::fromjson(R"({ "control" : { "min" : { "x" : 1 },
                                             "max" : { "x" : NumberLong(10) } } })"),

        // Insert -> {x: {y: [{a: Number(1.5)}, [{b: NumberLong(10)}]]}},
        //           {x: {y: [{a: Number(2.5)}, [{b: Number(3.5)}]]}}
        ::mongo::fromjson(
            R"({ "control" : { "min" : { "x" : { "y" : [ { "a" : 1.5 }, [ { "b" : 3.5 } ] ] } },
                               "max" : { "x" : { "y" : [ { "a" : 2.5 }, [ { "b" : NumberLong(10) } ] ] } } } })")};


    for (const auto& controlDoc : nonMixedSchemaControlDocs) {
        auto mixedSchema = coll->doesTimeseriesBucketsDocContainMixedSchemaData(controlDoc);
        ASSERT_OK(mixedSchema) << controlDoc;
        ASSERT_FALSE(mixedSchema.getValue()) << controlDoc;
    }

    std::vector<BSONObj> malformedControlDocs = {
        // Inconsistent field name ordering
        ::mongo::fromjson(R"({ "control" : { "min" : { "x" : 1, "y" : 1 },
                                             "max" : { "y" : 2, "x" : 2 } } })"),

        // Extra field in min
        ::mongo::fromjson(R"({ "control" : { "min" : { "x" : 1, "y" : 1 },
                                             "max" : { "x" : 2 } } })"),

        // Extra field in max
        ::mongo::fromjson(R"({ "control" : { "min" : { "y" : 1 },
                                             "max" : { "y" : 2, "x" : 2 } } })")};

    for (const auto& controlDoc : malformedControlDocs) {
        ASSERT_NOT_OK(coll->doesTimeseriesBucketsDocContainMixedSchemaData(controlDoc))
            << controlDoc;
    }
}

TEST_F(CatalogTestFixture, CollectionPtrYieldable) {
    CollectionMock beforeYield(NamespaceString::createNamespaceString_forTest("test.t"));
    CollectionMock afterYield(NamespaceString::createNamespaceString_forTest("test.t"));

    int numRestoreCalls = 0;

    CollectionPtr coll(&beforeYield);
    coll.makeYieldable(operationContext(),
                       [&afterYield, &numRestoreCalls](OperationContext*, UUID) {
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

TEST_F(CatalogTestFixture, IsNotCapped) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    CollectionOptions options;
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& coll = acfr.getCollection();
    ASSERT(!coll->isCapped());
}

TEST_F(CatalogTestFixture, CappedDeleteRecord) {
    // Insert a document into a capped collection that has a maximum document size of 1.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    CollectionOptions options;
    options.capped = true;
    options.cappedMaxDocs = 1;
    // Large enough to use 'cappedMaxDocs' as the primary indicator for capped deletes.
    options.cappedSize = 512 * 1024 * 1024;
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));

    AutoGetCollection autoColl(operationContext(), nss, MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();

    ASSERT_EQUALS(0, coll->numRecords(operationContext()));

    BSONObj firstDoc = BSON("_id" << 1);
    BSONObj secondDoc = BSON("_id" << 2);

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(collection_internal::insertDocument(
            operationContext(), coll, InsertStatement(firstDoc), nullptr));
        wuow.commit();
    }

    ASSERT_EQUALS(1, coll->numRecords(operationContext()));

    // Inserting the second document will remove the first one.
    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(collection_internal::insertDocument(
            operationContext(), coll, InsertStatement(secondDoc), nullptr));
        wuow.commit();
    }

    ASSERT_EQUALS(1, coll->numRecords(operationContext()));

    auto cursor = coll->getRecordStore()->getCursor(operationContext());
    auto record = cursor->next();
    ASSERT(record);
    ASSERT(record->data.toBson().woCompare(secondDoc) == 0);
    ASSERT(!cursor->next());
}

TEST_F(CatalogTestFixture, CappedDeleteMultipleRecords) {
    // Insert multiple records at once, requiring multiple deletes.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    CollectionOptions options;
    options.capped = true;
    options.cappedMaxDocs = 10;
    // Large enough to use 'cappedMaxDocs' as the primary indicator for capped deletes.
    options.cappedSize = 512 * 1024 * 1024;
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));

    AutoGetCollection autoColl(operationContext(), nss, MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();

    ASSERT_EQUALS(0, coll->numRecords(operationContext()));

    const int nToInsertFirst = options.cappedMaxDocs / 2;
    const int nToInsertSecond = options.cappedMaxDocs;

    {
        WriteUnitOfWork wuow(operationContext());
        for (int i = 0; i < nToInsertFirst; i++) {
            BSONObj doc = BSON("_id" << i);
            ASSERT_OK(collection_internal::insertDocument(
                operationContext(), coll, InsertStatement(doc), nullptr));
        }
        wuow.commit();
    }

    ASSERT_EQUALS(nToInsertFirst, coll->numRecords(operationContext()));

    {
        WriteUnitOfWork wuow(operationContext());
        for (int i = nToInsertFirst; i < nToInsertFirst + nToInsertSecond; i++) {
            BSONObj doc = BSON("_id" << i);
            ASSERT_OK(collection_internal::insertDocument(
                operationContext(), coll, InsertStatement(doc), nullptr));
        }
        wuow.commit();
    }

    ASSERT_EQUALS(options.cappedMaxDocs, coll->numRecords(operationContext()));

    const int firstExpectedId = nToInsertFirst + nToInsertSecond - options.cappedMaxDocs;

    int numSeen = 0;
    auto cursor = coll->getRecordStore()->getCursor(operationContext());
    while (auto record = cursor->next()) {
        const BSONObj expectedDoc = BSON("_id" << firstExpectedId + numSeen);
        ASSERT(record->data.toBson().woCompare(expectedDoc) == 0);
        numSeen++;
    }
}

TEST_F(CatalogTestFixture, CappedVisibilityEmptyInitialState) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    CollectionOptions options;
    options.capped = true;
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));

    AutoGetCollection autoColl(operationContext(), nss, MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();
    RecordStore* rs = coll->getRecordStore();

    auto doInsert = [&](OperationContext* opCtx) -> RecordId {
        Lock::GlobalLock globalLock{opCtx, MODE_IX};
        std::string data = "data";
        return uassertStatusOK(rs->insertRecord(opCtx, data.c_str(), data.size(), Timestamp()));
    };

    auto longLivedClient = getServiceContext()->getService()->makeClient("longLived");
    auto longLivedOpCtx = longLivedClient->makeOperationContext();
    WriteUnitOfWork longLivedWUOW(longLivedOpCtx.get());

    // Collection is really empty.
    ASSERT(!rs->getCursor(longLivedOpCtx.get(), true)->next());
    ASSERT(!rs->getCursor(longLivedOpCtx.get(), false)->next());

    RecordId lowestHiddenId = doInsert(longLivedOpCtx.get());
    RecordId otherId;

    {
        WriteUnitOfWork wuow(operationContext());

        // Can't see uncommitted write from other operation.
        ASSERT(!rs->getCursor(operationContext())->seekExact(lowestHiddenId));

        ASSERT(!rs->getCursor(operationContext(), true)->next());
        ASSERT(!rs->getCursor(operationContext(), false)->next());

        otherId = doInsert(operationContext());

        // Can read own writes.
        ASSERT_ID_EQ(rs->getCursor(operationContext(), true)->next(), otherId);
        ASSERT_ID_EQ(rs->getCursor(operationContext(), false)->next(), otherId);
        ASSERT_ID_EQ(rs->getCursor(operationContext())->seekExact(otherId), otherId);

        wuow.commit();
    }

    // longLivedOpCtx is still on old snapshot so it can't see otherId yet.
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), true)->next(), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), false)->next(), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get())->seekExact(lowestHiddenId), lowestHiddenId);
    ASSERT(!rs->getCursor(longLivedOpCtx.get())->seekExact(otherId));

    // Make all documents visible and let longLivedOp get a new snapshot.
    longLivedWUOW.commit();

    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), true)->next(), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), false)->next(), otherId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get())->seekExact(lowestHiddenId), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get())->seekExact(otherId), otherId);
}

TEST_F(CatalogTestFixture, CappedVisibilityNonEmptyInitialState) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    CollectionOptions options;
    options.capped = true;
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));

    AutoGetCollection autoColl(operationContext(), nss, MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();
    RecordStore* rs = coll->getRecordStore();

    auto doInsert = [&](OperationContext* opCtx) -> RecordId {
        Lock::GlobalLock globalLock{opCtx, MODE_IX};
        std::string data = "data";
        return uassertStatusOK(rs->insertRecord(opCtx, data.c_str(), data.size(), Timestamp()));
    };

    auto longLivedClient = getServiceContext()->getService()->makeClient("longLived");
    auto longLivedOpCtx = longLivedClient->makeOperationContext();

    RecordId initialId;
    {
        WriteUnitOfWork wuow(longLivedOpCtx.get());
        initialId = doInsert(longLivedOpCtx.get());
        wuow.commit();
    }

    WriteUnitOfWork longLivedWUOW(longLivedOpCtx.get());

    // Can see initial doc.
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), true)->next(), initialId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), false)->next(), initialId);

    RecordId lowestHiddenId = doInsert(longLivedOpCtx.get());

    // Collection still looks like it only has a single doc to iteration but not seekExact.
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), true)->next(), initialId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), false)->next(), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get())->seekExact(initialId), initialId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get())->seekExact(lowestHiddenId), lowestHiddenId);

    RecordId otherId;
    {
        WriteUnitOfWork wuow(operationContext());

        // Can only see committed writes from other operation.
        ASSERT_ID_EQ(rs->getCursor(operationContext())->seekExact(initialId), initialId);
        ASSERT(!rs->getCursor(operationContext())->seekExact(lowestHiddenId));

        ASSERT_ID_EQ(rs->getCursor(operationContext(), true)->next(), initialId);
        ASSERT_ID_EQ(rs->getCursor(operationContext(), false)->next(), initialId);

        otherId = doInsert(operationContext());

        ASSERT_ID_EQ(rs->getCursor(operationContext(), true)->next(), initialId);
        ASSERT_ID_EQ(rs->getCursor(operationContext(), false)->next(), otherId);
        ASSERT_ID_EQ(rs->getCursor(operationContext())->seekExact(otherId), otherId);

        wuow.commit();

        ASSERT_ID_EQ(rs->getCursor(operationContext(), true)->next(), initialId);
        ASSERT_ID_EQ(rs->getCursor(operationContext(), false)->next(), otherId);
        ASSERT_ID_EQ(rs->getCursor(operationContext())->seekExact(otherId), otherId);
        ASSERT(!rs->getCursor(operationContext())->seekExact(lowestHiddenId));
    }

    // longLivedOpCtx is still on old snapshot so it can't see otherId yet.
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), true)->next(), initialId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), false)->next(), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get())->seekExact(lowestHiddenId), lowestHiddenId);
    ASSERT(!rs->getCursor(longLivedOpCtx.get())->seekExact(otherId));

    // This makes all documents visible and lets longLivedOpCtx get a new snapshot.
    longLivedWUOW.commit();

    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), true)->next(), initialId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), false)->next(), otherId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get())->seekExact(initialId), initialId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get())->seekExact(lowestHiddenId), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get())->seekExact(otherId), otherId);
}

TEST_F(CollectionTest, CappedCursorRollover) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    CollectionOptions options;
    options.capped = true;
    options.cappedMaxDocs = 5;
    // Large enough to use 'cappedMaxDocs' as the primary indicator for capped deletes.
    options.cappedSize = 512 * 1024 * 1024;
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));

    AutoGetCollection autoColl(operationContext(), nss, MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();
    RecordStore* rs = coll->getRecordStore();

    // First insert 3 documents.
    const int numToInsertFirst = 3;

    {
        WriteUnitOfWork wuow(operationContext());
        for (int i = 0; i < numToInsertFirst; ++i) {
            const BSONObj doc = BSON("_id" << i);
            ASSERT_OK(collection_internal::insertDocument(
                operationContext(), coll, InsertStatement(doc), nullptr));
        }
        wuow.commit();
    }

    // Setup the cursor that should rollover.
    auto otherClient = getServiceContext()->getService()->makeClient("otherClient");
    auto otherOpCtx = otherClient->makeOperationContext();
    Lock::GlobalLock globalLock{otherOpCtx.get(), MODE_IS};
    auto cursor = rs->getCursor(otherOpCtx.get());
    ASSERT(cursor->next());
    cursor->save();
    shard_role_details::getRecoveryUnit(otherOpCtx.get())->abandonSnapshot();

    // Insert 10 documents which causes a rollover.
    {
        WriteUnitOfWork wuow(operationContext());
        for (int i = numToInsertFirst; i < numToInsertFirst + 10; ++i) {
            const BSONObj doc = BSON("_id" << i);
            ASSERT_OK(collection_internal::insertDocument(
                operationContext(), coll, InsertStatement(doc), nullptr));
        }
        wuow.commit();
    }

    // Cursor should now be dead.
    ASSERT_FALSE(cursor->restore(false));
    ASSERT(!cursor->next());
}

TEST_F(CollectionTest, BoundedSeek) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, {}));

    AutoGetCollection autoColl(operationContext(), nss, MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();
    RecordStore* rs = coll->getRecordStore();

    auto doInsert = [&](OperationContext* opCtx) -> RecordId {
        Lock::GlobalLock globalLock{opCtx, MODE_IX};
        std::string data = "data";
        return uassertStatusOK(rs->insertRecord(opCtx, data.c_str(), data.size(), Timestamp()));
    };

    // Insert 5 records and delete the first one.
    const int numToInsert = 5;
    RecordId recordIds[numToInsert];
    {
        WriteUnitOfWork wuow(operationContext());
        for (int i = 0; i < numToInsert; ++i) {
            recordIds[i] = doInsert(operationContext());
        }
        Lock::GlobalLock globalLock{operationContext(), MODE_IX};
        rs->deleteRecord(operationContext(), recordIds[0]);
        wuow.commit();
    }

    // Forward inclusive seek
    ASSERT_ID_EQ(rs->getCursor(operationContext())
                     ->seek(recordIds[1], SeekableRecordCursor::BoundInclusion::kInclude),
                 recordIds[1]);
    ASSERT_ID_EQ(rs->getCursor(operationContext())
                     ->seek(recordIds[0], SeekableRecordCursor::BoundInclusion::kInclude),
                 recordIds[1]);
    ASSERT(!rs->getCursor(operationContext())
                ->seek(RecordId(recordIds[numToInsert - 1].getLong() + 1),
                       SeekableRecordCursor::BoundInclusion::kInclude));

    // Forward exclusive seek
    ASSERT_ID_EQ(rs->getCursor(operationContext())
                     ->seek(recordIds[1], SeekableRecordCursor::BoundInclusion::kExclude),
                 recordIds[2]);
    ASSERT(!rs->getCursor(operationContext())
                ->seek(RecordId(recordIds[numToInsert - 1]),
                       SeekableRecordCursor::BoundInclusion::kExclude));

    // Reverse inclusive seek
    ASSERT_ID_EQ(
        rs->getCursor(operationContext(), false)
            ->seek(recordIds[numToInsert - 1], SeekableRecordCursor::BoundInclusion::kInclude),
        recordIds[numToInsert - 1]);
    ASSERT_ID_EQ(rs->getCursor(operationContext(), false)
                     ->seek(RecordId(recordIds[numToInsert - 1].getLong() + 1),
                            SeekableRecordCursor::BoundInclusion::kInclude),
                 recordIds[numToInsert - 1]);
    ASSERT(!rs->getCursor(operationContext(), false)
                ->seek(recordIds[0], SeekableRecordCursor::BoundInclusion::kInclude));

    // Reverse exclusive seek
    ASSERT_ID_EQ(
        rs->getCursor(operationContext(), false)
            ->seek(recordIds[numToInsert - 1], SeekableRecordCursor::BoundInclusion::kExclude),
        recordIds[numToInsert - 2]);
    ASSERT(!rs->getCursor(operationContext(), false)
                ->seek(RecordId(recordIds[1]), SeekableRecordCursor::BoundInclusion::kExclude));
}

TEST_F(CatalogTestFixture, CappedCursorYieldFirst) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    CollectionOptions options;
    options.capped = true;
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));

    AutoGetCollection autoColl(operationContext(), nss, MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();
    RecordStore* rs = coll->getRecordStore();

    RecordId recordId;
    {
        WriteUnitOfWork wuow(operationContext());
        std::string data = "data";
        StatusWith<RecordId> res =
            rs->insertRecord(operationContext(), data.c_str(), data.size(), Timestamp());
        ASSERT_OK(res.getStatus());
        recordId = res.getValue();
        wuow.commit();
    }

    auto cursor = rs->getCursor(operationContext());

    // See that things work if you yield before you first call next().
    cursor->save();
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    ASSERT_TRUE(cursor->restore());

    auto record = cursor->next();
    ASSERT(record);
    ASSERT_EQ(recordId, record->id);

    ASSERT(!cursor->next());
}

}  // namespace
}  // namespace mongo
