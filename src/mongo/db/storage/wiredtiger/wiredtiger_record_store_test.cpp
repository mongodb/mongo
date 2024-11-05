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

#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <wiredtiger.h>

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
#include "mongo/db/service_context.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/oplog_truncation.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_truncate_markers.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

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

            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp());
            ASSERT_OK(res.getStatus());
            id1 = res.getValue();

            res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp());
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

        rs->dataFor(t1.get(), id1);
        rs->dataFor(t2.get(), id1);

        ASSERT_OK(rs->updateRecord(t1.get(), id1, "b", 2));
        ASSERT_OK(rs->updateRecord(t1.get(), id2, "B", 2));

        try {
            // this should fail
            rs->updateRecord(t2.get(), id1, "c", 2).transitional_ignore();
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

            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp());
            ASSERT_OK(res.getStatus());
            id1 = res.getValue();

            res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp());
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
        rs->dataFor(t1.get(), id2);
        rs->dataFor(t2.get(), id2);

        {
            StorageWriteTransaction w(ru1);
            ASSERT_OK(rs->updateRecord(t1.get(), id1, "b", 2));
            w.commit();
        }

        {
            StorageWriteTransaction w(ru2);
            ASSERT_EQUALS(std::string("a"), rs->dataFor(t2.get(), id1).data());
            try {
                // this should fail as our version of id1 is too old
                rs->updateRecord(t2.get(), id1, "c", 2).transitional_ignore();
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
    StatusWith<RecordId> res = rs->insertRecord(opCtx, obj.objdata(), obj.objsize(), opTime);
    ASSERT_OK(res.getStatus());
    return res.getValue();
}

/**
 * Test that even when the oplog durability loop is paused, we can still advance the commit point as
 * long as the commit for each insert comes before the next insert starts.
 */
TEST(WiredTigerRecordStoreTest, OplogDurableVisibilityInOrder) {
    ON_BLOCK_EXIT([] { WTPauseOplogVisibilityUpdateLoop.setMode(FailPoint::off); });
    WTPauseOplogVisibilityUpdateLoop.setMode(FailPoint::alwaysOn);

    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());
    auto engine = harnessHelper->getEngine();

    auto isOpHidden = [&engine](const RecordId& id) {
        return static_cast<WiredTigerKVEngine*>(engine)
                   ->getOplogManager()
                   ->getOplogReadTimestamp() < static_cast<std::uint64_t>(id.getLong());
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
    ON_BLOCK_EXIT([] { WTPauseOplogVisibilityUpdateLoop.setMode(FailPoint::off); });
    WTPauseOplogVisibilityUpdateLoop.setMode(FailPoint::alwaysOn);

    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());
    auto engine = harnessHelper->getEngine();

    auto isOpHidden = [&engine](const RecordId& id) {
        return static_cast<WiredTigerKVEngine*>(engine)
                   ->getOplogManager()
                   ->getOplogReadTimestamp() < static_cast<std::uint64_t>(id.getLong());
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

    // Wait a bit and check again to make sure they don't become visible automatically.
    sleepsecs(1);
    ASSERT(isOpHidden(id1));
    ASSERT(isOpHidden(id2));

    WTPauseOplogVisibilityUpdateLoop.setMode(FailPoint::off);

    engine->waitForAllEarlierOplogWritesToBeVisible(longLivedOp.get(), rs.get());

    ASSERT(!isOpHidden(id1));
    ASSERT(!isOpHidden(id2));
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
    ASSERT_EQUALS(creationStringElement.type(), String);
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
    WiredTigerRecordStore* wtrs = checked_cast<WiredTigerRecordStore*>(rs);
    invariant(wtrs);
    Status status = engine->oplogDiskLocRegister(
        *shard_role_details::getRecoveryUnit(opCtx), rs, opTime, false);
    if (!status.isOK()) {
        return StatusWith<RecordId>(status);
    }
    StatusWith<RecordId> res = rs->insertRecord(opCtx, obj.objdata(), obj.objsize(), opTime);
    if (res.isOK()) {
        txn.commit();
    }
    return res;
}

// When the oplog collection is non-empty, but no OplogTruncateMarkers are
// generated because the estimated 'dataSize' is smaller than the minimum size for a truncate
// marker, tests that
//  (1) The oplog is scanned
//  (2) OplogTruncateMarkers::currentBytes_forTest() reflects the actual size of the oplog instead
//  of the estimated size.
TEST(WiredTigerRecordStoreTest, OplogTruncateMarkers_NoMarkersGeneratedFromScanning) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    auto wtHarnessHelper = dynamic_cast<WiredTigerHarnessHelper*>(harnessHelper.get());
    std::unique_ptr<RecordStore> rs(wtHarnessHelper->newOplogRecordStoreNoInit());

    auto wtrs = static_cast<WiredTigerRecordStore::Oplog*>(rs.get());

    int realNumRecords = 4;
    int realSizePerRecord = 100;
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        for (int i = 1; i <= realNumRecords; i++) {
            ASSERT_EQ(insertBSONWithSize(opCtx.get(),
                                         harnessHelper->getEngine(),
                                         rs.get(),
                                         Timestamp(i, 0),
                                         realSizePerRecord),
                      RecordId(i, 0));
        }
    }

    // Force the estimates of 'dataSize' and 'numRecords' to be lower than the real values.
    wtrs->setNumRecords(realNumRecords - 1);
    wtrs->setDataSize((realNumRecords - 1) * realSizePerRecord);

    // Initialize the truncate markers.
    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

    wtrs->postConstructorInit(opCtx.get());

    // Confirm that small oplogs are processed by scanning.
    auto oplogTruncateMarkers = wtrs->oplog()->getCollectionTruncateMarkers();
    ASSERT_EQ(CollectionTruncateMarkers::MarkersCreationMethod::Scanning,
              oplogTruncateMarkers->getMarkersCreationMethod());
    ASSERT_GTE(oplogTruncateMarkers->getCreationProcessingTime().count(), 0);
    auto numMarkers = oplogTruncateMarkers->numMarkers_forTest();
    ASSERT_EQ(numMarkers, 0U);

    // A forced scan over the RecordStore should force the 'currentBytes' to be accurate in the
    // truncate markers as well as the RecordStore's 'numRecords' and 'dataSize'.
    ASSERT_EQ(oplogTruncateMarkers->currentBytes_forTest(), realNumRecords * realSizePerRecord);
    ASSERT_EQ(wtrs->dataSize(), realNumRecords * realSizePerRecord);
    ASSERT_EQ(wtrs->numRecords(), realNumRecords);
}

// Ensure that if we sample and create duplicate oplog truncate markers, perform truncation
// correctly, and with no crashing behavior. This scenario may be possible if the same record is
// sampled multiple times during startup, which can be very likely if the size storer is very
// inaccurate.
TEST(WiredTigerRecordStoreTest, OplogTruncateMarkers_Duplicates) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    auto wtHarnessHelper = dynamic_cast<WiredTigerHarnessHelper*>(harnessHelper.get());
    std::unique_ptr<RecordStore> rs(wtHarnessHelper->newOplogRecordStoreNoInit());
    auto engine = harnessHelper->getEngine();

    auto wtrs = static_cast<WiredTigerRecordStore::Oplog*>(rs.get());

    {
        // Before initializing the RecordStore, which also starts the oplog sampling process,
        // populate with a few records.
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 0), 100),
                  RecordId(1, 0));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(2, 0), 100),
                  RecordId(2, 0));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(3, 0), 100),
                  RecordId(3, 0));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(4, 0), 100),
                  RecordId(4, 0));
    }

    {
        // Force initialize the oplog truncate markers to use sampling by providing very large,
        // inaccurate sizes. This should cause us to oversample the records in the oplog.
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        wtrs->setNumRecords(1024 * 1024);
        wtrs->setDataSize(1024 * 1024 * 1024);
        wtrs->postConstructorInit(opCtx.get());
    }

    // Confirm that some truncate markers were generated.
    auto oplogTruncateMarkers = wtrs->oplog()->getCollectionTruncateMarkers();
    ASSERT_EQ(CollectionTruncateMarkers::MarkersCreationMethod::Sampling,
              oplogTruncateMarkers->getMarkersCreationMethod());
    ASSERT_GTE(oplogTruncateMarkers->getCreationProcessingTime().count(), 0);
    auto truncateMarkersBefore = oplogTruncateMarkers->numMarkers_forTest();
    ASSERT_GT(truncateMarkersBefore, 0U);
    ASSERT_GT(oplogTruncateMarkers->currentBytes_forTest(), 0);

    {
        // Reclaiming should do nothing because the data size is still under the maximum.
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        wtHarnessHelper->advanceStableTimestamp(Timestamp(4, 0));
        auto mayTruncateUpTo = RecordId(engine->getPinnedOplog().asULL());
        oplog_truncation::reclaimOplog(opCtx.get(), *rs.get(), mayTruncateUpTo);
        ASSERT_EQ(truncateMarkersBefore, oplogTruncateMarkers->numMarkers_forTest());

        // Reduce the oplog size to ensure we create a truncate marker and truncate on the next
        // insert.
        ASSERT_OK(wtrs->updateSize(400));

        // Inserting these records should meet the requirements for truncation. That is: there is a
        // record, 5, after the last truncate marker, 4, and before the truncation point, 6.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(5, 0), 100),
                  RecordId(5, 0));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(6, 0), 100),
                  RecordId(6, 0));

        // Ensure every truncate marker has been cleaned up except for the last one ending in 6.
        wtHarnessHelper->advanceStableTimestamp(Timestamp(6, 0));
        mayTruncateUpTo = RecordId(engine->getPinnedOplog().asULL());
        oplog_truncation::reclaimOplog(opCtx.get(), *rs.get(), mayTruncateUpTo);
        ASSERT_EQ(1, oplogTruncateMarkers->numMarkers_forTest());

        // The original oplog should have rolled over and the size and count should be accurate.
        ASSERT_EQ(1, wtrs->numRecords());
        ASSERT_EQ(100, wtrs->dataSize());
    }
}

void testTruncateRange(int64_t numRecordsToInsert,
                       int64_t deletionPosBegin,
                       int64_t deletionPosEnd) {
    auto harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());
    auto engine = harnessHelper->getEngine();

    auto wtrs = checked_cast<WiredTigerRecordStore*>(rs.get());

    std::vector<RecordId> recordIds;

    auto opCtx = harnessHelper->newOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    for (int i = 0; i < numRecordsToInsert; i++) {
        auto recordId = insertBSONWithSize(opCtx.get(), engine, wtrs, Timestamp(1, i), 100);
        ASSERT_OK(recordId);
        recordIds.emplace_back(std::move(recordId.getValue()));
    }

    auto sizePerRecord = wtrs->dataSize() / wtrs->numRecords();

    std::sort(recordIds.begin(), recordIds.end());

    const auto& beginId = recordIds[deletionPosBegin];
    const auto& endId = recordIds[deletionPosEnd];
    {
        StorageWriteTransaction txn(ru);

        auto numRecordsDeleted = deletionPosEnd - deletionPosBegin + 1;

        ASSERT_OK(wtrs->rangeTruncate(
            opCtx.get(), beginId, endId, -(sizePerRecord * numRecordsDeleted), -numRecordsDeleted));

        ASSERT_EQ(wtrs->dataSize(), sizePerRecord * (numRecordsToInsert - numRecordsDeleted));
        ASSERT_EQ(wtrs->numRecords(), (numRecordsToInsert - numRecordsDeleted));

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

    auto cursor = wtrs->getCursor(opCtx.get(), true);
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

    auto wtrs = checked_cast<WiredTigerRecordStore::Oplog*>(rs.get());

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
    ASSERT_EQ(tsOne, wtrs->getLatestTimestamp(ru1));

    // 2) Open a hole at time "2".
    boost::optional<StorageWriteTransaction> op1txn(ru1);
    // Don't save the return value because the compiler complains about unused variables.
    oplogOrderInsertOplog(op1.get(), engine, rs, 2);

    // Store the client with an uncommitted transaction. Create a new, concurrent client.
    auto client1 = Client::releaseCurrent();
    Client::initThread("client2", getGlobalServiceContext()->getService());

    ServiceContext::UniqueOperationContext op2(harnessHelper->newOperationContext());
    auto& ru2 = *shard_role_details::getRecoveryUnit(op2.get());
    // Should not see uncommited write from op1.
    ASSERT_EQ(tsOne, wtrs->getLatestTimestamp(ru2));

    Timestamp tsThree = [&] {
        StorageWriteTransaction op2Txn(ru2);
        Timestamp tsThree = Timestamp(static_cast<unsigned long long>(
            oplogOrderInsertOplog(op2.get(), engine, rs, 3).getLong()));
        op2Txn.commit();
        return tsThree;
    }();
    // After committing, three is the top of oplog.
    ASSERT_EQ(tsThree, wtrs->getLatestTimestamp(ru2));

    // Switch to client 1.
    op2.reset();
    auto client2 = Client::releaseCurrent();
    Client::setCurrent(std::move(client1));

    op1txn->commit();
    // Committing the write at timestamp "2" does not change the top of oplog result. A new query
    // with client 1 will see timestamp "3".
    ASSERT_EQ(tsThree, wtrs->getLatestTimestamp(ru1));
}

TEST(WiredTigerRecordStoreTest, CursorInActiveTxnAfterNext) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    RecordId rid1;
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        StorageWriteTransaction txn(ru);
        StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp());
        ASSERT_OK(res.getStatus());
        rid1 = res.getValue();

        res = rs->insertRecord(opCtx.get(), "b", 2, Timestamp());
        ASSERT_OK(res.getStatus());

        txn.commit();
    }

    // Cursors should always ensure they are in an active transaction when next() is called.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        auto& ru = *WiredTigerRecoveryUnit::get(shard_role_details::getRecoveryUnit(opCtx.get()));

        auto cursor = rs->getCursor(opCtx.get());
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
        StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp());
        ASSERT_OK(res.getStatus());
        rid1 = res.getValue();

        res = rs->insertRecord(opCtx.get(), "b", 2, Timestamp());
        ASSERT_OK(res.getStatus());

        txn.commit();
    }

    // Cursors should always ensure they are in an active transaction when seekExact() or seek() is
    // called.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        auto& ru = *WiredTigerRecoveryUnit::get(shard_role_details::getRecoveryUnit(opCtx.get()));

        auto cursor = rs->getCursor(opCtx.get());
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

// Verify clustered record stores.
// This test case complements StorageEngineTest:TemporaryRecordStoreClustered which verifies
// clustered temporary record stores.
TEST(WiredTigerRecordStoreTest, ClusteredRecordStore) {
    const std::unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    const std::string ns = "testRecordStore";
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(ns);
    const std::string uri = WiredTigerKVEngine::kTableUriPrefix + ns;
    const StatusWith<std::string> result =
        WiredTigerRecordStore::generateCreateString(std::string{kWiredTigerEngineName},
                                                    NamespaceStringUtil::serializeForCatalog(nss),
                                                    "",
                                                    CollectionOptions(),
                                                    "",
                                                    KeyFormat::String,
                                                    WiredTigerUtil::useTableLogging(nss));
    ASSERT_TRUE(result.isOK());
    const std::string config = result.getValue();

    {
        StorageWriteTransaction txn(ru);
        WiredTigerRecoveryUnit* ru =
            checked_cast<WiredTigerRecoveryUnit*>(shard_role_details::getRecoveryUnit(opCtx.get()));
        WT_SESSION* s = ru->getSession()->getSession();
        invariantWTOK(s->create(s, uri.c_str(), config.c_str()), s);
        txn.commit();
    }

    WiredTigerRecordStore::Params params;
    params.uuid = boost::none;
    params.ident = ns;
    params.engineName = std::string{kWiredTigerEngineName};
    params.keyFormat = KeyFormat::String;
    params.overwrite = false;
    params.isEphemeral = false;
    params.isLogged = WiredTigerUtil::useTableLogging(nss);
    params.isChangeCollection = false;
    params.sizeStorer = nullptr;
    params.tracksSizeAdjustments = true;
    params.forceUpdateWithFullDocument = false;

    const auto wtKvEngine = dynamic_cast<WiredTigerKVEngine*>(harnessHelper->getEngine());
    auto rs = std::make_unique<WiredTigerRecordStore>(
        wtKvEngine,
        WiredTigerRecoveryUnit::get(*shard_role_details::getRecoveryUnit(opCtx.get())),
        params);

    const auto id = StringData{"1"};
    const auto rid = RecordId(id.rawData(), id.size());
    const auto data = "data";
    {
        StorageWriteTransaction txn(ru);
        StatusWith<RecordId> s =
            rs->insertRecord(opCtx.get(), rid, data, strlen(data), Timestamp());
        ASSERT_TRUE(s.isOK());
        ASSERT_EQUALS(1, rs->numRecords());
        txn.commit();
    }
    // Read the record back.
    RecordData rd;
    ASSERT_TRUE(rs->findRecord(opCtx.get(), rid, &rd));
    ASSERT_EQ(0, memcmp(data, rd.data(), strlen(data)));
    // Update the record.
    const auto dataUpdated = "updated";
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(rs->updateRecord(opCtx.get(), rid, dataUpdated, strlen(dataUpdated)));
        ASSERT_EQUALS(1, rs->numRecords());
        txn.commit();
    }
    ASSERT_TRUE(rs->findRecord(opCtx.get(), rid, &rd));
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
        rid = rs->insertRecord(ctx.get(), "a", 2, Timestamp()).getValue();
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
        rs->deleteRecord(ctx.get(), rid);
        shard_role_details::getRecoveryUnit(ctx.get())->onRollback(
            [&](OperationContext*) { deleted->countDownAndWait(); });
        shard_role_details::getRecoveryUnit(ctx.get())->onRollback(
            [&](OperationContext*) { aborted->countDownAndWait(); });
    });

    // Wait for the other thread to abort.
    aborted->countDownAndWait();

    rs->deleteRecord(ctx.get(), rid);

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
        rid = rs->insertRecord(ctx.get(), RecordId(7), "a", 2, Timestamp()).getValue();
        txn.commit();
    }

    // The next recordId reserved is higher than 7.
    rs->reserveRecordIds(ctx.get(), &reservedRids, 1);
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
        ASSERT_OK(rs->insertRecords(ctx.get(), &recordsToInsert, timestamps));
        txn.commit();
    }

    // The next recordId reserved is higher than 14.
    reservedRids.clear();
    rs->reserveRecordIds(ctx.get(), &reservedRids, 1);
    ASSERT_GT(reservedRids[0].getLong(), RecordId(14).getLong());
    ASSERT_EQ(3, rs->numRecords());

    // Insert a few records at once, where the recordIds are in order. And ensure that
    // we still reserve recordIds from the right point afterwards.
    recordsToInsert.clear();
    recordsToInsert.push_back(Record{RecordId(19), RecordData()});
    recordsToInsert.push_back(Record{RecordId(20), RecordData()});
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(rs->insertRecords(ctx.get(), &recordsToInsert, timestamps));
        txn.commit();
    }

    // The next recordId reserved is higher than 20.
    reservedRids.clear();
    rs->reserveRecordIds(ctx.get(), &reservedRids, 1);
    ASSERT_GT(reservedRids[0].getLong(), RecordId(20).getLong());
    ASSERT_EQ(5, rs->numRecords());
}

}  // namespace
}  // namespace mongo
