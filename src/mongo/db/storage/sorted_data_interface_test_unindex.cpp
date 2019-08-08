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

// Insert a key and verify that it can be unindexed.
void unindex(bool partial) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, partial));

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
            sorted->unindex(opCtx.get(), key1, loc1, true);
            ASSERT(sorted->isEmpty(opCtx.get()));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }
}

TEST(SortedDataInterface, Unindex) {
    unindex(false);
}

TEST(SortedDataInterface, UnindexPartial) {
    unindex(true);
}

/*
 * Insert a KeyString and verify that it can be unindexed.
 */
void unindexKeyString(bool partial) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, partial));

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
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->unindex(opCtx.get(), keyString1.getValueCopy(), loc1, true);
            ASSERT(sorted->isEmpty(opCtx.get()));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }
}

TEST(SortedDataInterface, UnindexKeyString) {
    unindexKeyString(false);
}

TEST(SortedDataInterface, UnindexKeyStringPartial) {
    unindexKeyString(true);
}

// Insert a compound key and verify that it can be unindexed.
void unindexCompoundKey(bool partial) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, partial));

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

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->unindex(opCtx.get(), compoundKey1a, loc1, true);
            ASSERT(sorted->isEmpty(opCtx.get()));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }
}

TEST(SortedDataInterface, UnindexCompoundKey) {
    unindexCompoundKey(false);
}

TEST(SortedDataInterface, UnindexCompoundKeyPartial) {
    unindexCompoundKey(true);
}

// Insert multiple, distinct keys and verify that they can be unindexed.
void unindexMultipleDistinct(bool partial) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, partial));

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
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->unindex(opCtx.get(), key2, loc2, true);
            ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
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
            ASSERT_OK(sorted->insert(opCtx.get(), key3, loc3, true));
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
            sorted->unindex(opCtx.get(), key1, loc1, true);
            ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
            sorted->unindex(opCtx.get(), key3, loc3, true);
            ASSERT(sorted->isEmpty(opCtx.get()));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }
}

TEST(SortedDataInterface, UnindexMultipleDistinct) {
    unindexMultipleDistinct(false);
}

TEST(SortedDataInterface, UnindexMultipleDistinctPartial) {
    unindexMultipleDistinct(true);
}

// Insert the same key multiple times and verify that each occurrence can be unindexed.
void unindexMultipleSameKey(bool partial) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, partial));

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
            sorted->unindex(opCtx.get(), key1, loc2, true);
            ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
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
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc3, true /* allow duplicates */));
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
            sorted->unindex(opCtx.get(), key1, loc1, true);
            ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
            sorted->unindex(opCtx.get(), key1, loc3, true);
            ASSERT(sorted->isEmpty(opCtx.get()));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }
}


TEST(SortedDataInterface, UnindexMultipleSameKey) {
    unindexMultipleSameKey(false);
}

TEST(SortedDataInterface, UnindexMultipleSameKeyPartial) {
    unindexMultipleSameKey(true);
}

/*
 * Insert the same KeyString multiple times and verify that each occurrence can be unindexed.
 */
void unindexMultipleSameKeyString(bool partial) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, partial));

    KeyString::Builder keyStringLoc1(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc1);
    KeyString::Builder keyStringLoc2(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc2);
    KeyString::Builder keyStringLoc3(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc3);

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), keyStringLoc1.getValueCopy(), loc1, true));
            ASSERT_OK(sorted->insert(
                opCtx.get(), keyStringLoc2.getValueCopy(), loc2, true /* allow duplicates */));
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
            sorted->unindex(opCtx.get(), keyStringLoc2.getValueCopy(), loc2, true);
            ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
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
            ASSERT_OK(sorted->insert(
                opCtx.get(), keyStringLoc3.getValueCopy(), loc3, true /* allow duplicates */));
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
            sorted->unindex(opCtx.get(), keyStringLoc1.getValueCopy(), loc1, true);
            ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
            sorted->unindex(opCtx.get(), keyStringLoc3.getValueCopy(), loc3, true);
            ASSERT(sorted->isEmpty(opCtx.get()));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }
}


TEST(SortedDataInterface, UnindexMultipleSameKeyString) {
    unindexMultipleSameKeyString(false);
}

TEST(SortedDataInterface, UnindexMultipleSameKeyStringPartial) {
    unindexMultipleSameKeyString(true);
}

// Call unindex() on a nonexistent key and verify the result is false.
void unindexEmpty(bool partial) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, partial));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->unindex(opCtx.get(), key1, loc1, true);
            ASSERT(sorted->isEmpty(opCtx.get()));
            uow.commit();
        }
    }
}

TEST(SortedDataInterface, UnindexEmpty) {
    unindexEmpty(false);
}

TEST(SortedDataInterface, UnindexEmptyPartial) {
    unindexEmpty(true);
}

// Test partial indexing and unindexing.
TEST(SortedDataInterface, PartialIndex) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/true, /*partial=*/true));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            {
                WriteUnitOfWork uow(opCtx.get());
                ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, true));
                // Assume key1 with loc2 was never indexed due to the partial index.
                ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
                uow.commit();
            }

            {
                WriteUnitOfWork uow(opCtx.get());
                // Shouldn't unindex anything as key1 with loc2 wasn't indexed in the first place.
                sorted->unindex(opCtx.get(), key1, loc2, true);
                ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
                uow.commit();
            }

            {
                WriteUnitOfWork uow(opCtx.get());
                sorted->unindex(opCtx.get(), key1, loc1, true);
                ASSERT(sorted->isEmpty(opCtx.get()));
                uow.commit();
            }
        }
    }
}

}  // namespace
}  // namespace mongo
