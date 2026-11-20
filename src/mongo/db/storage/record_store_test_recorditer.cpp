// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <initializer_list>
#include <memory>
#include <ostream>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <gtest/gtest.h>

namespace mongo {
namespace {

class RecordStoreTestWithBatching : public testing::TestWithParam<size_t> {};

// Insert multiple records and iterate through them in the forward direction.
// When curr() or getNext() is called on an iterator positioned at EOF,
// the iterator returns RecordId() and stays at EOF.
TEST_P(RecordStoreTestWithBatching, IterateOverMultipleRecords) {
    const auto maxBatch = GetParam();
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQ(0, rs->approximateNumRecords());

    const int nToInsert = 10;
    RecordId locs[nToInsert];
    std::string datas[nToInsert];
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
            datas[i] = data;
            txn.commit();
        }
    }

    ASSERT_EQ(nToInsert, rs->approximateNumRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        for (int i = 0; i < nToInsert; i++) {
            const auto record = cursor->next(maxBatch);
            ASSERT_TRUE(record);
            ASSERT_EQ(locs[i], record->id);
            ASSERT_EQ(datas[i], record->data.data());
        }
        ASSERT_FALSE(cursor->next(maxBatch));
    }
}

// Insert multiple records and iterate through them in the reverse direction.
// When curr() or getNext() is called on an iterator positioned at EOF,
// the iterator returns RecordId() and stays at EOF.
TEST_P(RecordStoreTestWithBatching, IterateOverMultipleRecordsReversed) {
    const auto maxBatch = GetParam();
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQ(0, rs->approximateNumRecords());

    const int nToInsert = 10;
    RecordId locs[nToInsert];
    std::string datas[nToInsert];
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
            datas[i] = data;
            txn.commit();
        }
    }

    ASSERT_EQ(nToInsert, rs->approximateNumRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        auto cursor =
            rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), false);
        for (int i = nToInsert - 1; i >= 0; i--) {
            const auto record = cursor->next(maxBatch);
            ASSERT_TRUE(record);
            ASSERT_EQ(locs[i], record->id);
            ASSERT_EQ(datas[i], record->data.data());
        }
        ASSERT_FALSE(cursor->next(maxBatch));
    }
}

// Insert multiple records and try to create a forward iterator
// starting at an interior position.
TEST_P(RecordStoreTestWithBatching, IterateStartFromMiddle) {
    const auto maxBatch = GetParam();
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQ(0, rs->approximateNumRecords());

    const int nToInsert = 10;
    RecordId locs[nToInsert];
    std::string datas[nToInsert];
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
            datas[i] = data;
            txn.commit();
        }
    }

    ASSERT_EQ(nToInsert, rs->approximateNumRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        const int start = nToInsert / 2;
        auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        for (int i = start; i < nToInsert; i++) {
            const auto record = (i == start) ? cursor->seekExact(locs[i]) : cursor->next(maxBatch);
            ASSERT_TRUE(record);
            ASSERT_EQ(locs[i], record->id);
            ASSERT_EQ(datas[i], record->data.data());
        }
        ASSERT_FALSE(cursor->next(maxBatch));
    }
}

// Insert multiple records and try to create a reverse iterator
// starting at an interior position.
TEST_P(RecordStoreTestWithBatching, IterateStartFromMiddleReversed) {
    const auto maxBatch = GetParam();
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQ(0, rs->approximateNumRecords());

    const int nToInsert = 10;
    RecordId locs[nToInsert];
    std::string datas[nToInsert];
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
            datas[i] = data;
            txn.commit();
        }
    }

    ASSERT_EQ(nToInsert, rs->approximateNumRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        int start = nToInsert / 2;
        auto cursor =
            rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), false);
        for (int i = start; i >= 0; i--) {
            const auto record = (i == start) ? cursor->seekExact(locs[i]) : cursor->next(maxBatch);
            ASSERT_TRUE(record);
            ASSERT_EQ(locs[i], record->id);
            ASSERT_EQ(datas[i], record->data.data());
        }
        ASSERT_FALSE(cursor->next(maxBatch));
    }
}

// Insert several records, and iterate to the end. Ensure that the record iterator
// is EOF. Add an additional record, saving and restoring the iterator state, and check
// that the iterator remains EOF.
TEST_P(RecordStoreTestWithBatching, RecordIteratorEOF) {
    const auto maxBatch = GetParam();
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQ(0, rs->approximateNumRecords());

    const int nToInsert = 10;
    RecordId locs[nToInsert];
    std::string datas[nToInsert];
    for (int i = 0; i < nToInsert; i++) {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StringBuilder sb;
            sb << "record " << i;
            std::string data = sb.str();

            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 data.c_str(),
                                 data.size() + 1,
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            locs[i] = res.getValue();
            datas[i] = data;
            txn.commit();
        }
    }

    ASSERT_EQ(nToInsert, rs->approximateNumRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        // Get a forward iterator starting at the beginning of the record store.
        auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));

        // Iterate, checking EOF along the way.
        for (int i = 0; i < nToInsert; i++) {
            const auto record = cursor->next(maxBatch);
            ASSERT_TRUE(record);
            ASSERT_EQ(locs[i], record->id);
            ASSERT_EQ(datas[i], record->data.data());
        }
        ASSERT_FALSE(cursor->next(maxBatch));

        // Add a record and ensure we're still EOF.
        cursor->save();

        StringBuilder sb;
        sb << "record " << nToInsert + 1;
        std::string data = sb.str();

        StorageWriteTransaction txn(ru);
        StatusWith<RecordId> res =
            rs->insertRecord(opCtx.get(),
                             *shard_role_details::getRecoveryUnit(opCtx.get()),
                             data.c_str(),
                             data.size() + 1,
                             Timestamp());
        ASSERT_OK(res.getStatus());
        txn.commit();

        ASSERT_TRUE(cursor->restore(ru));

        // Iterator should still be EOF.
        ASSERT_FALSE(cursor->next(maxBatch));
        ASSERT_FALSE(cursor->next(maxBatch));
    }
}

// Test calling save and restore after each call to next
TEST_P(RecordStoreTestWithBatching, RecordIteratorSaveRestore) {
    const auto maxBatch = GetParam();
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQ(0, rs->approximateNumRecords());

    const int nToInsert = 10;
    RecordId locs[nToInsert];
    std::string datas[nToInsert];
    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    for (int i = 0; i < nToInsert; i++) {
        {
            StringBuilder sb;
            sb << "record " << i;
            std::string data = sb.str();

            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 data.c_str(),
                                 data.size() + 1,
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            locs[i] = res.getValue();
            datas[i] = data;
            txn.commit();
        }
    }

    ASSERT_EQ(nToInsert, rs->approximateNumRecords());

    {
        // Get a forward iterator starting at the beginning of the record store.
        auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));

        // Iterate, checking EOF along the way.
        for (int i = 0; i < nToInsert; i++) {
            cursor->save();
            cursor->save();  // It is legal to save twice in a row.
            cursor->restore(ru);

            const auto record = cursor->next(maxBatch);
            ASSERT_TRUE(record);
            ASSERT_EQ(locs[i], record->id);
            ASSERT_EQ(datas[i], record->data.data());
        }

        cursor->save();
        cursor->save();  // It is legal to save twice in a row.
        cursor->restore(ru);

        ASSERT_FALSE(cursor->next(maxBatch));
    }
}

// Insert two records, and iterate a cursor to EOF. Seek the same cursor to the first and ensure
// that next() returns the second record.
TEST_P(RecordStoreTestWithBatching, SeekAfterEofAndContinue) {
    const auto maxBatch = GetParam();
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    const int nToInsert = 2;
    RecordId locs[nToInsert];
    std::string datas[nToInsert];
    for (int i = 0; i < nToInsert; i++) {
        StringBuilder sb;
        sb << "record " << i;
        std::string data = sb.str();

        StorageWriteTransaction txn(ru);
        StatusWith<RecordId> res =
            rs->insertRecord(opCtx.get(),
                             *shard_role_details::getRecoveryUnit(opCtx.get()),
                             data.c_str(),
                             data.size() + 1,
                             Timestamp());
        ASSERT_OK(res.getStatus());
        locs[i] = res.getValue();
        datas[i] = data;
        txn.commit();
    }


    // Get a forward iterator starting at the beginning of the record store.
    auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));

    // Iterate, checking EOF along the way.
    for (int i = 0; i < nToInsert; i++) {
        const auto record = cursor->next(maxBatch);
        ASSERT_TRUE(record);
        ASSERT_EQ(locs[i], record->id);
        ASSERT_EQ(datas[i], record->data.data());
    }
    ASSERT_FALSE(cursor->next(maxBatch));

    {
        const auto record = cursor->seekExact(locs[0]);
        ASSERT_TRUE(record);
        ASSERT_EQ(locs[0], record->id);
        ASSERT_EQ(datas[0], record->data.data());
    }

    {
        const auto record = cursor->next(maxBatch);
        ASSERT_TRUE(record);
        ASSERT_EQ(locs[1], record->id);
        ASSERT_EQ(datas[1], record->data.data());
    }

    ASSERT_FALSE(cursor->next(maxBatch));
}

INSTANTIATE_TEST_SUITE_P(IterTests,
                         RecordStoreTestWithBatching,
                         testing::Values(0, 1, 5, 10, 1000),
                         testing::PrintToStringParamName());

// seekExact() must return boost::none if the RecordId does not exist.
TEST(RecordStoreTest, SeekExactForMissingRecordReturnsNone) {
    const auto harnessHelper{newRecordStoreHarnessHelper()};
    auto recordStore = harnessHelper->newRecordStore();
    ServiceContext::UniqueOperationContext opCtx{harnessHelper->newOperationContext()};
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    // Insert three records and remember their record ids.
    const int nToInsert = 3;
    RecordId recordIds[nToInsert];
    for (int i = 0; i < nToInsert; ++i) {
        StringBuilder sb;
        sb << "record " << i;
        std::string data = sb.str();

        StorageWriteTransaction txn(ru);
        auto res = recordStore->insertRecord(opCtx.get(),
                                             *shard_role_details::getRecoveryUnit(opCtx.get()),
                                             data.c_str(),
                                             data.size() + 1,
                                             Timestamp{});
        ASSERT_OK(res.getStatus());
        recordIds[i] = res.getValue();
        txn.commit();
    }

    // Delete the second record.
    {
        StorageWriteTransaction txn(ru);
        recordStore->deleteRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), recordIds[1]);
        txn.commit();
    }

    // Seeking to the second record should now return boost::none, for both forward and reverse
    // cursors.
    for (bool direction : {true, false}) {
        auto cursor = recordStore->getCursor(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), direction);
        ASSERT_FALSE(cursor->seekExact(recordIds[1]));
    }

    // Similarly, findRecord() should not find the deleted record.
    RecordData outputData;
    ASSERT_FALSE(recordStore->findRecord(
        opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), recordIds[1], &outputData));
}

}  // namespace
}  // namespace mongo
