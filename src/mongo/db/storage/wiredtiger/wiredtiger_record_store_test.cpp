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
#include <sstream>
#include <string>
#include <time.h>

#include "mongo/base/checked_cast.h"
#include "mongo/base/init.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_oplog_stones.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using std::string;
using std::stringstream;
using std::unique_ptr;

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
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    RecordId id1;
    RecordId id2;

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());

            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp());
            ASSERT_OK(res.getStatus());
            id1 = res.getValue();

            res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp());
            ASSERT_OK(res.getStatus());
            id2 = res.getValue();

            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext t1(harnessHelper->newOperationContext());
        auto client2 = harnessHelper->serviceContext()->makeClient("c2");
        auto t2 = harnessHelper->newOperationContext(client2.get());

        unique_ptr<WriteUnitOfWork> w1(new WriteUnitOfWork(t1.get()));
        unique_ptr<WriteUnitOfWork> w2(new WriteUnitOfWork(t2.get()));

        rs->dataFor(t1.get(), id1);
        rs->dataFor(t2.get(), id1);

        ASSERT_OK(rs->updateRecord(t1.get(), id1, "b", 2));
        ASSERT_OK(rs->updateRecord(t1.get(), id2, "B", 2));

        try {
            // this should fail
            rs->updateRecord(t2.get(), id1, "c", 2).transitional_ignore();
            ASSERT(0);
        } catch (WriteConflictException&) {
            w2.reset(nullptr);
            t2.reset(nullptr);
        }

        w1->commit();  // this should succeed
    }
}

TEST(WiredTigerRecordStoreTest, Isolation2) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    RecordId id1;
    RecordId id2;

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());

            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp());
            ASSERT_OK(res.getStatus());
            id1 = res.getValue();

            res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp());
            ASSERT_OK(res.getStatus());
            id2 = res.getValue();

            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext t1(harnessHelper->newOperationContext());
        auto client2 = harnessHelper->serviceContext()->makeClient("c2");
        auto t2 = harnessHelper->newOperationContext(client2.get());

        // ensure we start transactions
        rs->dataFor(t1.get(), id2);
        rs->dataFor(t2.get(), id2);

        {
            WriteUnitOfWork w(t1.get());
            ASSERT_OK(rs->updateRecord(t1.get(), id1, "b", 2));
            w.commit();
        }

        {
            WriteUnitOfWork w(t2.get());
            ASSERT_EQUALS(string("a"), rs->dataFor(t2.get(), id1).data());
            try {
                // this should fail as our version of id1 is too old
                rs->updateRecord(t2.get(), id1, "c", 2).transitional_ignore();
                ASSERT(0);
            } catch (WriteConflictException&) {
            }
        }
    }
}

StatusWith<RecordId> insertBSON(ServiceContext::UniqueOperationContext& opCtx,
                                unique_ptr<RecordStore>& rs,
                                const Timestamp& opTime) {
    BSONObj obj = BSON("ts" << opTime);
    WriteUnitOfWork wuow(opCtx.get());
    WiredTigerRecordStore* wrs = checked_cast<WiredTigerRecordStore*>(rs.get());
    invariant(wrs);
    Status status = wrs->oplogDiskLocRegister(opCtx.get(), opTime, false);
    if (!status.isOK())
        return StatusWith<RecordId>(status);
    StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), obj.objdata(), obj.objsize(), opTime);
    if (res.isOK())
        wuow.commit();
    return res;
}

RecordId _oplogOrderInsertOplog(OperationContext* opCtx,
                                const unique_ptr<RecordStore>& rs,
                                int inc) {
    Timestamp opTime = Timestamp(5, inc);
    Status status = rs->oplogDiskLocRegister(opCtx, opTime, false);
    ASSERT_OK(status);
    BSONObj obj = BSON("ts" << opTime);
    StatusWith<RecordId> res = rs->insertRecord(opCtx, obj.objdata(), obj.objsize(), opTime);
    ASSERT_OK(res.getStatus());
    return res.getValue();
}

// Test that even when the oplog durability loop is paused, we can still advance the commit point as
// long as the commit for each insert comes before the next insert starts.
TEST(WiredTigerRecordStoreTest, OplogDurableVisibilityInOrder) {
    ON_BLOCK_EXIT([] { WTPauseOplogVisibilityUpdateLoop.setMode(FailPoint::off); });
    WTPauseOplogVisibilityUpdateLoop.setMode(FailPoint::alwaysOn);

    unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());
    auto wtrs = checked_cast<WiredTigerRecordStore*>(rs.get());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        WriteUnitOfWork uow(opCtx.get());
        RecordId id = _oplogOrderInsertOplog(opCtx.get(), rs, 1);
        ASSERT(wtrs->isOpHidden_forTest(id));
        uow.commit();
        ASSERT(wtrs->isOpHidden_forTest(id));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        WriteUnitOfWork uow(opCtx.get());
        RecordId id = _oplogOrderInsertOplog(opCtx.get(), rs, 2);
        ASSERT(wtrs->isOpHidden_forTest(id));
        uow.commit();
        ASSERT(wtrs->isOpHidden_forTest(id));
    }
}

// Test that Oplog entries inserted while there are hidden entries do not become visible until the
// op and all earlier ops are durable.
TEST(WiredTigerRecordStoreTest, OplogDurableVisibilityOutOfOrder) {
    ON_BLOCK_EXIT([] { WTPauseOplogVisibilityUpdateLoop.setMode(FailPoint::off); });
    WTPauseOplogVisibilityUpdateLoop.setMode(FailPoint::alwaysOn);

    unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());

    auto wtrs = checked_cast<WiredTigerRecordStore*>(rs.get());

    ServiceContext::UniqueOperationContext longLivedOp(harnessHelper->newOperationContext());
    WriteUnitOfWork uow(longLivedOp.get());
    RecordId id1 = _oplogOrderInsertOplog(longLivedOp.get(), rs, 1);
    ASSERT(wtrs->isOpHidden_forTest(id1));


    RecordId id2;
    {
        auto innerClient = harnessHelper->serviceContext()->makeClient("inner");
        ServiceContext::UniqueOperationContext opCtx(
            harnessHelper->newOperationContext(innerClient.get()));
        WriteUnitOfWork uow(opCtx.get());
        id2 = _oplogOrderInsertOplog(opCtx.get(), rs, 2);
        ASSERT(wtrs->isOpHidden_forTest(id2));
        uow.commit();
    }

    ASSERT(wtrs->isOpHidden_forTest(id1));
    ASSERT(wtrs->isOpHidden_forTest(id2));

    uow.commit();

    ASSERT(wtrs->isOpHidden_forTest(id1));
    ASSERT(wtrs->isOpHidden_forTest(id2));

    // Wait a bit and check again to make sure they don't become visible automatically.
    sleepsecs(1);
    ASSERT(wtrs->isOpHidden_forTest(id1));
    ASSERT(wtrs->isOpHidden_forTest(id2));

    WTPauseOplogVisibilityUpdateLoop.setMode(FailPoint::off);

    rs->waitForAllEarlierOplogWritesToBeVisible(longLivedOp.get());

    ASSERT(!wtrs->isOpHidden_forTest(id1));
    ASSERT(!wtrs->isOpHidden_forTest(id2));
}

TEST(WiredTigerRecordStoreTest, AppendCustomStatsMetadata) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore("a.b"));

    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    BSONObjBuilder builder;
    rs->appendAllCustomStats(opCtx.get(), &builder, 1.0);
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
    ASSERT_EQUALS(creationStringElement.type(), String);
}

TEST(WiredTigerRecordStoreTest, AppendCustomNumericStats) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore("a.c"));

    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    BSONObjBuilder builder;
    rs->appendNumericCustomStats(opCtx.get(), &builder, 1.0);
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

StatusWith<RecordId> insertBSONWithSize(OperationContext* opCtx,
                                        RecordStore* rs,
                                        const Timestamp& opTime,
                                        int size) {
    BSONObj obj = makeBSONObjWithSize(opTime, size);

    WriteUnitOfWork wuow(opCtx);
    WiredTigerRecordStore* wtrs = checked_cast<WiredTigerRecordStore*>(rs);
    invariant(wtrs);
    Status status = wtrs->oplogDiskLocRegister(opCtx, opTime, false);
    if (!status.isOK()) {
        return StatusWith<RecordId>(status);
    }
    StatusWith<RecordId> res = rs->insertRecord(opCtx, obj.objdata(), obj.objsize(), opTime);
    if (res.isOK()) {
        wuow.commit();
    }
    return res;
}

// Insert records into an oplog and verify the number of stones that are created.
TEST(WiredTigerRecordStoreTest, OplogStones_CreateNewStone) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    oplogStones->setMinBytesPerStone(100);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(0U, oplogStones->numStones());

        // Inserting a record smaller than 'minBytesPerStone' shouldn't create a new oplog stone.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 99), RecordId(1, 1));
        ASSERT_EQ(0U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(99, oplogStones->currentBytes());

        // Inserting another record such that their combined size exceeds 'minBytesPerStone' should
        // cause a new stone to be created.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 2), 51), RecordId(1, 2));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());

        // Inserting a record such that the combined size of this record and the previously inserted
        // one exceed 'minBytesPerStone' shouldn't cause a new stone to be created because we've
        // started filling a new stone.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 3), 50), RecordId(1, 3));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());

        // Inserting a record such that the combined size of this record and the previously inserted
        // one is exactly equal to 'minBytesPerStone' should cause a new stone to be created.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 4), 50), RecordId(1, 4));
        ASSERT_EQ(2U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());

        // Inserting a single record that exceeds 'minBytesPerStone' should cause a new stone to
        // be created.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 5), 101), RecordId(1, 5));
        ASSERT_EQ(3U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }
}

// Insert records into an oplog and try to update them. The updates shouldn't succeed if the size of
// record is changed.
TEST(WiredTigerRecordStoreTest, OplogStones_UpdateRecord) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    oplogStones->setMinBytesPerStone(100);

    // Insert two records such that one makes up a full stone and the other is a part of the stone
    // currently being filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 100), RecordId(1, 1));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 2), 50), RecordId(1, 2));

        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }

    // Attempts to grow the records should fail.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        BSONObj changed1 = makeBSONObjWithSize(Timestamp(1, 1), 101);
        BSONObj changed2 = makeBSONObjWithSize(Timestamp(1, 2), 51);

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_NOT_OK(
            rs->updateRecord(opCtx.get(), RecordId(1, 1), changed1.objdata(), changed1.objsize()));
        ASSERT_NOT_OK(
            rs->updateRecord(opCtx.get(), RecordId(1, 2), changed2.objdata(), changed2.objsize()));
    }

    // Attempts to shrink the records should also fail.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        BSONObj changed1 = makeBSONObjWithSize(Timestamp(1, 1), 99);
        BSONObj changed2 = makeBSONObjWithSize(Timestamp(1, 2), 49);

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_NOT_OK(
            rs->updateRecord(opCtx.get(), RecordId(1, 1), changed1.objdata(), changed1.objsize()));
        ASSERT_NOT_OK(
            rs->updateRecord(opCtx.get(), RecordId(1, 2), changed2.objdata(), changed2.objsize()));
    }

    // Changing the contents of the records without changing their size should succeed.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        BSONObj changed1 = makeBSONObjWithSize(Timestamp(1, 1), 100, 'y');
        BSONObj changed2 = makeBSONObjWithSize(Timestamp(1, 2), 50, 'z');

        WriteUnitOfWork wuow(opCtx.get());
        // Explicitly sets the timestamp to ensure ordered writes.
        ASSERT_OK(opCtx->recoveryUnit()->setTimestamp(Timestamp(1, 3)));
        ASSERT_OK(
            rs->updateRecord(opCtx.get(), RecordId(1, 1), changed1.objdata(), changed1.objsize()));
        ASSERT_OK(
            rs->updateRecord(opCtx.get(), RecordId(1, 2), changed2.objdata(), changed2.objsize()));
        wuow.commit();

        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }
}

// Insert multiple records and truncate the oplog using RecordStore::truncate(). The operation
// should leave no stones, including the partially filled one.
TEST(WiredTigerRecordStoreTest, OplogStones_Truncate) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    oplogStones->setMinBytesPerStone(100);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 50), RecordId(1, 1));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 2), 50), RecordId(1, 2));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 3), 50), RecordId(1, 3));

        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(3, rs->numRecords(opCtx.get()));
        ASSERT_EQ(150, rs->dataSize(opCtx.get()));

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(rs->truncate(opCtx.get()));
        wuow.commit();

        ASSERT_EQ(0, rs->dataSize(opCtx.get()));
        ASSERT_EQ(0, rs->numRecords(opCtx.get()));
        ASSERT_EQ(0U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }
}

// Insert multiple records, truncate the oplog using RecordStore::cappedTruncateAfter(), and
// verify that the metadata for each stone is updated. If a full stone is partially truncated, then
// it should become the stone currently being filled.
TEST(WiredTigerRecordStoreTest, OplogStones_CappedTruncateAfter) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    oplogStones->setMinBytesPerStone(1000);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 400), RecordId(1, 1));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 2), 800), RecordId(1, 2));

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 3), 200), RecordId(1, 3));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 4), 250), RecordId(1, 4));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 5), 300), RecordId(1, 5));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 6), 350), RecordId(1, 6));

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 7), 50), RecordId(1, 7));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 8), 100), RecordId(1, 8));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 9), 150), RecordId(1, 9));

        ASSERT_EQ(9, rs->numRecords(opCtx.get()));
        ASSERT_EQ(2600, rs->dataSize(opCtx.get()));
        ASSERT_EQ(2U, oplogStones->numStones());
        ASSERT_EQ(3, oplogStones->currentRecords());
        ASSERT_EQ(300, oplogStones->currentBytes());
    }

    // Make sure all are visible.
    rs->waitForAllEarlierOplogWritesToBeVisible(harnessHelper->newOperationContext().get());

    // Truncate data using an inclusive RecordId that exists inside the stone currently being
    // filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        rs->cappedTruncateAfter(opCtx.get(), RecordId(1, 8), true);

        ASSERT_EQ(7, rs->numRecords(opCtx.get()));
        ASSERT_EQ(2350, rs->dataSize(opCtx.get()));
        ASSERT_EQ(2U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }

    // Truncate data using an inclusive RecordId that refers to the 'lastRecord' of a full stone.
    // The stone should become the one currently being filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        rs->cappedTruncateAfter(opCtx.get(), RecordId(1, 6), true);

        ASSERT_EQ(5, rs->numRecords(opCtx.get()));
        ASSERT_EQ(1950, rs->dataSize(opCtx.get()));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(3, oplogStones->currentRecords());
        ASSERT_EQ(750, oplogStones->currentBytes());
    }

    // Truncate data using a non-inclusive RecordId that exists inside the stone currently being
    // filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        rs->cappedTruncateAfter(opCtx.get(), RecordId(1, 3), false);

        ASSERT_EQ(3, rs->numRecords(opCtx.get()));
        ASSERT_EQ(1400, rs->dataSize(opCtx.get()));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(200, oplogStones->currentBytes());
    }

    // Truncate data using a non-inclusive RecordId that refers to the 'lastRecord' of a full stone.
    // The stone should remain intact.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        rs->cappedTruncateAfter(opCtx.get(), RecordId(1, 2), false);

        ASSERT_EQ(2, rs->numRecords(opCtx.get()));
        ASSERT_EQ(1200, rs->dataSize(opCtx.get()));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }

    // Truncate data using a non-inclusive RecordId that exists inside a full stone. The stone
    // should become the one currently being filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        rs->cappedTruncateAfter(opCtx.get(), RecordId(1, 1), false);

        ASSERT_EQ(1, rs->numRecords(opCtx.get()));
        ASSERT_EQ(400, rs->dataSize(opCtx.get()));
        ASSERT_EQ(0U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(400, oplogStones->currentBytes());
    }
}

// Verify that oplog stones are reclaimed when cappedMaxSize is exceeded.
TEST(WiredTigerRecordStoreTest, OplogStones_ReclaimStones) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    ASSERT_OK(wtrs->updateOplogSize(230));

    oplogStones->setMinBytesPerStone(100);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 100), RecordId(1, 1));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 2), 110), RecordId(1, 2));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 3), 120), RecordId(1, 3));

        ASSERT_EQ(3, rs->numRecords(opCtx.get()));
        ASSERT_EQ(330, rs->dataSize(opCtx.get()));
        ASSERT_EQ(3U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }

    // Fail to truncate stone when cappedMaxSize is exceeded, but the persisted timestamp is
    // before the truncation point (i.e: leaves a gap that replication recovery would rely on).
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        wtrs->reclaimOplog(opCtx.get(), Timestamp(1, 0));

        ASSERT_EQ(3, rs->numRecords(opCtx.get()));
        ASSERT_EQ(330, rs->dataSize(opCtx.get()));
        ASSERT_EQ(3U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }

    // Truncate a stone when cappedMaxSize is exceeded.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        wtrs->reclaimOplog(opCtx.get(), Timestamp(1, 3));

        ASSERT_EQ(2, rs->numRecords(opCtx.get()));
        ASSERT_EQ(230, rs->dataSize(opCtx.get()));
        ASSERT_EQ(2U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 4), 130), RecordId(1, 4));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 5), 140), RecordId(1, 5));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 6), 50), RecordId(1, 6));

        ASSERT_EQ(5, rs->numRecords(opCtx.get()));
        ASSERT_EQ(550, rs->dataSize(opCtx.get()));
        ASSERT_EQ(4U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }

    // Truncate multiple stones if necessary.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        wtrs->reclaimOplog(opCtx.get(), Timestamp(1, 6));

        ASSERT_EQ(2, rs->numRecords(opCtx.get()));
        ASSERT_EQ(190, rs->dataSize(opCtx.get()));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }

    // No-op if dataSize <= cappedMaxSize.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        wtrs->reclaimOplog(opCtx.get(), Timestamp(1, 6));

        ASSERT_EQ(2, rs->numRecords(opCtx.get()));
        ASSERT_EQ(190, rs->dataSize(opCtx.get()));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }

    // Don't truncate the last stone before the truncate point, even if the truncate point
    // is ahead of it.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 7), 190), RecordId(1, 7));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 9), 120), RecordId(1, 9));

        ASSERT_EQ(4, rs->numRecords(opCtx.get()));
        ASSERT_EQ(500, rs->dataSize(opCtx.get()));
        ASSERT_EQ(3U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        wtrs->reclaimOplog(opCtx.get(), Timestamp(1, 8));

        ASSERT_EQ(3, rs->numRecords(opCtx.get()));
        ASSERT_EQ(360, rs->dataSize(opCtx.get()));
        ASSERT_EQ(2U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }

    // Don't truncate entire oplog.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 10), 90), RecordId(1, 10));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 11), 210),
                  RecordId(1, 11));

        ASSERT_EQ(5, rs->numRecords(opCtx.get()));
        ASSERT_EQ(660, rs->dataSize(opCtx.get()));
        ASSERT_EQ(3U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        wtrs->reclaimOplog(opCtx.get(), Timestamp(1, 12));

        ASSERT_EQ(2, rs->numRecords(opCtx.get()));
        ASSERT_EQ(300, rs->dataSize(opCtx.get()));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }

    // OK to truncate all stones if there are records in the oplog that are before or at the
    // truncate-up-to point, that have not yet created a stone.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 12), 90), RecordId(1, 12));

        ASSERT_EQ(3, rs->numRecords(opCtx.get()));
        ASSERT_EQ(390, rs->dataSize(opCtx.get()));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(90, oplogStones->currentBytes());
    }
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        wtrs->reclaimOplog(opCtx.get(), Timestamp(1, 12));

        ASSERT_EQ(1, rs->numRecords(opCtx.get()));
        ASSERT_EQ(90, rs->dataSize(opCtx.get()));
        ASSERT_EQ(0U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(90, oplogStones->currentBytes());
    }
}

// Verify that an oplog stone isn't created if it would cause the logical representation of the
// records to not be in increasing order.
TEST(WiredTigerRecordStoreTest, OplogStones_AscendingOrder) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    oplogStones->setMinBytesPerStone(100);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(0U, oplogStones->numStones());
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(2, 2), 50), RecordId(2, 2));
        ASSERT_EQ(0U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());

        // Inserting a record that has a smaller RecordId than the previously inserted record should
        // be able to create a new stone when no stones already exist.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(2, 1), 50), RecordId(2, 1));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());

        // However, inserting a record that has a smaller RecordId than most recently created
        // stone's last record shouldn't cause a new stone to be created, even if the size of the
        // inserted record exceeds 'minBytesPerStone'.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 100), RecordId(1, 1));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(100, oplogStones->currentBytes());

        // Inserting a record that has a larger RecordId than the most recently created stone's last
        // record should then cause a new stone to be created.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(2, 3), 50), RecordId(2, 3));
        ASSERT_EQ(2U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }
}

// Ensure that if we sample and create duplicate oplog stones, perform truncation correctly, and
// with no crashing behavior. This scenario may be possible if the same record is sampled multiple
// times during startup, which can be very likely if the size storer is very inaccurate.
TEST(WiredTigerRecordStoreTest, OplogStones_Duplicates) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    auto wtHarnessHelper = dynamic_cast<WiredTigerHarnessHelper*>(harnessHelper.get());
    std::unique_ptr<RecordStore> rs(wtHarnessHelper->newOplogRecordStoreNoInit());

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());

    {
        // Before initializing the RecordStore, which also starts the oplog sampling process,
        // populate with a few records.
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 0), 100), RecordId(1, 0));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(2, 0), 100), RecordId(2, 0));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(3, 0), 100), RecordId(3, 0));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(4, 0), 100), RecordId(4, 0));
    }

    // Force the oplog visibility timestamp to be up-to-date to the last record.
    auto wtKvEngine = dynamic_cast<WiredTigerKVEngine*>(harnessHelper->getEngine());
    wtKvEngine->getOplogManager()->setOplogReadTimestamp(Timestamp(4, 0));

    {
        // Force initialize the oplog stones to use sampling by providing very large, inaccurate
        // sizes. This should cause us to oversample the records in the oplog.
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        wtrs->setNumRecords(1024 * 1024);
        wtrs->setDataSize(1024 * 1024 * 1024);
        wtrs->postConstructorInit(opCtx.get());
    }

    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    // Confirm that sampling occurred and that some stones were generated.
    ASSERT(oplogStones->processedBySampling());
    auto stonesBefore = oplogStones->numStones();
    ASSERT_GT(stonesBefore, 0U);

    {
        // Reclaiming should do nothing because the data size is still under the maximum.
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        wtrs->reclaimOplog(opCtx.get(), Timestamp(4, 0));
        ASSERT_EQ(stonesBefore, oplogStones->numStones());

        // Reduce the oplog size to ensure we create a stone and truncate on the next insert.
        ASSERT_OK(wtrs->updateOplogSize(400));

        // Inserting these records should meet the requirements for truncation. That is: there is a
        // record, 5, after the last stone, 4, and before the truncation point, 6.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(5, 0), 100), RecordId(5, 0));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(6, 0), 100), RecordId(6, 0));

        // Ensure every stone has been cleaned up except for the last one ending in 6.
        wtrs->reclaimOplog(opCtx.get(), Timestamp(6, 0));
        ASSERT_EQ(1, oplogStones->numStones());

        // The original oplog should have rolled over and the size and count should be accurate.
        ASSERT_EQ(1, wtrs->numRecords(opCtx.get()));
        ASSERT_EQ(100, wtrs->dataSize(opCtx.get()));
    }
}

TEST(WiredTigerRecordStoreTest, GetLatestOplogTest) {
    unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());

    auto wtrs = checked_cast<WiredTigerRecordStore*>(rs.get());

    // 1) Initialize the top of oplog to "1".
    ServiceContext::UniqueOperationContext op1(harnessHelper->newOperationContext());
    op1->recoveryUnit()->beginUnitOfWork(op1->readOnly());
    Timestamp tsOne = Timestamp(
        static_cast<unsigned long long>(_oplogOrderInsertOplog(op1.get(), rs, 1).getLong()));
    op1->recoveryUnit()->commitUnitOfWork();
    // Asserting on a recovery unit without a snapshot.
    ASSERT_EQ(tsOne, wtrs->getLatestOplogTimestamp(op1.get()));

    // 2) Open a hole at time "2".
    op1->recoveryUnit()->beginUnitOfWork(op1->readOnly());
    // Don't save the return value because the compiler complains about unused variables.
    _oplogOrderInsertOplog(op1.get(), rs, 2);

    // Store the client with an uncommitted transaction. Create a new, concurrent client.
    auto client1 = Client::releaseCurrent();
    Client::initThread("client2");

    ServiceContext::UniqueOperationContext op2(harnessHelper->newOperationContext());
    // Should not see uncommited write from op1.
    ASSERT_EQ(tsOne, wtrs->getLatestOplogTimestamp(op2.get()));

    op2->recoveryUnit()->beginUnitOfWork(op2->readOnly());
    Timestamp tsThree = Timestamp(
        static_cast<unsigned long long>(_oplogOrderInsertOplog(op2.get(), rs, 3).getLong()));
    op2->recoveryUnit()->commitUnitOfWork();
    // After committing, three is the top of oplog.
    ASSERT_EQ(tsThree, wtrs->getLatestOplogTimestamp(op2.get()));

    // Switch to client 1.
    op2.reset();
    auto client2 = Client::releaseCurrent();
    Client::setCurrent(std::move(client1));

    op1->recoveryUnit()->commitUnitOfWork();
    // Committing the write at timestamp "2" does not change the top of oplog result. A new query
    // with client 1 will see timestamp "3".
    ASSERT_EQ(tsThree, wtrs->getLatestOplogTimestamp(op1.get()));
}

TEST(WiredTigerRecordStoreTest, CursorInActiveTxnAfterNext) {
    unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    RecordId rid1;
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        WriteUnitOfWork uow(opCtx.get());
        StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp());
        ASSERT_OK(res.getStatus());
        rid1 = res.getValue();

        res = rs->insertRecord(opCtx.get(), "b", 2, Timestamp());
        ASSERT_OK(res.getStatus());

        uow.commit();
    }

    // Cursors should always ensure they are in an active transaction when next() is called.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto ru = WiredTigerRecoveryUnit::get(opCtx.get());

        auto cursor = rs->getCursor(opCtx.get());
        ASSERT(cursor->next());
        ASSERT_TRUE(ru->isActive());

        // Committing a WriteUnitOfWork will end the current transaction.
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_TRUE(ru->isActive());
        wuow.commit();
        ASSERT_FALSE(ru->isActive());

        // If a cursor is used after a WUOW commits, it should implicitly start a new transaction.
        ASSERT(cursor->next());
        ASSERT_TRUE(ru->isActive());
    }
}

TEST(WiredTigerRecordStoreTest, CursorInActiveTxnAfterSeek) {
    unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    RecordId rid1;
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        WriteUnitOfWork uow(opCtx.get());
        StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp());
        ASSERT_OK(res.getStatus());
        rid1 = res.getValue();

        res = rs->insertRecord(opCtx.get(), "b", 2, Timestamp());
        ASSERT_OK(res.getStatus());

        uow.commit();
    }

    // Cursors should always ensure they are in an active transaction when seekExact() is called.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto ru = WiredTigerRecoveryUnit::get(opCtx.get());

        auto cursor = rs->getCursor(opCtx.get());
        ASSERT(cursor->seekExact(rid1));
        ASSERT_TRUE(ru->isActive());

        // Committing a WriteUnitOfWork will end the current transaction.
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_TRUE(ru->isActive());
        wuow.commit();
        ASSERT_FALSE(ru->isActive());

        // If a cursor is used after a WUOW commits, it should implicitly start a new transaction.
        ASSERT(cursor->seekExact(rid1));
        ASSERT_TRUE(ru->isActive());
    }
}

// Verify clustered record stores.
// This test case complements StorageEngineTest:TemporaryRecordStoreClustered which verifies
// clustered temporary record stores.
TEST(WiredTigerRecordStoreTest, ClusteredRecordStore) {
    const unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

    ASSERT(opCtx.get());
    const std::string ns = "testRecordStore";
    const NamespaceString nss(ns);
    const std::string uri = WiredTigerKVEngine::kTableUriPrefix + ns;
    const StatusWith<std::string> result =
        WiredTigerRecordStore::generateCreateString(kWiredTigerEngineName,
                                                    nss,
                                                    "",
                                                    CollectionOptions(),
                                                    "",
                                                    KeyFormat::String,
                                                    WiredTigerUtil::useTableLogging(nss));
    ASSERT_TRUE(result.isOK());
    const std::string config = result.getValue();

    {
        WriteUnitOfWork uow(opCtx.get());
        WiredTigerRecoveryUnit* ru =
            checked_cast<WiredTigerRecoveryUnit*>(opCtx.get()->recoveryUnit());
        WT_SESSION* s = ru->getSession()->getSession();
        invariantWTOK(s->create(s, uri.c_str(), config.c_str()), s);
        uow.commit();
    }

    WiredTigerRecordStore::Params params;
    params.nss = nss;
    params.ident = ns;
    params.engineName = kWiredTigerEngineName;
    params.isCapped = false;
    params.keyFormat = KeyFormat::String;
    params.overwrite = false;
    params.isEphemeral = false;
    params.isLogged = WiredTigerUtil::useTableLogging(nss);
    params.cappedCallback = nullptr;
    params.sizeStorer = nullptr;
    params.tracksSizeAdjustments = true;
    params.forceUpdateWithFullDocument = false;

    const auto wtKvEngine = dynamic_cast<WiredTigerKVEngine*>(harnessHelper->getEngine());
    auto rs = std::make_unique<StandardWiredTigerRecordStore>(wtKvEngine, opCtx.get(), params);
    rs->postConstructorInit(opCtx.get());

    const auto id = StringData{"1"};
    const auto rid = RecordId(id.rawData(), id.size());
    const auto data = "data";
    {
        WriteUnitOfWork wuow(opCtx.get());
        StatusWith<RecordId> s =
            rs->insertRecord(opCtx.get(), rid, data, strlen(data), Timestamp());
        ASSERT_TRUE(s.isOK());
        ASSERT_EQUALS(1, rs->numRecords(opCtx.get()));
        wuow.commit();
    }
    // Read the record back.
    RecordData rd;
    ASSERT_TRUE(rs->findRecord(opCtx.get(), rid, &rd));
    ASSERT_EQ(0, memcmp(data, rd.data(), strlen(data)));
    // Update the record.
    const auto dataUpdated = "updated";
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(rs->updateRecord(opCtx.get(), rid, dataUpdated, strlen(dataUpdated)));
        ASSERT_EQUALS(1, rs->numRecords(opCtx.get()));
        wuow.commit();
    }
    ASSERT_TRUE(rs->findRecord(opCtx.get(), rid, &rd));
    ASSERT_EQ(0, memcmp(dataUpdated, rd.data(), strlen(dataUpdated)));
}

// Make sure numRecords is accurate after a delete rolls back and some other transaction deletes the
// same rows before we have a chance of patching up the metadata.
TEST(WiredTigerRecordStoreTest, NumRecordsAccurateAfterRollbackWithDelete) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    RecordId rid;  // This record will be deleted by two transactions.

    ServiceContext::UniqueOperationContext ctx(harnessHelper->newOperationContext());
    {
        WriteUnitOfWork uow(ctx.get());
        rid = rs->insertRecord(ctx.get(), "a", 2, Timestamp()).getValue();
        uow.commit();
    }

    ASSERT_EQ(1, rs->numRecords(ctx.get()));

    WriteUnitOfWork uow(ctx.get());

    auto aborted = std::make_shared<unittest::Barrier>(2);
    auto deleted = std::make_shared<unittest::Barrier>(2);

    // This thread will delete the record and then rollback. We'll block the roll back process after
    // rolling back the WT transaction and before running the rest of the registered changes,
    // allowing the main thread to delete the same rows again.
    stdx::thread abortedThread([&harnessHelper, &rs, &rid, aborted, deleted]() {
        auto client = harnessHelper->serviceContext()->makeClient("c1");
        auto ctx = harnessHelper->newOperationContext(client.get());
        WriteUnitOfWork txn(ctx.get());
        // Registered changes are executed in reverse order.
        rs->deleteRecord(ctx.get(), rid);
        ctx.get()->recoveryUnit()->onRollback([&]() { deleted->countDownAndWait(); });
        ctx.get()->recoveryUnit()->onRollback([&]() { aborted->countDownAndWait(); });
    });

    // Wait for the other thread to abort.
    aborted->countDownAndWait();

    rs->deleteRecord(ctx.get(), rid);

    // Notify the other thread we have deleted, let it complete the rollback.
    deleted->countDownAndWait();

    uow.commit();

    abortedThread.join();
    ASSERT_EQ(0, rs->numRecords(ctx.get()));
}

}  // namespace
}  // namespace mongo
