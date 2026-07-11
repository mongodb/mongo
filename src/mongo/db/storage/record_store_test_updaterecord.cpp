// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <memory>
#include <ostream>
#include <string>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

// Insert a record and try to update it.
TEST(RecordStoreTest, UpdateRecord) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

    std::string data = "my record";
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

    data = "updated record-";
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            Status res = rs->updateRecord(opCtx.get(), ru, loc, data.c_str(), data.size() + 1);
            ASSERT_OK(res);

            txn.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            RecordData record =
                rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc);
            ASSERT_EQUALS(data.size() + 1, static_cast<size_t>(record.size()));
            ASSERT_EQUALS(data, record.data());
        }
    }
}

// Insert multiple records and try to update them.
TEST(RecordStoreTest, UpdateMultipleRecords) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

    const int nToInsert = 10;
    RecordId locs[nToInsert];
    for (int i = 0; i < nToInsert; i++) {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            std::stringstream ss;
            ss << "record " << i;
            std::string data = ss.str();

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
            std::stringstream ss;
            ss << "update record-" << i;
            std::string data = ss.str();

            StorageWriteTransaction txn(ru);
            Status res = rs->updateRecord(opCtx.get(), ru, locs[i], data.c_str(), data.size() + 1);
            ASSERT_OK(res);

            txn.commit();
        }
    }

    for (int i = 0; i < nToInsert; i++) {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            std::stringstream ss;
            ss << "update record-" << i;
            std::string data = ss.str();

            RecordData record = rs->dataFor(
                opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), locs[i]);
            ASSERT_EQUALS(data.size() + 1, static_cast<size_t>(record.size()));
            ASSERT_EQUALS(data, record.data());
        }
    }
}

}  // namespace
}  // namespace mongo
