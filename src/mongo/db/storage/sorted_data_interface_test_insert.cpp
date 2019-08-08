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

#include "mongo/db/storage/sorted_data_interface_test_harness.h"

#include <memory>

#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// Insert a key and verify that the number of entries in the index equals 1.
TEST(SortedDataInterface, Insert) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

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

        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
    }
}

// Insert a KeyString and verify that the number of entries in the index equals 1.
TEST(SortedDataInterface, InsertKeyString) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    KeyString::Builder keyString1(sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc1);

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), keyString1.getValueCopy(), loc1, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));

        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
    }
}

// Insert a compound key and verify that the number of entries in the index equals 1.
TEST(SortedDataInterface, InsertCompoundKey) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey1a, loc1, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }
}

// Insert multiple, distinct keys at the same RecordId and verify that the
// number of entries in the index equals the number that were inserted, even
// when duplicates are not allowed.
TEST(SortedDataInterface, InsertSameDiskLoc) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, true));
            ASSERT_OK(sorted->insert(opCtx.get(), key2, loc1, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(2, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc1, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(3, sorted->numEntries(opCtx.get()));
    }
}

// Insert multiple, distinct keys at the same RecordId and verify that the
// number of entries in the index equals the number that were inserted, even
// when duplicates are allowed.
TEST(SortedDataInterface, InsertSameDiskLocWithDupsAllowed) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/true, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, false));
            ASSERT_OK(sorted->insert(opCtx.get(), key2, loc1, true /* allow duplicates */));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(2, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc1, true /* allow duplicates */));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(3, sorted->numEntries(opCtx.get()));
    }
}

// Insert the same key multiple times and verify that only 1 entry exists
// in the index when duplicates are not allowed.
TEST(SortedDataInterface, InsertSameKey) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/true, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, false));
            ASSERT_NOT_OK(sorted->insert(opCtx.get(), key1, loc2, false));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));

        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_NOT_OK(sorted->insert(opCtx.get(), key1, loc2, false));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));

        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
    }
}

/*
 * Insert the same KeyString multiple times and verify that only 1 entry exists in the index when
 * duplicates are not allowed.
 */
TEST(SortedDataInterface, InsertSameKeyString) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/true, /*partial=*/false));

    KeyString::Builder keyStringLoc1(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc1);
    KeyString::Builder keyStringLoc2(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc2);

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), keyStringLoc1.getValueCopy(), loc1, false));
            ASSERT_NOT_OK(sorted->insert(opCtx.get(), keyStringLoc2.getValueCopy(), loc2, false));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));

        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_NOT_OK(sorted->insert(opCtx.get(), keyStringLoc2.getValueCopy(), loc2, false));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));

        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
    }
}

namespace {

// Insert the same key multiple times and verify that all entries exists
// in the index when duplicates are allowed. Since it is illegal to open a cursor to an unique
// index while the unique constraint is violated, this is tested by running the test 3 times,
// removing all but one loc each time and verifying the correct loc remains.
void _testInsertSameKeyWithDupsAllowed(const RecordId locs[3]) {
    for (int keeper = 0; keeper < 3; keeper++) {
        const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
        const std::unique_ptr<SortedDataInterface> sorted(
            harnessHelper->newSortedDataInterface(/*unique=*/true, /*partial=*/false));

        {
            const ServiceContext::UniqueOperationContext opCtx(
                harnessHelper->newOperationContext());
            ASSERT(sorted->isEmpty(opCtx.get()));
        }

        {
            const ServiceContext::UniqueOperationContext opCtx(
                harnessHelper->newOperationContext());
            {
                WriteUnitOfWork uow(opCtx.get());
                ASSERT_OK(sorted->insert(opCtx.get(), key1, locs[0], false));
                ASSERT_OK(sorted->insert(opCtx.get(), key1, locs[1], true));
                ASSERT_OK(sorted->insert(opCtx.get(), key1, locs[2], true));
                uow.commit();
            }
        }

        {
            const ServiceContext::UniqueOperationContext opCtx(
                harnessHelper->newOperationContext());
            {
                WriteUnitOfWork uow(opCtx.get());
                for (int i = 0; i < 3; i++) {
                    if (i != keeper) {
                        sorted->unindex(opCtx.get(), key1, locs[i], true);
                    }
                }
                uow.commit();
            }
        }

        {
            const ServiceContext::UniqueOperationContext opCtx(
                harnessHelper->newOperationContext());
            ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));

            const std::unique_ptr<SortedDataInterface::Cursor> cursor(
                sorted->newCursor(opCtx.get()));
            ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, locs[keeper]));
        }
    }
}

}  // namespace

TEST(SortedDataInterface, InsertSameKeyWithDupsAllowedLocsAscending) {
    const RecordId locs[3] = {loc1, loc2, loc3};
    _testInsertSameKeyWithDupsAllowed(locs);
}

TEST(SortedDataInterface, InsertSameKeyWithDupsAllowedLocsDescending) {
    const RecordId locs[3] = {loc3, loc2, loc1};
    _testInsertSameKeyWithDupsAllowed(locs);
}

// Insert multiple keys and verify that the number of entries
// in the index equals the number that were inserted.
TEST(SortedDataInterface, InsertMultiple) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/true, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, false));
            ASSERT_OK(sorted->insert(opCtx.get(), key2, loc2, false));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(2, sorted->numEntries(opCtx.get()));

        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->seek(key2, true), IndexKeyEntry(key2, loc2));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc3, false));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(3, sorted->numEntries(opCtx.get()));

        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->seek(key2, true), IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->seek(key3, true), IndexKeyEntry(key3, loc3));
    }
}

/*
 * Insert multiple KeyStrings and verify that the number of entries in the index equals the number
 * that were inserted.
 */
TEST(SortedDataInterface, InsertMultipleKeyStrings) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/true, /*partial=*/false));

    KeyString::Builder keyString1(sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc1);
    KeyString::Builder keyString2(sorted->getKeyStringVersion(), key2, sorted->getOrdering(), loc2);
    KeyString::Builder keyString3(sorted->getKeyStringVersion(), key3, sorted->getOrdering(), loc3);

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), keyString1.getValueCopy(), loc1, false));
            ASSERT_OK(sorted->insert(opCtx.get(), keyString2.getValueCopy(), loc2, false));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(2, sorted->numEntries(opCtx.get()));

        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->seek(key2, true), IndexKeyEntry(key2, loc2));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), keyString3.getValueCopy(), loc3, false));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(3, sorted->numEntries(opCtx.get()));

        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->seek(key2, true), IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->seek(key3, true), IndexKeyEntry(key3, loc3));
    }
}

// Insert multiple compound keys and verify that the number of entries
// in the index equals the number that were inserted.
TEST(SortedDataInterface, InsertMultipleCompoundKeys) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/true, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey1a, loc1, false));
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey1b, loc2, false));
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey2b, loc3, false));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(3, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey1c, loc4, false));
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey3a, loc5, false));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(5, sorted->numEntries(opCtx.get()));
    }
}

TEST(SortedDataInterface, InsertReservedRecordId) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));
    const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    ASSERT(sorted->isEmpty(opCtx.get()));
    WriteUnitOfWork uow(opCtx.get());
    RecordId reservedLoc(RecordId::ReservedId::kWildcardMultikeyMetadataId);
    ASSERT(reservedLoc.isReserved());
    ASSERT_OK(sorted->insert(opCtx.get(), key1, reservedLoc, /*dupsAllowed*/ true));
    uow.commit();
    ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
}

}  // namespace
}  // namespace mongo
