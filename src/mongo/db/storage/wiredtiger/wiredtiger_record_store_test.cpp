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

#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

#include <wiredtiger.h>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

TEST(WiredTigerRecordStoreTest, GenerateCreateStringEmptyDocument) {
    BSONObj spec = fromjson("{}");
    StatusWith<std::string> result = WiredTigerRecordStore::parseOptionsField(spec);
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue(), "");  // "," would also be valid.
}

TEST(WiredTigerRecordStoreTest, GenerateCreateStringUnknownField) {
    BSONObj spec = fromjson("{unknownField: 1}");
    StatusWith<std::string> result = WiredTigerRecordStore::parseOptionsField(spec);
    const Status& status = result.getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::InvalidOptions, status);
}

TEST(WiredTigerRecordStoreTest, GenerateCreateStringNonStringConfig) {
    BSONObj spec = fromjson("{configString: 12345}");
    StatusWith<std::string> result = WiredTigerRecordStore::parseOptionsField(spec);
    const Status& status = result.getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
}

TEST(WiredTigerRecordStoreTest, GenerateCreateStringEmptyConfigString) {
    BSONObj spec = fromjson("{configString: ''}");
    StatusWith<std::string> result = WiredTigerRecordStore::parseOptionsField(spec);
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue(), ",");  // "" would also be valid.
}

TEST(WiredTigerRecordStoreTest, GenerateCreateStringInvalidConfigStringOption) {
    BSONObj spec = fromjson("{configString: 'abc=def'}");
    ASSERT_EQ(WiredTigerRecordStore::parseOptionsField(spec), ErrorCodes::BadValue);
}

TEST(WiredTigerRecordStoreTest, GenerateCreateStringValidConfigStringOption) {
    BSONObj spec = fromjson("{configString: 'prefix_compression=true'}");
    ASSERT_EQ(WiredTigerRecordStore::parseOptionsField(spec),
              std::string("prefix_compression=true,"));
}

TEST(WiredTigerRecordStoreTest, Isolation1) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());
    RecordId id1;
    RecordId id2;

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);

            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 "a",
                                 2,
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            id1 = res.getValue();

            res = rs->insertRecord(opCtx.get(),
                                   *shard_role_details::getRecoveryUnit(opCtx.get()),
                                   "a",
                                   2,
                                   Timestamp());
            ASSERT_OK(res.getStatus());
            id2 = res.getValue();

            txn.commit();
        }
    }

    {
        auto client1 = harnessHelper->serviceContext()->getService()->makeClient("c1");
        auto t1 = harnessHelper->newOperationContext(client1.get());
        auto& ru1 = *shard_role_details::getRecoveryUnit(t1.get());

        auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
        auto t2 = harnessHelper->newOperationContext(client2.get());
        auto& ru2 = *shard_role_details::getRecoveryUnit(t2.get());

        auto w1 = std::make_unique<StorageWriteTransaction>(ru1);
        auto w2 = std::make_unique<StorageWriteTransaction>(ru2);

        rs->dataFor(t1.get(), ru1, id1);
        rs->dataFor(t2.get(), ru2, id1);

        ASSERT_OK(rs->updateRecord(
            t1.get(), *shard_role_details::getRecoveryUnit(t1.get()), id1, "b", 2));
        ASSERT_OK(rs->updateRecord(
            t1.get(), *shard_role_details::getRecoveryUnit(t1.get()), id2, "B", 2));

        try {
            // this should fail
            rs->updateRecord(t2.get(), *shard_role_details::getRecoveryUnit(t2.get()), id1, "c", 2)
                .transitional_ignore();
            ASSERT(0);
        } catch (const StorageUnavailableException&) {
            w2.reset();
            t2.reset();
        }

        w1->commit();  // this should succeed
    }
}

TEST(WiredTigerRecordStoreTest, Isolation2) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    RecordId id1;
    RecordId id2;

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);

            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 "a",
                                 2,
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            id1 = res.getValue();

            res = rs->insertRecord(opCtx.get(),
                                   *shard_role_details::getRecoveryUnit(opCtx.get()),
                                   "a",
                                   2,
                                   Timestamp());
            ASSERT_OK(res.getStatus());
            id2 = res.getValue();

            txn.commit();
        }
    }

    {
        auto client1 = harnessHelper->serviceContext()->getService()->makeClient("c1");
        auto t1 = harnessHelper->newOperationContext(client1.get());
        auto& ru1 = *shard_role_details::getRecoveryUnit(t1.get());

        auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
        auto t2 = harnessHelper->newOperationContext(client2.get());
        auto& ru2 = *shard_role_details::getRecoveryUnit(t2.get());

        // ensure we start transactions
        rs->dataFor(t1.get(), ru1, id2);
        rs->dataFor(t2.get(), ru2, id2);

        {
            StorageWriteTransaction w(ru1);
            ASSERT_OK(rs->updateRecord(
                t1.get(), *shard_role_details::getRecoveryUnit(t1.get()), id1, "b", 2));
            w.commit();
        }

        {
            StorageWriteTransaction w(ru2);
            ASSERT_EQUALS(std::string("a"), rs->dataFor(t2.get(), ru2, id1).data());
            try {
                // this should fail as our version of id1 is too old
                rs->updateRecord(
                      t2.get(), *shard_role_details::getRecoveryUnit(t2.get()), id1, "c", 2)
                    .transitional_ignore();
                ASSERT(0);
            } catch (const StorageUnavailableException&) {
            }
        }
    }
}

RecordId oplogOrderInsertOplog(OperationContext* opCtx,
                               KVEngine* engine,
                               const std::unique_ptr<RecordStore>& rs,
                               int inc) {
    Timestamp opTime = Timestamp(5, inc);
    Status status = engine->oplogDiskLocRegister(
        *shard_role_details::getRecoveryUnit(opCtx), rs.get(), opTime, false);
    ASSERT_OK(status);
    BSONObj obj = BSON("ts" << opTime);
    StatusWith<RecordId> res = rs->insertRecord(
        opCtx, *shard_role_details::getRecoveryUnit(opCtx), obj.objdata(), obj.objsize(), opTime);
    ASSERT_OK(res.getStatus());
    return res.getValue();
}

/**
 * Test that even when the oplog durability loop is paused, we can still advance the commit point as
 * long as the commit for each insert comes before the next insert starts.
 */
TEST(WiredTigerRecordStoreTest, OplogDurableVisibilityInOrder) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());
    auto engine = static_cast<WiredTigerKVEngine*>(harnessHelper->getEngine());
    engine->getOplogManager()->stop();

    auto isOpHidden = [&engine](const RecordId& id) {
        return engine->getOplogManager()->getOplogReadTimestamp() <
            static_cast<std::uint64_t>(id.getLong());
    };

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        StorageWriteTransaction txn(ru);
        RecordId id = oplogOrderInsertOplog(opCtx.get(), engine, rs, 1);
        ASSERT(isOpHidden(id));
        txn.commit();
        ASSERT(isOpHidden(id));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        StorageWriteTransaction txn(ru);
        RecordId id = oplogOrderInsertOplog(opCtx.get(), engine, rs, 2);
        ASSERT(isOpHidden(id));
        txn.commit();
        ASSERT(isOpHidden(id));
    }
}

/**
 * Test that Oplog entries inserted while there are hidden entries do not become visible until the
 * op and all earlier ops are durable.
 */
TEST(WiredTigerRecordStoreTest, OplogDurableVisibilityOutOfOrder) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());
    auto engine = static_cast<WiredTigerKVEngine*>(harnessHelper->getEngine());
    engine->getOplogManager()->stop();

    auto isOpHidden = [&engine](const RecordId& id) {
        return engine->getOplogManager()->getOplogReadTimestamp() <
            static_cast<std::uint64_t>(id.getLong());
    };

    ServiceContext::UniqueOperationContext longLivedOp(harnessHelper->newOperationContext());
    auto& longLivedRu = *shard_role_details::getRecoveryUnit(longLivedOp.get());
    StorageWriteTransaction txn(longLivedRu);
    RecordId id1 = oplogOrderInsertOplog(longLivedOp.get(), engine, rs, 1);
    ASSERT(isOpHidden(id1));


    RecordId id2;
    {
        auto innerClient = harnessHelper->serviceContext()->getService()->makeClient("inner");
        ServiceContext::UniqueOperationContext opCtx(
            harnessHelper->newOperationContext(innerClient.get()));
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        StorageWriteTransaction txn(ru);
        id2 = oplogOrderInsertOplog(opCtx.get(), engine, rs, 2);
        ASSERT(isOpHidden(id2));
        txn.commit();
    }

    ASSERT(isOpHidden(id1));
    ASSERT(isOpHidden(id2));

    txn.commit();

    ASSERT(isOpHidden(id1));
    ASSERT(isOpHidden(id2));

    bool isReplSet = false;
    engine->getOplogManager()->start(longLivedOp.get(), *engine, *rs, isReplSet);
    engine->waitForAllEarlierOplogWritesToBeVisible(longLivedOp.get(), rs.get());

    ASSERT_FALSE(isOpHidden(id1));
    ASSERT_FALSE(isOpHidden(id2));
}

TEST(WiredTigerRecordStoreTest, AppendCustomStatsMetadata) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore("a.b"));

    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    BSONObjBuilder builder;
    rs->appendAllCustomStats(ru, &builder, 1.0);
    BSONObj customStats = builder.obj();

    BSONElement wiredTigerElement = customStats.getField(kWiredTigerEngineName);
    ASSERT_TRUE(wiredTigerElement.isABSONObj());
    BSONObj wiredTiger = wiredTigerElement.Obj();

    BSONElement metadataElement = wiredTiger.getField("metadata");
    ASSERT_TRUE(metadataElement.isABSONObj());
    BSONObj metadata = metadataElement.Obj();

    BSONElement versionElement = metadata.getField("formatVersion");
    ASSERT_TRUE(versionElement.isNumber());

    BSONElement creationStringElement = wiredTiger.getField("creationString");
    ASSERT_EQUALS(creationStringElement.type(), BSONType::string);
}

TEST(WiredTigerRecordStoreTest, AppendCustomNumericStats) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore("a.c"));

    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    BSONObjBuilder builder;
    rs->appendNumericCustomStats(ru, &builder, 1.0);
    BSONObj customStats = builder.obj();

    BSONElement wiredTigerElement = customStats.getField(kWiredTigerEngineName);
    ASSERT_TRUE(wiredTigerElement.isABSONObj());
    BSONObj wiredTiger = wiredTigerElement.Obj();

    ASSERT_FALSE(wiredTiger.hasField("metadata"));
    ASSERT_FALSE(wiredTiger.hasField("creationString"));

    BSONElement cacheElement = wiredTiger.getField("cache");
    ASSERT_TRUE(cacheElement.isABSONObj());
    BSONObj cache = cacheElement.Obj();

    BSONElement bytesElement = cache.getField("bytes currently in the cache");
    ASSERT_TRUE(bytesElement.isNumber());
}

BSONObj makeBSONObjWithSize(const Timestamp& opTime, int size, char fill = 'x') {
    BSONObj objTemplate = BSON("ts" << opTime << "str"
                                    << "");
    ASSERT_LTE(objTemplate.objsize(), size);
    std::string str(size - objTemplate.objsize(), fill);

    BSONObj obj = BSON("ts" << opTime << "str" << str);
    ASSERT_EQ(size, obj.objsize());

    return obj;
}

StatusWith<RecordId> insertBSONWithSize(
    OperationContext* opCtx, KVEngine* engine, RecordStore* rs, const Timestamp& opTime, int size) {
    BSONObj obj = makeBSONObjWithSize(opTime, size);

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    StorageWriteTransaction txn(ru);
    WiredTigerRecordStore* wtRS = checked_cast<WiredTigerRecordStore*>(rs);
    invariant(wtRS);
    Status status = engine->oplogDiskLocRegister(
        *shard_role_details::getRecoveryUnit(opCtx), rs, opTime, false);
    if (!status.isOK()) {
        return StatusWith<RecordId>(status);
    }
    StatusWith<RecordId> res = rs->insertRecord(
        opCtx, *shard_role_details::getRecoveryUnit(opCtx), obj.objdata(), obj.objsize(), opTime);
    if (res.isOK()) {
        txn.commit();
    }
    return res;
}

void testTruncateRange(int64_t numRecordsToInsert,
                       int64_t deletionPosBegin,
                       int64_t deletionPosEnd) {
    auto harnessHelper = newRecordStoreHarnessHelper(RecordStoreHarnessHelper::Options::Standalone);
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());
    auto engine = harnessHelper->getEngine();

    auto wtRS = checked_cast<WiredTigerRecordStore*>(rs.get());

    std::vector<RecordId> recordIds;

    auto opCtx = harnessHelper->newOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    for (int i = 0; i < numRecordsToInsert; i++) {
        auto recordId = insertBSONWithSize(opCtx.get(), engine, wtRS, Timestamp(1, i), 100);
        ASSERT_OK(recordId);
        recordIds.emplace_back(std::move(recordId.getValue()));
    }

    auto sizePerRecord = wtRS->dataSize() / wtRS->numRecords();

    std::sort(recordIds.begin(), recordIds.end());

    const auto& beginId = recordIds[deletionPosBegin];
    const auto& endId = recordIds[deletionPosEnd];
    {
        StorageWriteTransaction txn(ru);

        auto numRecordsDeleted = deletionPosEnd - deletionPosBegin + 1;

        ASSERT_OK(wtRS->rangeTruncate(opCtx.get(),
                                      *shard_role_details::getRecoveryUnit(opCtx.get()),
                                      beginId,
                                      endId,
                                      -(sizePerRecord * numRecordsDeleted),
                                      -numRecordsDeleted));

        ASSERT_EQ(wtRS->dataSize(), sizePerRecord * (numRecordsToInsert - numRecordsDeleted));
        ASSERT_EQ(wtRS->numRecords(), (numRecordsToInsert - numRecordsDeleted));

        txn.commit();
    }
    std::set<RecordId> expectedRemainingRecordIds;
    std::copy(recordIds.begin(),
              recordIds.begin() + deletionPosBegin,
              std::inserter(expectedRemainingRecordIds, expectedRemainingRecordIds.end()));
    std::copy(recordIds.begin() + deletionPosEnd + 1,
              recordIds.end(),
              std::inserter(expectedRemainingRecordIds, expectedRemainingRecordIds.end()));

    std::set<RecordId> actualRemainingRecordIds;

    auto cursor = wtRS->getCursor(opCtx.get(), ru, true);
    while (auto record = cursor->next()) {
        actualRemainingRecordIds.emplace(record->id);
    }
    ASSERT_EQ(expectedRemainingRecordIds, actualRemainingRecordIds);
}
TEST(WiredTigerRecordStoreTest, RangeTruncateTest) {
    testTruncateRange(100, 3, 50);
}
TEST(WiredTigerRecordStoreTest, RangeTruncateSameValueTest) {
    testTruncateRange(100, 3, 3);
}
DEATH_TEST(WiredTigerRecordStoreTest,
           RangeTruncateIncorrectOrderTest,
           "Start position cannot be after end position") {
    testTruncateRange(100, 4, 3);
}

TEST(WiredTigerRecordStoreTest, GetLatestOplogTest) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());
    auto engine = harnessHelper->getEngine();

    auto wtRS = checked_cast<WiredTigerRecordStore::Oplog*>(rs.get());

    // 1) Initialize the top of oplog to "1".
    ServiceContext::UniqueOperationContext op1(harnessHelper->newOperationContext());
    auto& ru1 = *shard_role_details::getRecoveryUnit(op1.get());

    Timestamp tsOne = [&] {
        StorageWriteTransaction op1Txn(ru1);
        Timestamp tsOne = Timestamp(static_cast<unsigned long long>(
            oplogOrderInsertOplog(op1.get(), engine, rs, 1).getLong()));
        op1Txn.commit();
        return tsOne;
    }();
    // Asserting on a recovery unit without a snapshot.
    ASSERT_EQ(tsOne, wtRS->getLatestTimestamp(ru1));

    // 2) Open a hole at time "2".
    boost::optional<StorageWriteTransaction> op1txn(ru1);
    // Don't save the return value because the compiler complains about unused variables.
    oplogOrderInsertOplog(op1.get(), engine, rs, 2);

    // Store the client with an uncommitted transaction. Create a new, concurrent client.
    auto client1 = Client::releaseCurrent();
    Client::initThread("client2", getGlobalServiceContext()->getService());

    ServiceContext::UniqueOperationContext op2(harnessHelper->newOperationContext());
    auto& ru2 = *shard_role_details::getRecoveryUnit(op2.get());
    // Should not see uncommitted write from op1.
    ASSERT_EQ(tsOne, wtRS->getLatestTimestamp(ru2));

    Timestamp tsThree = [&] {
        StorageWriteTransaction op2Txn(ru2);
        Timestamp tsThree = Timestamp(static_cast<unsigned long long>(
            oplogOrderInsertOplog(op2.get(), engine, rs, 3).getLong()));
        op2Txn.commit();
        return tsThree;
    }();
    // After committing, three is the top of oplog.
    ASSERT_EQ(tsThree, wtRS->getLatestTimestamp(ru2));

    // Switch to client 1.
    op2.reset();
    auto client2 = Client::releaseCurrent();
    Client::setCurrent(std::move(client1));

    op1txn->commit();
    // Committing the write at timestamp "2" does not change the top of oplog result. A new query
    // with client 1 will see timestamp "3".
    ASSERT_EQ(tsThree, wtRS->getLatestTimestamp(ru1));
}

TEST(WiredTigerRecordStoreTest, CursorInActiveTxnAfterNext) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    RecordId rid1;
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        StorageWriteTransaction txn(ru);
        StatusWith<RecordId> res = rs->insertRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), "a", 2, Timestamp());
        ASSERT_OK(res.getStatus());
        rid1 = res.getValue();

        res = rs->insertRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), "b", 2, Timestamp());
        ASSERT_OK(res.getStatus());

        txn.commit();
    }

    // Cursors should always ensure they are in an active transaction when next() is called.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        auto& ru = *WiredTigerRecoveryUnit::get(shard_role_details::getRecoveryUnit(opCtx.get()));

        auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        ASSERT(cursor->next());
        ASSERT_TRUE(ru.isActive());

        // Committing a StorageWriteTransaction will end the current transaction.
        StorageWriteTransaction txn(ru);
        ASSERT_TRUE(ru.isActive());
        txn.commit();
        ASSERT_FALSE(ru.isActive());

        // If a cursor is used after a WUOW commits, it should implicitly start a new transaction.
        ASSERT(cursor->next());
        ASSERT_TRUE(ru.isActive());
    }
}

TEST(WiredTigerRecordStoreTest, CursorInActiveTxnAfterSeek) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    RecordId rid1;
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        StorageWriteTransaction txn(ru);
        StatusWith<RecordId> res = rs->insertRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), "a", 2, Timestamp());
        ASSERT_OK(res.getStatus());
        rid1 = res.getValue();

        res = rs->insertRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), "b", 2, Timestamp());
        ASSERT_OK(res.getStatus());

        txn.commit();
    }

    // Cursors should always ensure they are in an active transaction when seekExact() or seek() is
    // called.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        auto& ru = *WiredTigerRecoveryUnit::get(shard_role_details::getRecoveryUnit(opCtx.get()));

        auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        ASSERT(cursor->seekExact(rid1));
        ASSERT_TRUE(ru.isActive());

        // Committing a StorageWriteTransaction will end the current transaction.
        StorageWriteTransaction txn(ru);
        ASSERT_TRUE(ru.isActive());
        txn.commit();
        ASSERT_FALSE(ru.isActive());

        // If a cursor is used after a WUOW commits, it should implicitly start a new transaction.
        ASSERT(cursor->seekExact(rid1));
        ASSERT_TRUE(ru.isActive());

        // End the transaction and test seek()
        {
            StorageWriteTransaction txn(ru);
            txn.commit();
        }
        ASSERT_FALSE(ru.isActive());
        ASSERT(cursor->seek(rid1, SeekableRecordCursor::BoundInclusion::kInclude));
        ASSERT_TRUE(ru.isActive());
    }
}

TEST(WiredTigerRecordStoreTest, CreateOnExistingIdentFails) {
    const std::unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    Lock::GlobalLock globalLock(opCtx.get(), MODE_X);

    const std::string ns = "testRecordStore";
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(ns);
    const std::string uri = WiredTigerUtil::kTableUriPrefix + ns;
    bool isReplSet = false;
    bool shouldRecoverFromOplogAsStandalone =
        repl::ReplSettings::shouldRecoverFromOplogAsStandalone();
    WiredTigerRecordStore::WiredTigerTableConfig wtTableConfig;
    auto& provider = rss::ReplicatedStorageService::get(opCtx.get()).getPersistenceProvider();
    wtTableConfig.logEnabled = WiredTigerUtil::useTableLogging(
        provider, nss, isReplSet, shouldRecoverFromOplogAsStandalone);
    const std::string config = WiredTigerRecordStore::generateCreateString(
        NamespaceStringUtil::serializeForCatalog(nss), wtTableConfig);
    {
        WriteUnitOfWork uow(opCtx.get());
        WiredTigerRecoveryUnit* ru =
            checked_cast<WiredTigerRecoveryUnit*>(shard_role_details::getRecoveryUnit(opCtx.get()));
        WiredTigerSession* s = ru->getSession();
        invariantWTOK(s->create(uri.c_str(), config.c_str()), *s);
        uow.commit();
    }

    {
        WriteUnitOfWork uow(opCtx.get());
        WiredTigerRecoveryUnit* ru =
            checked_cast<WiredTigerRecoveryUnit*>(shard_role_details::getRecoveryUnit(opCtx.get()));
        WiredTigerSession* s = ru->getSession();
        const auto ret = s->create(uri.c_str(), config.c_str());
        ASSERT_EQ(EEXIST, ret);
        const auto status = wtRCToStatus(ret, *s);
        ASSERT_NOT_OK(status);
        uow.commit();
    }
}

// Verify clustered record stores.
// This test case complements StorageEngineTest:TemporaryRecordStoreClustered which verifies
// clustered temporary record stores.
TEST(WiredTigerRecordStoreTest, ClusteredRecordStore) {
    const std::unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    const std::string ns = "testRecordStore";
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(ns);
    const std::string uri = WiredTigerUtil::kTableUriPrefix + ns;
    WiredTigerRecordStore::WiredTigerTableConfig wtTableConfig;
    wtTableConfig.keyFormat = KeyFormat::String;
    wtTableConfig.blockCompressor = wiredTigerGlobalOptions.collectionBlockCompressor;
    bool isReplSet = false;
    bool shouldRecoverFromOplogAsStandalone = false;
    auto& provider = rss::ReplicatedStorageService::get(opCtx.get()).getPersistenceProvider();
    wtTableConfig.logEnabled = WiredTigerUtil::useTableLogging(
        provider, nss, isReplSet, shouldRecoverFromOplogAsStandalone);
    const std::string config = WiredTigerRecordStore::generateCreateString(
        NamespaceStringUtil::serializeForCatalog(nss), wtTableConfig);
    {
        StorageWriteTransaction txn(ru);
        WiredTigerRecoveryUnit* ru =
            checked_cast<WiredTigerRecoveryUnit*>(shard_role_details::getRecoveryUnit(opCtx.get()));
        WiredTigerSession* s = ru->getSession();
        invariantWTOK(s->create(uri.c_str(), config.c_str()), *s);
        txn.commit();
    }

    WiredTigerRecordStore::Params params;
    params.uuid = boost::none;
    params.ident = ns;
    params.engineName = std::string{kWiredTigerEngineName};
    params.keyFormat = KeyFormat::String;
    params.overwrite = false;
    params.isLogged = WiredTigerUtil::useTableLogging(
        provider, nss, isReplSet, shouldRecoverFromOplogAsStandalone);
    params.forceUpdateWithFullDocument = false;
    params.inMemory = false;
    params.sizeStorer = nullptr;
    params.tracksSizeAdjustments = true;

    const auto wtKvEngine = static_cast<WiredTigerKVEngine*>(harnessHelper->getEngine());
    auto rs = std::make_unique<WiredTigerRecordStore>(
        wtKvEngine,
        WiredTigerRecoveryUnit::get(*shard_role_details::getRecoveryUnit(opCtx.get())),
        params);

    const auto id = StringData{"1"};
    const auto rid = RecordId(id);
    const auto data = "data";
    {
        StorageWriteTransaction txn(ru);
        StatusWith<RecordId> s = rs->insertRecord(opCtx.get(),
                                                  *shard_role_details::getRecoveryUnit(opCtx.get()),
                                                  rid,
                                                  data,
                                                  strlen(data),
                                                  Timestamp());
        ASSERT_TRUE(s.isOK());
        ASSERT_EQUALS(1, rs->numRecords());
        txn.commit();
    }
    // Read the record back.
    RecordData rd;
    ASSERT_TRUE(
        rs->findRecord(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), rid, &rd));
    ASSERT_EQ(0, memcmp(data, rd.data(), strlen(data)));
    // Update the record.
    const auto dataUpdated = "updated";
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(rs->updateRecord(opCtx.get(),
                                   *shard_role_details::getRecoveryUnit(opCtx.get()),
                                   rid,
                                   dataUpdated,
                                   strlen(dataUpdated)));
        ASSERT_EQUALS(1, rs->numRecords());
        txn.commit();
    }
    ASSERT_TRUE(
        rs->findRecord(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), rid, &rd));
    ASSERT_EQ(0, memcmp(dataUpdated, rd.data(), strlen(dataUpdated)));
}

// Make sure numRecords and dataSize are accurate after a delete rolls back and some other
// transaction deletes the same rows before we have a chance of patching up the metadata.
TEST(WiredTigerRecordStoreTest, SizeInfoAccurateAfterRollbackWithDelete) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    RecordId rid;  // This record will be deleted by two transactions.

    ServiceContext::UniqueOperationContext ctx(harnessHelper->newOperationContext());
    auto& ru = *shard_role_details::getRecoveryUnit(ctx.get());

    {
        StorageWriteTransaction txn(ru);
        rid = rs->insertRecord(
                    ctx.get(), *shard_role_details::getRecoveryUnit(ctx.get()), "a", 2, Timestamp())
                  .getValue();
        txn.commit();
    }

    ASSERT_EQ(1, rs->numRecords());
    ASSERT_EQ(2, rs->dataSize());

    StorageWriteTransaction txn(ru);

    auto aborted = std::make_shared<unittest::Barrier>(2);
    auto deleted = std::make_shared<unittest::Barrier>(2);

    // This thread will delete the record and then rollback. We'll block the roll back process after
    // rolling back the WT transaction and before running the rest of the registered changes,
    // allowing the main thread to delete the same rows again.
    stdx::thread abortedThread([&harnessHelper, &rs, &rid, aborted, deleted]() {
        auto client = harnessHelper->serviceContext()->getService()->makeClient("c1");
        auto ctx = harnessHelper->newOperationContext(client.get());
        auto& ru = *shard_role_details::getRecoveryUnit(ctx.get());
        StorageWriteTransaction txn(ru);
        // Registered changes are executed in reverse order.
        rs->deleteRecord(ctx.get(), ru, rid);
        ru.onRollback([&](OperationContext*) { deleted->countDownAndWait(); });
        ru.onRollback([&](OperationContext*) { aborted->countDownAndWait(); });
    });

    // Wait for the other thread to abort.
    aborted->countDownAndWait();

    rs->deleteRecord(ctx.get(), ru, rid);

    // Notify the other thread we have deleted, let it complete the rollback.
    deleted->countDownAndWait();

    txn.commit();

    abortedThread.join();
    ASSERT_EQ(0, rs->numRecords());
    ASSERT_EQ(0, rs->dataSize());
}

// Makes sure that when records are inserted with provided recordIds, the
// WiredTigerRecordStore correctly keeps track of the largest recordId it
// has seen in the in-memory variable.
// TODO (SERVER-88375): Revise / delete this test after the recordId tracking
// problem has been properly solved.
TEST(WiredTigerRecordStoreTest, LargestRecordIdSeenIsCorrectWhenGivenRecordIds) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    std::vector<RecordId> reservedRids;
    RecordId rid;

    ServiceContext::UniqueOperationContext ctx(harnessHelper->newOperationContext());
    auto& ru = *shard_role_details::getRecoveryUnit(ctx.get());

    {
        // Insert a single record with recordId 7.
        StorageWriteTransaction txn(ru);
        rid = rs->insertRecord(ctx.get(),
                               *shard_role_details::getRecoveryUnit(ctx.get()),
                               RecordId(7),
                               "a",
                               2,
                               Timestamp())
                  .getValue();
        txn.commit();
    }

    // The next recordId reserved is higher than 7.
    rs->reserveRecordIds(
        ctx.get(), *shard_role_details::getRecoveryUnit(ctx.get()), &reservedRids, 1);
    ASSERT_GT(reservedRids[0].getLong(), RecordId(7).getLong());
    ASSERT_EQ(1, rs->numRecords());

    // Insert a few records at once, where the recordIds are not in order. And ensure that
    // we still reserve recordIds from the right point afterwards.
    std::vector<Record> recordsToInsert;
    std::vector<Timestamp> timestamps{Timestamp(), Timestamp()};
    recordsToInsert.push_back(Record{RecordId(14), RecordData()});
    recordsToInsert.push_back(Record{RecordId(13), RecordData()});
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(rs->insertRecords(ctx.get(),
                                    *shard_role_details::getRecoveryUnit(ctx.get()),
                                    &recordsToInsert,
                                    timestamps));
        txn.commit();
    }

    // The next recordId reserved is higher than 14.
    reservedRids.clear();
    rs->reserveRecordIds(
        ctx.get(), *shard_role_details::getRecoveryUnit(ctx.get()), &reservedRids, 1);
    ASSERT_GT(reservedRids[0].getLong(), RecordId(14).getLong());
    ASSERT_EQ(3, rs->numRecords());

    // Insert a few records at once, where the recordIds are in order. And ensure that
    // we still reserve recordIds from the right point afterwards.
    recordsToInsert.clear();
    recordsToInsert.push_back(Record{RecordId(19), RecordData()});
    recordsToInsert.push_back(Record{RecordId(20), RecordData()});
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(rs->insertRecords(ctx.get(),
                                    *shard_role_details::getRecoveryUnit(ctx.get()),
                                    &recordsToInsert,
                                    timestamps));
        txn.commit();
    }

    // The next recordId reserved is higher than 20.
    reservedRids.clear();
    rs->reserveRecordIds(
        ctx.get(), *shard_role_details::getRecoveryUnit(ctx.get()), &reservedRids, 1);
    ASSERT_GT(reservedRids[0].getLong(), RecordId(20).getLong());
    ASSERT_EQ(5, rs->numRecords());
}

// Test WiredTiger fails to create a table, with the configuration string generated by
// WiredTigerRecordStore::generateCreateString(), if a table already exists with the same ident and
// same table configuration.
TEST(WiredTigerRecordStoreTest, EnforceTableCreateExclusiveSameConfiguration) {
    const std::unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("testRecordStore");
    WiredTigerRecordStore::WiredTigerTableConfig wtTableConfig;
    wtTableConfig.blockCompressor = wiredTigerGlobalOptions.collectionBlockCompressor;
    const auto config =
        WiredTigerRecordStore::generateCreateString(nss.toString_forTest(), wtTableConfig);
    const std::string ident = "uniqueIdentifierForTableFile";
    const std::string uri = WiredTigerUtil::kTableUriPrefix + ident;

    // First creation of table with the ident succeeds.
    WiredTigerRecoveryUnit* ru =
        checked_cast<WiredTigerRecoveryUnit*>(shard_role_details::getRecoveryUnit(opCtx.get()));
    WiredTigerSession* s = ru->getSession();
    invariantWTOK(s->create(uri.c_str(), config.c_str()), *s);

    // Fail when trying to create the table that already exists.
    const auto createRes = s->create(uri.c_str(), config.c_str());
    ASSERT_EQ(EEXIST, createRes);
}

// Test WiredTiger fails to create a table, with the configuration string generated by
// WiredTigerRecordStore::generateCreateString(), if a table already exists with the same ident and
// different table configurations.
TEST(WiredTigerRecordStoreTest, EnforceTableCreateExclusiveDifferentConfiguration) {
    const std::unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("testRecordStore");
    WiredTigerRecordStore::WiredTigerTableConfig wtTableConfig;
    wtTableConfig.blockCompressor = wiredTigerGlobalOptions.collectionBlockCompressor;
    const std::string config =
        WiredTigerRecordStore::generateCreateString(nss.toString_forTest(), wtTableConfig);
    const std::string ident = "uniqueIdentifierForTableFile";
    const std::string uri = WiredTigerUtil::kTableUriPrefix + ident;

    // First creation of a table with the ident succeeds.
    WiredTigerRecoveryUnit* ru =
        checked_cast<WiredTigerRecoveryUnit*>(shard_role_details::getRecoveryUnit(opCtx.get()));
    WiredTigerSession* s = ru->getSession();
    invariantWTOK(s->create(uri.c_str(), config.c_str()), *s);

    // Generate a different table configuration than the original.
    WiredTigerRecordStore::WiredTigerTableConfig newWtTableConfig = wtTableConfig;
    newWtTableConfig.keyFormat = KeyFormat::String;
    const std::string newConfig =
        WiredTigerRecordStore::generateCreateString(nss.toString_forTest(), newWtTableConfig);
    ASSERT_NE(newConfig, config);

    // The uri for the ident is occupied, fail to create a new table with the ident.
    const auto createRes = s->create(uri.c_str(), newConfig.c_str());
    ASSERT_EQ(EEXIST, createRes);
}

}  // namespace
}  // namespace mongo
