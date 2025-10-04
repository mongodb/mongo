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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_assert.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

// Verify that a forward cursor is positioned at EOF when the index is empty.
TEST_F(SortedDataInterfaceTest, CursorIsEOFWhenEmpty) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));
    ASSERT(!cursor->seek(
        recoveryUnit(),
        makeKeyStringForSeek(sorted.get(), BSONObj(), true, true).finishAndGetBuffer()));
    // Cursor at EOF should remain at EOF when advanced
    ASSERT(!cursor->next(recoveryUnit()));
}

// Verify that a reverse cursor is positioned at EOF when the index is empty.
TEST_F(SortedDataInterfaceTest, CursorIsEOFWhenEmptyReversed) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));
    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    const auto cursor(sorted->newCursor(opCtx(), recoveryUnit(), false));

    ASSERT(!cursor->seek(
        recoveryUnit(),
        makeKeyStringForSeek(sorted.get(), kMaxBSONKey, false, true).finishAndGetBuffer()));

    // Cursor at EOF should remain at EOF when advanced
    ASSERT(!cursor->next(recoveryUnit()));
}

// Call advance() on a forward cursor until it is exhausted.
// When a cursor positioned at EOF is advanced, it stays at EOF.
TEST_F(SortedDataInterfaceTest, ExhaustCursor) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));
    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        StorageWriteTransaction txn(recoveryUnit());
        BSONObj key = BSON("" << i);
        RecordId loc(42, i * 2);
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key, loc), true));
        txn.commit();
    }

    ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx(), recoveryUnit()));

    const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));
    for (int i = 0; i < nToInsert; i++) {
        auto entry = cursor->next(recoveryUnit());
        ASSERT_EQ(entry, IndexKeyEntry(BSON("" << i), RecordId(42, i * 2)));
    }
    ASSERT(!cursor->next(recoveryUnit()));

    // Cursor at EOF should remain at EOF when advanced
    ASSERT(!cursor->next(recoveryUnit()));
}

TEST_F(SortedDataInterfaceTest, ExhaustKeyStringCursor) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    std::vector<key_string::Value> keyStrings;
    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        StorageWriteTransaction txn(recoveryUnit());
        BSONObj key = BSON("" << i);
        RecordId loc(42, i * 2);
        key_string::Value ks = makeKeyString(sorted.get(), key, loc);
        keyStrings.push_back(ks);
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(), recoveryUnit(), ks, true));
        txn.commit();
    }

    ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));
        for (int i = 0; i < nToInsert; i++) {
            auto entry = cursor->nextKeyString(recoveryUnit());
            ASSERT(entry);
            ASSERT_EQ(entry->keyString, keyStrings.at(i));
        }
        ASSERT(!cursor->nextKeyString(recoveryUnit()));

        // Cursor at EOF should remain at EOF when advanced
        ASSERT(!cursor->nextKeyString(recoveryUnit()));
    }

    {
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));
        for (int i = 0; i < nToInsert; i++) {
            auto entry = cursor->nextKeyValueView(recoveryUnit());
            ASSERT(!entry.isEmpty());
            ASSERT_EQ(entry.getValueCopy(), keyStrings.at(i));
        }
        ASSERT(cursor->nextKeyValueView(recoveryUnit()).isEmpty());

        // Cursor at EOF should remain at EOF when advanced
        ASSERT(cursor->nextKeyValueView(recoveryUnit()).isEmpty());
    }
}

// Call advance() on a reverse cursor until it is exhausted.
// When a cursor positioned at EOF is advanced, it stays at EOF.
TEST_F(SortedDataInterfaceTest, ExhaustCursorReversed) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        StorageWriteTransaction txn(recoveryUnit());
        BSONObj key = BSON("" << i);
        RecordId loc(42, i * 2);
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), recoveryUnit(), makeKeyString(sorted.get(), key, loc), true));
        txn.commit();
    }

    ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit(), false));
        for (int i = nToInsert - 1; i >= 0; i--) {
            auto entry = cursor->next(recoveryUnit());
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << i), RecordId(42, i * 2)));
        }
        ASSERT(!cursor->next(recoveryUnit()));

        // Cursor at EOF should remain at EOF when advanced
        ASSERT(!cursor->next(recoveryUnit()));
    }
}

TEST_F(SortedDataInterfaceTest, ExhaustKeyStringCursorReversed) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    std::vector<key_string::Value> keyStrings;
    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        StorageWriteTransaction txn(recoveryUnit());
        BSONObj key = BSON("" << i);
        RecordId loc(42, i * 2);
        key_string::Value ks = makeKeyString(sorted.get(), key, loc);
        keyStrings.push_back(ks);
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(), recoveryUnit(), ks, true));
        txn.commit();
    }

    ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit(), false));
        for (int i = nToInsert - 1; i >= 0; i--) {
            auto entry = cursor->nextKeyString(recoveryUnit());
            ASSERT(entry);
            ASSERT_EQ(entry->keyString, keyStrings.at(i));
        }
        ASSERT(!cursor->nextKeyString(recoveryUnit()));

        // Cursor at EOF should remain at EOF when advanced
        ASSERT(!cursor->nextKeyString(recoveryUnit()));
    }

    {
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit(), false));
        for (int i = nToInsert - 1; i >= 0; i--) {
            auto entry = cursor->nextKeyValueView(recoveryUnit());
            ASSERT(!entry.isEmpty());
            ASSERT_EQ(entry.getValueCopy(), keyStrings.at(i));
        }
        ASSERT(cursor->nextKeyValueView(recoveryUnit()).isEmpty());

        // Cursor at EOF should remain at EOF when advanced
        ASSERT(cursor->nextKeyValueView(recoveryUnit()).isEmpty());
    }
}


TEST_F(SortedDataInterfaceTest, CursorIterate1) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    int N = 5;
    for (int i = 0; i < N; i++) {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), BSON("" << i), RecordId(5, i * 2)),
                           true));
        txn.commit();
    }

    {
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));
        int n = 0;
        for (auto entry = cursor->next(recoveryUnit()); entry;
             entry = cursor->next(recoveryUnit())) {
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << n), RecordId(5, n * 2)));
            n++;
        }
        ASSERT_EQUALS(N, n);
    }
}

TEST_F(SortedDataInterfaceTest, CursorIterate1WithSaveRestore) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    int N = 5;
    for (int i = 0; i < N; i++) {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), BSON("" << i), RecordId(5, i * 2)),
                           true));
        txn.commit();
    }

    {
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));
        int n = 0;
        for (auto entry = cursor->next(recoveryUnit()); entry;
             entry = cursor->next(recoveryUnit())) {
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << n), RecordId(5, n * 2)));
            n++;
            cursor->save();
            cursor->restore(recoveryUnit());
        }
        ASSERT_EQUALS(N, n);
    }
}


TEST_F(SortedDataInterfaceTest, CursorIterateAllDupKeysWithSaveRestore) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    int N = 5;
    for (int i = 0; i < N; i++) {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(),
                           recoveryUnit(),
                           makeKeyString(sorted.get(), BSON("" << 5), RecordId(5, i * 2)),
                           true));
        txn.commit();
    }

    {
        const auto cursor(sorted->newCursor(opCtx(), recoveryUnit()));
        int n = 0;
        for (auto entry = cursor->next(recoveryUnit()); entry;
             entry = cursor->next(recoveryUnit())) {
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << 5), RecordId(5, n * 2)));
            n++;
            cursor->save();
            cursor->restore(recoveryUnit());
        }
        ASSERT_EQUALS(N, n);
    }
}

class SortedDataInterfaceTestBoundaryTest : public SortedDataInterfaceTest {
public:
    enum class IndexType { kId, kUnique, kNonUnique };
    void testBoundaries(IndexType type, bool forward, bool inclusive) {
        std::unique_ptr<SortedDataInterface> sorted;
        if (IndexType::kId == type) {
            sorted = harnessHelper()->newIdIndexSortedDataInterface(opCtx());
        } else {
            sorted = harnessHelper()->newSortedDataInterface(opCtx(),
                                                             IndexType::kUnique == type,
                                                             /*partial=*/false);
        }

        ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

        int nToInsert = 10;
        for (int i = 0; i < nToInsert; i++) {
            StorageWriteTransaction txn(recoveryUnit());
            BSONObj key = BSON("" << i);
            RecordId loc(42 + i * 2);
            ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                                recoveryUnit(),
                                                makeKeyString(sorted.get(), key, loc),
                                                false /* dupsAllowed*/));
            txn.commit();
        }

        {
            const auto cursor(sorted->newCursor(opCtx(), recoveryUnit(), forward));
            int startVal = 2;
            int endVal = 6;
            if (!forward)
                std::swap(startVal, endVal);

            auto startKey = BSON("" << startVal);
            auto endKey = BSON("" << endVal);
            cursor->setEndPosition(endKey, inclusive);

            auto entry =
                cursor->seek(recoveryUnit(),
                             makeKeyStringForSeek(sorted.get(), startKey, forward, inclusive)
                                 .finishAndGetBuffer());

            // Check that the cursor returns the expected values in range.
            int step = forward ? 1 : -1;
            for (int i = startVal + (inclusive ? 0 : step); i != endVal + (inclusive ? step : 0);
                 i += step) {
                ASSERT_EQ(entry, IndexKeyEntry(BSON("" << i), RecordId(42 + i * 2)));
                entry = cursor->next(recoveryUnit());
            }
            ASSERT(!entry);

            // Cursor at EOF should remain at EOF when advanced
            ASSERT(!cursor->next(recoveryUnit()));
        }
    }
};

TEST_F(SortedDataInterfaceTestBoundaryTest, UniqueForwardWithNonInclusiveBoundaries) {
    testBoundaries(IndexType::kUnique, /*forward*/ true, /*inclusive*/ false);
}

TEST_F(SortedDataInterfaceTestBoundaryTest, NonUniqueForwardWithNonInclusiveBoundaries) {
    testBoundaries(IndexType::kNonUnique, /*forward*/ true, /*inclusive*/ false);
}

TEST_F(SortedDataInterfaceTestBoundaryTest, IdForwardWithNonInclusiveBoundaries) {
    testBoundaries(IndexType::kId, /*forward*/ true, /*inclusive*/ false);
}

TEST_F(SortedDataInterfaceTestBoundaryTest, UniqueForwardWithInclusiveBoundaries) {
    testBoundaries(IndexType::kUnique, /*forward*/ true, /*inclusive*/ true);
}

TEST_F(SortedDataInterfaceTestBoundaryTest, NonUniqueForwardWithInclusiveBoundaries) {
    testBoundaries(IndexType::kNonUnique, /*forward*/ true, /*inclusive*/ true);
}

TEST_F(SortedDataInterfaceTestBoundaryTest, IdForwardWithInclusiveBoundaries) {
    testBoundaries(IndexType::kId, /*forward*/ true, /*inclusive*/ true);
}

TEST_F(SortedDataInterfaceTestBoundaryTest, UniqueBackwardWithNonInclusiveBoundaries) {
    testBoundaries(IndexType::kUnique, /*forward*/ false, /*inclusive*/ false);
}

TEST_F(SortedDataInterfaceTestBoundaryTest, NonUniqueBackwardWithNonInclusiveBoundaries) {
    testBoundaries(IndexType::kNonUnique, /*forward*/ false, /*inclusive*/ false);
}

TEST_F(SortedDataInterfaceTestBoundaryTest, IdBackwardWithNonInclusiveBoundaries) {
    testBoundaries(IndexType::kId, /*forward*/ false, /*inclusive*/ false);
}

TEST_F(SortedDataInterfaceTestBoundaryTest, UniqueBackwardWithInclusiveBoundaries) {
    testBoundaries(IndexType::kUnique, /*forward*/ false, /*inclusive*/ true);
}

TEST_F(SortedDataInterfaceTestBoundaryTest, NonUniqueBackwardWithInclusiveBoundaries) {
    testBoundaries(IndexType::kNonUnique, /*forward*/ false, /*inclusive*/ true);
}

TEST_F(SortedDataInterfaceTestBoundaryTest, IdBackwardWithInclusiveBoundaries) {
    testBoundaries(IndexType::kId, /*forward*/ false, /*inclusive*/ true);
}

}  // namespace
}  // namespace mongo
