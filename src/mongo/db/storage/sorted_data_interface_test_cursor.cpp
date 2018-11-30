
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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/sorted_data_interface_test_harness.h"

#include <algorithm>
#include <memory>

#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// Verify that a forward cursor is positioned at EOF when the index is empty.
TEST(SortedDataInterface, CursorIsEOFWhenEmpty) {
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

        ASSERT(!cursor->seek(kMinBSONKey, true));

        // Cursor at EOF should remain at EOF when advanced
        ASSERT(!cursor->next());
    }
}

// Verify that a reverse cursor is positioned at EOF when the index is empty.
TEST(SortedDataInterface, CursorIsEOFWhenEmptyReversed) {
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

        ASSERT(!cursor->seek(kMaxBSONKey, true));

        // Cursor at EOF should remain at EOF when advanced
        ASSERT(!cursor->next());
    }
}

// Call advance() on a forward cursor until it is exhausted.
// When a cursor positioned at EOF is advanced, it stays at EOF.
TEST(SortedDataInterface, ExhaustCursor) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            BSONObj key = BSON("" << i);
            RecordId loc(42, i * 2);
            ASSERT_OK(sorted->insert(opCtx.get(), key, loc, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        for (int i = 0; i < nToInsert; i++) {
            auto entry = i == 0 ? cursor->seek(kMinBSONKey, true) : cursor->next();
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << i), RecordId(42, i * 2)));
        }
        ASSERT(!cursor->next());

        // Cursor at EOF should remain at EOF when advanced
        ASSERT(!cursor->next());
    }
}

// Call advance() on a reverse cursor until it is exhausted.
// When a cursor positioned at EOF is advanced, it stays at EOF.
TEST(SortedDataInterface, ExhaustCursorReversed) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            BSONObj key = BSON("" << i);
            RecordId loc(42, i * 2);
            ASSERT_OK(sorted->insert(opCtx.get(), key, loc, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        for (int i = nToInsert - 1; i >= 0; i--) {
            auto entry = (i == nToInsert - 1) ? cursor->seek(kMaxBSONKey, true) : cursor->next();
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << i), RecordId(42, i * 2)));
        }
        ASSERT(!cursor->next());

        // Cursor at EOF should remain at EOF when advanced
        ASSERT(!cursor->next());
    }
}

void testBoundaries(bool unique, bool forward, bool inclusive) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(unique, /*partial=*/false));

    const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    ASSERT(sorted->isEmpty(opCtx.get()));

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        WriteUnitOfWork uow(opCtx.get());
        BSONObj key = BSON("" << i);
        RecordId loc(42 + i * 2);
        ASSERT_OK(sorted->insert(opCtx.get(), key, loc, true));
        uow.commit();
    }

    {
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), forward));
        int startVal = 2;
        int endVal = 6;
        if (!forward)
            std::swap(startVal, endVal);

        auto startKey = BSON("" << startVal);
        auto endKey = BSON("" << endVal);
        cursor->setEndPosition(endKey, inclusive);

        auto entry = cursor->seek(startKey, inclusive);

        // Check that the cursor returns the expected values in range.
        int step = forward ? 1 : -1;
        for (int i = startVal + (inclusive ? 0 : step); i != endVal + (inclusive ? step : 0);
             i += step) {
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << i), RecordId(42 + i * 2)));
            entry = cursor->next();
        }
        ASSERT(!entry);

        // Cursor at EOF should remain at EOF when advanced
        ASSERT(!cursor->next());
    }
}

TEST(SortedDataInterfaceBoundaryTest, UniqueForwardWithNonInclusiveBoundaries) {
    testBoundaries(/*unique*/ true, /*forward*/ true, /*inclusive*/ false);
}

TEST(SortedDataInterfaceBoundaryTest, NonUniqueForwardWithNonInclusiveBoundaries) {
    testBoundaries(/*unique*/ false, /*forward*/ true, /*inclusive*/ false);
}

TEST(SortedDataInterfaceBoundaryTest, UniqueForwardWithInclusiveBoundaries) {
    testBoundaries(/*unique*/ true, /*forward*/ true, /*inclusive*/ true);
}

TEST(SortedDataInterfaceBoundaryTest, NonUniqueForwardWithInclusiveBoundaries) {
    testBoundaries(/*unique*/ false, /*forward*/ true, /*inclusive*/ true);
}

TEST(SortedDataInterfaceBoundaryTest, UniqueBackwardWithNonInclusiveBoundaries) {
    testBoundaries(/*unique*/ true, /*forward*/ false, /*inclusive*/ false);
}

TEST(SortedDataInterfaceBoundaryTest, NonUniqueBackwardWithNonInclusiveBoundaries) {
    testBoundaries(/*unique*/ false, /*forward*/ false, /*inclusive*/ false);
}

TEST(SortedDataInterfaceBoundaryTest, UniqueBackwardWithInclusiveBoundaries) {
    testBoundaries(/*unique*/ true, /*forward*/ false, /*inclusive*/ true);
}

TEST(SortedDataInterfaceBoundaryTest, NonUniqueBackwardWithInclusiveBoundaries) {
    testBoundaries(/*unique*/ false, /*forward*/ false, /*inclusive*/ true);
}

}  // namespace
}  // namespace mongo
