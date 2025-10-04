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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_assert.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {

// Insert multiple single-field keys and advance to each of them
// using a forward cursor by specifying their exact key. When
// advanceTo() is called on a duplicate key, the cursor is
// positioned at the first occurrence of that key in ascending
// order by RecordId.
TEST_F(SortedDataInterfaceTest, AdvanceTo) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key1, loc2),
                                            true /* allow duplicates */));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key1, loc3),
                                            true /* allow duplicates */));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key2, loc4), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key3, loc5), true));
        txn.commit();
    }

    ASSERT_EQUALS(5, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));

        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key1, loc1));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = key1;
        seekPoint.prefixLen = 1;
        seekPoint.firstExclusive = -1;
        key_string::Builder builder(sorted->getKeyStringVersion(), sorted->getOrdering());
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(key1, loc1));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = key2;
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(key2, loc4));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = key3;
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(key3, loc5));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = key4;
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  boost::none);
    }
}

// Insert multiple single-field keys and advance to each of them
// using a reverse cursor by specifying their exact key. When
// advanceTo() is called on a duplicate key, the cursor is
// positioned at the first occurrence of that key in descending
// order by RecordId (last occurrence in index order).
TEST_F(SortedDataInterfaceTest, AdvanceToReversed) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key2, loc2), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key3, loc3), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key3, loc4),
                                            true /* allow duplicates */));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key3, loc5),
                                            true /* allow duplicates */));
        txn.commit();
    }

    ASSERT_EQUALS(5, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        bool isForward = false;
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit(), isForward));

        ASSERT_EQ(
            cursor->seek(
                recoveryUnit(),
                makeKeyStringForSeek(sorted.get(), key3, isForward, true).finishAndGetBuffer()),
            IndexKeyEntry(key3, loc5));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = key3;
        seekPoint.prefixLen = 1;
        seekPoint.firstExclusive = -1;
        key_string::Builder builder(sorted->getKeyStringVersion(), sorted->getOrdering());
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, isForward, builder)),
                  IndexKeyEntry(key3, loc5));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = key2;
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, isForward, builder)),
                  IndexKeyEntry(key2, loc2));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = key1;
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, isForward, builder)),
                  IndexKeyEntry(key1, loc1));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = key0;
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, isForward, builder)),
                  boost::none);
    }
}

// Insert two single-field keys, then seek a forward cursor to the larger one then seek behind
// the smaller one.  Ending position is on the smaller one since a seek describes where to go
// and should not be effected by current position.
TEST_F(SortedDataInterfaceTest, AdvanceToKeyBeforeCursorPosition) {
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
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));

        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key1, loc1));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = key0;
        seekPoint.prefixLen = 1;
        seekPoint.firstExclusive = -1;
        key_string::Builder builder(sorted->getKeyStringVersion(), sorted->getOrdering());
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(key1, loc1));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.firstExclusive = 0;
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(key1, loc1));
    }
}

// Insert two single-field keys, then seek a reverse cursor to the smaller one then seek behind
// the larger one.  Ending position is on the larger one since a seek describes where to go
// and should not be effected by current position.
TEST_F(SortedDataInterfaceTest, AdvanceToKeyAfterCursorPositionReversed) {
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
        bool isForward = false;
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit(), isForward));

        ASSERT_EQ(
            cursor->seek(
                recoveryUnit(),
                makeKeyStringForSeek(sorted.get(), key2, isForward, true).finishAndGetBuffer()),
            IndexKeyEntry(key2, loc2));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = key3;
        seekPoint.prefixLen = 1;
        seekPoint.firstExclusive = -1;
        key_string::Builder builder(sorted->getKeyStringVersion(), sorted->getOrdering());
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, isForward, builder)),
                  IndexKeyEntry(key2, loc2));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.firstExclusive = 0;
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, isForward, builder)),
                  IndexKeyEntry(key2, loc2));
    }
}

// Insert a single-field key and advance to EOF using a forward cursor
// by specifying that exact key. When seek() is called with the key
// where the cursor is positioned (and it is the first entry for that key),
// the cursor should remain at its current position. An exclusive seek will
// position the cursor on the next position, which may be EOF.
TEST_F(SortedDataInterfaceTest, AdvanceToKeyAtCursorPosition) {
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

    {
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));

        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key1, loc1));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = key1;
        seekPoint.prefixLen = 1;
        seekPoint.firstExclusive = -1;
        key_string::Builder builder(sorted->getKeyStringVersion(), sorted->getOrdering());
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(key1, loc1));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.firstExclusive = 0;
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  boost::none);
    }
}

// Insert a single-field key and advance to EOF using a reverse cursor
// by specifying that exact key. When seek() is called with the key
// where the cursor is positioned (and it is the first entry for that key),
// the cursor should remain at its current position. An exclusive seek will
// position the cursor on the next position, which may be EOF.
TEST_F(SortedDataInterfaceTest, AdvanceToKeyAtCursorPositionReversed) {
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

    {
        bool isForward = false;
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit(), isForward));

        ASSERT_EQ(
            cursor->seek(
                recoveryUnit(),
                makeKeyStringForSeek(sorted.get(), key1, isForward, true).finishAndGetBuffer()),
            IndexKeyEntry(key1, loc1));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = key1;
        seekPoint.prefixLen = 1;
        seekPoint.firstExclusive = -1;
        key_string::Builder builder(sorted->getKeyStringVersion(), sorted->getOrdering());
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, isForward, builder)),
                  IndexKeyEntry(key1, loc1));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.firstExclusive = 0;
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, isForward, builder)),
                  boost::none);
    }
}

// Insert multiple single-field keys and advance to each of them using
// a forward cursor by specifying a key that comes immediately before.
// When advanceTo() is called in non-inclusive mode, the cursor is
// positioned at the key that comes after the one specified.
TEST_F(SortedDataInterfaceTest, AdvanceToExclusive) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key1, loc2),
                                            true /* allow duplicates */));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key1, loc3),
                                            true /* allow duplicates */));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key2, loc4), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key3, loc5), true));
        txn.commit();
    }

    ASSERT_EQUALS(5, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));

        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key1, loc1));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = key1;
        seekPoint.prefixLen = 1;
        seekPoint.firstExclusive = 0;
        key_string::Builder builder(sorted->getKeyStringVersion(), sorted->getOrdering());
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(key2, loc4));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = key2;
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(key3, loc5));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = key3;
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  boost::none);
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = key4;
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  boost::none);
    }
}

// Insert multiple single-field keys and advance to each of them using
// a reverse cursor by specifying a key that comes immediately after.
// When advanceTo() is called in non-inclusive mode, the cursor is
// positioned at the key that comes before the one specified.
TEST_F(SortedDataInterfaceTest, AdvanceToExclusiveReversed) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key2, loc2), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key3, loc3), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key3, loc4),
                                            true /* allow duplicates */));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key3, loc5),
                                            true /* allow duplicates */));
        txn.commit();
    }

    ASSERT_EQUALS(5, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        bool isForward = false;
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit(), isForward));

        ASSERT_EQ(
            cursor->seek(
                recoveryUnit(),
                makeKeyStringForSeek(sorted.get(), key3, isForward, true).finishAndGetBuffer()),
            IndexKeyEntry(key3, loc5));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = key3;
        seekPoint.prefixLen = 1;
        seekPoint.firstExclusive = 0;
        key_string::Builder builder(sorted->getKeyStringVersion(), sorted->getOrdering());
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, isForward, builder)),
                  IndexKeyEntry(key2, loc2));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = key2;
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, isForward, builder)),
                  IndexKeyEntry(key1, loc1));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = key1;
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, isForward, builder)),
                  boost::none);
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = key0;
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, isForward, builder)),
                  boost::none);
    }
}

// Insert multiple, non-consecutive, single-field keys and advance to
// each of them using a forward cursor by specifying a key between their
// exact key and the current position of the cursor.
TEST_F(SortedDataInterfaceTest, AdvanceToIndirect) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    BSONObj unusedKey = key6;  // larger than any inserted key

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key3, loc2), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key5, loc3), true));
        txn.commit();
    }

    ASSERT_EQUALS(3, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));

        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key1, loc1));

        IndexSeekPoint seekPoint;
        seekPoint.prefixLen = 0;
        seekPoint.keySuffix = {key2.firstElement()};
        seekPoint.firstExclusive = -1;

        key_string::Builder builder(sorted->getKeyStringVersion(), sorted->getOrdering());
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(key3, loc2));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keySuffix = {key4.firstElement()};
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(key5, loc3));
    }
}

// Insert multiple, non-consecutive, single-field keys and advance to
// each of them using a reverse cursor by specifying a key between their
// exact key and the current position of the cursor.
TEST_F(SortedDataInterfaceTest, AdvanceToIndirectReversed) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    BSONObj unusedKey = key0;  // smaller than any inserted key

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key3, loc2), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key5, loc3), true));
        txn.commit();
    }

    ASSERT_EQUALS(3, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit(), false));

        ASSERT_EQ(cursor->seek(
                      recoveryUnit(),
                      makeKeyStringForSeek(sorted.get(), key5, false, true).finishAndGetBuffer()),
                  IndexKeyEntry(key5, loc3));

        IndexSeekPoint seekPoint;
        seekPoint.prefixLen = 0;
        seekPoint.keySuffix = {key4.firstElement()};
        seekPoint.firstExclusive = -1;

        key_string::Builder builder(sorted->getKeyStringVersion(), sorted->getOrdering());
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(key3, loc2));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keySuffix = {key2.firstElement()};
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(key1, loc1));
    }
}

// Insert multiple, non-consecutive, single-field keys and advance to
// each of them using a forward cursor by specifying a key between their
// exact key and the current position of the cursor. When advanceTo()
// is called in non-inclusive mode, the cursor is positioned at the key
// that comes after the one specified.
TEST_F(SortedDataInterfaceTest, AdvanceToIndirectExclusive) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    BSONObj unusedKey = key6;  // larger than any inserted key

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key3, loc2), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key5, loc3), true));
        txn.commit();
    }

    ASSERT_EQUALS(3, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));

        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key1, loc1));

        IndexSeekPoint seekPoint;
        seekPoint.prefixLen = 0;
        seekPoint.keySuffix = {key2.firstElement()};
        seekPoint.firstExclusive = 0;

        key_string::Builder builder(sorted->getKeyStringVersion(), sorted->getOrdering());
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(key3, loc2));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keySuffix = {key4.firstElement()};
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(key5, loc3));
        builder.resetToEmpty(sorted->getOrdering());

        ASSERT_EQ(
            cursor->seek(recoveryUnit(),
                         makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key1, loc1));

        seekPoint.keySuffix = {key3.firstElement()};
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(key5, loc3));
    }
}

// Insert multiple, non-consecutive, single-field keys and advance to
// each of them using a reverse cursor by specifying a key between their
// exact key and the current position of the cursor. When advanceTo()
// is called in non-inclusive mode, the cursor is positioned at the key
// that comes before the one specified.
TEST_F(SortedDataInterfaceTest, AdvanceToIndirectExclusiveReversed) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    BSONObj unusedKey = key0;  // smaller than any inserted key

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key1, loc1), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key3, loc2), true));
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key5, loc3), true));
        txn.commit();
    }

    ASSERT_EQUALS(3, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        bool isForward = false;
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit(), isForward));

        ASSERT_EQ(
            cursor->seek(
                recoveryUnit(),
                makeKeyStringForSeek(sorted.get(), key5, isForward, true).finishAndGetBuffer()),
            IndexKeyEntry(key5, loc3));

        IndexSeekPoint seekPoint;
        seekPoint.prefixLen = 0;
        seekPoint.keySuffix = {key4.firstElement()};
        seekPoint.firstExclusive = 0;

        key_string::Builder builder(sorted->getKeyStringVersion(), sorted->getOrdering());
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, isForward, builder)),
                  IndexKeyEntry(key3, loc2));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keySuffix = {key2.firstElement()};
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, isForward, builder)),
                  IndexKeyEntry(key1, loc1));
        builder.resetToEmpty(sorted->getOrdering());

        ASSERT_EQ(
            cursor->seek(
                recoveryUnit(),
                makeKeyStringForSeek(sorted.get(), key5, isForward, true).finishAndGetBuffer()),
            IndexKeyEntry(key5, loc3));

        seekPoint.keySuffix = {key3.firstElement()};
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, isForward, builder)),
                  IndexKeyEntry(key1, loc1));
    }
}

// Insert multiple two-field keys and advance to each of them using a forward cursor by specifying
// their exact key. When advanceTo() is called on a duplicate key, the cursor is positioned at the
// first occurrence of that key in ascending order by RecordId.
TEST_F(SortedDataInterfaceTest, AdvanceToCompoundWithPrefixAndSuffixInclusive) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey1a, loc1), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), compoundKey1a, loc2),
                                            true /* allow duplicates */));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), compoundKey1a, loc3),
                                            true /* allow duplicates */));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey2b, loc4), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey3b, loc5), true));
        txn.commit();
    }

    ASSERT_EQUALS(5, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));

        ASSERT_EQ(
            cursor->seek(
                recoveryUnit(),
                makeKeyStringForSeek(sorted.get(), compoundKey1a, true, true).finishAndGetBuffer()),
            IndexKeyEntry(compoundKey1a, loc1));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = compoundKey1a;
        seekPoint.prefixLen = 1;  // Get first field from the prefix
        std::vector<BSONElement> suffix;
        compoundKey1a.elems(suffix);
        seekPoint.keySuffix = {suffix[0], suffix[1]};
        seekPoint.firstExclusive = -1;  // Get second field from the suffix, no exclusive fields

        key_string::Builder builder(sorted->getKeyStringVersion(), sorted->getOrdering());
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(compoundKey1a, loc1));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = compoundKey2b;
        suffix.clear();
        compoundKey2b.elems(suffix);
        seekPoint.keySuffix = {suffix[0], suffix[1]};
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(compoundKey2b, loc4));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = compoundKey3b;
        suffix.clear();
        compoundKey3b.elems(suffix);
        seekPoint.keySuffix = {suffix[0], suffix[1]};
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(compoundKey3b, loc5));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = compoundKey3c;
        suffix.clear();
        compoundKey3c.elems(suffix);
        seekPoint.keySuffix = {suffix[0], suffix[1]};
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  boost::none);
    }
}

// Insert multiple two-field keys and advance to each of them using a forward cursor by specifying a
// key that comes before. When advanceTo() is called in non-inclusive mode, the cursor is positioned
// at the key that comes after the one specified. When dealing with prefixes, that means that any
// keys that match on the prefix are skipped.
TEST_F(SortedDataInterfaceTest, AdvanceToCompoundWithPrefixExclusive) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey1a, loc1), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey1b, loc2), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey1c, loc3), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey2b, loc4), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey3b, loc5), true));
        txn.commit();
    }

    ASSERT_EQUALS(5, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));

        ASSERT_EQ(
            cursor->seek(
                recoveryUnit(),
                makeKeyStringForSeek(sorted.get(), compoundKey1a, true, true).finishAndGetBuffer()),
            IndexKeyEntry(compoundKey1a, loc1));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = compoundKey1a;
        seekPoint.prefixLen = 1;  // Get first field from prefix
        std::vector<BSONElement> suffix;
        compoundKey1a.elems(suffix);
        seekPoint.keySuffix = {suffix[0], suffix[1]};
        seekPoint.firstExclusive = 0;  // Ignore the suffix, make prefix exclusive

        key_string::Builder builder(sorted->getKeyStringVersion(), sorted->getOrdering());
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(compoundKey2b, loc4));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = compoundKey2b;
        suffix.clear();
        compoundKey2b.elems(suffix);
        seekPoint.keySuffix = {suffix[0], suffix[1]};
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(compoundKey3b, loc5));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = compoundKey3b;
        suffix.clear();
        compoundKey3b.elems(suffix);
        seekPoint.keySuffix = {suffix[0], suffix[1]};
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  boost::none);
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = compoundKey3c;
        suffix.clear();
        compoundKey3c.elems(suffix);
        seekPoint.keySuffix = {suffix[0], suffix[1]};
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  boost::none);
    }
}

// Insert multiple two-field keys and advance to each of them using a forward cursor by specifying a
// key that comes before. When advanceTo() is called in non-inclusive mode, the cursor is positioned
// at the key that comes after the one specified.
TEST_F(SortedDataInterfaceTest, AdvanceToCompoundWithPrefixAndSuffixExclusive) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey1a, loc1), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey1b, loc2), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey1c, loc3), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey2b, loc4), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey3b, loc5), true));
        txn.commit();
    }

    ASSERT_EQUALS(5, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));

        ASSERT_EQ(
            cursor->seek(
                recoveryUnit(),
                makeKeyStringForSeek(sorted.get(), compoundKey1a, true, true).finishAndGetBuffer()),
            IndexKeyEntry(compoundKey1a, loc1));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = compoundKey1a;
        seekPoint.prefixLen = 1;  // Get first field from the prefix
        std::vector<BSONElement> suffix;
        compoundKey1a.elems(suffix);
        seekPoint.keySuffix = {suffix[0], suffix[1]};
        seekPoint.firstExclusive = 1;  // Get second field from suffix, make it exclusive

        key_string::Builder builder(sorted->getKeyStringVersion(), sorted->getOrdering());
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(compoundKey1b, loc2));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = compoundKey2b;
        suffix.clear();
        compoundKey2b.elems(suffix);
        seekPoint.keySuffix = {suffix[0], suffix[1]};
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(compoundKey3b, loc5));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = compoundKey3b;
        suffix.clear();
        compoundKey3b.elems(suffix);
        seekPoint.keySuffix = {suffix[0], suffix[1]};
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  boost::none);
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = compoundKey3c;
        suffix.clear();
        compoundKey3c.elems(suffix);
        seekPoint.keySuffix = {suffix[0], suffix[1]};
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  boost::none);
    }
}

// Insert multiple two-field keys and advance to each of them using a forward cursor by specifying a
// key that comes before. When advanceTo() is called in non-inclusive mode, the cursor is positioned
// at the key that comes after the one specified.
TEST_F(SortedDataInterfaceTest, AdvanceToCompoundWithSuffixExclusive) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey1a, loc1), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey1b, loc2), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey1c, loc3), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey2b, loc4), true));
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), recoveryUnit(), makeKeyString(sorted.get(), compoundKey3b, loc5), true));
        txn.commit();
    }

    ASSERT_EQUALS(5, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));

        ASSERT_EQ(
            cursor->seek(
                recoveryUnit(),
                makeKeyStringForSeek(sorted.get(), compoundKey1a, true, true).finishAndGetBuffer()),
            IndexKeyEntry(compoundKey1a, loc1));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = compoundKey1a;
        seekPoint.prefixLen = 0;  // Ignore the prefix
        std::vector<BSONElement> suffix;
        compoundKey1a.elems(suffix);
        seekPoint.keySuffix = {suffix[0], suffix[1]};
        seekPoint.firstExclusive = 1;  // Get both fields from the suffix, make the second exclusive
        key_string::Builder builder(sorted->getKeyStringVersion(), sorted->getOrdering());

        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(compoundKey1b, loc2));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = compoundKey2b;
        suffix.clear();
        compoundKey2b.elems(suffix);
        seekPoint.keySuffix = {suffix[0], suffix[1]};
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  IndexKeyEntry(compoundKey3b, loc5));
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = compoundKey3b;
        suffix.clear();
        compoundKey3b.elems(suffix);
        seekPoint.keySuffix = {suffix[0], suffix[1]};
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  boost::none);
        builder.resetToEmpty(sorted->getOrdering());

        seekPoint.keyPrefix = compoundKey3c;
        suffix.clear();
        compoundKey3c.elems(suffix);
        seekPoint.keySuffix = {suffix[0], suffix[1]};
        ASSERT_EQ(cursor->seek(recoveryUnit(),
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                   seekPoint, true, builder)),
                  boost::none);
    }
}
}  // namespace
}  // namespace mongo
