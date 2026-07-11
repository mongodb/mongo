// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/write_unit_of_work.h"
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

// Verify that calling truncate() on an already empty collection returns an OK status.
TEST(RecordStoreTest, TruncateEmpty) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            ASSERT_OK(rs->truncate(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get())));
            txn.commit();
        }
    }

    ASSERT_EQUALS(0, rs->numRecords());
}

// Insert multiple records, and verify that calling truncate() on a nonempty collection
// removes all of them and returns an OK status.
TEST(RecordStoreTest, TruncateNonEmpty) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

    int nToInsert = 10;
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
            txn.commit();
        }
    }

    ASSERT_EQUALS(nToInsert, rs->numRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            ASSERT_OK(rs->truncate(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get())));
            txn.commit();
        }
    }

    ASSERT_EQUALS(0, rs->numRecords());
}

DEATH_TEST(RecordStoreTestDeathTest,
           RangeTruncateMustHaveBoundsTest,
           "Ranged truncate must have one bound defined") {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    auto opCtx = harnessHelper->newOperationContext();

    auto result = rs->rangeTruncate(opCtx.get(),
                                    *shard_role_details::getRecoveryUnit(opCtx.get()),
                                    RecordId(),
                                    RecordId(),
                                    0,
                                    0);
}
}  // namespace
}  // namespace mongo
