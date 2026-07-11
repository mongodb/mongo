// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <ostream>
#include <string>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

// Verify that a nonempty collection maybe takes up some space on disk.
TEST(RecordStoreTest, StorageSizeNonEmpty) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

    int nToInsert = 10;
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
            txn.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(nToInsert, rs->numRecords());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        ASSERT(rs->storageSize(ru, nullptr) >= 0);
    }
}

}  // namespace
}  // namespace mongo
