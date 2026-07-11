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

// Insert multiple keys and verify that fullValidate() either sets
// the `numKeysOut` as the number of entries in the index, or as -1.
TEST_F(SortedDataInterfaceTest, FullValidate) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        StorageWriteTransaction txn(recoveryUnit());
        BSONObj key = BSON("" << i);
        RecordId loc(42, i * 2);
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key, loc), true));
        txn.commit();
    }

    ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx(), recoveryUnit()));
}

}  // namespace
}  // namespace mongo
