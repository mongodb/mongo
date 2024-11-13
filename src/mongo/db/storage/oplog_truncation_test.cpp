/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/oplog_truncation.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/unittest/assert.h"

namespace mongo {
namespace {
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
    Status status = engine->oplogDiskLocRegister(ru, rs, opTime, false);
    if (!status.isOK()) {
        return StatusWith<RecordId>(status);
    }
    StatusWith<RecordId> res = rs->insertRecord(opCtx, obj.objdata(), obj.objsize(), opTime);
    if (res.isOK()) {
        txn.commit();
    }
    return res;
}
}  // namespace

/**
 * Insert records into an oplog and verify the number of truncate markers that are created.
 */
TEST(OplogTruncationTest, OplogTruncateMarkers_CreateNewMarker) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());
    auto engine = harnessHelper->getEngine();

    auto oplogTruncateMarkers = rs->oplog()->getCollectionTruncateMarkers();

    oplogTruncateMarkers->setMinBytesPerMarker(100);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());

        // Inserting a record smaller than 'minBytesPerTruncateMarker' shouldn't create a new oplog
        // truncate marker.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 1), 99),
                  RecordId(1, 1));
        ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(99, oplogTruncateMarkers->currentBytes_forTest());

        // Inserting another record such that their combined size exceeds
        // 'minBytesPerTruncateMarker' should cause a new truncate marker to be created.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 2), 51),
                  RecordId(1, 2));
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());

        // Inserting a record such that the combined size of this record and the previously inserted
        // one exceed 'minBytesPerTruncateMarker' shouldn't cause a new truncate marker to be
        // created because we've started filling a new truncate marker.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 3), 50),
                  RecordId(1, 3));
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(50, oplogTruncateMarkers->currentBytes_forTest());

        // Inserting a record such that the combined size of this record and the previously inserted
        // one is exactly equal to 'minBytesPerTruncateMarker' should cause a new truncate marker to
        // be created.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 4), 50),
                  RecordId(1, 4));
        ASSERT_EQ(2U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());

        // Inserting a single record that exceeds 'minBytesPerTruncateMarker' should cause a new
        // truncate marker to be created.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 5), 101),
                  RecordId(1, 5));
        ASSERT_EQ(3U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }
}

/**
 * Insert multiple records and truncate the oplog using RecordStore::truncate(). The operation
 * should leave no truncate markers, including the partially filled one.
 */
TEST(OplogTruncationTest, OplogTruncateMarkers_Truncate) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());
    auto engine = harnessHelper->getEngine();

    auto oplogTruncateMarkers = rs->oplog()->getCollectionTruncateMarkers();

    oplogTruncateMarkers->setMinBytesPerMarker(100);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 1), 50),
                  RecordId(1, 1));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 2), 50),
                  RecordId(1, 2));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 3), 50),
                  RecordId(1, 3));

        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(50, oplogTruncateMarkers->currentBytes_forTest());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        ASSERT_EQ(3, rs->numRecords());
        ASSERT_EQ(150, rs->dataSize());

        StorageWriteTransaction txn(ru);
        ASSERT_OK(rs->truncate(opCtx.get()));
        txn.commit();

        ASSERT_EQ(0, rs->dataSize());
        ASSERT_EQ(0, rs->numRecords());
        ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }
}


/**
 * Insert records into an oplog and try to update them. The updates shouldn't succeed if the size of
 * record is changed.
 */
TEST(OplogTruncationTest, OplogTruncateMarkers_UpdateRecord) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());
    auto engine = harnessHelper->getEngine();

    auto oplogTruncateMarkers = rs->oplog()->getCollectionTruncateMarkers();

    oplogTruncateMarkers->setMinBytesPerMarker(100);

    // Insert two records such that one makes up a full truncate marker and the other is a part of
    // the truncate marker currently being filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 1), 100),
                  RecordId(1, 1));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 2), 50),
                  RecordId(1, 2));

        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(50, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Attempts to grow the records should fail.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        BSONObj changed1 = makeBSONObjWithSize(Timestamp(1, 1), 101);
        BSONObj changed2 = makeBSONObjWithSize(Timestamp(1, 2), 51);

        StorageWriteTransaction txn(ru);
        ASSERT_NOT_OK(
            rs->updateRecord(opCtx.get(), RecordId(1, 1), changed1.objdata(), changed1.objsize()));
        ASSERT_NOT_OK(
            rs->updateRecord(opCtx.get(), RecordId(1, 2), changed2.objdata(), changed2.objsize()));
    }

    // Attempts to shrink the records should also fail.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        BSONObj changed1 = makeBSONObjWithSize(Timestamp(1, 1), 99);
        BSONObj changed2 = makeBSONObjWithSize(Timestamp(1, 2), 49);

        StorageWriteTransaction txn(ru);
        ASSERT_NOT_OK(
            rs->updateRecord(opCtx.get(), RecordId(1, 1), changed1.objdata(), changed1.objsize()));
        ASSERT_NOT_OK(
            rs->updateRecord(opCtx.get(), RecordId(1, 2), changed2.objdata(), changed2.objsize()));
    }

    // Changing the contents of the records without changing their size should succeed.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        BSONObj changed1 = makeBSONObjWithSize(Timestamp(1, 1), 100, 'y');
        BSONObj changed2 = makeBSONObjWithSize(Timestamp(1, 2), 50, 'z');

        StorageWriteTransaction txn(ru);
        // Explicitly sets the timestamp to ensure ordered writes.
        ASSERT_OK(shard_role_details::getRecoveryUnit(opCtx.get())->setTimestamp(Timestamp(1, 3)));
        ASSERT_OK(
            rs->updateRecord(opCtx.get(), RecordId(1, 1), changed1.objdata(), changed1.objsize()));
        ASSERT_OK(
            rs->updateRecord(opCtx.get(), RecordId(1, 2), changed2.objdata(), changed2.objsize()));
        txn.commit();

        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(50, oplogTruncateMarkers->currentBytes_forTest());
    }
}


/**
 * Insert multiple records, truncate the oplog using RecordStore::cappedTruncateAfter(), and verify
 * that the metadata for each truncate marker is updated. If a full truncate marker is partially
 * truncated, then it should become the truncate marker currently being filled.
 */
TEST(OplogTruncationTest, OplogTruncateMarkers_CappedTruncateAfter) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());
    auto engine = harnessHelper->getEngine();

    auto oplogTruncateMarkers = rs->oplog()->getCollectionTruncateMarkers();

    oplogTruncateMarkers->setMinBytesPerMarker(1000);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 1), 400),
                  RecordId(1, 1));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 2), 800),
                  RecordId(1, 2));

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 3), 200),
                  RecordId(1, 3));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 4), 250),
                  RecordId(1, 4));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 5), 300),
                  RecordId(1, 5));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 6), 350),
                  RecordId(1, 6));

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 7), 50),
                  RecordId(1, 7));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 8), 100),
                  RecordId(1, 8));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 9), 150),
                  RecordId(1, 9));

        ASSERT_EQ(9, rs->numRecords());
        ASSERT_EQ(2600, rs->dataSize());
        ASSERT_EQ(2U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(3, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(300, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Make sure all are visible.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        engine->waitForAllEarlierOplogWritesToBeVisible(opCtx.get(), rs.get());
    }

    // Truncate data using an inclusive RecordId that exists inside the truncate marker currently
    // being filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        rs->capped()->truncateAfter(opCtx.get(),
                                    RecordId(1, 8),
                                    true /* inclusive */,
                                    nullptr /* aboutToDelete callback */);

        ASSERT_EQ(7, rs->numRecords());
        ASSERT_EQ(2350, rs->dataSize());
        ASSERT_EQ(2U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(50, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Truncate data using an inclusive RecordId that refers to the 'lastRecord' of a full truncate
    // marker.
    // The truncate marker should become the one currently being filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        rs->capped()->truncateAfter(opCtx.get(),
                                    RecordId(1, 6),
                                    true /* inclusive */,
                                    nullptr /* aboutToDelete callback */);

        ASSERT_EQ(5, rs->numRecords());
        ASSERT_EQ(1950, rs->dataSize());
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(3, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(750, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Truncate data using a non-inclusive RecordId that exists inside the truncate marker currently
    // being filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        rs->capped()->truncateAfter(opCtx.get(),
                                    RecordId(1, 3),
                                    false /* inclusive */,
                                    nullptr /* aboutToDelete callback */);

        ASSERT_EQ(3, rs->numRecords());
        ASSERT_EQ(1400, rs->dataSize());
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(200, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Truncate data using a non-inclusive RecordId that refers to the 'lastRecord' of a full
    // truncate marker.
    // The truncate marker should remain intact.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        rs->capped()->truncateAfter(opCtx.get(),
                                    RecordId(1, 2),
                                    false /* inclusive */,
                                    nullptr /* aboutToDelete callback */);

        ASSERT_EQ(2, rs->numRecords());
        ASSERT_EQ(1200, rs->dataSize());
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Truncate data using a non-inclusive RecordId that exists inside a full truncate marker. The
    // truncate marker should become the one currently being filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        rs->capped()->truncateAfter(opCtx.get(),
                                    RecordId(1, 1),
                                    false /* inclusive */,
                                    nullptr /* aboutToDelete callback */);

        ASSERT_EQ(1, rs->numRecords());
        ASSERT_EQ(400, rs->dataSize());
        ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(400, oplogTruncateMarkers->currentBytes_forTest());
    }
}

/**
 * Verify that oplog truncate markers are reclaimed when cappedMaxSize is exceeded.
 */
TEST(OplogTruncationTest, ReclaimTruncateMarkers) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());
    auto engine = harnessHelper->getEngine();

    auto oplogTruncateMarkers = rs->oplog()->getCollectionTruncateMarkers();

    ASSERT_OK(rs->oplog()->updateSize(230));

    oplogTruncateMarkers->setMinBytesPerMarker(100);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 1), 100),
                  RecordId(1, 1));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 2), 110),
                  RecordId(1, 2));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 3), 120),
                  RecordId(1, 3));

        ASSERT_EQ(3, rs->numRecords());
        ASSERT_EQ(330, rs->dataSize());
        ASSERT_EQ(3U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Fail to truncate the truncate marker when cappedMaxSize is exceeded, but the persisted
    // timestamp is before the truncation point (i.e: leaves a gap that replication recovery would
    // rely on).
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        harnessHelper->advanceStableTimestamp(Timestamp(1, 0));
        auto mayTruncateUpTo = RecordId(engine->getPinnedOplog().asULL());
        auto truncatedUpTo =
            oplog_truncation::reclaimOplog(opCtx.get(), *rs.get(), mayTruncateUpTo);

        ASSERT_EQ(RecordId(), truncatedUpTo);
        ASSERT_EQ(Timestamp(1, 1), rs->oplog()->getEarliestTimestamp(ru).getValue());

        ASSERT_EQ(3, rs->numRecords());
        ASSERT_EQ(330, rs->dataSize());
        ASSERT_EQ(3U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Truncate a truncate marker when cappedMaxSize is exceeded.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        harnessHelper->advanceStableTimestamp(Timestamp(1, 3));
        auto mayTruncateUpTo = RecordId(engine->getPinnedOplog().asULL());
        auto truncatedUpTo =
            oplog_truncation::reclaimOplog(opCtx.get(), *rs.get(), mayTruncateUpTo);

        ASSERT_EQ(RecordId(1, 1), truncatedUpTo);
        ASSERT_EQ(Timestamp(1, 2), rs->oplog()->getEarliestTimestamp(ru).getValue());

        ASSERT_EQ(2, rs->numRecords());
        ASSERT_EQ(230, rs->dataSize());
        ASSERT_EQ(2U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 4), 130),
                  RecordId(1, 4));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 5), 140),
                  RecordId(1, 5));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 6), 50),
                  RecordId(1, 6));

        ASSERT_EQ(5, rs->numRecords());
        ASSERT_EQ(550, rs->dataSize());
        ASSERT_EQ(4U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(50, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Truncate multiple truncate markers if necessary.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        harnessHelper->advanceStableTimestamp(Timestamp(1, 6));
        auto mayTruncateUpTo = RecordId(engine->getPinnedOplog().asULL());
        auto truncatedUpTo =
            oplog_truncation::reclaimOplog(opCtx.get(), *rs.get(), mayTruncateUpTo);

        ASSERT_EQ(RecordId(1, 4), truncatedUpTo);
        ASSERT_EQ(Timestamp(1, 5), rs->oplog()->getEarliestTimestamp(ru).getValue());

        ASSERT_EQ(2, rs->numRecords());
        ASSERT_EQ(190, rs->dataSize());
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(50, oplogTruncateMarkers->currentBytes_forTest());
    }

    // No-op if dataSize <= cappedMaxSize.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        harnessHelper->advanceStableTimestamp(Timestamp(1, 6));
        auto mayTruncateUpTo = RecordId(engine->getPinnedOplog().asULL());
        auto truncatedUpTo =
            oplog_truncation::reclaimOplog(opCtx.get(), *rs.get(), mayTruncateUpTo);

        ASSERT_EQ(RecordId(), truncatedUpTo);
        ASSERT_EQ(Timestamp(1, 5), rs->oplog()->getEarliestTimestamp(ru).getValue());

        ASSERT_EQ(2, rs->numRecords());
        ASSERT_EQ(190, rs->dataSize());
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(50, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Don't truncate the last truncate marker before the truncate point, even if the truncate point
    // is ahead of it.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 7), 190),
                  RecordId(1, 7));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 9), 120),
                  RecordId(1, 9));

        ASSERT_EQ(4, rs->numRecords());
        ASSERT_EQ(500, rs->dataSize());
        ASSERT_EQ(3U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        harnessHelper->advanceStableTimestamp(Timestamp(1, 8));
        auto mayTruncateUpTo = RecordId(engine->getPinnedOplog().asULL());
        auto truncatedUpTo =
            oplog_truncation::reclaimOplog(opCtx.get(), *rs.get(), mayTruncateUpTo);

        ASSERT_EQ(RecordId(1, 5), truncatedUpTo);
        ASSERT_EQ(Timestamp(1, 6), rs->oplog()->getEarliestTimestamp(ru).getValue());

        ASSERT_EQ(3, rs->numRecords());
        ASSERT_EQ(360, rs->dataSize());
        ASSERT_EQ(2U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Don't truncate entire oplog.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 10), 90),
                  RecordId(1, 10));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 11), 210),
                  RecordId(1, 11));

        ASSERT_EQ(5, rs->numRecords());
        ASSERT_EQ(660, rs->dataSize());
        ASSERT_EQ(3U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        harnessHelper->advanceStableTimestamp(Timestamp(1, 12));
        auto mayTruncateUpTo = RecordId(engine->getPinnedOplog().asULL());
        auto truncatedUpTo =
            oplog_truncation::reclaimOplog(opCtx.get(), *rs.get(), mayTruncateUpTo);

        ASSERT_EQ(RecordId(1, 9), truncatedUpTo);
        ASSERT_EQ(Timestamp(1, 10), rs->oplog()->getEarliestTimestamp(ru).getValue());

        ASSERT_EQ(2, rs->numRecords());
        ASSERT_EQ(300, rs->dataSize());
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }

    // OK to truncate all truncate markers if there are records in the oplog that are before or at
    // the truncate-up-to point, that have not yet created a truncate marker.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        // Use timestamp (1, 13) as we can't commit at the stable timestamp (1, 12).
        auto t = insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 13), 90);
        ASSERT_EQ(t, RecordId(1, 13));

        ASSERT_EQ(3, rs->numRecords());
        ASSERT_EQ(390, rs->dataSize());
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(90, oplogTruncateMarkers->currentBytes_forTest());
    }
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        harnessHelper->advanceStableTimestamp(Timestamp(1, 13));
        auto mayTruncateUpTo = RecordId(engine->getPinnedOplog().asULL());
        auto truncatedUpTo =
            oplog_truncation::reclaimOplog(opCtx.get(), *rs.get(), mayTruncateUpTo);

        ASSERT_EQ(RecordId(1, 11), truncatedUpTo);
        ASSERT_EQ(Timestamp(1, 13), rs->oplog()->getEarliestTimestamp(ru).getValue());

        ASSERT_EQ(1, rs->numRecords());
        ASSERT_EQ(90, rs->dataSize());
        ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(90, oplogTruncateMarkers->currentBytes_forTest());
    }
}

/**
 * Verify that an oplog truncate marker isn't created if it would cause the logical representation
 * of the records to not be in increasing order.
 */
TEST(OplogTruncationTest, OplogTruncateMarkers_AscendingOrder) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());
    auto engine = harnessHelper->getEngine();

    auto oplogTruncateMarkers = rs->oplog()->getCollectionTruncateMarkers();

    oplogTruncateMarkers->setMinBytesPerMarker(100);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(2, 2), 50),
                  RecordId(2, 2));
        ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(50, oplogTruncateMarkers->currentBytes_forTest());

        // Inserting a record that has a smaller RecordId than the previously inserted record should
        // be able to create a new truncate marker when no truncate markers already exist.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(2, 1), 50),
                  RecordId(2, 1));
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());

        // However, inserting a record that has a smaller RecordId than most recently created
        // truncate marker's last record shouldn't cause a new truncate marker to be created, even
        // if the size of the inserted record exceeds 'minBytesPerTruncateMarker'.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(1, 1), 100),
                  RecordId(1, 1));
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(100, oplogTruncateMarkers->currentBytes_forTest());

        // Inserting a record that has a larger RecordId than the most recently created truncate
        // marker's last record should then cause a new truncate marker to be created.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), engine, rs.get(), Timestamp(2, 3), 50),
                  RecordId(2, 3));
        ASSERT_EQ(2U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }
}

}  // namespace mongo
