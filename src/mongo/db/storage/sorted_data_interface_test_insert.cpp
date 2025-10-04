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
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_assert.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

// Insert a key and verify that the number of entries in the index equals 1.
TEST_F(SortedDataInterfaceTest, Insert) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));

    const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));
    ASSERT_EQ(
        cursor->seek(recoveryUnit(),
                     makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
        IndexKeyEntry(key1, loc1));
}

// Insert a KeyString and verify that the number of entries in the index equals 1.
TEST_F(SortedDataInterfaceTest, InsertKeyString) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    auto keyString1 = makeKeyString(sorted.get(), key1, loc1);

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(), recoveryUnit(), keyString1, true));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));

    const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));
    ASSERT_EQ(
        cursor->seek(recoveryUnit(),
                     makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
        IndexKeyEntry(key1, loc1));
}

// Insert a compound key and verify that the number of entries in the index equals 1.
TEST_F(SortedDataInterfaceTest, InsertCompoundKey) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey1a, loc1), true));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
}

// Insert multiple, distinct keys at the same RecordId and verify that the
// number of entries in the index equals the number that were inserted, even
// when duplicates are not allowed.
TEST_F(SortedDataInterfaceTest, InsertSameDiskLoc) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key2, loc1), true));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key3, loc1), true));
        txn.commit();
    }

    ASSERT_EQUALS(3, sorted->numEntries(opCtx(), recoveryUnit()));
}

// Insert multiple, distinct keys at the same RecordId and verify that the
// number of entries in the index equals the number that were inserted, even
// when duplicates are allowed.
TEST_F(SortedDataInterfaceTest, InsertSameDiskLocWithDupsAllowed) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), false));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key2, loc1),
                                            true /* allow duplicates */));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key3, loc1),
                                            true /* allow duplicates */));
        txn.commit();
    }

    ASSERT_EQUALS(3, sorted->numEntries(opCtx(), recoveryUnit()));
}

// Insert the same key multiple times and verify that only 1 entry exists
// in the index when duplicates are not allowed.
TEST_F(SortedDataInterfaceTest, InsertSameKey) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), false));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), false));
        ASSERT_SDI_INSERT_DUPLICATE_KEY(sorted->insert(opCtx(),
                                                       recoveryUnit(),
                                                       makeKeyString(sorted.get(), key1, loc2),
                                                       false,
                                                       IncludeDuplicateRecordId::kOff),
                                        key1,
                                        boost::none);
        ASSERT_SDI_INSERT_DUPLICATE_KEY(sorted->insert(opCtx(),
                                                       recoveryUnit(),
                                                       makeKeyString(sorted.get(), key1, loc2),
                                                       false,
                                                       IncludeDuplicateRecordId::kOn),
                                        key1,
                                        loc1);
        txn.commit();
    }

    {
        ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));

        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));
        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key1, loc1));
    }

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_DUPLICATE_KEY(sorted->insert(opCtx(),
                                                       recoveryUnit(),
                                                       makeKeyString(sorted.get(), key1, loc2),
                                                       false,
                                                       IncludeDuplicateRecordId::kOff),
                                        key1,
                                        boost::none);
        ASSERT_SDI_INSERT_DUPLICATE_KEY(sorted->insert(opCtx(),
                                                       recoveryUnit(),
                                                       makeKeyString(sorted.get(), key1, loc2),
                                                       false,
                                                       IncludeDuplicateRecordId::kOn),
                                        key1,
                                        loc1);
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));

    const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));
    ASSERT_EQ(
        cursor->seek(recoveryUnit(),
                     makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
        IndexKeyEntry(key1, loc1));
}

/*
 * Insert the same KeyString multiple times and verify that only 1 entry exists in the index when
 * duplicates are not allowed.
 */
TEST_F(SortedDataInterfaceTest, InsertSameKeyString) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    key_string::Builder keyStringLoc1(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc1);
    key_string::Builder keyStringLoc2(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc2);

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), keyStringLoc1.getValueCopy(), false));
        ASSERT_SDI_INSERT_DUPLICATE_KEY(sorted->insert(opCtx(),
                                                       recoveryUnit(),
                                                       keyStringLoc2.getValueCopy(),
                                                       false,
                                                       IncludeDuplicateRecordId::kOff),
                                        key1,
                                        boost::none);
        ASSERT_SDI_INSERT_DUPLICATE_KEY(sorted->insert(opCtx(),
                                                       recoveryUnit(),
                                                       keyStringLoc2.getValueCopy(),
                                                       false,
                                                       IncludeDuplicateRecordId::kOn),
                                        key1,
                                        loc1);
        txn.commit();
    }

    {
        ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));

        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));
        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key1, loc1));
    }

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_DUPLICATE_KEY(sorted->insert(opCtx(),
                                                       recoveryUnit(),
                                                       keyStringLoc2.getValueCopy(),
                                                       false,
                                                       IncludeDuplicateRecordId::kOff),
                                        key1,
                                        boost::none);
        ASSERT_SDI_INSERT_DUPLICATE_KEY(sorted->insert(opCtx(),
                                                       recoveryUnit(),
                                                       keyStringLoc2.getValueCopy(),
                                                       false,
                                                       IncludeDuplicateRecordId::kOn),
                                        key1,
                                        loc1);
        txn.commit();
    }

    {
        ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));

        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));
        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key1, loc1));
    }
}

/**
 * Insert the same key multiple times and verify that all entries exists in the index when
 * duplicates are allowed. Since it is illegal to open a cursor to an unique index while the unique
 * constraint is violated, this is tested by running the test 3 times, removing all but one loc each
 * time and verifying the correct loc remains.
 */
void testInsertSameKeyWithDupsAllowed(OperationContext* opCtx,
                                      RecoveryUnit& recoveryUnit,
                                      SortedDataInterfaceHarnessHelper* harnessHelper,
                                      const RecordId locs[3],
                                      int keeper) {
    const auto sorted(
        harnessHelper->newSortedDataInterface(opCtx, /*unique=*/true, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx, recoveryUnit));

    {
        StorageWriteTransaction txn(recoveryUnit);
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx, recoveryUnit, makeKeyString(sorted.get(), key1, locs[0]), false));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx, recoveryUnit, makeKeyString(sorted.get(), key1, locs[1]), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx, recoveryUnit, makeKeyString(sorted.get(), key1, locs[2]), true));
        txn.commit();
    }
    {
        StorageWriteTransaction txn(recoveryUnit);
        for (int i = 0; i < 3; i++) {
            if (i != keeper) {
                sorted->unindex(
                    opCtx, recoveryUnit, makeKeyString(sorted.get(), key1, locs[i]), true);
            }
        }
        txn.commit();
    }

    {
        ASSERT_EQUALS(1, sorted->numEntries(opCtx, recoveryUnit));

        const auto cursor(sorted->newCursor(opCtx, recoveryUnit));
        ASSERT_EQ(
            cursor->seek(recoveryUnit,
                         makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key1, locs[keeper]));
    }
}

const RecordId ascLocs[3] = {loc1, loc2, loc3};
TEST_F(SortedDataInterfaceTest, InsertSameKeyWithDupsAllowedLocsAscending_Keep0) {
    testInsertSameKeyWithDupsAllowed(opCtx(), recoveryUnit(), harnessHelper(), ascLocs, 0);
}

TEST_F(SortedDataInterfaceTest, InsertSameKeyWithDupsAllowedLocsAscending_Keep1) {
    testInsertSameKeyWithDupsAllowed(opCtx(), recoveryUnit(), harnessHelper(), ascLocs, 1);
}

TEST_F(SortedDataInterfaceTest, InsertSameKeyWithDupsAllowedLocsAscending_Keep2) {
    testInsertSameKeyWithDupsAllowed(opCtx(), recoveryUnit(), harnessHelper(), ascLocs, 2);
}

const RecordId descLocs[3] = {loc3, loc2, loc1};
TEST_F(SortedDataInterfaceTest, InsertSameKeyWithDupsAllowedLocsDescending_Keep0) {
    testInsertSameKeyWithDupsAllowed(opCtx(), recoveryUnit(), harnessHelper(), descLocs, 0);
}

TEST_F(SortedDataInterfaceTest, InsertSameKeyWithDupsAllowedLocsDescending_Keep1) {
    testInsertSameKeyWithDupsAllowed(opCtx(), recoveryUnit(), harnessHelper(), descLocs, 1);
}

TEST_F(SortedDataInterfaceTest, InsertSameKeyWithDupsAllowedLocsDescending_Keep2) {
    testInsertSameKeyWithDupsAllowed(opCtx(), recoveryUnit(), harnessHelper(), descLocs, 2);
}

/**
 * Insert multiple keys and verify that the number of entries in the index equals the number that
 * were inserted.
 */
TEST_F(SortedDataInterfaceTest, InsertMultiple) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), false));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key2, loc2), false));
        txn.commit();
    }

    {
        ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));

        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));
        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key1, loc1));
        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key2, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key2, loc2));
    }

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key3, loc3), false));
        txn.commit();
    }

    {
        ASSERT_EQUALS(3, sorted->numEntries(opCtx(), recoveryUnit()));

        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));
        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key1, loc1));
        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key2, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key2, loc2));
        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key3, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key3, loc3));
    }
}

/**
 * Insert multiple KeyStrings and verify that the number of entries in the index equals the number
 * that were inserted.
 */
TEST_F(SortedDataInterfaceTest, InsertMultipleKeyStrings) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    auto keyString1 = makeKeyString(sorted.get(), key1, loc1);
    auto keyString2 = makeKeyString(sorted.get(), key2, loc2);
    auto keyString3 = makeKeyString(sorted.get(), key3, loc3);

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(), recoveryUnit(), keyString1, false));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(), recoveryUnit(), keyString2, false));
        txn.commit();
    }

    {
        ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));

        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));
        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key1, loc1));
        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key2, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key2, loc2));
    }

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(), recoveryUnit(), keyString3, false));
        txn.commit();
    }

    {
        ASSERT_EQUALS(3, sorted->numEntries(opCtx(), recoveryUnit()));

        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));
        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key1, loc1));
        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key2, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key2, loc2));
        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key3, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key3, loc3));
    }
}

/**
 * Insert multiple KeyStrings and seek to the inserted KeyStrings
 */
TEST_F(SortedDataInterfaceTest, InsertAndSeekKeyString) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    auto keyString1 = makeKeyString(sorted.get(), key1, loc1);
    auto keyString2 = makeKeyString(sorted.get(), key2, loc2);

    auto keyString1WithoutRecordId = makeKeyString(sorted.get(), key1);
    auto keyString2WithoutRecordId = makeKeyString(sorted.get(), key2);

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(), recoveryUnit(), keyString1, false));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(), recoveryUnit(), keyString2, false));
        txn.commit();
    }

    {
        ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));

        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));

        auto ksEntry1 =
            cursor->seekForKeyString(recoveryUnit(), keyString1WithoutRecordId.getView());
        ASSERT_EQ(ksEntry1->keyString.compare(keyString1), 0);
        ASSERT(ksEntry1->keyString.compare(keyString2) < 0);
        auto kvEntry1 =
            cursor->seekForKeyValueView(recoveryUnit(), keyString1WithoutRecordId.getView());
        ASSERT(!kvEntry1.isEmpty());
        ASSERT_EQ(ksEntry1->keyString.compare(kvEntry1.getValueCopy()), 0);

        auto ksEntry2 =
            cursor->seekForKeyString(recoveryUnit(), keyString2WithoutRecordId.getView());
        ASSERT_EQ(ksEntry2->keyString.compare(keyString2), 0);
        ASSERT(ksEntry2->keyString.compare(keyString1) > 0);
        auto kvEntry2 =
            cursor->seekForKeyValueView(recoveryUnit(), keyString2WithoutRecordId.getView());
        ASSERT(!kvEntry2.isEmpty());
        ASSERT_EQ(ksEntry2->keyString.compare(kvEntry2.getValueCopy()), 0);
    }
}

/**
 * Insert multiple KeyStrings and use findLoc on the inserted KeyStrings.
 */
TEST_F(SortedDataInterfaceTest, InsertAndSeekExactKeyString) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    auto keyString1 = makeKeyString(sorted.get(), key1, loc1);
    auto keyString2 = makeKeyString(sorted.get(), key2, loc2);

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(), recoveryUnit(), keyString1, false));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(), recoveryUnit(), keyString2, false));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));

    ASSERT_EQ(loc1,
              sorted->findLoc(opCtx(),
                              recoveryUnit(),
                              makeKeyStringForSeek(sorted.get(), key1).finishAndGetBuffer()));
    ASSERT_EQ(loc2,
              sorted->findLoc(opCtx(),
                              recoveryUnit(),
                              makeKeyStringForSeek(sorted.get(), key2).finishAndGetBuffer()));
}

/**
 * Insert multiple compound keys and verify that the number of entries in the index equals the
 * number that were inserted.
 */
TEST_F(SortedDataInterfaceTest, InsertMultipleCompoundKeys) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey1a, loc1), false));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey1b, loc2), false));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey2b, loc3), false));
        txn.commit();
    }

    ASSERT_EQUALS(3, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey1c, loc4), false));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey3a, loc5), false));
        txn.commit();
    }

    ASSERT_EQUALS(5, sorted->numEntries(opCtx(), recoveryUnit()));
}

TEST_F(SortedDataInterfaceTest, InsertReservedRecordIdLong) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));
    StorageWriteTransaction txn(recoveryUnit());
    RecordId reservedLoc(record_id_helpers::reservedIdFor(
        record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, KeyFormat::Long));
    invariant(record_id_helpers::isReserved(reservedLoc));
    ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                        recoveryUnit(),
                                        makeKeyString(sorted.get(), key1, reservedLoc),
                                        /*dupsAllowed*/ true));
    txn.commit();
    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
}

TEST_F(SortedDataInterfaceTest, InsertReservedRecordIdIntoUniqueIndex) {
    const auto sorted(harnessHelper()->newSortedDataInterface(opCtx(),
                                                              /*unique=*/true,
                                                              /*partial=*/false,
                                                              KeyFormat::String));

    {
        constexpr char reservation[] = {
            static_cast<char>(0xFF),
            static_cast<char>(record_id_helpers::ReservationId::kWildcardMultikeyMetadataId)};
        RecordId reservedId = RecordId(reservation);
        ASSERT(record_id_helpers::isReserved(reservedId));

        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key1, reservedId),
                                            /*dupsAllowed=*/false));
        txn.commit();
    }

    {
        ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));

        // There is only one reserved RecordId, kWildcardMultikeyMetadataId. In order to test that
        // the upper bound for unique indexes works properly we insert a key with RecordId
        // kWildcardMultikeyMetadataId + 1. This will result in a DuplicateKey as the key with
        // RecordId kWildcardMultikeyMetadataId will be detected by the bounded cursor.
        constexpr char reservation[] = {
            static_cast<char>(0xFF),
            static_cast<char>(record_id_helpers::ReservationId::kWildcardMultikeyMetadataId) + 1};
        RecordId reservedId = RecordId(reservation);
        ASSERT(record_id_helpers::isReserved(reservedId));

        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_DUPLICATE_KEY(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), key1, reservedId),
                           false,
                           IncludeDuplicateRecordId::kOff),
            key1,
            boost::none);
        ASSERT_SDI_INSERT_DUPLICATE_KEY(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), key1, reservedId),
                           false,
                           IncludeDuplicateRecordId::kOn),
            key1,
            record_id_helpers::reservedIdFor(
                record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, KeyFormat::String));
    }
}

TEST_F(SortedDataInterfaceTest, InsertWithDups1) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), BSON("" << 1), RecordId(5, 2)),
                           true));
        txn.commit();
    }

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), BSON("" << 1), RecordId(6, 2)),
                           true));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));
}

TEST_F(SortedDataInterfaceTest, InsertWithDups2) {
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

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), BSON("" << 1), RecordId(5, 20)),
                           true));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));
}

TEST_F(SortedDataInterfaceTest, InsertWithDups3AndRollback) {
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

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), BSON("" << 1), RecordId(5, 20)),
                           true));
        // no commit
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
}

TEST_F(SortedDataInterfaceTest, InsertNoDups1) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), BSON("" << 1), RecordId(5, 18)),
                           false));
        txn.commit();
    }

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), BSON("" << 2), RecordId(5, 20)),
                           false));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));
}

TEST_F(SortedDataInterfaceTest, InsertNoDups2) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), BSON("" << 1), RecordId(5, 2)),
                           false));
        txn.commit();
    }

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_DUPLICATE_KEY(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), BSON("" << 1), RecordId(5, 4)),
                           false,
                           IncludeDuplicateRecordId::kOff),
            BSON("" << 1),
            boost::none);
        ASSERT_SDI_INSERT_DUPLICATE_KEY(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), BSON("" << 1), RecordId(5, 4)),
                           false,
                           IncludeDuplicateRecordId::kOn),
            BSON("" << 1),
            RecordId(5, 2));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
}

TEST_F(SortedDataInterfaceTest, InsertNoDups3) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), BSON("" << 1), RecordId(5, 2)),
                           false));
        txn.commit();
    }

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_DUPLICATE_KEY(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), BSON("" << 1), RecordId(5, 4)),
                           false,
                           IncludeDuplicateRecordId::kOff),
            BSON("" << 1),
            boost::none);
        ASSERT_SDI_INSERT_DUPLICATE_KEY(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), BSON("" << 1), RecordId(5, 4)),
                           false,
                           IncludeDuplicateRecordId::kOn),
            BSON("" << 1),
            RecordId(5, 2));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
}
}  // namespace
}  // namespace mongo
