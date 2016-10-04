// sorted_data_interface_test_cursor_locate.cpp

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

// Insert a key and try to locate it using a forward cursor
// by specifying its exact key and RecordId.
TEST(SortedDataInterface, Locate) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT(!cursor->seek(key1, true));
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
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert a key and try to locate it using a reverse cursor
// by specifying its exact key and RecordId.
TEST(SortedDataInterface, LocateReversed) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        ASSERT(!cursor->seek(key1, true));
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
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert a compound key and try to locate it using a forward cursor
// by specifying its exact key and RecordId.
TEST(SortedDataInterface, LocateCompoundKey) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT(!cursor->seek(compoundKey1a, true));
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
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(compoundKey1a, true), IndexKeyEntry(compoundKey1a, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert a compound key and try to locate it using a reverse cursor
// by specifying its exact key and RecordId.
TEST(SortedDataInterface, LocateCompoundKeyReversed) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        ASSERT(!cursor->seek(compoundKey1a, true));
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
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(compoundKey1a, true), IndexKeyEntry(compoundKey1a, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert multiple keys and try to locate them using a forward cursor
// by specifying their exact key and RecordId.
TEST(SortedDataInterface, LocateMultiple) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT(!cursor->seek(key1, true));
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
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->next(), boost::none);
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc3, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(key2, true), IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key3, loc3));
        ASSERT_EQ(cursor->next(), boost::none);

        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key3, loc3));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert multiple keys and try to locate them using a reverse cursor
// by specifying their exact key and RecordId.
TEST(SortedDataInterface, LocateMultipleReversed) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        ASSERT(!cursor->seek(key3, true));
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
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(key2, true), IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc3, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(key2, true), IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), boost::none);

        ASSERT_EQ(cursor->seek(key3, true), IndexKeyEntry(key3, loc3));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert multiple compound keys and try to locate them using a forward cursor
// by specifying their exact key and RecordId.
TEST(SortedDataInterface, LocateMultipleCompoundKeys) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT(!cursor->seek(compoundKey1a, true));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey1a, loc1, true));
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey1b, loc2, true));
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey2b, loc3, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(compoundKey1a, true), IndexKeyEntry(compoundKey1a, loc1));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey1b, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey2b, loc3));
        ASSERT_EQ(cursor->next(), boost::none);
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey1c, loc4, true));
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey3a, loc5, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(compoundKey1a, true), IndexKeyEntry(compoundKey1a, loc1));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey1b, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey1c, loc4));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey2b, loc3));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey3a, loc5));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert multiple compound keys and try to locate them using a reverse cursor
// by specifying their exact key and RecordId.
TEST(SortedDataInterface, LocateMultipleCompoundKeysReversed) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        ASSERT(!cursor->seek(compoundKey3a, true));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey1a, loc1, true));
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey1b, loc2, true));
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey2b, loc3, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(compoundKey2b, true), IndexKeyEntry(compoundKey2b, loc3));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey1b, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey1a, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey1c, loc4, true));
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey3a, loc5, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(compoundKey3a, true), IndexKeyEntry(compoundKey3a, loc5));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey2b, loc3));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey1c, loc4));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey1b, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey1a, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert multiple keys and try to locate them using a forward cursor
// by specifying either a smaller key or RecordId.
TEST(SortedDataInterface, LocateIndirect) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT(!cursor->seek(key1, true));
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
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(key1, false), IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->next(), boost::none);
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc3, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key3, loc3));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert multiple keys and try to locate them using a reverse cursor
// by specifying either a larger key or RecordId.
TEST(SortedDataInterface, LocateIndirectReversed) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        ASSERT(!cursor->seek(key3, true));
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
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(key2, false), IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc3, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(key3, true), IndexKeyEntry(key3, loc3));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert multiple compound keys and try to locate them using a forward cursor
// by specifying either a smaller key or RecordId.
TEST(SortedDataInterface, LocateIndirectCompoundKeys) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT(!cursor->seek(compoundKey1a, true));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey1a, loc1, true));
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey1b, loc2, true));
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey2b, loc3, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(compoundKey1a, false), IndexKeyEntry(compoundKey1b, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey2b, loc3));
        ASSERT_EQ(cursor->next(), boost::none);
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey1c, loc4, true));
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey3a, loc5, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(compoundKey2a, true), IndexKeyEntry(compoundKey2b, loc3));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey3a, loc5));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert multiple compound keys and try to locate them using a reverse cursor
// by specifying either a larger key or RecordId.
TEST(SortedDataInterface, LocateIndirectCompoundKeysReversed) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        ASSERT(!cursor->seek(compoundKey3a, true));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey1a, loc1, true));
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey1b, loc2, true));
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey2b, loc3, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(compoundKey2b, false), IndexKeyEntry(compoundKey1b, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey1a, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey1c, loc4, true));
            ASSERT_OK(sorted->insert(opCtx.get(), compoundKey3a, loc5, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(compoundKey1d, true), IndexKeyEntry(compoundKey1c, loc4));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey1b, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey1a, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Call locate on a forward cursor of an empty index and verify that the cursor
// is positioned at EOF.
TEST(SortedDataInterface, LocateEmpty) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT(!cursor->seek(BSONObj(), true));
        ASSERT(!cursor->next());
    }
}

// Call locate on a reverse cursor of an empty index and verify that the cursor
// is positioned at EOF.
TEST(SortedDataInterface, LocateEmptyReversed) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT(!cursor->seek(BSONObj(), true));
        ASSERT(!cursor->next());
    }
}

}  // namespace mongo
