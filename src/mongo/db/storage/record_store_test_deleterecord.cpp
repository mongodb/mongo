// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/unittest/death_test.h"
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

// Insert a record and try to delete it.
TEST(RecordStoreTest, DeleteRecord) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

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

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        {
            StorageWriteTransaction txn(ru);
            rs->deleteRecord(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc);
            txn.commit();
        }
    }

    ASSERT_EQUALS(0, rs->numRecords());
}

// Insert multiple records and try to delete them.
TEST(RecordStoreTest, DeleteMultipleRecords) {
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

    for (int i = 0; i < nToInsert; i++) {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            rs->deleteRecord(
                opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), locs[i]);
            txn.commit();
        }
    }

    ASSERT_EQUALS(0, rs->numRecords());
}

// Delete a non-existent record and expect it to crash with a log message.
DEATH_TEST_REGEX(RecordStoreTestDeathTest,
                 DeleteNonExistentRecord,
                 "Record to be deleted not found") {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    string data = "my record";
    RecordId loc;
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
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

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        StorageWriteTransaction txn(ru);
        // Should crash with a log message.
        rs->deleteRecord(opCtx.get(),
                         *shard_role_details::getRecoveryUnit(opCtx.get()),
                         RecordId(loc.getLong() + 1));
        MONGO_UNREACHABLE;
    }
}

}  // namespace
}  // namespace mongo
