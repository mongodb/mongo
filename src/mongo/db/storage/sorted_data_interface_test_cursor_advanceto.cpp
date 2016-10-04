// sorted_data_interface_test_cursor_advanceto.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/storage/sorted_data_interface_test_harness.h"

#include <memory>

#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

// Insert multiple single-field keys and advance to each of them
// using a forward cursor by specifying their exact key. When
// advanceTo() is called on a duplicate key, the cursor is
// positioned at the first occurrence of that key in ascending
// order by RecordId.
TEST(SortedDataInterface, AdvanceTo) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc2, true /* allow duplicates */));
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc3, true /* allow duplicates */));
            ASSERT_OK(sorted->insert(opCtx.get(), key2, loc4, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc5, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(5, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = key1;
        seekPoint.prefixLen = 1;
        seekPoint.prefixExclusive = false;
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key1, loc1));

        seekPoint.keyPrefix = key2;
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key2, loc4));

        seekPoint.keyPrefix = key3;
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key3, loc5));

        seekPoint.keyPrefix = key4;
        ASSERT_EQ(cursor->seek(seekPoint), boost::none);
    }
}

// Insert multiple single-field keys and advance to each of them
// using a reverse cursor by specifying their exact key. When
// advanceTo() is called on a duplicate key, the cursor is
// positioned at the first occurrence of that key in descending
// order by RecordId (last occurrence in index order).
TEST(SortedDataInterface, AdvanceToReversed) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key2, loc2, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc3, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc4, true /* allow duplicates */));
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc5, true /* allow duplicates */));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(5, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(key3, true), IndexKeyEntry(key3, loc5));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = key3;
        seekPoint.prefixLen = 1;
        seekPoint.prefixExclusive = false;
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key3, loc5));

        seekPoint.keyPrefix = key2;
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key2, loc2));

        seekPoint.keyPrefix = key1;
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key1, loc1));

        seekPoint.keyPrefix = key0;
        ASSERT_EQ(cursor->seek(seekPoint), boost::none);
    }
}

// Insert two single-field keys, then seek a forward cursor to the larger one then seek behind
// the smaller one.  Ending position is on the smaller one since a seek describes where to go
// and should not be effected by current position.
TEST(SortedDataInterface, AdvanceToKeyBeforeCursorPosition) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key2, loc2, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(2, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = key0;
        seekPoint.prefixLen = 1;
        seekPoint.prefixExclusive = false;
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key1, loc1));

        seekPoint.prefixExclusive = true;
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key1, loc1));
    }
}

// Insert two single-field keys, then seek a reverse cursor to the smaller one then seek behind
// the larger one.  Ending position is on the larger one since a seek describes where to go
// and should not be effected by current position.
TEST(SortedDataInterface, AdvanceToKeyAfterCursorPositionReversed) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key2, loc2, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(2, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(key2, true), IndexKeyEntry(key2, loc2));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = key3;
        seekPoint.prefixLen = 1;
        seekPoint.prefixExclusive = false;
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key2, loc2));

        seekPoint.prefixExclusive = true;
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key2, loc2));
    }
}

// Insert a single-field key and advance to EOF using a forward cursor
// by specifying that exact key. When seek() is called with the key
// where the cursor is positioned (and it is the first entry for that key),
// the cursor should remain at its current position. An exclusive seek will
// position the cursor on the next position, which may be EOF.
TEST(SortedDataInterface, AdvanceToKeyAtCursorPosition) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = key1;
        seekPoint.prefixLen = 1;
        seekPoint.prefixExclusive = false;
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key1, loc1));

        seekPoint.prefixExclusive = true;
        ASSERT_EQ(cursor->seek(seekPoint), boost::none);
    }
}

// Insert a single-field key and advance to EOF using a reverse cursor
// by specifying that exact key. When seek() is called with the key
// where the cursor is positioned (and it is the first entry for that key),
// the cursor should remain at its current position. An exclusive seek will
// position the cursor on the next position, which may be EOF.
TEST(SortedDataInterface, AdvanceToKeyAtCursorPositionReversed) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = key1;
        seekPoint.prefixLen = 1;
        seekPoint.prefixExclusive = false;
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key1, loc1));

        seekPoint.prefixExclusive = true;
        ASSERT_EQ(cursor->seek(seekPoint), boost::none);
    }
}

// Insert multiple single-field keys and advance to each of them using
// a forward cursor by specifying a key that comes immediately before.
// When advanceTo() is called in non-inclusive mode, the cursor is
// positioned at the key that comes after the one specified.
TEST(SortedDataInterface, AdvanceToExclusive) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc2, true /* allow duplicates */));
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc3, true /* allow duplicates */));
            ASSERT_OK(sorted->insert(opCtx.get(), key2, loc4, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc5, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(5, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = key1;
        seekPoint.prefixLen = 1;
        seekPoint.prefixExclusive = true;
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key2, loc4));

        seekPoint.keyPrefix = key2;
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key3, loc5));

        seekPoint.keyPrefix = key3;
        ASSERT_EQ(cursor->seek(seekPoint), boost::none);

        seekPoint.keyPrefix = key4;
        ASSERT_EQ(cursor->seek(seekPoint), boost::none);
    }
}

// Insert multiple single-field keys and advance to each of them using
// a reverse cursor by specifying a key that comes immediately after.
// When advanceTo() is called in non-inclusive mode, the cursor is
// positioned at the key that comes before the one specified.
TEST(SortedDataInterface, AdvanceToExclusiveReversed) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key2, loc2, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc3, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc4, true /* allow duplicates */));
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc5, true /* allow duplicates */));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(5, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(key3, true), IndexKeyEntry(key3, loc5));

        IndexSeekPoint seekPoint;
        seekPoint.keyPrefix = key3;
        seekPoint.prefixLen = 1;
        seekPoint.prefixExclusive = true;
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key2, loc2));

        seekPoint.keyPrefix = key2;
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key1, loc1));

        seekPoint.keyPrefix = key1;
        ASSERT_EQ(cursor->seek(seekPoint), boost::none);

        seekPoint.keyPrefix = key0;
        ASSERT_EQ(cursor->seek(seekPoint), boost::none);
    }
}

// Insert multiple, non-consecutive, single-field keys and advance to
// each of them using a forward cursor by specifying a key between their
// exact key and the current position of the cursor.
TEST(SortedDataInterface, AdvanceToIndirect) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    BSONObj unusedKey = key6;  // larger than any inserted key

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc2, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key5, loc3, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(3, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));

        IndexSeekPoint seekPoint;
        seekPoint.prefixLen = 0;
        BSONElement suffix0;
        seekPoint.keySuffix = {&suffix0};
        seekPoint.suffixInclusive = {true};

        suffix0 = key2.firstElement();
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key3, loc2));

        suffix0 = key4.firstElement();
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key5, loc3));
    }
}

// Insert multiple, non-consecutive, single-field keys and advance to
// each of them using a reverse cursor by specifying a key between their
// exact key and the current position of the cursor.
TEST(SortedDataInterface, AdvanceToIndirectReversed) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    BSONObj unusedKey = key0;  // smaller than any inserted key

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc2, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key5, loc3, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(3, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(key5, true), IndexKeyEntry(key5, loc3));

        IndexSeekPoint seekPoint;
        seekPoint.prefixLen = 0;
        BSONElement suffix0;
        seekPoint.keySuffix = {&suffix0};
        seekPoint.suffixInclusive = {true};

        suffix0 = key4.firstElement();
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key3, loc2));

        suffix0 = key2.firstElement();
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key1, loc1));
    }
}

// Insert multiple, non-consecutive, single-field keys and advance to
// each of them using a forward cursor by specifying a key between their
// exact key and the current position of the cursor. When advanceTo()
// is called in non-inclusive mode, the cursor is positioned at the key
// that comes after the one specified.
TEST(SortedDataInterface, AdvanceToIndirectExclusive) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    BSONObj unusedKey = key6;  // larger than any inserted key

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc2, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key5, loc3, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(3, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));

        IndexSeekPoint seekPoint;
        seekPoint.prefixLen = 0;
        BSONElement suffix0;
        seekPoint.keySuffix = {&suffix0};
        seekPoint.suffixInclusive = {false};

        suffix0 = key2.firstElement();
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key3, loc2));

        suffix0 = key4.firstElement();
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key5, loc3));

        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));

        suffix0 = key3.firstElement();
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key5, loc3));
    }
}

// Insert multiple, non-consecutive, single-field keys and advance to
// each of them using a reverse cursor by specifying a key between their
// exact key and the current position of the cursor. When advanceTo()
// is called in non-inclusive mode, the cursor is positioned at the key
// that comes before the one specified.
TEST(SortedDataInterface, AdvanceToIndirectExclusiveReversed) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    BSONObj unusedKey = key0;  // smaller than any inserted key

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc2, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key5, loc3, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(3, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(key5, true), IndexKeyEntry(key5, loc3));

        IndexSeekPoint seekPoint;
        seekPoint.prefixLen = 0;
        BSONElement suffix0;
        seekPoint.keySuffix = {&suffix0};
        seekPoint.suffixInclusive = {false};

        suffix0 = key4.firstElement();
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key3, loc2));

        suffix0 = key2.firstElement();
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key1, loc1));

        ASSERT_EQ(cursor->seek(key5, true), IndexKeyEntry(key5, loc3));

        suffix0 = key3.firstElement();
        ASSERT_EQ(cursor->seek(seekPoint), IndexKeyEntry(key1, loc1));
    }
}

}  // namespace mongo
