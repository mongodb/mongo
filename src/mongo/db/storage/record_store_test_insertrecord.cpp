// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <ostream>
#include <string>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

using std::string;
using std::stringstream;
using std::unique_ptr;

// Insert a record and verify the number of entries in the collection is 1.
TEST(RecordStoreTest, InsertRecord) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords());
    }

    string data = "my record";
    RecordId loc;
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 data.c_str(),
                                 data.size() + 1,
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            txn.commit();
        }
    }

    ASSERT_EQUALS(1, rs->numRecords());
}

// Insert multiple records and verify the number of entries in the collection
// equals the number that were inserted.
TEST(RecordStoreTest, InsertMultipleRecords) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

    const int nToInsert = 10;
    RecordId locs[nToInsert];
    for (int i = 0; i < nToInsert; i++) {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            stringstream ss;
            ss << "record " << i;
            string data = ss.str();

            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 data.c_str(),
                                 data.size() + 1,
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            locs[i] = res.getValue();
            txn.commit();
        }
    }

    ASSERT_EQUALS(nToInsert, rs->numRecords());
}

// Insert a record with duplicated key and verify we got a DuplicateKeyError when allowOverwrite is
// false
TEST(RecordStoreTest, InsertRecordDuplicateKey) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(
        harnessHelper->newRecordStore(RecordStore::Options{.allowOverwrite = false}));
    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    ASSERT_EQUALS(0, rs->numRecords());

    // Insert an initial record
    auto data = BSON("_id" << 1);
    RecordId loc;
    {
        StorageWriteTransaction txn(ru);
        StatusWith<RecordId> res =
            rs->insertRecord(opCtx.get(), ru, data.objdata(), data.objsize(), Timestamp());
        ASSERT_OK(res.getStatus());
        loc = res.getValue();
        txn.commit();
    }
    ASSERT_EQUALS(1, rs->numRecords());

    // Insert another record with the same record id
    auto newData = BSON("_id" << 2);
    {
        StorageWriteTransaction txn(ru);
        StatusWith<RecordId> res =
            rs->insertRecord(opCtx.get(),
                             ru,
                             loc,  // We are reusing the same record id as the previous insert
                             newData.objdata(),
                             newData.objsize(),
                             Timestamp());
        ASSERT_EQUALS(ErrorCodes::DuplicateKey, res.getStatus());
        txn.abort();
    }
    ASSERT_EQUALS(1, rs->numRecords());

    // Verify that the data was unchanged
    auto cursor = rs->getCursor(opCtx.get(), ru);
    auto record = cursor->next();
    ASSERT(record);
    ASSERT_EQUALS(0, record->data.toBson().woCompare(data));
    ASSERT(!cursor->next());
}

}  // namespace
}  // namespace mongo
