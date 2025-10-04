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

namespace mongo {
namespace {

// Insert multiple records and iterate through them in the forward direction.
// When curr() or getNext() is called on an iterator positioned at EOF,
// the iterator returns RecordId() and stays at EOF.
TEST(RecordStoreTest, IterateOverMultipleRecords) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

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

    ASSERT_EQUALS(nToInsert, rs->numRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        for (int i = 0; i < nToInsert; i++) {
            const auto record = cursor->next();
            ASSERT(record);
            ASSERT_EQUALS(locs[i], record->id);
            ASSERT_EQUALS(datas[i], record->data.data());
        }
        ASSERT(!cursor->next());
    }
}

// Insert multiple records and iterate through them in the reverse direction.
// When curr() or getNext() is called on an iterator positioned at EOF,
// the iterator returns RecordId() and stays at EOF.
TEST(RecordStoreTest, IterateOverMultipleRecordsReversed) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

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

    ASSERT_EQUALS(nToInsert, rs->numRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        auto cursor =
            rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), false);
        for (int i = nToInsert - 1; i >= 0; i--) {
            const auto record = cursor->next();
            ASSERT(record);
            ASSERT_EQUALS(locs[i], record->id);
            ASSERT_EQUALS(datas[i], record->data.data());
        }
        ASSERT(!cursor->next());
    }
}

// Insert multiple records and try to create a forward iterator
// starting at an interior position.
TEST(RecordStoreTest, IterateStartFromMiddle) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

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

    ASSERT_EQUALS(nToInsert, rs->numRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        int start = nToInsert / 2;
        auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        for (int i = start; i < nToInsert; i++) {
            const auto record = (i == start) ? cursor->seekExact(locs[i]) : cursor->next();
            ASSERT(record);
            ASSERT_EQUALS(locs[i], record->id);
            ASSERT_EQUALS(datas[i], record->data.data());
        }
        ASSERT(!cursor->next());
    }
}

// Insert multiple records and try to create a reverse iterator
// starting at an interior position.
TEST(RecordStoreTest, IterateStartFromMiddleReversed) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

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

    ASSERT_EQUALS(nToInsert, rs->numRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        int start = nToInsert / 2;
        auto cursor =
            rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), false);
        for (int i = start; i >= 0; i--) {
            const auto record = (i == start) ? cursor->seekExact(locs[i]) : cursor->next();
            ASSERT(record);
            ASSERT_EQUALS(locs[i], record->id);
            ASSERT_EQUALS(datas[i], record->data.data());
        }
        ASSERT(!cursor->next());
    }
}

// Insert several records, and iterate to the end. Ensure that the record iterator
// is EOF. Add an additional record, saving and restoring the iterator state, and check
// that the iterator remains EOF.
TEST(RecordStoreTest, RecordIteratorEOF) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

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

    ASSERT_EQUALS(nToInsert, rs->numRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        // Get a forward iterator starting at the beginning of the record store.
        auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));

        // Iterate, checking EOF along the way.
        for (int i = 0; i < nToInsert; i++) {
            const auto record = cursor->next();
            ASSERT(record);
            ASSERT_EQUALS(locs[i], record->id);
            ASSERT_EQUALS(datas[i], record->data.data());
        }
        ASSERT(!cursor->next());

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

        ASSERT(cursor->restore(ru));

        // Iterator should still be EOF.
        ASSERT(!cursor->next());
        ASSERT(!cursor->next());
    }
}

// Test calling save and restore after each call to next
TEST(RecordStoreTest, RecordIteratorSaveRestore) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

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

    ASSERT_EQUALS(nToInsert, rs->numRecords());

    {
        // Get a forward iterator starting at the beginning of the record store.
        auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));

        // Iterate, checking EOF along the way.
        for (int i = 0; i < nToInsert; i++) {
            cursor->save();
            cursor->save();  // It is legal to save twice in a row.
            cursor->restore(ru);

            const auto record = cursor->next();
            ASSERT(record);
            ASSERT_EQUALS(locs[i], record->id);
            ASSERT_EQUALS(datas[i], record->data.data());
        }

        cursor->save();
        cursor->save();  // It is legal to save twice in a row.
        cursor->restore(ru);

        ASSERT(!cursor->next());
    }
}

// Insert two records, and iterate a cursor to EOF. Seek the same cursor to the first and ensure
// that next() returns the second record.
TEST(RecordStoreTest, SeekAfterEofAndContinue) {
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
        const auto record = cursor->next();
        ASSERT(record);
        ASSERT_EQUALS(locs[i], record->id);
        ASSERT_EQUALS(datas[i], record->data.data());
    }
    ASSERT(!cursor->next());

    {
        const auto record = cursor->seekExact(locs[0]);
        ASSERT(record);
        ASSERT_EQUALS(locs[0], record->id);
        ASSERT_EQUALS(datas[0], record->data.data());
    }

    {
        const auto record = cursor->next();
        ASSERT(record);
        ASSERT_EQUALS(locs[1], record->id);
        ASSERT_EQUALS(datas[1], record->data.data());
    }

    ASSERT(!cursor->next());
}

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
        ASSERT(!cursor->seekExact(recordIds[1]));
    }

    // Similarly, findRecord() should not find the deleted record.
    RecordData outputData;
    ASSERT_FALSE(recordStore->findRecord(
        opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), recordIds[1], &outputData));
}

}  // namespace
}  // namespace mongo
