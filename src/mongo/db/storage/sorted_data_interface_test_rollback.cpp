// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/sorted_data_interface_test_assert.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

// Insert multiple keys and verify that omitting the commit()
// on the WriteUnitOfWork causes the changes to not become visible.
TEST_F(SortedDataInterfaceTest, InsertWithoutCommit) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), false));
        // no commit
    }

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key2, loc1), false));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key3, loc2), false));
        // no commit
    }

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));
}

// Insert multiple keys, then unindex those same keys and verify that
// omitting the commit() on the WriteUnitOfWork causes the changes to
// not become visible.
TEST_F(SortedDataInterfaceTest, UnindexWithoutCommit) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key2, loc2), true));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        sorted->unindex(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key2, loc2), true);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
        // no commit
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key3, loc3), true));
        txn.commit();
    }

    ASSERT_EQUALS(3, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        sorted->unindex(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true);
        ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));
        sorted->unindex(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key3, loc3), true);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
        // no commit
    }

    ASSERT_EQUALS(3, sorted->numEntries(opCtx(), recoveryUnit()));
}

}  // namespace
}  // namespace mongo
