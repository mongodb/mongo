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

#include "mongo/db/service_context.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_assert.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

bool dupKeyCheck(SortedDataInterface& sorted,
                 OperationContext* opCtx,
                 RecoveryUnit& ru,
                 const key_string::Value& keyString) {
    return sorted.dupKeyCheck(opCtx, ru, keyString).has_value();
}
bool dupKeyCheck(SortedDataInterface& sorted,
                 OperationContext* opCtx,
                 RecoveryUnit& ru,
                 BSONObj bsonKey) {
    return dupKeyCheck(sorted, opCtx, ru, makeKeyString(&sorted, bsonKey));
}

// Insert a key and verify that dupKeyCheck() returns a non-OK status for
// the same key. When dupKeyCheck() is called with the exact (key, RecordId)
// pair that was inserted, it should still return an OK status.
TEST_F(SortedDataInterfaceTest, DupKeyCheckAfterInsert) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), false));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_FALSE(dupKeyCheck(*sorted, opCtx(), recoveryUnit(), key1));
        txn.commit();
    }
}

// Insert a KeyString and verify that dupKeyCheck() returns a non-OK status for
// the same KeyString. When dupKeyCheck() is called with the exact KeyString
// pair that was inserted, it should still return an OK status.
TEST_F(SortedDataInterfaceTest, DupKeyCheckAfterInsertKeyString) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    auto keyString1 = makeKeyString(sorted.get(), key1, loc1);
    auto keyString1WithoutRecordId = makeKeyString(sorted.get(), key1);

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(), recoveryUnit(), keyString1, false));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));

    ASSERT_FALSE(dupKeyCheck(*sorted, opCtx(), recoveryUnit(), keyString1WithoutRecordId));
}

// Verify that dupKeyCheck() returns an OK status for a key that does
// not exist in the index.
TEST_F(SortedDataInterfaceTest, DupKeyCheckEmpty) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    ASSERT_FALSE(dupKeyCheck(*sorted, opCtx(), recoveryUnit(), key1));
}

// Verify that dupKeyCheck() returns an OK status for a KeyString that does
// not exist in the index.
TEST_F(SortedDataInterfaceTest, DupKeyCheckEmptyKeyString) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    auto keyString1WithoutRecordId = makeKeyString(sorted.get(), key1);

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    ASSERT_FALSE(dupKeyCheck(*sorted, opCtx(), recoveryUnit(), keyString1WithoutRecordId));
}

// Insert a key and verify that dupKeyCheck() acknowledges the duplicate key, even
// when the insert key is located at a RecordId that comes after the one specified.
TEST_F(SortedDataInterfaceTest, DupKeyCheckWhenDiskLocBefore) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_FALSE(dupKeyCheck(*sorted, opCtx(), recoveryUnit(), key1));
        txn.commit();
    }
}

// Insert a key and verify that dupKeyCheck() acknowledges the duplicate key, even
// when the insert key is located at a RecordId that comes before the one specified.
TEST_F(SortedDataInterfaceTest, DupKeyCheckWhenDiskLocAfter) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_FALSE(dupKeyCheck(*sorted, opCtx(), recoveryUnit(), key1));
        txn.commit();
    }
}

TEST_F(SortedDataInterfaceTest, DupKeyCheckWithDuplicates) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {

        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc2), true));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));
    ASSERT_TRUE(dupKeyCheck(*sorted, opCtx(), recoveryUnit(), key1));
}

TEST_F(SortedDataInterfaceTest, DupKeyCheckWithDuplicateKeyStrings) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    auto keyString1 = makeKeyString(sorted.get(), key1, loc1);
    auto keyString2 = makeKeyString(sorted.get(), key1, loc2);
    auto keyString1WithoutRecordId = makeKeyString(sorted.get(), key1);

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(), recoveryUnit(), keyString1, true));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(), recoveryUnit(), keyString2, true));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));
    ASSERT_TRUE(dupKeyCheck(*sorted, opCtx(), recoveryUnit(), keyString1WithoutRecordId));
}

TEST_F(SortedDataInterfaceTest, DupKeyCheckWithDeletedFirstEntry) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc2), true));
        txn.commit();
    }

    {
        StorageWriteTransaction txn(recoveryUnit());
        sorted->unindex(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true);
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
    ASSERT_FALSE(dupKeyCheck(*sorted, opCtx(), recoveryUnit(), key1));
}

TEST_F(SortedDataInterfaceTest, DupKeyCheckWithDeletedSecondEntry) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc2), true));
        txn.commit();
    }

    {
        StorageWriteTransaction txn(recoveryUnit());
        sorted->unindex(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc2), true);
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
    ASSERT_FALSE(dupKeyCheck(*sorted, opCtx(), recoveryUnit(), key1));
}

}  // namespace
}  // namespace mongo
