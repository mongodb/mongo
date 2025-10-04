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

#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

// Add a key using a bulk builder.
TEST_F(SortedDataInterfaceTest, BuilderAddKey) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        const auto builder(sorted->makeBulkBuilder(opCtx(), recoveryUnit()));

        StorageWriteTransaction txn(recoveryUnit());
        builder->addKey(recoveryUnit(), makeKeyString(sorted.get(), key1, loc1));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
}

/*
 * Add a KeyString using a bulk builder.
 */
TEST_F(SortedDataInterfaceTest, BuilderAddKeyString) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    key_string::Builder keyString1(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc1);

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        const auto builder(sorted->makeBulkBuilder(opCtx(), recoveryUnit()));

        StorageWriteTransaction txn(recoveryUnit());
        builder->addKey(recoveryUnit(), keyString1.getValueCopy());
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
}

// Add a reserved RecordId using a bulk builder.
TEST_F(SortedDataInterfaceTest, BuilderAddKeyWithReservedRecordIdLong) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));
    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        const auto builder(sorted->makeBulkBuilder(opCtx(), recoveryUnit()));

        RecordId reservedLoc(record_id_helpers::reservedIdFor(
            record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, KeyFormat::Long));
        ASSERT(record_id_helpers::isReserved(reservedLoc));

        StorageWriteTransaction txn(recoveryUnit());
        builder->addKey(recoveryUnit(), makeKeyString(sorted.get(), key1, reservedLoc));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
}

// Add a compound key using a bulk builder.
TEST_F(SortedDataInterfaceTest, BuilderAddCompoundKey) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        const auto builder(sorted->makeBulkBuilder(opCtx(), recoveryUnit()));

        StorageWriteTransaction txn(recoveryUnit());
        builder->addKey(recoveryUnit(), makeKeyString(sorted.get(), compoundKey1a, loc1));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
}

// Add the same key multiple times using a bulk builder results in an invalid index with duplicates.
TEST_F(SortedDataInterfaceTest, BuilderAddSameKey) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        const auto builder(sorted->makeBulkBuilder(opCtx(), recoveryUnit()));

        StorageWriteTransaction txn(recoveryUnit());
        builder->addKey(recoveryUnit(), makeKeyString(sorted.get(), key1, loc1));
        builder->addKey(recoveryUnit(), makeKeyString(sorted.get(), key1, loc2));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));
}

// Add the same key multiple times using a bulk builder and verify that
// the returned status is OK when duplicates are allowed.
TEST_F(SortedDataInterfaceTest, BuilderAddSameKeyWithDupsAllowed) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        const auto builder(sorted->makeBulkBuilder(opCtx(), recoveryUnit()));

        StorageWriteTransaction txn(recoveryUnit());
        builder->addKey(recoveryUnit(), makeKeyString(sorted.get(), key1, loc1));
        builder->addKey(recoveryUnit(), makeKeyString(sorted.get(), key1, loc2));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));
}

/*
 * Add the same KeyString multiple times using a bulk builder and verify that the returned status is
 * OK when duplicates are allowed.
 */
TEST_F(SortedDataInterfaceTest, BuilderAddSameKeyStringWithDupsAllowed) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    key_string::Builder keyStringLoc1(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc1);
    key_string::Builder keyStringLoc2(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc2);

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        const auto builder(sorted->makeBulkBuilder(opCtx(), recoveryUnit()));

        StorageWriteTransaction txn(recoveryUnit());
        builder->addKey(recoveryUnit(), keyStringLoc1.getValueCopy());
        builder->addKey(recoveryUnit(), keyStringLoc2.getValueCopy());
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));
}

// Add multiple keys using a bulk builder.
TEST_F(SortedDataInterfaceTest, BuilderAddMultipleKeys) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        const auto builder(sorted->makeBulkBuilder(opCtx(), recoveryUnit()));

        StorageWriteTransaction txn(recoveryUnit());
        builder->addKey(recoveryUnit(), makeKeyString(sorted.get(), key1, loc1));
        builder->addKey(recoveryUnit(), makeKeyString(sorted.get(), key2, loc2));
        builder->addKey(recoveryUnit(), makeKeyString(sorted.get(), key3, loc3));
        txn.commit();
    }

    ASSERT_EQUALS(3, sorted->numEntries(opCtx(), recoveryUnit()));
}

/*
 * Add multiple KeyStrings using a bulk builder.
 */
TEST_F(SortedDataInterfaceTest, BuilderAddMultipleKeyStrings) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    key_string::Builder keyString1(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc1);
    key_string::Builder keyString2(
        sorted->getKeyStringVersion(), key2, sorted->getOrdering(), loc2);
    key_string::Builder keyString3(
        sorted->getKeyStringVersion(), key3, sorted->getOrdering(), loc3);

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        const auto builder(sorted->makeBulkBuilder(opCtx(), recoveryUnit()));

        StorageWriteTransaction txn(recoveryUnit());
        builder->addKey(recoveryUnit(), keyString1.getValueCopy());
        builder->addKey(recoveryUnit(), keyString2.getValueCopy());
        builder->addKey(recoveryUnit(), keyString3.getValueCopy());
        txn.commit();
    }

    ASSERT_EQUALS(3, sorted->numEntries(opCtx(), recoveryUnit()));
}

// Add multiple compound keys using a bulk builder.
TEST_F(SortedDataInterfaceTest, BuilderAddMultipleCompoundKeys) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        const auto builder(sorted->makeBulkBuilder(opCtx(), recoveryUnit()));

        StorageWriteTransaction txn(recoveryUnit());
        builder->addKey(recoveryUnit(), makeKeyString(sorted.get(), compoundKey1a, loc1));
        builder->addKey(recoveryUnit(), makeKeyString(sorted.get(), compoundKey1b, loc2));
        builder->addKey(recoveryUnit(), makeKeyString(sorted.get(), compoundKey1c, loc4));
        builder->addKey(recoveryUnit(), makeKeyString(sorted.get(), compoundKey2b, loc3));
        builder->addKey(recoveryUnit(), makeKeyString(sorted.get(), compoundKey3a, loc5));
        txn.commit();
    }

    ASSERT_EQUALS(5, sorted->numEntries(opCtx(), recoveryUnit()));
}

}  // namespace
}  // namespace mongo
