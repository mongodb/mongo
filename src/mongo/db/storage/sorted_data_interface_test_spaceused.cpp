// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_assert.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

// Verify that an empty index takes up no space.
TEST_F(SortedDataInterfaceTest, GetSpaceUsedBytesEmpty) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));
}

// Verify that a nonempty index takes up some space.
TEST_F(SortedDataInterfaceTest, GetSpaceUsedBytesNonEmpty) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    auto& ru = recoveryUnit();
    ASSERT(sorted->isEmpty(opCtx(), ru));

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx());
        StorageWriteTransaction txn(ru);
        BSONObj key = BSON("" << i);
        RecordId loc(42, i * 2);
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), ru, makeKeyString(sorted.get(), key, loc), true));
        txn.commit();
    }

    ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx(), recoveryUnit()));
}

}  // namespace
}  // namespace mongo
