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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_assert.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

/**
 * Insert a key and verify that it can be unindexed.
 */
void unindex(OperationContext* opCtx,
             RecoveryUnit& recoveryUnit,
             SortedDataInterfaceHarnessHelper* harnessHelper,
             bool partial) {
    const auto sorted(harnessHelper->newSortedDataInterface(opCtx, /*unique=*/false, partial));

    ASSERT(sorted->isEmpty(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx, recoveryUnit, makeKeyString(sorted.get(), key1, loc1), true));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        sorted->unindex(opCtx, recoveryUnit, makeKeyString(sorted.get(), key1, loc1), true);
        ASSERT(sorted->isEmpty(opCtx, recoveryUnit));
        txn.commit();
    }

    ASSERT(sorted->isEmpty(opCtx, recoveryUnit));
}
TEST_F(SortedDataInterfaceTest, Unindex) {
    unindex(opCtx(), recoveryUnit(), harnessHelper(), false);
}
TEST_F(SortedDataInterfaceTest, UnindexPartial) {
    unindex(opCtx(), recoveryUnit(), harnessHelper(), true);
}

/**
 * Insert a KeyString and verify that it can be unindexed.
 */
void unindexKeyString(OperationContext* opCtx,
                      RecoveryUnit& recoveryUnit,
                      SortedDataInterfaceHarnessHelper* harnessHelper,
                      bool partial) {
    const auto sorted(harnessHelper->newSortedDataInterface(opCtx, /*unique=*/false, partial));

    auto keyString1 = makeKeyString(sorted.get(), key1, loc1);

    ASSERT(sorted->isEmpty(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx, recoveryUnit, keyString1, true));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        sorted->unindex(opCtx, recoveryUnit, keyString1, true);
        ASSERT(sorted->isEmpty(opCtx, recoveryUnit));
        txn.commit();
    }

    ASSERT(sorted->isEmpty(opCtx, recoveryUnit));
}
TEST_F(SortedDataInterfaceTest, UnindexKeyString) {
    unindexKeyString(opCtx(), recoveryUnit(), harnessHelper(), false);
}
TEST_F(SortedDataInterfaceTest, UnindexKeyStringPartial) {
    unindexKeyString(opCtx(), recoveryUnit(), harnessHelper(), true);
}

/**
 * Insert a compound key and verify that it can be unindexed.
 */
void unindexCompoundKey(OperationContext* opCtx,
                        RecoveryUnit& recoveryUnit,
                        SortedDataInterfaceHarnessHelper* harnessHelper,
                        bool partial) {
    const auto sorted(harnessHelper->newSortedDataInterface(opCtx, /*unique=*/false, partial));

    ASSERT(sorted->isEmpty(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx, recoveryUnit, makeKeyString(sorted.get(), compoundKey1a, loc1), true));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        sorted->unindex(
            opCtx, recoveryUnit, makeKeyString(sorted.get(), compoundKey1a, loc1), true);
        ASSERT(sorted->isEmpty(opCtx, recoveryUnit));
        txn.commit();
    }

    ASSERT(sorted->isEmpty(opCtx, recoveryUnit));
}
TEST_F(SortedDataInterfaceTest, UnindexCompoundKey) {
    unindexCompoundKey(opCtx(), recoveryUnit(), harnessHelper(), false);
}
TEST_F(SortedDataInterfaceTest, UnindexCompoundKeyPartial) {
    unindexCompoundKey(opCtx(), recoveryUnit(), harnessHelper(), true);
}

/**
 * Insert multiple, distinct keys and verify that they can be unindexed.
 */
void unindexMultipleDistinct(OperationContext* opCtx,
                             RecoveryUnit& recoveryUnit,
                             SortedDataInterfaceHarnessHelper* harnessHelper,
                             bool partial) {
    const auto sorted(harnessHelper->newSortedDataInterface(opCtx, /*unique=*/false, partial));

    ASSERT(sorted->isEmpty(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx, recoveryUnit, makeKeyString(sorted.get(), key1, loc1), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx, recoveryUnit, makeKeyString(sorted.get(), key2, loc2), true));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        sorted->unindex(opCtx, recoveryUnit, makeKeyString(sorted.get(), key2, loc2), true);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx, recoveryUnit));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx, recoveryUnit, makeKeyString(sorted.get(), key3, loc3), true));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        sorted->unindex(opCtx, recoveryUnit, makeKeyString(sorted.get(), key1, loc1), true);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx, recoveryUnit));
        sorted->unindex(opCtx, recoveryUnit, makeKeyString(sorted.get(), key3, loc3), true);
        ASSERT(sorted->isEmpty(opCtx, recoveryUnit));
        txn.commit();
    }

    ASSERT(sorted->isEmpty(opCtx, recoveryUnit));
}
TEST_F(SortedDataInterfaceTest, UnindexMultipleDistinct) {
    unindexMultipleDistinct(opCtx(), recoveryUnit(), harnessHelper(), false);
}
TEST_F(SortedDataInterfaceTest, UnindexMultipleDistinctPartial) {
    unindexMultipleDistinct(opCtx(), recoveryUnit(), harnessHelper(), true);
}

/**
 * Insert the same key multiple times and verify that each occurrence can be unindexed.
 */
void unindexMultipleSameKey(OperationContext* opCtx,
                            RecoveryUnit& recoveryUnit,
                            SortedDataInterfaceHarnessHelper* harnessHelper,
                            bool partial) {
    const auto sorted(harnessHelper->newSortedDataInterface(opCtx, /*unique=*/false, partial));

    ASSERT(sorted->isEmpty(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx, recoveryUnit, makeKeyString(sorted.get(), key1, loc1), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx,
                                            recoveryUnit,
                                            makeKeyString(sorted.get(), key1, loc2),
                                            true /* allow duplicates */));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        sorted->unindex(opCtx, recoveryUnit, makeKeyString(sorted.get(), key1, loc2), true);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx, recoveryUnit));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx,
                                            recoveryUnit,
                                            makeKeyString(sorted.get(), key1, loc3),
                                            true /* allow duplicates */));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        sorted->unindex(opCtx, recoveryUnit, makeKeyString(sorted.get(), key1, loc1), true);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx, recoveryUnit));
        sorted->unindex(opCtx, recoveryUnit, makeKeyString(sorted.get(), key1, loc3), true);
        ASSERT(sorted->isEmpty(opCtx, recoveryUnit));
        txn.commit();
    }

    ASSERT(sorted->isEmpty(opCtx, recoveryUnit));
}
TEST_F(SortedDataInterfaceTest, UnindexMultipleSameKey) {
    unindexMultipleSameKey(opCtx(), recoveryUnit(), harnessHelper(), false);
}
TEST_F(SortedDataInterfaceTest, UnindexMultipleSameKeyPartial) {
    unindexMultipleSameKey(opCtx(), recoveryUnit(), harnessHelper(), true);
}

/**
 * Insert the same KeyString multiple times and verify that each occurrence can be unindexed.
 */
void unindexMultipleSameKeyString(OperationContext* opCtx,
                                  RecoveryUnit& recoveryUnit,
                                  SortedDataInterfaceHarnessHelper* harnessHelper,
                                  bool partial) {
    const auto sorted(harnessHelper->newSortedDataInterface(opCtx, /*unique=*/false, partial));

    auto keyStringLoc1 = makeKeyString(sorted.get(), key1, loc1);
    auto keyStringLoc2 = makeKeyString(sorted.get(), key1, loc2);
    auto keyStringLoc3 = makeKeyString(sorted.get(), key1, loc3);

    ASSERT(sorted->isEmpty(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx, recoveryUnit, keyStringLoc1, true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx, recoveryUnit, keyStringLoc2, true /* allow duplicates */));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        sorted->unindex(opCtx, recoveryUnit, keyStringLoc2, true);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx, recoveryUnit));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx, recoveryUnit, keyStringLoc3, true /* allow duplicates */));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        sorted->unindex(opCtx, recoveryUnit, keyStringLoc1, true);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx, recoveryUnit));
        sorted->unindex(opCtx, recoveryUnit, keyStringLoc3, true);
        ASSERT(sorted->isEmpty(opCtx, recoveryUnit));
        txn.commit();
    }

    ASSERT(sorted->isEmpty(opCtx, recoveryUnit));
}
TEST_F(SortedDataInterfaceTest, UnindexMultipleSameKeyString) {
    unindexMultipleSameKeyString(opCtx(), recoveryUnit(), harnessHelper(), false);
}
TEST_F(SortedDataInterfaceTest, UnindexMultipleSameKeyStringPartial) {
    unindexMultipleSameKeyString(opCtx(), recoveryUnit(), harnessHelper(), true);
}

/**
 * Call unindex() on a nonexistent key and verify the result is false.
 */
void unindexEmpty(OperationContext* opCtx,
                  RecoveryUnit& recoveryUnit,
                  SortedDataInterfaceHarnessHelper* harnessHelper,
                  bool partial) {
    const auto sorted(harnessHelper->newSortedDataInterface(opCtx, /*unique=*/false, partial));

    ASSERT(sorted->isEmpty(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        sorted->unindex(opCtx, recoveryUnit, makeKeyString(sorted.get(), key1, loc1), true);
        ASSERT(sorted->isEmpty(opCtx, recoveryUnit));
        txn.commit();
    }
}
TEST_F(SortedDataInterfaceTest, UnindexEmpty) {
    unindexEmpty(opCtx(), recoveryUnit(), harnessHelper(), false);
}
TEST_F(SortedDataInterfaceTest, UnindexEmptyPartial) {
    unindexEmpty(opCtx(), recoveryUnit(), harnessHelper(), true);
}

/**
 * Test partial indexing and unindexing.
 */
TEST_F(SortedDataInterfaceTest, PartialIndex) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/true));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true));
        // Assume key1 with loc2 was never indexed due to the partial index.
        ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
        txn.commit();
    }

    {
        StorageWriteTransaction txn(recoveryUnit());
        // Shouldn't unindex anything as key1 with loc2 wasn't indexed in the first place.
        sorted->unindex(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc2), true);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
        txn.commit();
    }

    {
        StorageWriteTransaction txn(recoveryUnit());
        sorted->unindex(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true);
        ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));
        txn.commit();
    }
}

TEST_F(SortedDataInterfaceTest, Unindex1) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), BSON("" << 1), RecordId(5, 18)),
                           true));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        sorted->unindex(opCtx(),
                        recoveryUnit(),
                        makeKeyString(sorted.get(), BSON("" << 1), RecordId(5, 20)),
                        true);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        sorted->unindex(opCtx(),
                        recoveryUnit(),
                        makeKeyString(sorted.get(), BSON("" << 2), RecordId(5, 18)),
                        true);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        sorted->unindex(opCtx(),
                        recoveryUnit(),
                        makeKeyString(sorted.get(), BSON("" << 1), RecordId(5, 18)),
                        true);
        ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));
        txn.commit();
    }

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));
}

TEST_F(SortedDataInterfaceTest, Unindex2Rollback) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), BSON("" << 1), RecordId(5, 18)),
                           true));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        sorted->unindex(opCtx(),
                        recoveryUnit(),
                        makeKeyString(sorted.get(), BSON("" << 1), RecordId(5, 18)),
                        true);
        ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));
        // no commit
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
}

}  // namespace
}  // namespace mongo
