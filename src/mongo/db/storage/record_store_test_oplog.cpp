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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <memory>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

StatusWith<RecordId> insertBSON(ServiceContext::UniqueOperationContext& opCtx,
                                std::unique_ptr<RecordStore>& rs,
                                const Timestamp& opTime) {
    BSONObj obj = BSON("ts" << opTime);
    WriteUnitOfWork wuow(opCtx.get());
    Status status = rs->oplogDiskLocRegister(opCtx.get(), opTime, false);
    if (!status.isOK())
        return StatusWith<RecordId>(status);
    StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), obj.objdata(), obj.objsize(), opTime);
    if (res.isOK())
        wuow.commit();
    return res;
}

RecordId _oplogOrderInsertOplog(OperationContext* opCtx,
                                const std::unique_ptr<RecordStore>& rs,
                                int inc) {
    Timestamp opTime = Timestamp(5, inc);
    Status status = rs->oplogDiskLocRegister(opCtx, opTime, false);
    ASSERT_OK(status);
    BSONObj obj = BSON("ts" << opTime);
    StatusWith<RecordId> res = rs->insertRecord(opCtx, obj.objdata(), obj.objsize(), opTime);
    ASSERT_OK(res.getStatus());
    return res.getValue();
}

TEST(RecordStoreTestHarness, SeekNearOplog) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        // always illegal
        ASSERT_EQ(insertBSON(opCtx, rs, Timestamp(2, -1)).getStatus(), ErrorCodes::BadValue);

        {
            WriteUnitOfWork wuow(opCtx.get());
            BSONObj obj = BSON("not_ts" << Timestamp(2, 1));
            ASSERT_EQ(rs->insertRecord(opCtx.get(), obj.objdata(), obj.objsize(), Timestamp())
                          .getStatus(),
                      ErrorCodes::BadValue);
        }
        {
            WriteUnitOfWork wuow(opCtx.get());
            BSONObj obj = BSON("ts"
                               << "not a Timestamp");
            ASSERT_EQ(rs->insertRecord(opCtx.get(), obj.objdata(), obj.objsize(), Timestamp())
                          .getStatus(),
                      ErrorCodes::BadValue);
        }

        ASSERT_EQ(insertBSON(opCtx, rs, Timestamp(-2, 1)).getStatus(), ErrorCodes::BadValue);

        // success cases
        ASSERT_EQ(insertBSON(opCtx, rs, Timestamp(1, 1)).getValue(), RecordId(1, 1));
        ASSERT_EQ(insertBSON(opCtx, rs, Timestamp(1, 2)).getValue(), RecordId(1, 2));
        ASSERT_EQ(insertBSON(opCtx, rs, Timestamp(2, 2)).getValue(), RecordId(2, 2));
    }

    // Make sure all are visible.
    rs->waitForAllEarlierOplogWritesToBeVisible(harnessHelper->newOperationContext().get());

    // Forward cursor seeks
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        WriteUnitOfWork wuow(opCtx.get());
        auto cur = rs->getCursor(opCtx.get());
        auto rec = cur->seekNear(RecordId(0, 1));
        ASSERT(rec);
        ASSERT_EQ(rec->id, RecordId(1, 1));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        WriteUnitOfWork wuow(opCtx.get());
        auto cur = rs->getCursor(opCtx.get());
        auto rec = cur->seekNear(RecordId(2, 1));
        ASSERT(rec);
        ASSERT_EQ(rec->id, RecordId(1, 2));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        WriteUnitOfWork wuow(opCtx.get());
        auto cur = rs->getCursor(opCtx.get());
        auto rec = cur->seekNear(RecordId(2, 2));
        ASSERT(rec);
        ASSERT_EQ(rec->id, RecordId(2, 2));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        WriteUnitOfWork wuow(opCtx.get());
        auto cur = rs->getCursor(opCtx.get());
        auto rec = cur->seekNear(RecordId(2, 3));
        ASSERT(rec);
        ASSERT_EQ(rec->id, RecordId(2, 2));
    }

    // Reverse cursor seeks
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        WriteUnitOfWork wuow(opCtx.get());
        auto cur = rs->getCursor(opCtx.get(), false /* forward */);
        auto rec = cur->seekNear(RecordId(0, 1));
        ASSERT(rec);
        ASSERT_EQ(rec->id, RecordId(1, 1));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        WriteUnitOfWork wuow(opCtx.get());
        auto cur = rs->getCursor(opCtx.get(), false /* forward */);
        auto rec = cur->seekNear(RecordId(2, 1));
        ASSERT(rec);
        ASSERT_EQ(rec->id, RecordId(2, 2));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        WriteUnitOfWork wuow(opCtx.get());
        auto cur = rs->getCursor(opCtx.get(), false /* forward */);
        auto rec = cur->seekNear(RecordId(2, 2));
        ASSERT(rec);
        ASSERT_EQ(rec->id, RecordId(2, 2));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        WriteUnitOfWork wuow(opCtx.get());
        auto cur = rs->getCursor(opCtx.get(), false /* forward */);
        auto rec = cur->seekNear(RecordId(2, 3));
        ASSERT(rec);
        ASSERT_EQ(rec->id, RecordId(2, 2));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        rs->cappedTruncateAfter(opCtx.get(),
                                RecordId(2, 2),
                                false /* inclusive */,
                                nullptr /* aboutToDelete callback */);  // no-op
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        auto cur = rs->getCursor(opCtx.get());
        auto rec = cur->seekNear(RecordId(2, 3));
        ASSERT(rec);
        ASSERT_EQ(rec->id, RecordId(2, 2));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        rs->cappedTruncateAfter(opCtx.get(),
                                RecordId(1, 2),
                                false /* inclusive */,
                                nullptr /* aboutToDelete callback */);  // deletes 2,2
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        auto cur = rs->getCursor(opCtx.get());
        auto rec = cur->seekNear(RecordId(2, 3));
        ASSERT(rec);
        ASSERT_EQ(rec->id, RecordId(1, 2));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        rs->cappedTruncateAfter(opCtx.get(),
                                RecordId(1, 2),
                                true /* inclusive */,
                                nullptr /* aboutToDelete callback */);  // deletes 1,2
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        auto cur = rs->getCursor(opCtx.get());
        auto rec = cur->seekNear(RecordId(2, 3));
        ASSERT(rec);
        ASSERT_EQ(rec->id, RecordId(1, 1));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(rs->truncate(opCtx.get()));  // deletes 1,1 and leaves collection empty
        wuow.commit();
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        auto cur = rs->getCursor(opCtx.get());
        auto rec = cur->seekNear(RecordId(2, 3));
        ASSERT_FALSE(rec);
    }
}

TEST(RecordStoreTestHarness, OplogInsertOutOfOrder) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        // RecordId's are inserted out-of-order.
        ASSERT_EQ(insertBSON(opCtx, rs, Timestamp(1, 1)).getValue(), RecordId(1, 1));
        ASSERT_EQ(insertBSON(opCtx, rs, Timestamp(2, 2)).getValue(), RecordId(2, 2));
        ASSERT_EQ(insertBSON(opCtx, rs, Timestamp(1, 2)).getValue(), RecordId(1, 2));
    }
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        rs->waitForAllEarlierOplogWritesToBeVisible(opCtx.get());
        auto cursor = rs->getCursor(opCtx.get());
        ASSERT_EQ(cursor->next()->id, RecordId(1, 1));
        ASSERT_EQ(cursor->next()->id, RecordId(1, 2));
        ASSERT_EQ(cursor->next()->id, RecordId(2, 2));
        ASSERT(!cursor->next());
    }
}

TEST(RecordStoreTestHarness, SeekNearOnNonOplog) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    CollectionOptions options;
    options.capped = true;
    auto rs = harnessHelper->newRecordStore("local.NOT_oplog.foo", options);

    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    Lock::GlobalLock globalLock(opCtx.get(), MODE_X);

    BSONObj obj = BSON("ts" << Timestamp(2, -1));
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(rs->insertRecord(opCtx.get(), obj.objdata(), obj.objsize(), Timestamp(2, -1))
                      .getStatus());
        wuow.commit();
    }
    auto cur = rs->getCursor(opCtx.get());
    auto rec = cur->seekNear(RecordId(0, 1));
    ASSERT(rec);
    // Regular record stores don't use timestamps for their RecordId, so expect the first
    // auto-incrementing RecordId to be 1.
    ASSERT_EQ(rec->id, RecordId(1));
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

TEST(RecordStoreTestHarness, OplogOrder) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());

    RecordId id1, id2, id3;

    {  // first insert a document
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            id1 = _oplogOrderInsertOplog(opCtx.get(), rs, 1);
            uow.commit();
        }
    }

    // Make sure it is visible.
    rs->waitForAllEarlierOplogWritesToBeVisible(harnessHelper->newOperationContext().get());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        auto cursor = rs->getCursor(opCtx.get());
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
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        auto cursor = rs->getCursor(opCtx.get());
        auto record = cursor->seekNear(RecordId(id1.getLong() + 1));
        ASSERT(record);
        ASSERT_EQ(id1, record->id);
        ASSERT(!cursor->next());
    }

    {
        // now we insert 2 docs, but commit the 2nd one first.
        // we make sure we can't find the 2nd until the first is committed.
        ServiceContext::UniqueOperationContext earlyReader(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(earlyReader.get(), MODE_IS);
        auto earlyCursor = rs->getCursor(earlyReader.get());
        ASSERT_EQ(earlyCursor->seekExact(id1)->id, id1);
        earlyCursor->save();
        shard_role_details::getRecoveryUnit(earlyReader.get())->abandonSnapshot();

        auto client1 = harnessHelper->serviceContext()->getService()->makeClient("c1");
        auto t1 = harnessHelper->newOperationContext(client1.get());
        WriteUnitOfWork w1(t1.get());
        id2 = _oplogOrderInsertOplog(t1.get(), rs, 20);
        // do not commit yet

        {  // create 2nd doc
            auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
            auto t2 = harnessHelper->newOperationContext(client2.get());
            {
                WriteUnitOfWork w2(t2.get());
                id3 = _oplogOrderInsertOplog(t2.get(), rs, 30);
                w2.commit();
            }
        }

        {  // Other operations should not be able to see 2nd doc until w1 commits.
            earlyCursor->restore();
            ASSERT(!earlyCursor->next());

            auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
            auto opCtx = harnessHelper->newOperationContext(client2.get());
            Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
            auto cursor = rs->getCursor(opCtx.get());
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

            // seekExact and seekNear should still work after seek
            record = cursor->seekExact(id1);
            ASSERT(record) << stringifyForDebug(opCtx.get(), record, cursor.get());
            ASSERT_EQ(id1, record->id) << stringifyForDebug(opCtx.get(), record, cursor.get());
            record = cursor->seekNear(id1);
            ASSERT(record) << stringifyForDebug(opCtx.get(), record, cursor.get());
            ASSERT_EQ(id1, record->id) << stringifyForDebug(opCtx.get(), record, cursor.get());
        }

        {
            auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
            auto opCtx = harnessHelper->newOperationContext(client2.get());
            Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
            auto cursor = rs->getCursor(opCtx.get());
            auto record = cursor->seekNear(id2);
            ASSERT(record) << stringifyForDebug(opCtx.get(), record, cursor.get());
            ASSERT_EQ(id1, record->id) << stringifyForDebug(opCtx.get(), record, cursor.get());
            auto nextRecord = cursor->next();
            ASSERT(!nextRecord) << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());
        }

        {
            auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
            auto opCtx = harnessHelper->newOperationContext(client2.get());
            Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
            auto cursor = rs->getCursor(opCtx.get());
            auto record = cursor->seekNear(id3);
            ASSERT(record) << stringifyForDebug(opCtx.get(), record, cursor.get());
            ASSERT_EQ(id1, record->id) << stringifyForDebug(opCtx.get(), record, cursor.get());
            auto nextRecord = cursor->next();
            ASSERT(!nextRecord) << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());
        }

        {  // Test reverse cursors and visibility
            auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
            auto opCtx = harnessHelper->newOperationContext(client2.get());
            Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
            auto cursor = rs->getCursor(opCtx.get(), false);
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

    rs->waitForAllEarlierOplogWritesToBeVisible(harnessHelper->newOperationContext().get());

    {  // now all 3 docs should be visible
        auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
        auto opCtx = harnessHelper->newOperationContext(client2.get());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        auto cursor = rs->getCursor(opCtx.get());
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
        rs->cappedTruncateAfter(
            opCtx.get(), id1, false /* inclusive */, nullptr /* aboutToDelete callback */);
    }

    rs->waitForAllEarlierOplogWritesToBeVisible(harnessHelper->newOperationContext().get());

    {
        // Now we insert 2 docs with timestamps earlier than before, but commit the 2nd one first.
        // We make sure we can't find the 2nd until the first is committed.
        ServiceContext::UniqueOperationContext earlyReader(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(earlyReader.get(), MODE_IS);
        auto earlyCursor = rs->getCursor(earlyReader.get());
        ASSERT_EQ(earlyCursor->seekExact(id1)->id, id1);
        earlyCursor->save();
        shard_role_details::getRecoveryUnit(earlyReader.get())->abandonSnapshot();

        auto client1 = harnessHelper->serviceContext()->getService()->makeClient("c1");
        auto t1 = harnessHelper->newOperationContext(client1.get());
        WriteUnitOfWork w1(t1.get());
        RecordId id2 = _oplogOrderInsertOplog(t1.get(), rs, 2);

        // do not commit yet

        RecordId id3;
        {  // create 2nd doc
            auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
            auto t2 = harnessHelper->newOperationContext(client2.get());
            {
                WriteUnitOfWork w2(t2.get());
                id3 = _oplogOrderInsertOplog(t2.get(), rs, 3);
                w2.commit();
            }
        }

        {  // Other operations should not be able to see 2nd doc until w1 commits.
            ASSERT(earlyCursor->restore());
            ASSERT(!earlyCursor->next());

            auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
            auto opCtx = harnessHelper->newOperationContext(client2.get());
            auto cursor = rs->getCursor(opCtx.get());
            auto record = cursor->seekExact(id1);
            ASSERT(record);
            ASSERT_EQ(id1, record->id) << stringifyForDebug(opCtx.get(), record, cursor.get());
            auto nextRecord = cursor->next();
            ASSERT(!nextRecord) << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());
        }

        {
            auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
            auto opCtx = harnessHelper->newOperationContext(client2.get());
            Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
            auto cursor = rs->getCursor(opCtx.get());
            auto record = cursor->seekNear(id2);
            ASSERT(record);
            ASSERT_EQ(id1, record->id) << stringifyForDebug(opCtx.get(), record, cursor.get());
            auto nextRecord = cursor->next();
            ASSERT(!nextRecord) << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());
        }

        {
            auto client2 = harnessHelper->serviceContext()->getService()->makeClient("c2");
            auto opCtx = harnessHelper->newOperationContext(client2.get());
            Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
            auto cursor = rs->getCursor(opCtx.get());
            auto record = cursor->seekNear(id3);
            ASSERT(record);
            ASSERT_EQ(id1, record->id) << stringifyForDebug(opCtx.get(), record, cursor.get());
            auto nextRecord = cursor->next();
            ASSERT(!nextRecord) << stringifyForDebug(opCtx.get(), nextRecord, cursor.get());
        }

        w1.commit();
    }

    rs->waitForAllEarlierOplogWritesToBeVisible(harnessHelper->newOperationContext().get());

    {  // now all 3 docs should be visible
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get());
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

TEST(RecordStoreTestHarness, OplogVisibilityStandalone) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper(
        newRecordStoreHarnessHelper(RecordStoreHarnessHelper::Options::Standalone));
    std::unique_ptr<RecordStore> rs(harnessHelper->newOplogRecordStore());

    RecordId id1;

    // insert a document
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            // We must have a "ts" field with a timestamp.
            Timestamp ts(5, 1);
            BSONObj obj = BSON("ts" << ts);
            // However, the insert is not timestamped in standalone mode.
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), obj.objdata(), obj.objsize(), Timestamp());
            ASSERT_OK(res.getStatus());
            id1 = res.getValue();
            StatusWith<RecordId> expectedId = record_id_helpers::keyForOptime(ts, KeyFormat::Long);
            ASSERT_OK(expectedId.getStatus());
            // RecordId should be extracted from 'ts' field when inserting into oplog namespace
            ASSERT(expectedId.getValue().compare(id1) == 0);

            uow.commit();
        }
    }

    // verify that we can read it
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get());
        auto record = cursor->seekExact(id1);
        ASSERT(record);
        ASSERT_EQ(id1, record->id);
        ASSERT(!cursor->next());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        auto cursor = rs->getCursor(opCtx.get());
        auto record = cursor->seekNear(RecordId(id1.getLong() + 1));
        ASSERT(record);
        ASSERT_EQ(id1, record->id);
        ASSERT(!cursor->next());
    }
}
}  // namespace
}  // namespace mongo
