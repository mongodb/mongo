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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_assert.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

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
        ASSERT(!cursor->seek(
            makeKeyStringForSeek(sorted.get(), BSONObj(), true, true).finishAndGetBuffer()));
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

        ASSERT(!cursor->seek(
            makeKeyStringForSeek(sorted.get(), kMaxBSONKey, false, true).finishAndGetBuffer()));

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
            auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
            StorageWriteTransaction txn(ru);
            BSONObj key = BSON("" << i);
            RecordId loc(42, i * 2);
            ASSERT_SDI_INSERT_OK(
                sorted->insert(opCtx.get(), makeKeyString(sorted.get(), key, loc), true));
            txn.commit();
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
            auto entry = cursor->next();
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << i), RecordId(42, i * 2)));
        }
        ASSERT(!cursor->next());

        // Cursor at EOF should remain at EOF when advanced
        ASSERT(!cursor->next());
    }
}

TEST(SortedDataInterface, ExhaustKeyStringCursor) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    std::vector<key_string::Value> keyStrings;
    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
            StorageWriteTransaction txn(ru);
            BSONObj key = BSON("" << i);
            RecordId loc(42, i * 2);
            key_string::Value ks = makeKeyString(sorted.get(), key, loc);
            keyStrings.push_back(ks);
            ASSERT_SDI_INSERT_OK(sorted->insert(opCtx.get(), ks, true));
            txn.commit();
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
            auto entry = cursor->nextKeyString();
            ASSERT(entry);
            ASSERT_EQ(entry->keyString, keyStrings.at(i));
        }
        ASSERT(!cursor->nextKeyString());

        // Cursor at EOF should remain at EOF when advanced
        ASSERT(!cursor->nextKeyString());
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        for (int i = 0; i < nToInsert; i++) {
            auto entry = cursor->nextKeyValueView();
            ASSERT(!entry.isEmpty());
            ASSERT_EQ(entry.getValueCopy(), keyStrings.at(i));
        }
        ASSERT(cursor->nextKeyValueView().isEmpty());

        // Cursor at EOF should remain at EOF when advanced
        ASSERT(cursor->nextKeyValueView().isEmpty());
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
            auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
            StorageWriteTransaction txn(ru);
            BSONObj key = BSON("" << i);
            RecordId loc(42, i * 2);
            ASSERT_SDI_INSERT_OK(
                sorted->insert(opCtx.get(), makeKeyString(sorted.get(), key, loc), true));
            txn.commit();
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
            auto entry = cursor->next();
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << i), RecordId(42, i * 2)));
        }
        ASSERT(!cursor->next());

        // Cursor at EOF should remain at EOF when advanced
        ASSERT(!cursor->next());
    }
}

TEST(SortedDataInterface, ExhaustKeyStringCursorReversed) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    std::vector<key_string::Value> keyStrings;
    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
            StorageWriteTransaction txn(ru);
            BSONObj key = BSON("" << i);
            RecordId loc(42, i * 2);
            key_string::Value ks = makeKeyString(sorted.get(), key, loc);
            keyStrings.push_back(ks);
            ASSERT_SDI_INSERT_OK(sorted->insert(opCtx.get(), ks, true));
            txn.commit();
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
            auto entry = cursor->nextKeyString();
            ASSERT(entry);
            ASSERT_EQ(entry->keyString, keyStrings.at(i));
        }
        ASSERT(!cursor->nextKeyString());

        // Cursor at EOF should remain at EOF when advanced
        ASSERT(!cursor->nextKeyString());
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        for (int i = nToInsert - 1; i >= 0; i--) {
            auto entry = cursor->nextKeyValueView();
            ASSERT(!entry.isEmpty());
            ASSERT_EQ(entry.getValueCopy(), keyStrings.at(i));
        }
        ASSERT(cursor->nextKeyValueView().isEmpty());

        // Cursor at EOF should remain at EOF when advanced
        ASSERT(cursor->nextKeyValueView().isEmpty());
    }
}


TEST(SortedDataInterface, CursorIterate1) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    int N = 5;
    for (int i = 0; i < N; i++) {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
            StorageWriteTransaction txn(ru);
            ASSERT_SDI_INSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), BSON("" << i), RecordId(5, i * 2)), true));
            txn.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        int n = 0;
        for (auto entry = cursor->next(); entry; entry = cursor->next()) {
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << n), RecordId(5, n * 2)));
            n++;
        }
        ASSERT_EQUALS(N, n);
    }
}

TEST(SortedDataInterface, CursorIterate1WithSaveRestore) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    int N = 5;
    for (int i = 0; i < N; i++) {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
            StorageWriteTransaction txn(ru);
            ASSERT_SDI_INSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), BSON("" << i), RecordId(5, i * 2)), true));
            txn.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        int n = 0;
        for (auto entry = cursor->next(); entry; entry = cursor->next()) {
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << n), RecordId(5, n * 2)));
            n++;
            cursor->save();
            cursor->restore();
        }
        ASSERT_EQUALS(N, n);
    }
}


TEST(SortedDataInterface, CursorIterateAllDupKeysWithSaveRestore) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    int N = 5;
    for (int i = 0; i < N; i++) {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
            StorageWriteTransaction txn(ru);
            ASSERT_SDI_INSERT_OK(sorted->insert(
                opCtx.get(), makeKeyString(sorted.get(), BSON("" << 5), RecordId(5, i * 2)), true));
            txn.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        int n = 0;
        for (auto entry = cursor->next(); entry; entry = cursor->next()) {
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << 5), RecordId(5, n * 2)));
            n++;
            cursor->save();
            cursor->restore();
        }
        ASSERT_EQUALS(N, n);
    }
}

enum class IndexType { kId, kUnique, kNonUnique };
void testBoundaries(IndexType type, bool forward, bool inclusive) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    std::unique_ptr<SortedDataInterface> sorted;
    if (IndexType::kId == type) {
        sorted = harnessHelper->newIdIndexSortedDataInterface();
    } else {
        sorted =
            harnessHelper->newSortedDataInterface(IndexType::kUnique == type, /*partial=*/false);
    }

    const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    ASSERT(sorted->isEmpty(opCtx.get()));

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        StorageWriteTransaction txn(ru);
        BSONObj key = BSON("" << i);
        RecordId loc(42 + i * 2);
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx.get(), makeKeyString(sorted.get(), key, loc), false /* dupsAllowed*/));
        txn.commit();
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

        auto entry = cursor->seek(
            makeKeyStringForSeek(sorted.get(), startKey, forward, inclusive).finishAndGetBuffer());

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
    testBoundaries(IndexType::kUnique, /*forward*/ true, /*inclusive*/ false);
}

TEST(SortedDataInterfaceBoundaryTest, NonUniqueForwardWithNonInclusiveBoundaries) {
    testBoundaries(IndexType::kNonUnique, /*forward*/ true, /*inclusive*/ false);
}

TEST(SortedDataInterfaceBoundaryTest, IdForwardWithNonInclusiveBoundaries) {
    testBoundaries(IndexType::kId, /*forward*/ true, /*inclusive*/ false);
}

TEST(SortedDataInterfaceBoundaryTest, UniqueForwardWithInclusiveBoundaries) {
    testBoundaries(IndexType::kUnique, /*forward*/ true, /*inclusive*/ true);
}

TEST(SortedDataInterfaceBoundaryTest, NonUniqueForwardWithInclusiveBoundaries) {
    testBoundaries(IndexType::kNonUnique, /*forward*/ true, /*inclusive*/ true);
}

TEST(SortedDataInterfaceBoundaryTest, IdForwardWithInclusiveBoundaries) {
    testBoundaries(IndexType::kId, /*forward*/ true, /*inclusive*/ true);
}

TEST(SortedDataInterfaceBoundaryTest, UniqueBackwardWithNonInclusiveBoundaries) {
    testBoundaries(IndexType::kUnique, /*forward*/ false, /*inclusive*/ false);
}

TEST(SortedDataInterfaceBoundaryTest, NonUniqueBackwardWithNonInclusiveBoundaries) {
    testBoundaries(IndexType::kNonUnique, /*forward*/ false, /*inclusive*/ false);
}

TEST(SortedDataInterfaceBoundaryTest, IdBackwardWithNonInclusiveBoundaries) {
    testBoundaries(IndexType::kId, /*forward*/ false, /*inclusive*/ false);
}

TEST(SortedDataInterfaceBoundaryTest, UniqueBackwardWithInclusiveBoundaries) {
    testBoundaries(IndexType::kUnique, /*forward*/ false, /*inclusive*/ true);
}

TEST(SortedDataInterfaceBoundaryTest, NonUniqueBackwardWithInclusiveBoundaries) {
    testBoundaries(IndexType::kNonUnique, /*forward*/ false, /*inclusive*/ true);
}

TEST(SortedDataInterfaceBoundaryTest, IdBackwardWithInclusiveBoundaries) {
    testBoundaries(IndexType::kId, /*forward*/ false, /*inclusive*/ true);
}

}  // namespace
}  // namespace mongo
