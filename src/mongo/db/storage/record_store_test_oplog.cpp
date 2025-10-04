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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

StatusWith<RecordId> insertBSON(ServiceContext::UniqueOperationContext& opCtx,
                                KVEngine* engine,
                                std::unique_ptr<RecordStore>& rs,
                                const Timestamp& opTime) {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    BSONObj obj = BSON("ts" << opTime);
    StorageWriteTransaction txn(ru);
    Status status = engine->oplogDiskLocRegister(
        *shard_role_details::getRecoveryUnit(opCtx.get()), rs.get(), opTime, false);
    if (!status.isOK())
        return StatusWith<RecordId>(status);
    StatusWith<RecordId> res = rs->insertRecord(opCtx.get(),
                                                *shard_role_details::getRecoveryUnit(opCtx.get()),
                                                obj.objdata(),
                                                obj.objsize(),
                                                opTime);
    if (res.isOK())
        txn.commit();
    return res;
}

RecordId _oplogOrderInsertOplog(OperationContext* opCtx,
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

TEST(RecordStoreTest, SeekOplog) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());
    auto engine = harnessHelper->getEngine();

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        // always illegal
        ASSERT_EQ(insertBSON(opCtx, engine, rs, Timestamp(2, -1)).getStatus(),
                  ErrorCodes::BadValue);

        {
            StorageWriteTransaction txn(ru);
            BSONObj obj = BSON("not_ts" << Timestamp(2, 1));
            ASSERT_EQ(rs->insertRecord(opCtx.get(),
                                       *shard_role_details::getRecoveryUnit(opCtx.get()),
                                       obj.objdata(),
                                       obj.objsize(),
                                       Timestamp())
                          .getStatus(),
                      ErrorCodes::BadValue);
        }
        {
            StorageWriteTransaction txn(ru);
            BSONObj obj = BSON("ts" << "not a Timestamp");
            ASSERT_EQ(rs->insertRecord(opCtx.get(),
                                       *shard_role_details::getRecoveryUnit(opCtx.get()),
                                       obj.objdata(),
                                       obj.objsize(),
                                       Timestamp())
                          .getStatus(),
                      ErrorCodes::BadValue);
        }

        ASSERT_EQ(insertBSON(opCtx, engine, rs, Timestamp(-2, 1)).getStatus(),
                  ErrorCodes::BadValue);

        // success cases
        ASSERT_EQ(insertBSON(opCtx, engine, rs, Timestamp(1, 1)).getValue(), RecordId(1, 1));
        ASSERT_EQ(insertBSON(opCtx, engine, rs, Timestamp(1, 2)).getValue(), RecordId(1, 2));
        ASSERT_EQ(insertBSON(opCtx, engine, rs, Timestamp(2, 2)).getValue(), RecordId(2, 2));
    }

    // Make sure all are visible.
    {
        auto opCtx = harnessHelper->newOperationContext();
        engine->waitForAllEarlierOplogWritesToBeVisible(opCtx.get(), rs.get());
    }

    // Forward cursor seeks
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cur = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        auto rec = cur->seek(RecordId(0, 1), SeekableRecordCursor::BoundInclusion::kInclude);
        ASSERT(rec);
        ASSERT_EQ(rec->id, RecordId(1, 1));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cur = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        auto rec = cur->seek(RecordId(2, 1), SeekableRecordCursor::BoundInclusion::kInclude);
        ASSERT(rec);
        ASSERT_EQ(rec->id, RecordId(2, 2));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cur = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        auto rec = cur->seek(RecordId(2, 2), SeekableRecordCursor::BoundInclusion::kInclude);
        ASSERT(rec);
        ASSERT_EQ(rec->id, RecordId(2, 2));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cur = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        auto rec = cur->seek(RecordId(2, 3), SeekableRecordCursor::BoundInclusion::kInclude);
        ASSERT_FALSE(rec);
    }

    // Reverse cursor seeks
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cur = rs->getCursor(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), /*forward=*/false);
        auto rec = cur->seek(RecordId(0, 1), SeekableRecordCursor::BoundInclusion::kInclude);
        ASSERT_FALSE(rec);
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cur = rs->getCursor(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), /*forward=*/false);
        auto rec = cur->seek(RecordId(2, 1), SeekableRecordCursor::BoundInclusion::kInclude);
        ASSERT(rec);
        ASSERT_EQ(rec->id, RecordId(1, 2));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cur = rs->getCursor(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), /*forward=*/false);
        auto rec = cur->seek(RecordId(2, 2), SeekableRecordCursor::BoundInclusion::kInclude);
        ASSERT(rec);
        ASSERT_EQ(rec->id, RecordId(2, 2));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cur = rs->getCursor(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), /*forward=*/false);
        auto rec = cur->seek(RecordId(2, 3), SeekableRecordCursor::BoundInclusion::kInclude);
        ASSERT(rec);
        ASSERT_EQ(rec->id, RecordId(2, 2));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        StorageWriteTransaction txn(*shard_role_details::getRecoveryUnit(opCtx.get()));
        rs->capped()->truncateAfter(opCtx.get(),
                                    *shard_role_details::getRecoveryUnit(opCtx.get()),
                                    RecordId(2, 2),
                                    false /* inclusive */);
        txn.commit();
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cur = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        auto rec = cur->seek(RecordId(2, 3), SeekableRecordCursor::BoundInclusion::kInclude);
        ASSERT_FALSE(rec);
    }
}

TEST(RecordStoreTest, OplogInsertOutOfOrder) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());
    auto engine = harnessHelper->getEngine();

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        // RecordId's are inserted out-of-order.
        ASSERT_EQ(insertBSON(opCtx, engine, rs, Timestamp(1, 1)).getValue(), RecordId(1, 1));
        ASSERT_EQ(insertBSON(opCtx, engine, rs, Timestamp(2, 2)).getValue(), RecordId(2, 2));
        ASSERT_EQ(insertBSON(opCtx, engine, rs, Timestamp(1, 2)).getValue(), RecordId(1, 2));
    }
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        engine->waitForAllEarlierOplogWritesToBeVisible(opCtx.get(), rs.get());
        auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        ASSERT_EQ(cursor->next()->id, RecordId(1, 1));
        ASSERT_EQ(cursor->next()->id, RecordId(1, 2));
        ASSERT_EQ(cursor->next()->id, RecordId(2, 2));
        ASSERT(!cursor->next());
    }
}

/**
 * Stringifies the current 'record', as well as any more records in the 'cursor'. Additionally adds
 * the latest oplog visibitility timestamp (this is the current oplog read timestamp, but may not
 * have been the timestamp used by the cursor).
 */
std::string stringifyForDebug(OperationContext* opCtx,
                              boost::optional<Record> record,
                              SeekableRecordCursor* cursor) {
    str::stream output;

    auto optOplogReadTimestampInt =
        shard_role_details::getRecoveryUnit(opCtx)->getOplogVisibilityTs();
    if (optOplogReadTimestampInt) {
        output << "Latest oplog visibility timestamp: "
               << Timestamp(optOplogReadTimestampInt.value());
    }

    if (record) {
        output << ". Current record: " << record->id << ", " << record->data.toBson();
        while (auto nextRecord = cursor->next()) {
            if (nextRecord) {
                output << ". Cursor Record: " << nextRecord->id << ", "
                       << nextRecord->data.toBson();
            }
        }
    }

    return output;
}

TEST(RecordStoreTest, OplogOrder) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());
    auto engine = harnessHelper->getEngine();

    RecordId id1, id2, id3;

    {  // first insert a document
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            id1 = _oplogOrderInsertOplog(opCtx.get(), engine, rs, 1);
            txn.commit();
        }
    }

    // Make sure it is visible.
    {
        auto opCtx = harnessHelper->newOperationContext();
        engine->waitForAllEarlierOplogWritesToBeVisible(opCtx.get(), rs.get());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        auto record = cursor->seekExact(id1);
        ASSERT(record);
        ASSERT_EQ(id1, record->id);
        ASSERT(!cursor->next());

        record = cursor->seek(id1, SeekableRecordCursor::BoundInclusion::kInclude);
        ASSERT(record);
        ASSERT_EQ(id1, record->id);
        ASSERT(!cursor->next());
    }

    {
        // now we insert 2 docs, but commit the 2nd one first.
        // we make sure we can't find the 2nd until the first is committed.
        ServiceContext::UniqueOperationContext earlyReader(harnessHelper->newOperationContext());
        auto earlyCursor = rs->getCursor(earlyReader.get(),
                                         *shard_role_details::getRecoveryUnit(earlyReader.get()));
        ASSERT_EQ(earlyCursor->seekExact(id1)->id, id1);
        earlyCursor->save();
        shard_role_details::getRecoveryUnit(earlyReader.get())->abandonSnapshot();

        auto client1 = harnessHelper->serviceContext()->getService()->makeClient("c1");
        auto t1 = harnessHelper->newOperationContext(client1.get());
        auto& ru1 = *shard_role_details::getRecoveryUnit(t1.get());
        StorageWriteTransaction w1(ru1);
        id2 = _oplogOrderInsertOplog(t1.get(), engine, rs, 20);
        // do not commit yet

        {  // create 2nd doc
            auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
            auto t2 = harnessHelper->newOperationContext(client2.get());
            auto& ru2 = *shard_role_details::getRecoveryUnit(t2.get());
            {
                StorageWriteTransaction w2(ru2);
                id3 = _oplogOrderInsertOplog(t2.get(), engine, rs, 30);
                w2.commit();
            }
        }

        {  // Other operations should not be able to see 2nd doc until w1 commits.
            earlyCursor->restore(*shard_role_details::getRecoveryUnit(earlyReader.get()));
            ASSERT(!earlyCursor->next());

            auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
            auto opCtx = harnessHelper->newOperationContext(client2.get());
            auto cursor =
                rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
            auto record = cursor->seekExact(id1);
            ASSERT(record) << stringifyForDebug(opCtx.get(), record, cursor.get());
            ASSERT_EQ(id1, record->id) << stringifyForDebug(opCtx.get(), record, cursor.get());
            // 1st doc is not yet committed, and 2nd doc (id3) is invisible to next()
            auto nextRecord = cursor->next();
            ASSERT(!nextRecord) << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());
            // id2 and id3 are also invisible to seekExact()
            record = cursor->seekExact(id2);
            ASSERT(!record) << stringifyForDebug(opCtx.get(), record, cursor.get());

            record = cursor->seek(id1, SeekableRecordCursor::BoundInclusion::kInclude);
            ASSERT(record) << stringifyForDebug(opCtx.get(), record, cursor.get());
            ASSERT_EQ(id1, record->id) << stringifyForDebug(opCtx.get(), record, cursor.get());
            nextRecord = cursor->next();
            ASSERT(!nextRecord) << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());
            record = cursor->seek(id1, SeekableRecordCursor::BoundInclusion::kExclude);
            ASSERT(!record) << stringifyForDebug(opCtx.get(), record, cursor.get());

            // seekExact should still work after seek
            record = cursor->seekExact(id1);
            ASSERT(record) << stringifyForDebug(opCtx.get(), record, cursor.get());
            ASSERT_EQ(id1, record->id) << stringifyForDebug(opCtx.get(), record, cursor.get());
        }

        {
            auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
            auto opCtx = harnessHelper->newOperationContext(client2.get());
            auto cursor =
                rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
            auto record = cursor->seek(id2, SeekableRecordCursor::BoundInclusion::kInclude);
            ASSERT(!record) << stringifyForDebug(opCtx.get(), record, cursor.get());
        }

        {  // Test reverse cursors and visibility
            auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
            auto opCtx = harnessHelper->newOperationContext(client2.get());
            auto cursor = rs->getCursor(
                opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), false);
            // 2nd doc (id3) is committed and visibility filter does not apply to reverse cursor
            auto record = cursor->seekExact(id3);
            ASSERT(record) << stringifyForDebug(opCtx.get(), record, cursor.get());
            ASSERT_EQ(id3, record->id) << stringifyForDebug(opCtx.get(), record, cursor.get());
            // 1st doc (id2) is not yet commited, so we should see id1 next.
            auto nextRecord = cursor->next();
            ASSERT(nextRecord) << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());
            ASSERT_EQ(id1, nextRecord->id)
                << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());
            nextRecord = cursor->next();
            ASSERT(!nextRecord) << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());

            record = cursor->seek(id3, SeekableRecordCursor::BoundInclusion::kInclude);
            ASSERT(record) << stringifyForDebug(opCtx.get(), record, cursor.get());
            ASSERT_EQ(id3, record->id) << stringifyForDebug(opCtx.get(), record, cursor.get());
            record = cursor->seek(id3, SeekableRecordCursor::BoundInclusion::kExclude);
            ASSERT(record) << stringifyForDebug(opCtx.get(), record, cursor.get());
            ASSERT_EQ(id1, record->id) << stringifyForDebug(opCtx.get(), record, cursor.get());
            record = cursor->seek(id1, SeekableRecordCursor::BoundInclusion::kExclude);
            ASSERT(!record) << stringifyForDebug(opCtx.get(), record, cursor.get());
        }

        w1.commit();
    }

    {
        auto opCtx = harnessHelper->newOperationContext();
        engine->waitForAllEarlierOplogWritesToBeVisible(opCtx.get(), rs.get());
    }

    {  // now all 3 docs should be visible
        auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
        auto opCtx = harnessHelper->newOperationContext(client2.get());
        auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        auto record = cursor->seekExact(id1);
        ASSERT_EQ(id1, record->id) << stringifyForDebug(opCtx.get(), record, cursor.get());
        auto nextRecord = cursor->next();
        ASSERT(nextRecord) << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());
        ASSERT_EQ(id2, nextRecord->id) << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());
        nextRecord = cursor->next();
        ASSERT(nextRecord) << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());
        ASSERT_EQ(id3, nextRecord->id) << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());
        nextRecord = cursor->next();
        ASSERT(!nextRecord) << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());

        record = cursor->seek(id1, SeekableRecordCursor::BoundInclusion::kInclude);
        ASSERT(record) << stringifyForDebug(opCtx.get(), record, cursor.get());
        ASSERT_EQ(id1, record->id) << stringifyForDebug(opCtx.get(), record, cursor.get());
        record = cursor->seek(id1, SeekableRecordCursor::BoundInclusion::kExclude);
        ASSERT(record) << stringifyForDebug(opCtx.get(), record, cursor.get());
        ASSERT_EQ(id2, record->id) << stringifyForDebug(opCtx.get(), record, cursor.get());
    }

    // Rollback the last two oplog entries, then insert entries with older optimes and ensure that
    // the visibility rules aren't violated. See SERVER-21645
    {
        auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
        auto opCtx = harnessHelper->newOperationContext(client2.get());
        StorageWriteTransaction txn(*shard_role_details::getRecoveryUnit(opCtx.get()));
        rs->capped()->truncateAfter(opCtx.get(),
                                    *shard_role_details::getRecoveryUnit(opCtx.get()),
                                    id1,
                                    false /* inclusive */);
        txn.commit();
    }

    {
        auto opCtx = harnessHelper->newOperationContext();
        engine->waitForAllEarlierOplogWritesToBeVisible(opCtx.get(), rs.get());
    }

    {
        // Now we insert 2 docs with timestamps earlier than before, but commit the 2nd one first.
        // We make sure we can't find the 2nd until the first is committed.
        ServiceContext::UniqueOperationContext earlyReader(harnessHelper->newOperationContext());
        auto earlyCursor = rs->getCursor(earlyReader.get(),
                                         *shard_role_details::getRecoveryUnit(earlyReader.get()));
        ASSERT_EQ(earlyCursor->seekExact(id1)->id, id1);
        earlyCursor->save();
        shard_role_details::getRecoveryUnit(earlyReader.get())->abandonSnapshot();

        auto client1 = harnessHelper->serviceContext()->getService()->makeClient("c1");
        auto t1 = harnessHelper->newOperationContext(client1.get());
        auto& ru1 = *shard_role_details::getRecoveryUnit(t1.get());
        StorageWriteTransaction w1(ru1);
        RecordId id2 = _oplogOrderInsertOplog(t1.get(), engine, rs, 2);

        // do not commit yet

        RecordId id3;
        {  // create 2nd doc
            auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
            auto t2 = harnessHelper->newOperationContext(client2.get());
            auto& ru2 = *shard_role_details::getRecoveryUnit(t2.get());
            {
                StorageWriteTransaction w2(ru2);
                id3 = _oplogOrderInsertOplog(t2.get(), engine, rs, 3);
                w2.commit();
            }
        }

        {  // Other operations should not be able to see 2nd doc until w1 commits.
            ASSERT(earlyCursor->restore(*shard_role_details::getRecoveryUnit(earlyReader.get())));
            ASSERT(!earlyCursor->next());

            auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
            auto opCtx = harnessHelper->newOperationContext(client2.get());
            auto cursor =
                rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
            auto record = cursor->seekExact(id1);
            ASSERT(record);
            ASSERT_EQ(id1, record->id) << stringifyForDebug(opCtx.get(), record, cursor.get());
            auto nextRecord = cursor->next();
            ASSERT(!nextRecord) << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());
        }

        {
            auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
            auto opCtx = harnessHelper->newOperationContext(client2.get());
            auto cursor =
                rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
            auto record = cursor->seek(id2, SeekableRecordCursor::BoundInclusion::kInclude);
            ASSERT_FALSE(record);
        }

        {
            auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
            auto opCtx = harnessHelper->newOperationContext(client2.get());
            auto cursor =
                rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
            auto record = cursor->seek(id3, SeekableRecordCursor::BoundInclusion::kInclude);
            ASSERT_FALSE(record);
        }

        w1.commit();
    }

    {
        auto opCtx = harnessHelper->newOperationContext();
        engine->waitForAllEarlierOplogWritesToBeVisible(opCtx.get(), rs.get());
    }

    {  // now all 3 docs should be visible
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        auto record = cursor->seekExact(id1);
        ASSERT_EQ(id1, record->id);
        auto nextRecord = cursor->next();
        ASSERT(nextRecord) << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());
        nextRecord = cursor->next();
        ASSERT(nextRecord) << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());
        nextRecord = cursor->next();
        ASSERT(!nextRecord) << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());
    }
}

TEST(RecordStoreTest, OplogVisibilityStandalone) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper(
        newRecordStoreHarnessHelper(RecordStoreHarnessHelper::Options::Standalone));
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());

    RecordId id1;

    // insert a document
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            // We must have a "ts" field with a timestamp.
            Timestamp ts(5, 1);
            BSONObj obj = BSON("ts" << ts);
            // However, the insert is not timestamped in standalone mode.
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 obj.objdata(),
                                 obj.objsize(),
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            id1 = res.getValue();
            StatusWith<RecordId> expectedId = record_id_helpers::keyForOptime(ts, KeyFormat::Long);
            ASSERT_OK(expectedId.getStatus());
            // RecordId should be extracted from 'ts' field when inserting into oplog namespace
            ASSERT(expectedId.getValue().compare(id1) == 0);

            txn.commit();
        }
    }

    // verify that we can read it
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        auto record = cursor->seekExact(id1);
        ASSERT(record);
        ASSERT_EQ(id1, record->id);
        ASSERT(!cursor->next());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        auto record = cursor->seek(RecordId(id1.getLong() + 1),
                                   SeekableRecordCursor::BoundInclusion::kInclude);
        ASSERT_FALSE(record);
    }
}
}  // namespace
}  // namespace mongo
