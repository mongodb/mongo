// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_assert.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

// Verify that isEmpty() returns true when the index is empty,
// returns false when a key is inserted, and returns true again
// when that is unindex.
TEST_F(SortedDataInterfaceTest, IsEmpty) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), false));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key2, loc2), false));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key3, loc3), false));
        txn.commit();
    }

    ASSERT(!sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        sorted->unindex(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), false);
        sorted->unindex(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key2, loc2), false);
        sorted->unindex(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key3, loc3), false);
        ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));
        txn.commit();
    }

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));
}

}  // namespace
}  // namespace mongo
