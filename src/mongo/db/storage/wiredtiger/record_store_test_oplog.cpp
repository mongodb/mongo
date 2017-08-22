/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/storage/record_store_test_harness.h"

#include "mongo/db/storage/record_store.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

StatusWith<RecordId> insertBSON(ServiceContext::UniqueOperationContext& opCtx,
                                std::unique_ptr<RecordStore>& rs,
                                const Timestamp& opTime) {
    BSONObj obj = BSON("ts" << opTime);
    WriteUnitOfWork wuow(opCtx.get());
    Status status = rs->oplogDiskLocRegister(opCtx.get(), opTime);
    if (!status.isOK())
        return StatusWith<RecordId>(status);
    StatusWith<RecordId> res =
        rs->insertRecord(opCtx.get(), obj.objdata(), obj.objsize(), opTime, false);
    if (res.isOK())
        wuow.commit();
    return res;
}

RecordId _oplogOrderInsertOplog(OperationContext* opCtx,
                                const std::unique_ptr<RecordStore>& rs,
                                int inc) {
    Timestamp opTime = Timestamp(5, inc);
    Status status = rs->oplogDiskLocRegister(opCtx, opTime);
    ASSERT_OK(status);
    BSONObj obj = BSON("ts" << opTime);
    StatusWith<RecordId> res = rs->insertRecord(opCtx, obj.objdata(), obj.objsize(), opTime, false);
    ASSERT_OK(res.getStatus());
    return res.getValue();
}

TEST(RecordStore_Oplog, OplogHack) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    // Use a large enough cappedMaxSize so that the limit is not reached by doing the inserts within
    // the test itself.
    const int64_t cappedMaxSize = 10 * 1024;  // 10KB
    std::unique_ptr<RecordStore> rs(
        harnessHelper->newCappedRecordStore("local.oplog.foo", cappedMaxSize, -1));
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        // always illegal
        ASSERT_EQ(insertBSON(opCtx, rs, Timestamp(2, -1)).getStatus(), ErrorCodes::BadValue);

        {
            WriteUnitOfWork wuow(opCtx.get());
            BSONObj obj = BSON("not_ts" << Timestamp(2, 1));
            ASSERT_EQ(
                rs->insertRecord(opCtx.get(), obj.objdata(), obj.objsize(), Timestamp(2, 1), false)
                    .getStatus(),
                ErrorCodes::BadValue);
        }
        {
            WriteUnitOfWork wuow(opCtx.get());
            BSONObj obj = BSON("ts"
                               << "not a Timestamp");
            ASSERT_EQ(
                rs->insertRecord(opCtx.get(), obj.objdata(), obj.objsize(), Timestamp(), false)
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

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        WriteUnitOfWork wuow(opCtx.get());
        // find start
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(0, 1)), RecordId());      // nothing <=
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 1)), RecordId(1, 2));  // between
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 2)), RecordId(2, 2));  // ==
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 3)), RecordId(2, 2));  // > highest
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        rs->cappedTruncateAfter(opCtx.get(), RecordId(2, 2), false);  // no-op
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 3)), RecordId(2, 2));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        rs->cappedTruncateAfter(opCtx.get(), RecordId(1, 2), false);  // deletes 2,2
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 3)), RecordId(1, 2));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        rs->cappedTruncateAfter(opCtx.get(), RecordId(1, 2), true);  // deletes 1,2
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 3)), RecordId(1, 1));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(rs->truncate(opCtx.get()));  // deletes 1,1 and leaves collection empty
        wuow.commit();
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 3)), RecordId());
    }
}

TEST(RecordStore_Oplog, OplogHackOnNonOplog) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    std::unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore("local.NOT_oplog.foo"));

    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

    BSONObj obj = BSON("ts" << Timestamp(2, -1));
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(
            rs->insertRecord(opCtx.get(), obj.objdata(), obj.objsize(), Timestamp(2, -1), false)
                .getStatus());
        wuow.commit();
    }
    ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(0, 1)), boost::none);
}


TEST(RecordStore_Oplog, OplogOrder) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(
        harnessHelper->newCappedRecordStore("local.oplog.rs", 100000, -1));

    RecordId id1;

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
        auto cursor = rs->getCursor(opCtx.get());
        auto record = cursor->seekExact(id1);
        ASSERT(record);
        ASSERT_EQ(id1, record->id);
        ASSERT(!cursor->next());
    }

    {
        // now we insert 2 docs, but commit the 2nd one first.
        // we make sure we can't find the 2nd until the first is committed.
        ServiceContext::UniqueOperationContext earlyReader(harnessHelper->newOperationContext());

        auto earlyCursor = rs->getCursor(earlyReader.get());
        ASSERT_EQ(earlyCursor->seekExact(id1)->id, id1);
        earlyCursor->save();
        earlyReader->recoveryUnit()->abandonSnapshot();

        auto client1 = harnessHelper->serviceContext()->makeClient("c1");
        auto t1 = harnessHelper->newOperationContext(client1.get());
        WriteUnitOfWork w1(t1.get());
        _oplogOrderInsertOplog(t1.get(), rs, 20);
        // do not commit yet

        {  // create 2nd doc
            auto client2 = harnessHelper->serviceContext()->makeClient("c2");
            auto t2 = harnessHelper->newOperationContext(client2.get());
            {
                WriteUnitOfWork w2(t2.get());
                _oplogOrderInsertOplog(t2.get(), rs, 30);
                w2.commit();
            }
        }

        {  // Other operations should not be able to see 2nd doc until w1 commits.
            earlyCursor->restore();
            ASSERT(!earlyCursor->next());

            auto client2 = harnessHelper->serviceContext()->makeClient("c2");
            auto opCtx = harnessHelper->newOperationContext(client2.get());
            auto cursor = rs->getCursor(opCtx.get());
            auto record = cursor->seekExact(id1);
            ASSERT_EQ(id1, record->id);
            ASSERT(!cursor->next());
        }

        w1.commit();
    }

    rs->waitForAllEarlierOplogWritesToBeVisible(harnessHelper->newOperationContext().get());

    {  // now all 3 docs should be visible
        auto client2 = harnessHelper->serviceContext()->makeClient("c2");
        auto opCtx = harnessHelper->newOperationContext(client2.get());
        auto cursor = rs->getCursor(opCtx.get());
        auto record = cursor->seekExact(id1);
        ASSERT_EQ(id1, record->id);
        ASSERT(cursor->next());
        ASSERT(cursor->next());
        ASSERT(!cursor->next());
    }

    // Rollback the last two oplog entries, then insert entries with older optimes and ensure that
    // the visibility rules aren't violated. See SERVER-21645
    {
        auto client2 = harnessHelper->serviceContext()->makeClient("c2");
        auto opCtx = harnessHelper->newOperationContext(client2.get());
        rs->cappedTruncateAfter(opCtx.get(), id1, /*inclusive*/ false);
    }

    rs->waitForAllEarlierOplogWritesToBeVisible(harnessHelper->newOperationContext().get());

    {
        // Now we insert 2 docs with timestamps earlier than before, but commit the 2nd one first.
        // We make sure we can't find the 2nd until the first is committed.
        ServiceContext::UniqueOperationContext earlyReader(harnessHelper->newOperationContext());
        auto earlyCursor = rs->getCursor(earlyReader.get());
        ASSERT_EQ(earlyCursor->seekExact(id1)->id, id1);
        earlyCursor->save();
        earlyReader->recoveryUnit()->abandonSnapshot();

        auto client1 = harnessHelper->serviceContext()->makeClient("c1");
        auto t1 = harnessHelper->newOperationContext(client1.get());
        WriteUnitOfWork w1(t1.get());
        _oplogOrderInsertOplog(t1.get(), rs, 2);
        // do not commit yet

        {  // create 2nd doc
            auto client2 = harnessHelper->serviceContext()->makeClient("c2");
            auto t2 = harnessHelper->newOperationContext(client2.get());
            {
                WriteUnitOfWork w2(t2.get());
                _oplogOrderInsertOplog(t2.get(), rs, 3);
                w2.commit();
            }
        }

        {  // Other operations should not be able to see 2nd doc until w1 commits.
            ASSERT(earlyCursor->restore());
            ASSERT(!earlyCursor->next());

            auto client2 = harnessHelper->serviceContext()->makeClient("c2");
            auto opCtx = harnessHelper->newOperationContext(client2.get());
            auto cursor = rs->getCursor(opCtx.get());
            auto record = cursor->seekExact(id1);
            ASSERT_EQ(id1, record->id);
            ASSERT(!cursor->next());
        }

        w1.commit();
    }

    rs->waitForAllEarlierOplogWritesToBeVisible(harnessHelper->newOperationContext().get());

    {  // now all 3 docs should be visible
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get());
        auto record = cursor->seekExact(id1);
        ASSERT_EQ(id1, record->id);
        ASSERT(cursor->next());
        ASSERT(cursor->next());
        ASSERT(!cursor->next());
    }
}
}  // namespace
}  // namespace mongo
