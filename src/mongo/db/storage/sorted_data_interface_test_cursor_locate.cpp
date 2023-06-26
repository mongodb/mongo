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

#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// Insert a key and try to locate it using a forward cursor
// by specifying its exact key and RecordId.
TEST(SortedDataInterface, Locate) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT(!cursor->seek(makeKeyStringForSeek(sorted.get(), key1, true, true)));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), makeKeyString(sorted.get(), key1, loc1), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), key1, true, true)),
                  IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert a key and try to locate it using a reverse cursor
// by specifying its exact key and RecordId.
TEST(SortedDataInterface, LocateReversed) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        ASSERT(!cursor->seek(makeKeyStringForSeek(sorted.get(), key1, false, true)));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), makeKeyString(sorted.get(), key1, loc1), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), key1, false, true)),
                  IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert a compound key and try to locate it using a forward cursor
// by specifying its exact key and RecordId.
TEST(SortedDataInterface, LocateCompoundKey) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT(!cursor->seek(makeKeyStringForSeek(sorted.get(), compoundKey1a, true, true)));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey1a, loc1), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), compoundKey1a, true, true)),
                  IndexKeyEntry(compoundKey1a, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert a compound key and try to locate it using a reverse cursor
// by specifying its exact key and RecordId.
TEST(SortedDataInterface, LocateCompoundKeyReversed) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        ASSERT(!cursor->seek(makeKeyStringForSeek(sorted.get(), compoundKey1a, false, true)));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey1a, loc1), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), compoundKey1a, false, true)),
                  IndexKeyEntry(compoundKey1a, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert multiple keys and try to locate them using a forward cursor
// by specifying their exact key and RecordId.
TEST(SortedDataInterface, LocateMultiple) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT(!cursor->seek(makeKeyStringForSeek(sorted.get(), key1, true, true)));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), makeKeyString(sorted.get(), key1, loc1), true));
            ASSERT_OK(sorted->insert(opCtx.get(), makeKeyString(sorted.get(), key2, loc2), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), key1, true, true)),
                  IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->next(), boost::none);
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), makeKeyString(sorted.get(), key3, loc3), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), key2, true, true)),
                  IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key3, loc3));
        ASSERT_EQ(cursor->next(), boost::none);

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), key1, true, true)),
                  IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key3, loc3));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert multiple keys and try to locate them using a reverse cursor
// by specifying their exact key and RecordId.
TEST(SortedDataInterface, LocateMultipleReversed) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        ASSERT(!cursor->seek(makeKeyStringForSeek(sorted.get(), key3, true, true)));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), makeKeyString(sorted.get(), key1, loc1), true));
            ASSERT_OK(sorted->insert(opCtx.get(), makeKeyString(sorted.get(), key2, loc2), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), key2, false, true)),
                  IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), makeKeyString(sorted.get(), key3, loc3), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), key2, false, true)),
                  IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), boost::none);

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), key3, false, true)),
                  IndexKeyEntry(key3, loc3));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert multiple compound keys and try to locate them using a forward cursor
// by specifying their exact key and RecordId.
TEST(SortedDataInterface, LocateMultipleCompoundKeys) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT(!cursor->seek(makeKeyStringForSeek(sorted.get(), compoundKey1a, true, true)));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey1a, loc1), true));
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey1b, loc2), true));
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey2b, loc3), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), compoundKey1a, true, true)),
                  IndexKeyEntry(compoundKey1a, loc1));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey1b, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey2b, loc3));
        ASSERT_EQ(cursor->next(), boost::none);
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey1c, loc4), true));
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey3a, loc5), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), compoundKey1a, true, true)),
                  IndexKeyEntry(compoundKey1a, loc1));
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
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        ASSERT(!cursor->seek(makeKeyStringForSeek(sorted.get(), compoundKey3a, false, true)));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey1a, loc1), true));
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey1b, loc2), true));
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey2b, loc3), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), compoundKey2b, false, true)),
                  IndexKeyEntry(compoundKey2b, loc3));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey1b, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey1a, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey1c, loc4), true));
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey3a, loc5), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), compoundKey3a, false, true)),
                  IndexKeyEntry(compoundKey3a, loc5));
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
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT(!cursor->seek(makeKeyStringForSeek(sorted.get(), key1, true, true)));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), makeKeyString(sorted.get(), key1, loc1), true));
            ASSERT_OK(sorted->insert(opCtx.get(), makeKeyString(sorted.get(), key2, loc2), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), key1, true, false)),
                  IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->next(), boost::none);
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), makeKeyString(sorted.get(), key3, loc3), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), key1, true, true)),
                  IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key3, loc3));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert multiple keys and try to locate them using a reverse cursor
// by specifying either a larger key or RecordId.
TEST(SortedDataInterface, LocateIndirectReversed) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        ASSERT(!cursor->seek(makeKeyStringForSeek(sorted.get(), key3, false, true)));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), makeKeyString(sorted.get(), key1, loc1), true));
            ASSERT_OK(sorted->insert(opCtx.get(), makeKeyString(sorted.get(), key2, loc2), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), key2, false, false)),
                  IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), makeKeyString(sorted.get(), key3, loc3), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), key3, false, true)),
                  IndexKeyEntry(key3, loc3));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key1, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert multiple compound keys and try to locate them using a forward cursor
// by specifying either a smaller key or RecordId.
TEST(SortedDataInterface, LocateIndirectCompoundKeys) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT(!cursor->seek(makeKeyStringForSeek(sorted.get(), compoundKey1a, true, true)));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey1a, loc1), true));
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey1b, loc2), true));
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey2b, loc3), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), compoundKey1a, true, false)),
                  IndexKeyEntry(compoundKey1b, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey2b, loc3));
        ASSERT_EQ(cursor->next(), boost::none);
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey1c, loc4), true));
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey3a, loc5), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), compoundKey2a, true, true)),
                  IndexKeyEntry(compoundKey2b, loc3));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey3a, loc5));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Insert multiple compound keys and try to locate them using a reverse cursor
// by specifying either a larger key or RecordId.
TEST(SortedDataInterface, LocateIndirectCompoundKeysReversed) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        ASSERT(!cursor->seek(makeKeyStringForSeek(sorted.get(), compoundKey3a, false, true)));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey1a, loc1), true));
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey1b, loc2), true));
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey2b, loc3), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), compoundKey2b, true, true)),
                  IndexKeyEntry(compoundKey1b, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey1a, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey1c, loc4), true));
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), compoundKey3a, loc5), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), compoundKey1d, true, true)),
                  IndexKeyEntry(compoundKey1c, loc4));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey1b, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(compoundKey1a, loc1));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

// Call locate on a forward cursor of an empty index and verify that the cursor
// is positioned at EOF.
TEST(SortedDataInterface, LocateEmpty) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));

        ASSERT(!cursor->seek(makeKeyStringForSeek(sorted.get(), BSONObj(), true, true)));
        ASSERT(!cursor->next());
    }
}

// Call locate on a reverse cursor of an empty index and verify that the cursor
// is positioned at EOF.
TEST(SortedDataInterface, LocateEmptyReversed) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));

        ASSERT(!cursor->seek(makeKeyStringForSeek(sorted.get(), BSONObj(), false, true)));
        ASSERT(!cursor->next());
    }
}


TEST(SortedDataInterface, Locate1) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    BSONObj key = BSON("" << 1);
    RecordId loc(5, 16);

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT(!cursor->seek(makeKeyStringForSeek(sorted.get(), key, true, true)));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), makeKeyString(sorted.get(), key, loc), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), key, true, true)),
                  IndexKeyEntry(key, loc));
    }
}

TEST(SortedDataInterface, Locate2) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());

            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), BSON("" << 1), RecordId(1, 2)), true));
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), BSON("" << 2), RecordId(1, 4)), true));
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), BSON("" << 3), RecordId(1, 6)), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), BSON("a" << 2), true, true)),
                  IndexKeyEntry(BSON("" << 2), RecordId(1, 4)));

        ASSERT_EQ(cursor->next(), IndexKeyEntry(BSON("" << 3), RecordId(1, 6)));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

TEST(SortedDataInterface, Locate2Empty) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());

            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), BSON("" << 1), RecordId(1, 2)), true));
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), BSON("" << 2), RecordId(1, 4)), true));
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), BSON("" << 3), RecordId(1, 6)), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), BSONObj(), true, true)),
                  IndexKeyEntry(BSON("" << 1), RecordId(1, 2)));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), BSONObj(), false, false)),
                  boost::none);
    }
}


TEST(SortedDataInterface, Locate3Descending) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    auto buildEntry = [](int i) {
        return IndexKeyEntry(BSON("" << i), RecordId(1, i * 2));
    };

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        for (int i = 0; i < 10; i++) {
            if (i == 6)
                continue;
            WriteUnitOfWork uow(opCtx.get());
            auto entry = buildEntry(i);
            ASSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), entry.key, entry.loc), true));
            uow.commit();
        }
    }

    const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get(), true));
    ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), BSON("" << 5), true, true)),
              buildEntry(5));
    ASSERT_EQ(cursor->next(), buildEntry(7));

    cursor = sorted->newCursor(opCtx.get(), /*forward*/ false);
    ASSERT_EQ(
        cursor->seek(makeKeyStringForSeek(sorted.get(), BSON("" << 5), false, /*inclusive*/ false)),
        buildEntry(4));

    cursor = sorted->newCursor(opCtx.get(), /*forward*/ false);
    ASSERT_EQ(
        cursor->seek(makeKeyStringForSeek(sorted.get(), BSON("" << 5), false, /*inclusive*/ true)),
        buildEntry(5));
    ASSERT_EQ(cursor->next(), buildEntry(4));

    cursor = sorted->newCursor(opCtx.get(), /*forward*/ false);
    ASSERT_EQ(
        cursor->seek(makeKeyStringForSeek(sorted.get(), BSON("" << 5), false, /*inclusive*/ false)),
        buildEntry(4));
    ASSERT_EQ(cursor->next(), buildEntry(3));

    cursor = sorted->newCursor(opCtx.get(), /*forward*/ false);
    ASSERT_EQ(
        cursor->seek(makeKeyStringForSeek(sorted.get(), BSON("" << 6), false, /*inclusive*/ true)),
        buildEntry(5));
    ASSERT_EQ(cursor->next(), buildEntry(4));

    cursor = sorted->newCursor(opCtx.get(), /*forward*/ false);
    ASSERT_EQ(cursor->seek(
                  makeKeyStringForSeek(sorted.get(), BSON("" << 500), false, /*inclusive*/ true)),
              buildEntry(9));
    ASSERT_EQ(cursor->next(), buildEntry(8));
}

TEST(SortedDataInterface, Locate4) {
    const auto harnessHelper = newSortedDataInterfaceHarnessHelper();
    auto sorted = harnessHelper->newSortedDataInterface(/*unique=*/false,
                                                        /*partial=*/false,
                                                        {
                                                            {BSON("" << 1), RecordId(1, 2)},
                                                            {BSON("" << 1), RecordId(1, 4)},
                                                            {BSON("" << 1), RecordId(1, 6)},
                                                            {BSON("" << 2), RecordId(1, 8)},
                                                        });

    {
        auto opCtx = harnessHelper->newOperationContext();
        auto cursor = sorted->newCursor(opCtx.get());
        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), BSON("a" << 1), true, true)),
                  IndexKeyEntry(BSON("" << 1), RecordId(1, 2)));

        ASSERT_EQ(cursor->next(), IndexKeyEntry(BSON("" << 1), RecordId(1, 4)));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(BSON("" << 1), RecordId(1, 6)));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(BSON("" << 2), RecordId(1, 8)));
        ASSERT_EQ(cursor->next(), boost::none);
    }

    {
        auto opCtx = harnessHelper->newOperationContext();
        auto cursor = sorted->newCursor(opCtx.get(), false);
        ASSERT_EQ(cursor->seek(makeKeyStringForSeek(sorted.get(), BSON("a" << 1), false, true)),
                  IndexKeyEntry(BSON("" << 1), RecordId(1, 6)));

        ASSERT_EQ(cursor->next(), IndexKeyEntry(BSON("" << 1), RecordId(1, 4)));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(BSON("" << 1), RecordId(1, 2)));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}


}  // namespace
}  // namespace mongo
