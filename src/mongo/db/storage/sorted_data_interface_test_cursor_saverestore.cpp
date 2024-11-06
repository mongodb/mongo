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
#include <limits>
#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_assert.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

// Insert multiple keys and try to iterate through all of them
// using a forward cursor while calling save() and
// restore() in succession.
TEST_F(SortedDataInterfaceTest, SaveAndRestoreWhileIterateCursor) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx()));

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        StorageWriteTransaction txn(recoveryUnit());
        BSONObj key = BSON("" << i);
        RecordId loc(42, i * 2);
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(), makeKeyString(sorted.get(), key, loc), true));
        txn.commit();
    }

    ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx()));

    {
        const auto cursor(sorted->newCursor(opCtx()));
        int i = 0;
        for (auto entry = cursor->next(); entry; i++, entry = cursor->next()) {
            ASSERT_LT(i, nToInsert);
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << i), RecordId(42, i * 2)));

            cursor->save();
            cursor->save();  // It is legal to save twice in a row.
            cursor->restore();
        }
        ASSERT(!cursor->next());
        ASSERT_EQ(i, nToInsert);

        cursor->save();
        cursor->save();  // It is legal to save twice in a row.
        cursor->restore();
        ASSERT(!cursor->next());
    }
}

// Insert multiple keys and try to iterate through all of them
// using a forward cursor with set end position, while calling save() and
// restore() in succession.
TEST_F(SortedDataInterfaceTest, SaveAndRestoreWhileIterateCursorWithEndPosition) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx()));

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        StorageWriteTransaction txn(recoveryUnit());
        BSONObj key = BSON("" << i);
        RecordId loc(42, i * 2);
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(), makeKeyString(sorted.get(), key, loc), true));
        txn.commit();
    }

    ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx()));

    {
        const auto cursor(sorted->newCursor(opCtx()));
        cursor->setEndPosition(BSON("" << std::numeric_limits<double>::infinity()), true);

        int i = 0;
        for (auto entry = cursor->next(); entry; i++, entry = cursor->next()) {
            ASSERT_LT(i, nToInsert);
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << i), RecordId(42, i * 2)));

            cursor->save();
            cursor->restore();
        }
        ASSERT(!cursor->next());
        ASSERT_EQ(i, nToInsert);
    }
}

// Insert multiple keys and try to iterate through all of them
// using a reverse cursor while calling save() and
// restore() in succession.
TEST_F(SortedDataInterfaceTest, SaveAndRestoreWhileIterateCursorReversed) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx()));

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        StorageWriteTransaction txn(recoveryUnit());
        BSONObj key = BSON("" << i);
        RecordId loc(42, i * 2);
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(), makeKeyString(sorted.get(), key, loc), true));
        txn.commit();
    }

    ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx()));

    {
        const auto cursor(sorted->newCursor(opCtx(), false));
        int i = nToInsert - 1;
        for (auto entry = cursor->next(); entry; i--, entry = cursor->next()) {
            ASSERT_GTE(i, 0);
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << i), RecordId(42, i * 2)));

            cursor->save();
            cursor->restore();
        }
        ASSERT(!cursor->next());
        ASSERT_EQ(i, -1);
    }
}

// Insert multiple keys on the _id index and try to iterate through all of them using a forward
// cursor while calling save() and restore() in succession.
TEST_F(SortedDataInterfaceTest, SaveAndRestoreWhileIterateCursorOnIdIndex) {
    const auto sorted(harnessHelper()->newIdIndexSortedDataInterface(opCtx()));

    ASSERT(sorted->isEmpty(opCtx()));

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        StorageWriteTransaction txn(recoveryUnit());
        BSONObj key = BSON("" << i);
        RecordId loc(42, i * 2);
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(), makeKeyString(sorted.get(), key, loc), false));
        txn.commit();
    }

    ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx()));

    {
        const auto cursor(sorted->newCursor(opCtx(), false));
        int i = nToInsert - 1;
        for (auto entry = cursor->next(); entry; i--, entry = cursor->next()) {
            ASSERT_GTE(i, 0);
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << i), RecordId(42, i * 2)));

            cursor->save();
            cursor->restore();
        }
        ASSERT(!cursor->next());
        ASSERT_EQ(i, -1);
    }
}

// Insert multiple keys on the _id index and try to iterate through all of them using a reverse
// cursor while calling save() and restore() in succession.
TEST_F(SortedDataInterfaceTest, SaveAndRestoreWhileIterateCursorReversedOnIdIndex) {
    const auto sorted(harnessHelper()->newIdIndexSortedDataInterface(opCtx()));

    ASSERT(sorted->isEmpty(opCtx()));

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        StorageWriteTransaction txn(recoveryUnit());
        BSONObj key = BSON("" << i);
        RecordId loc(42, i * 2);
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(), makeKeyString(sorted.get(), key, loc), false));
        txn.commit();
    }

    ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx()));

    {
        const auto cursor(sorted->newCursor(opCtx(), false));
        int i = nToInsert - 1;
        for (auto entry = cursor->next(); entry; i--, entry = cursor->next()) {
            ASSERT_GTE(i, 0);
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << i), RecordId(42, i * 2)));

            cursor->save();
            cursor->restore();
        }
        ASSERT(!cursor->next());
        ASSERT_EQ(i, -1);
    }
}

// Insert the same key multiple times and try to iterate through each
// occurrence using a forward cursor while calling save() and
// restore() in succession. Verify that the RecordId is saved
// as part of the current position of the cursor.
TEST_F(SortedDataInterfaceTest, SaveAndRestoreWhileIterateCursorWithDupKeys) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx()));

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        StorageWriteTransaction txn(recoveryUnit());
        RecordId loc(42, i * 2);
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), makeKeyString(sorted.get(), key1, loc), true /* allow duplicates */));
        txn.commit();
    }

    ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx()));

    {
        const auto cursor(sorted->newCursor(opCtx()));
        int i = 0;
        for (auto entry = cursor->next(); entry; i++, entry = cursor->next()) {
            ASSERT_LT(i, nToInsert);
            ASSERT_EQ(entry, IndexKeyEntry(key1, RecordId(42, i * 2)));

            cursor->save();
            cursor->restore();
        }
        ASSERT(!cursor->next());
        ASSERT_EQ(i, nToInsert);
    }
}

// Insert the same key multiple times and try to iterate through each
// occurrence using a reverse cursor while calling save() and
// restore() in succession. Verify that the RecordId is saved
// as part of the current position of the cursor.
TEST_F(SortedDataInterfaceTest, SaveAndRestoreWhileIterateCursorWithDupKeysReversed) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx()));

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        StorageWriteTransaction txn(recoveryUnit());
        RecordId loc(42, i * 2);
        ASSERT_SDI_INSERT_OK(sorted->insert(
            opCtx(), makeKeyString(sorted.get(), key1, loc), true /* allow duplicates */));
        txn.commit();
    }

    ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx()));

    {
        const auto cursor(sorted->newCursor(opCtx(), false));
        int i = nToInsert - 1;
        for (auto entry = cursor->next(); entry; i--, entry = cursor->next()) {
            ASSERT_GTE(i, 0);
            ASSERT_EQ(entry, IndexKeyEntry(key1, RecordId(42, i * 2)));

            cursor->save();
            cursor->restore();
        }
        ASSERT(!cursor->next());
        ASSERT_EQ(i, -1);
    }
}

// Call save() on a forward cursor without ever calling restore().
// May be useful to run this test under valgrind to verify there are no leaks.
TEST_F(SortedDataInterfaceTest, saveWithoutRestore) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/true, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), makeKeyString(sorted.get(), key1, loc1), false));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx()));

    {
        const auto cursor(sorted->newCursor(opCtx()));
        cursor->save();
    }
}

// Call save() on a reverse cursor without ever calling restore().
// May be useful to run this test under valgrind to verify there are no leaks.
TEST_F(SortedDataInterfaceTest, saveWithoutRestoreReversed) {
    const auto sorted(
        harnessHelper()->newSortedDataInterface(opCtx(), /*unique=*/false, /*partial=*/false));

    ASSERT(sorted->isEmpty(opCtx()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(
            sorted->insert(opCtx(), makeKeyString(sorted.get(), key1, loc1), true));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx()));

    {
        const auto cursor(sorted->newCursor(opCtx(), false));
        cursor->save();
    }
}

// Ensure that restore lands as close as possible to original position, even if data inserted
// while saved.
enum class IndexType { kId, kUnique, kNonUnique };
void testSaveAndRestoreSeesNewInserts(OperationContext* opCtx,
                                      SortedDataInterfaceHarnessHelper* harnessHelper,
                                      bool forward,
                                      IndexType type) {
    std::unique_ptr<SortedDataInterface> sorted;
    if (IndexType::kId == type) {
        sorted = harnessHelper->newIdIndexSortedDataInterface(opCtx);
    } else {
        sorted = harnessHelper->newSortedDataInterface(
            opCtx, IndexType::kUnique == type, /*partial=*/false);
    }

    insertToIndex(opCtx, sorted.get(), {{key1, loc1}, {key3, loc3}}, /*dupsAllowed*/ false);

    auto cursor = sorted->newCursor(opCtx, forward);
    if (forward) {
        const auto seekPoint = key1;
        ASSERT_EQ(
            cursor->seek(
                makeKeyStringForSeek(sorted.get(), seekPoint, forward, true).finishAndGetBuffer()),
            IndexKeyEntry(seekPoint, loc1));
    } else {
        const auto seekPoint = key3;
        ASSERT_EQ(
            cursor->seek(
                makeKeyStringForSeek(sorted.get(), seekPoint, forward, true).finishAndGetBuffer()),
            IndexKeyEntry(seekPoint, loc3));
    }

    cursor->save();
    insertToIndex(opCtx, sorted.get(), {{key2, loc2}}, /*dupsAllowed*/ false);
    cursor->restore();

    ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc2));
}
TEST_F(SortedDataInterfaceTest, SaveAndRestoreSeesNewInserts_Forward_Unique) {
    testSaveAndRestoreSeesNewInserts(opCtx(), harnessHelper(), true, IndexType::kUnique);
}
TEST_F(SortedDataInterfaceTest, SaveAndRestoreSeesNewInserts_Forward_Standard) {
    testSaveAndRestoreSeesNewInserts(opCtx(), harnessHelper(), true, IndexType::kNonUnique);
}
TEST_F(SortedDataInterfaceTest, SaveAndRestoreSeesNewInserts_Forward_Id) {
    testSaveAndRestoreSeesNewInserts(opCtx(), harnessHelper(), true, IndexType::kId);
}
TEST_F(SortedDataInterfaceTest, SaveAndRestoreSeesNewInserts_Reverse_Unique) {
    testSaveAndRestoreSeesNewInserts(opCtx(), harnessHelper(), false, IndexType::kUnique);
}
TEST_F(SortedDataInterfaceTest, SaveAndRestoreSeesNewInserts_Reverse_Standard) {
    testSaveAndRestoreSeesNewInserts(opCtx(), harnessHelper(), false, IndexType::kNonUnique);
}
TEST_F(SortedDataInterfaceTest, SaveAndRestoreSeesNewInserts_Reverse_Id) {
    testSaveAndRestoreSeesNewInserts(opCtx(), harnessHelper(), false, IndexType::kId);
}

// Ensure that repeated restores lands as close as possible to original position, even if data
// inserted while saved and the current position removed.
void testSaveAndRestoreSeesNewInsertsAfterRemove(OperationContext* opCtx,
                                                 SortedDataInterfaceHarnessHelper* harnessHelper,
                                                 bool forward,
                                                 IndexType type) {
    std::unique_ptr<SortedDataInterface> sorted;
    if (IndexType::kId == type) {
        sorted = harnessHelper->newIdIndexSortedDataInterface(opCtx);
    } else {
        sorted = harnessHelper->newSortedDataInterface(
            opCtx, IndexType::kUnique == type, /*partial=*/false);
    }

    insertToIndex(opCtx, sorted.get(), {{key1, loc1}, {key3, loc1}}, /*dupsAllowed*/ false);

    auto cursor = sorted->newCursor(opCtx, forward);
    const auto seekPoint = forward ? key1 : key3;

    ASSERT_EQ(
        cursor->seek(
            makeKeyStringForSeek(sorted.get(), seekPoint, forward, true).finishAndGetBuffer()),
        IndexKeyEntry(seekPoint, loc1));

    cursor->save();
    removeFromIndex(opCtx, sorted.get(), {{key1, loc1}});
    cursor->restore();
    // The restore may have seeked since it can't return to the saved position.

    cursor->save();  // Should still save originally saved key as "current position".
    insertToIndex(opCtx, sorted.get(), {{key2, loc1}}, /*dupsAllowed*/ false);
    cursor->restore();

    ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc1));
}
TEST_F(SortedDataInterfaceTest, SaveAndRestoreSeesNewInsertsAfterRemove_Forward_Unique) {
    testSaveAndRestoreSeesNewInsertsAfterRemove(opCtx(), harnessHelper(), true, IndexType::kUnique);
}
TEST_F(SortedDataInterfaceTest, SaveAndRestoreSeesNewInsertsAfterRemove_Forward_Standard) {
    testSaveAndRestoreSeesNewInsertsAfterRemove(
        opCtx(), harnessHelper(), true, IndexType::kNonUnique);
}
TEST_F(SortedDataInterfaceTest, SaveAndRestoreSeesNewInsertsAfterRemove_Forward_Id) {
    testSaveAndRestoreSeesNewInsertsAfterRemove(opCtx(), harnessHelper(), true, IndexType::kId);
}
TEST_F(SortedDataInterfaceTest, SaveAndRestoreSeesNewInsertsAfterRemove_Reverse_Unique) {
    testSaveAndRestoreSeesNewInsertsAfterRemove(
        opCtx(), harnessHelper(), false, IndexType::kUnique);
}
TEST_F(SortedDataInterfaceTest, SaveAndRestoreSeesNewInsertsAfterRemove_Reverse_Standard) {
    testSaveAndRestoreSeesNewInsertsAfterRemove(
        opCtx(), harnessHelper(), false, IndexType::kNonUnique);
}
TEST_F(SortedDataInterfaceTest, SaveAndRestoreSeesNewInsertsAfterRemove_Reverse_Id) {
    testSaveAndRestoreSeesNewInsertsAfterRemove(opCtx(), harnessHelper(), false, IndexType::kId);
}

// Ensure that repeated restores lands as close as possible to original position, even if data
// inserted while saved and the current position removed in a way that temporarily makes the
// cursor EOF.
void testSaveAndRestoreSeesNewInsertsAfterEOF(OperationContext* opCtx,
                                              SortedDataInterfaceHarnessHelper* harnessHelper,
                                              bool forward,
                                              IndexType type) {
    std::unique_ptr<SortedDataInterface> sorted;
    if (IndexType::kId == type) {
        sorted = harnessHelper->newIdIndexSortedDataInterface(opCtx);
    } else {
        sorted = harnessHelper->newSortedDataInterface(
            opCtx, IndexType::kUnique == type, /*partial=*/false);
    }

    insertToIndex(opCtx, sorted.get(), {{key1, loc1}}, /*dupsAllowed*/ false);

    auto cursor = sorted->newCursor(opCtx, forward);

    ASSERT_EQ(
        cursor->seek(makeKeyStringForSeek(sorted.get(), key1, forward, true).finishAndGetBuffer()),
        IndexKeyEntry(key1, loc1));
    // next() would return EOF now.

    cursor->save();
    removeFromIndex(opCtx, sorted.get(), {{key1, loc1}});
    cursor->restore();
    // The restore may have seeked to EOF.

    auto insertPoint = forward ? key2 : key0;
    cursor->save();  // Should still save key1 as "current position".
    insertToIndex(opCtx, sorted.get(), {{insertPoint, loc1}}, /*dupsAllowed*/ false);
    cursor->restore();

    ASSERT_EQ(cursor->next(), IndexKeyEntry(insertPoint, loc1));
}

TEST_F(SortedDataInterfaceTest, SaveAndRestoreSeesNewInsertsAfterEOF_Forward_Unique) {
    testSaveAndRestoreSeesNewInsertsAfterEOF(opCtx(), harnessHelper(), true, IndexType::kUnique);
}
TEST_F(SortedDataInterfaceTest, SaveAndRestoreSeesNewInsertsAfterEOF_Forward_Standard) {
    testSaveAndRestoreSeesNewInsertsAfterEOF(opCtx(), harnessHelper(), true, IndexType::kNonUnique);
}
TEST_F(SortedDataInterfaceTest, SaveAndRestoreSeesNewInsertsAfterEOF_Forward_Id) {
    testSaveAndRestoreSeesNewInsertsAfterEOF(opCtx(), harnessHelper(), true, IndexType::kId);
}
TEST_F(SortedDataInterfaceTest, SaveAndRestoreSeesNewInsertsAfterEOF_Reverse_Unique) {
    testSaveAndRestoreSeesNewInsertsAfterEOF(opCtx(), harnessHelper(), false, IndexType::kUnique);
}
TEST_F(SortedDataInterfaceTest, SaveAndRestoreSeesNewInsertsAfterEOF_Reverse_Standard) {
    testSaveAndRestoreSeesNewInsertsAfterEOF(
        opCtx(), harnessHelper(), false, IndexType::kNonUnique);
}
TEST_F(SortedDataInterfaceTest, SaveAndRestoreSeesNewInsertsAfterEOF_Reverse_Id) {
    testSaveAndRestoreSeesNewInsertsAfterEOF(opCtx(), harnessHelper(), false, IndexType::kId);
}

// Make sure we restore to a RecordId at or ahead of save point if same key.
TEST_F(SortedDataInterfaceTest, SaveAndRestoreStandardIndexConsidersRecordId_Forward) {
    auto sorted = harnessHelper()->newSortedDataInterface(opCtx(),
                                                          /*unique*/ false,
                                                          /*partial=*/false,
                                                          {
                                                              {key1, loc1},
                                                              {key2, loc1},
                                                              {key3, loc1},
                                                          });

    auto cursor = sorted->newCursor(opCtx());

    ASSERT_EQ(
        cursor->seek(makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
        IndexKeyEntry(key1, loc1));

    cursor->save();
    removeFromIndex(opCtx(), sorted.get(), {{key1, loc1}});
    insertToIndex(opCtx(), sorted.get(), {{key1, loc2}});
    cursor->restore();  // Lands on inserted key.

    ASSERT_EQ(cursor->next(), IndexKeyEntry(key1, loc2));

    cursor->save();
    removeFromIndex(opCtx(), sorted.get(), {{key1, loc2}});
    insertToIndex(opCtx(), sorted.get(), {{key1, loc1}});
    cursor->restore();  // Lands after inserted.

    ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc1));

    cursor->save();
    removeFromIndex(opCtx(), sorted.get(), {{key2, loc1}});
    cursor->restore();

    cursor->save();
    insertToIndex(opCtx(), sorted.get(), {{key2, loc1}});
    cursor->restore();  // Lands at same point as initial save.

    // Advances from restore point since restore didn't move position.
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key3, loc1));
}

// Test that cursors over unique indices will never return the same key twice.
TEST_F(SortedDataInterfaceTest, SaveAndRestoreUniqueIndexWontReturnDupKeys_Forward) {
    auto sorted = harnessHelper()->newSortedDataInterface(opCtx(),
                                                          /*unique*/ true,
                                                          /*partial=*/false,
                                                          {
                                                              {key1, loc1},
                                                              {key2, loc2},
                                                              {key3, loc2},
                                                              {key4, loc2},
                                                          });

    auto cursor = sorted->newCursor(opCtx());

    ASSERT_EQ(
        cursor->seek(makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
        IndexKeyEntry(key1, loc1));

    cursor->save();
    removeFromIndex(opCtx(), sorted.get(), {{key1, loc1}});
    insertToIndex(opCtx(), sorted.get(), {{key1, loc2}});
    cursor->restore();

    // We should skip over (key1, loc2) since we already returned (key1, loc1).
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc2));

    cursor->save();
    removeFromIndex(opCtx(), sorted.get(), {{key2, loc2}});
    insertToIndex(opCtx(), sorted.get(), {{key2, loc1}});
    cursor->restore();

    // We should skip over (key2, loc1) since we already returned (key2, loc2).
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key3, loc2));

    // If the key we just returned is removed, we should simply return the next key after restoring.
    cursor->save();
    removeFromIndex(opCtx(), sorted.get(), {{key3, loc2}});
    cursor->restore();
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key4, loc2));

    // If a key is inserted just ahead of our position, we should return it after restoring.
    cursor->save();
    insertToIndex(opCtx(), sorted.get(), {{key5, loc2}});
    cursor->restore();
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key5, loc2));
}

// Make sure we restore to a RecordId at or ahead of save point if same key on reverse cursor.
TEST_F(SortedDataInterfaceTest, SaveAndRestoreStandardIndexConsidersRecordId_Reverse) {
    auto sorted = harnessHelper()->newSortedDataInterface(opCtx(),
                                                          /*unique*/ false,
                                                          /*partial=*/false,
                                                          {
                                                              {key0, loc1},
                                                              {key1, loc1},
                                                              {key2, loc2},
                                                          });

    auto cursor = sorted->newCursor(opCtx(), false);

    ASSERT_EQ(
        cursor->seek(makeKeyStringForSeek(sorted.get(), key2, false, true).finishAndGetBuffer()),
        IndexKeyEntry(key2, loc2));

    cursor->save();
    removeFromIndex(opCtx(), sorted.get(), {{key2, loc2}});
    insertToIndex(opCtx(), sorted.get(), {{key2, loc1}});
    cursor->restore();

    ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc1));

    cursor->save();
    removeFromIndex(opCtx(), sorted.get(), {{key2, loc1}});
    insertToIndex(opCtx(), sorted.get(), {{key2, loc2}});
    cursor->restore();

    ASSERT_EQ(cursor->next(), IndexKeyEntry(key1, loc1));

    cursor->save();
    removeFromIndex(opCtx(), sorted.get(), {{key1, loc1}});
    cursor->restore();

    cursor->save();
    insertToIndex(opCtx(), sorted.get(), {{key1, loc1}});
    cursor->restore();  // Lands at same point as initial save.

    // Advances from restore point since restore didn't move position.
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key0, loc1));
}

// Test that reverse cursors over unique indices will never return the same key twice.
TEST_F(SortedDataInterfaceTest, SaveAndRestoreUniqueIndexWontReturnDupKeys_Reverse) {
    auto sorted = harnessHelper()->newSortedDataInterface(opCtx(),
                                                          /*unique*/ true,
                                                          /*partial=*/false,
                                                          {
                                                              {key1, loc1},
                                                              {key2, loc1},
                                                              {key3, loc1},
                                                              {key4, loc2},
                                                          });

    auto cursor = sorted->newCursor(opCtx(), false);

    ASSERT_EQ(
        cursor->seek(makeKeyStringForSeek(sorted.get(), key4, false, true).finishAndGetBuffer()),
        IndexKeyEntry(key4, loc2));

    cursor->save();
    removeFromIndex(opCtx(), sorted.get(), {{key4, loc2}});
    insertToIndex(opCtx(), sorted.get(), {{key4, loc1}});
    cursor->restore();

    // We should skip over (key4, loc1) since we already returned (key4, loc2).
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key3, loc1));

    cursor->save();
    removeFromIndex(opCtx(), sorted.get(), {{key3, loc1}});
    insertToIndex(opCtx(), sorted.get(), {{key3, loc2}});
    cursor->restore();

    // We should skip over (key3, loc2) since we already returned (key3, loc1).
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc1));

    // If the key we just returned is removed, we should simply return the next key after restoring.
    cursor->save();
    removeFromIndex(opCtx(), sorted.get(), {{key2, loc1}});
    cursor->restore();
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key1, loc1));

    // If a key is inserted just ahead of our position, we should return it after restoring.
    cursor->save();
    insertToIndex(opCtx(), sorted.get(), {{key0, loc1}});
    cursor->restore();
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key0, loc1));
}

// Ensure that SaveUnpositioned allows later use of the cursor.
TEST_F(SortedDataInterfaceTest, SaveUnpositionedAndRestore) {
    auto sorted = harnessHelper()->newSortedDataInterface(opCtx(),
                                                          /*unique=*/false,
                                                          /*partial=*/false,
                                                          {
                                                              {key1, loc1},
                                                              {key2, loc1},
                                                              {key3, loc1},
                                                          });

    auto cursor = sorted->newCursor(opCtx());

    ASSERT_EQ(
        cursor->seek(makeKeyStringForSeek(sorted.get(), key2, true, true).finishAndGetBuffer()),
        IndexKeyEntry(key2, loc1));

    cursor->saveUnpositioned();
    removeFromIndex(opCtx(), sorted.get(), {{key2, loc1}});
    cursor->restore();

    ASSERT_EQ(
        cursor->seek(makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
        IndexKeyEntry(key1, loc1));

    cursor->saveUnpositioned();
    cursor->restore();

    ASSERT_EQ(
        cursor->seek(makeKeyStringForSeek(sorted.get(), key3, true, true).finishAndGetBuffer()),
        IndexKeyEntry(key3, loc1));
}

TEST_F(SortedDataInterfaceTest, SaveRestoreLex) {
    const auto key1 = BSON(""
                           << "abc");
    const auto key2 = BSON(""
                           << "abcd");
    auto sorted =
        harnessHelper()->newSortedDataInterface(opCtx(),
                                                /*unique=*/false,
                                                /*partial=*/false,
                                                {{key1, RecordId(1)}, {key2, RecordId(2)}});

    // Check that these keys are lexicographically sorted.
    auto cursor = sorted->newCursor(opCtx());
    auto entry = cursor->seek(
        makeKeyStringForSeek(sorted.get(), BSONObj(), true, true).finishAndGetBuffer());
    ASSERT_EQ(entry, IndexKeyEntry(key1, RecordId(1)));

    entry = cursor->next();
    ASSERT_EQ(entry, IndexKeyEntry(key2, RecordId(2)));

    cursor->save();

    // Delete abcd, restore make sure that we don't get abc.
    {
        StorageWriteTransaction txn(recoveryUnit());
        sorted->unindex(
            opCtx(), makeKeyString(sorted.get(), key2, RecordId(2)), true /* allow duplicates */);
        txn.commit();
    }

    cursor->restore();
    entry = cursor->next();
    ASSERT_EQ(boost::none, entry);
}

TEST_F(SortedDataInterfaceTest, SaveRestoreLexWithEndPosition) {
    const auto key1 = BSON(""
                           << "abc");
    const auto key2 = BSON(""
                           << "abcd");
    auto sorted =
        harnessHelper()->newSortedDataInterface(opCtx(),
                                                /*unique=*/false,
                                                /*partial=*/false,
                                                {{key1, RecordId(1)}, {key2, RecordId(2)}});

    // Check that these keys are lexicographically sorted.
    auto cursor = sorted->newCursor(opCtx());
    cursor->setEndPosition(key1, true);

    auto entry =
        cursor->seek(makeKeyStringForSeek(sorted.get(), key2, true, true).finishAndGetBuffer());
    ASSERT_EQ(boost::none, entry);

    cursor->setEndPosition(key2, true);

    entry = cursor->seek(makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer());
    ASSERT_EQ(entry, IndexKeyEntry(key1, RecordId(1)));

    entry = cursor->next();
    ASSERT_EQ(entry, IndexKeyEntry(key2, RecordId(2)));

    cursor->save();

    cursor->setEndPosition(key1, true);
    cursor->restore();
    entry = cursor->next();
    ASSERT_EQ(boost::none, entry);
}

}  // namespace
}  // namespace mongo
