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

// Insert a key and verify that dupKeyCheck() returns a non-OK status for
// the same key. When dupKeyCheck() is called with the exact (key, RecordId)
// pair that was inserted, it should still return an OK status.
TEST(SortedDataInterface, DupKeyCheckAfterInsert) {
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
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->dupKeyCheck(opCtx.get(), key1));
            uow.commit();
        }
    }
}

// Insert a KeyString and verify that dupKeyCheck() returns a non-OK status for
// the same KeyString. When dupKeyCheck() is called with the exact KeyString
// pair that was inserted, it should still return an OK status.
TEST(SortedDataInterface, DupKeyCheckAfterInsertKeyString) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/true, /*partial=*/false));

    KeyString::Builder keyString1(sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc1);
    KeyString::Builder keyString1WithoutRecordId(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering());

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), keyString1.getValueCopy(), loc1, false));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_OK(sorted->dupKeyCheck(opCtx.get(), keyString1WithoutRecordId.getValueCopy()));
    }
}

// Verify that dupKeyCheck() returns an OK status for a key that does
// not exist in the index.
TEST(SortedDataInterface, DupKeyCheckEmpty) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/true, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_OK(sorted->dupKeyCheck(opCtx.get(), key1));
    }
}

// Verify that dupKeyCheck() returns an OK status for a KeyString that does
// not exist in the index.
TEST(SortedDataInterface, DupKeyCheckEmptyKeyString) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/true, /*partial=*/false));

    KeyString::Builder keyString1WithoutRecordId(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering());

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_OK(sorted->dupKeyCheck(opCtx.get(), keyString1WithoutRecordId.getValueCopy()));
    }
}

// Insert a key and verify that dupKeyCheck() acknowledges the duplicate key, even
// when the insert key is located at a RecordId that comes after the one specified.
TEST(SortedDataInterface, DupKeyCheckWhenDiskLocBefore) {
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
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->dupKeyCheck(opCtx.get(), key1));
            uow.commit();
        }
    }
}

// Insert a key and verify that dupKeyCheck() acknowledges the duplicate key, even
// when the insert key is located at a RecordId that comes before the one specified.
TEST(SortedDataInterface, DupKeyCheckWhenDiskLocAfter) {
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
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->dupKeyCheck(opCtx.get(), key1));
            uow.commit();
        }
    }
}

TEST(SortedDataInterface, DupKeyCheckWithDuplicates) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/true, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));

        WriteUnitOfWork uow(opCtx.get());
        ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, true));
        ASSERT_OK(sorted->insert(opCtx.get(), key1, loc2, true));
        uow.commit();
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(2, sorted->numEntries(opCtx.get()));
        ASSERT_NOT_OK(sorted->dupKeyCheck(opCtx.get(), key1));
    }
}

TEST(SortedDataInterface, DupKeyCheckWithDuplicateKeyStrings) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/true, /*partial=*/false));

    KeyString::Builder keyString1(sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc1);
    KeyString::Builder keyString2(sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc2);
    KeyString::Builder keyString1WithoutRecordId(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering());

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));

        WriteUnitOfWork uow(opCtx.get());
        ASSERT_OK(sorted->insert(opCtx.get(), keyString1.getValueCopy(), loc1, true));
        ASSERT_OK(sorted->insert(opCtx.get(), keyString2.getValueCopy(), loc2, true));
        uow.commit();
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(2, sorted->numEntries(opCtx.get()));
        ASSERT_NOT_OK(sorted->dupKeyCheck(opCtx.get(), keyString1WithoutRecordId.getValueCopy()));
    }
}

TEST(SortedDataInterface, DupKeyCheckWithDeletedFirstEntry) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/true, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));

        WriteUnitOfWork uow(opCtx.get());
        ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, true));
        ASSERT_OK(sorted->insert(opCtx.get(), key1, loc2, true));
        uow.commit();
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        WriteUnitOfWork uow(opCtx.get());
        sorted->unindex(opCtx.get(), key1, loc1, true);
        uow.commit();
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
        ASSERT_OK(sorted->dupKeyCheck(opCtx.get(), key1));
    }
}

TEST(SortedDataInterface, DupKeyCheckWithDeletedSecondEntry) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/true, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));

        WriteUnitOfWork uow(opCtx.get());
        ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, true));
        ASSERT_OK(sorted->insert(opCtx.get(), key1, loc2, true));
        uow.commit();
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        WriteUnitOfWork uow(opCtx.get());
        sorted->unindex(opCtx.get(), key1, loc2, true);
        uow.commit();
    }
    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
        ASSERT_OK(sorted->dupKeyCheck(opCtx.get(), key1));
    }
}

}  // namespace
}  // namespace mongo
