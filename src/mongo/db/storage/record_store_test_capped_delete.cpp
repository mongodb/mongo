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

#include "mongo/db/storage/record_store_test_harness.h"


#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/unittest/unittest.h"


namespace mongo {
namespace {

using std::string;
using std::stringstream;
using std::unique_ptr;

// Insert a record in a store with capped max docs 1,  and try to delete it by inserting another.
TEST(RecordStoreTestHarness, CappedDeleteRecord) {
    const auto harness(newRecordStoreHarnessHelper());
    if (!harness->supportsDocLocking())
        return;
    auto rs(harness->newCappedRecordStore(RecordStoreHarnessHelper::kDefaultCapedSizeBytes,
                                          /*cappedMaxDocs*/ 1));

    {
        ServiceContext::UniqueOperationContext opCtx(harness->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    string data = "my record";
    RecordId loc1, loc2;
    {
        ServiceContext::UniqueOperationContext opCtx(harness->newOperationContext());
        WriteUnitOfWork uow(opCtx.get());
        StatusWith<RecordId> res =
            rs->insertRecord(opCtx.get(), data.c_str(), data.size() + 1, Timestamp());
        ASSERT_OK(res.getStatus());
        loc1 = res.getValue();
        uow.commit();
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harness->newOperationContext());
        ASSERT_EQUALS(1, rs->numRecords(opCtx.get()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harness->newOperationContext());
        WriteUnitOfWork uow(opCtx.get());
        StatusWith<RecordId> res =
            rs->insertRecord(opCtx.get(), data.c_str(), data.size() + 1, Timestamp());
        ASSERT_OK(res.getStatus());
        loc2 = res.getValue();
        ASSERT_GT(loc2, loc1);
        uow.commit();
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harness->newOperationContext());
        ASSERT_EQUALS(1, rs->numRecords(opCtx.get()));
    }
}

// Insert multiple records at once, requiring multiple deletes.
TEST(RecordStoreTestHarness, DeleteMultipleRecords) {
    const auto harness(newRecordStoreHarnessHelper());
    if (!harness->supportsDocLocking())
        return;
    const int cappedMaxDocs = 10;
    auto rs(harness->newCappedRecordStore(RecordStoreHarnessHelper::kDefaultCapedSizeBytes,
                                          cappedMaxDocs));

    const int nToInsertFirst = cappedMaxDocs / 2;
    const int nToInsertSecond = cappedMaxDocs;
    RecordId lastLoc = RecordId();

    // First insert some records that fit without exceeding the cap.
    {
        ServiceContext::UniqueOperationContext opCtx(harness->newOperationContext());
        WriteUnitOfWork uow(opCtx.get());
        for (int i = 0; i < nToInsertFirst; i++) {
            stringstream ss;
            ss << "record " << i;
            string data = ss.str();

            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), data.c_str(), data.size() + 1, Timestamp());
            ASSERT_OK(res.getStatus());
            RecordId loc = res.getValue();
            ASSERT_GT(loc, lastLoc);
            lastLoc = loc;
        }
        uow.commit();
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harness->newOperationContext());
        ASSERT_EQUALS(nToInsertFirst, rs->numRecords(opCtx.get()));
    }

    // Then insert the largest batch possible (number of docs equal to the cap), causing deletes.
    {
        ServiceContext::UniqueOperationContext opCtx(harness->newOperationContext());
        WriteUnitOfWork uow(opCtx.get());
        for (int i = nToInsertFirst; i < nToInsertFirst + nToInsertSecond; i++) {
            stringstream ss;
            ss << "record " << i;
            string data = ss.str();

            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), data.c_str(), data.size() + 1, Timestamp());
            ASSERT_OK(res.getStatus());
            RecordId loc = res.getValue();
            ASSERT_GT(loc, lastLoc);
            lastLoc = loc;
        }
        uow.commit();
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harness->newOperationContext());
        ASSERT_EQUALS(cappedMaxDocs, rs->numRecords(opCtx.get()));
    }
}

}  // namespace
}  // namespace mongo
