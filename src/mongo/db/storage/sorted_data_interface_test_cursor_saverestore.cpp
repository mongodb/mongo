// sorted_data_interface_test_cursor_saverestore.cpp

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

// Insert multiple keys and try to iterate through all of them
// using a forward cursor while calling savePosition() and
// restorePosition() in succession.
TEST(SortedDataInterface, SaveAndRestorePositionWhileIterateCursor) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            BSONObj key = BSON("" << i);
            RecordId loc(42, i * 2);
            ASSERT_OK(sorted->insert(opCtx.get(), key, loc, true));
            uow.commit();
        }
    }

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx.get()));
    }

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        int i = 0;
        for (auto entry = cursor->seek(minKey, true); entry; i++, entry = cursor->next()) {
            ASSERT_LT(i, nToInsert);
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << i), RecordId(42, i * 2)));

            cursor->savePositioned();
            cursor->restore(opCtx.get());
        }
        ASSERT(!cursor->next());
        ASSERT_EQ(i, nToInsert);
    }
}

// Insert multiple keys and try to iterate through all of them
// using a reverse cursor while calling savePosition() and
// restorePosition() in succession.
TEST(SortedDataInterface, SaveAndRestorePositionWhileIterateCursorReversed) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            BSONObj key = BSON("" << i);
            RecordId loc(42, i * 2);
            ASSERT_OK(sorted->insert(opCtx.get(), key, loc, true));
            uow.commit();
        }
    }

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx.get()));
    }

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        int i = nToInsert - 1;
        for (auto entry = cursor->seek(maxKey, true); entry; i--, entry = cursor->next()) {
            ASSERT_GTE(i, 0);
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << i), RecordId(42, i * 2)));

            cursor->savePositioned();
            cursor->restore(opCtx.get());
        }
        ASSERT(!cursor->next());
        ASSERT_EQ(i, -1);
    }
}

// Insert the same key multiple times and try to iterate through each
// occurrence using a forward cursor while calling savePosition() and
// restorePosition() in succession. Verify that the RecordId is saved
// as part of the current position of the cursor.
TEST(SortedDataInterface, SaveAndRestorePositionWhileIterateCursorWithDupKeys) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            RecordId loc(42, i * 2);
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc, true /* allow duplicates */));
            uow.commit();
        }
    }

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx.get()));
    }

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        int i = 0;
        for (auto entry = cursor->seek(minKey, true); entry; i++, entry = cursor->next()) {
            ASSERT_LT(i, nToInsert);
            ASSERT_EQ(entry, IndexKeyEntry(key1, RecordId(42, i * 2)));

            cursor->savePositioned();
            cursor->restore(opCtx.get());
        }
        ASSERT(!cursor->next());
        ASSERT_EQ(i, nToInsert);
    }
}

// Insert the same key multiple times and try to iterate through each
// occurrence using a reverse cursor while calling savePosition() and
// restorePosition() in succession. Verify that the RecordId is saved
// as part of the current position of the cursor.
TEST(SortedDataInterface, SaveAndRestorePositionWhileIterateCursorWithDupKeysReversed) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            RecordId loc(42, i * 2);
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc, true /* allow duplicates */));
            uow.commit();
        }
    }

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx.get()));
    }

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        int i = nToInsert - 1;
        for (auto entry = cursor->seek(maxKey, true); entry; i--, entry = cursor->next()) {
            ASSERT_GTE(i, 0);
            ASSERT_EQ(entry, IndexKeyEntry(key1, RecordId(42, i * 2)));

            cursor->savePositioned();
            cursor->restore(opCtx.get());
        }
        ASSERT(!cursor->next());
        ASSERT_EQ(i, -1);
    }
}

// Call savePosition() on a forward cursor without ever calling restorePosition().
// May be useful to run this test under valgrind to verify there are no leaks.
TEST(SortedDataInterface, SavePositionWithoutRestore) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(true));

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, false));
            uow.commit();
        }
    }

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        cursor->savePositioned();
    }
}

// Call savePosition() on a reverse cursor without ever calling restorePosition().
// May be useful to run this test under valgrind to verify there are no leaks.
TEST(SortedDataInterface, SavePositionWithoutRestoreReversed) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, true));
            uow.commit();
        }
    }

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }

    {
        const std::unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        cursor->savePositioned();
    }
}

// Ensure that restore lands as close as possible to original position, even if data inserted
// while saved.
void testSaveAndRestorePositionSeesNewInserts(bool forward, bool unique) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(unique,
                                                        {
                                                         {key1, loc1}, {key3, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx.get(), forward);
    const auto seekPoint = forward ? key1 : key3;

    ASSERT_EQ(cursor->seek(seekPoint, true), IndexKeyEntry(seekPoint, loc1));

    cursor->savePositioned();
    insertToIndex(opCtx, sorted, {{key2, loc1}});
    cursor->restore(opCtx.get());

    ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc1));
}
TEST(SortedDataInterface, SaveAndRestorePositionSeesNewInserts_Forward_Unique) {
    testSaveAndRestorePositionSeesNewInserts(true, true);
}
TEST(SortedDataInterface, SaveAndRestorePositionSeesNewInserts_Forward_Standard) {
    testSaveAndRestorePositionSeesNewInserts(true, false);
}
TEST(SortedDataInterface, SaveAndRestorePositionSeesNewInserts_Reverse_Unique) {
    testSaveAndRestorePositionSeesNewInserts(false, true);
}
TEST(SortedDataInterface, SaveAndRestorePositionSeesNewInserts_Reverse_Standard) {
    testSaveAndRestorePositionSeesNewInserts(false, false);
}

// Ensure that repeated restores lands as close as possible to original position, even if data
// inserted while saved and the current position removed.
void testSaveAndRestorePositionSeesNewInsertsAfterRemove(bool forward, bool unique) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(unique,
                                                        {
                                                         {key1, loc1}, {key3, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx.get(), forward);
    const auto seekPoint = forward ? key1 : key3;

    ASSERT_EQ(cursor->seek(seekPoint, true), IndexKeyEntry(seekPoint, loc1));

    cursor->savePositioned();
    removeFromIndex(opCtx, sorted, {{key1, loc1}});
    cursor->restore(opCtx.get());
    // The restore may have seeked since it can't return to the saved position.

    cursor->savePositioned();  // Should still save originally saved key as "current position".
    insertToIndex(opCtx, sorted, {{key2, loc1}});
    cursor->restore(opCtx.get());

    ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc1));
}
TEST(SortedDataInterface, SaveAndRestorePositionSeesNewInsertsAfterRemove_Forward_Unique) {
    testSaveAndRestorePositionSeesNewInsertsAfterRemove(true, true);
}
TEST(SortedDataInterface, SaveAndRestorePositionSeesNewInsertsAfterRemove_Forward_Standard) {
    testSaveAndRestorePositionSeesNewInsertsAfterRemove(true, false);
}
TEST(SortedDataInterface, SaveAndRestorePositionSeesNewInsertsAfterRemove_Reverse_Unique) {
    testSaveAndRestorePositionSeesNewInsertsAfterRemove(false, true);
}
TEST(SortedDataInterface, SaveAndRestorePositionSeesNewInsertsAfterRemove_Reverse_Standard) {
    testSaveAndRestorePositionSeesNewInsertsAfterRemove(false, false);
}

// Ensure that repeated restores lands as close as possible to original position, even if data
// inserted while saved and the current position removed in a way that temporarily makes the
// cursor EOF.
void testSaveAndRestorePositionSeesNewInsertsAfterEOF(bool forward, bool unique) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(false,
                                                        {
                                                         {key1, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx.get(), forward);

    ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
    // next() would return EOF now.

    cursor->savePositioned();
    removeFromIndex(opCtx, sorted, {{key1, loc1}});
    cursor->restore(opCtx.get());
    // The restore may have seeked to EOF.

    auto insertPoint = forward ? key2 : key0;
    cursor->savePositioned();  // Should still save key1 as "current position".
    insertToIndex(opCtx, sorted, {{insertPoint, loc1}});
    cursor->restore(opCtx.get());

    ASSERT_EQ(cursor->next(), IndexKeyEntry(insertPoint, loc1));
}

TEST(SortedDataInterface, SaveAndRestorePositionSeesNewInsertsAfterEOF_Forward_Unique) {
    testSaveAndRestorePositionSeesNewInsertsAfterEOF(true, true);
}
TEST(SortedDataInterface, SaveAndRestorePositionSeesNewInsertsAfterEOF_Forward_Standard) {
    testSaveAndRestorePositionSeesNewInsertsAfterEOF(true, false);
}
TEST(SortedDataInterface, SaveAndRestorePositionSeesNewInsertsAfterEOF_Reverse_Unique) {
    testSaveAndRestorePositionSeesNewInsertsAfterEOF(false, true);
}
TEST(SortedDataInterface, SaveAndRestorePositionSeesNewInsertsAfterEOF_Reverse_Standard) {
    testSaveAndRestorePositionSeesNewInsertsAfterEOF(false, false);
}

// Make sure we restore to a RecordId at or ahead of save point if same key.
void testSaveAndRestorePositionConsidersRecordId_Forward(bool unique) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(unique,
                                                        {
                                                         {key1, loc1}, {key2, loc1}, {key3, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx.get());

    ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));

    cursor->savePositioned();
    removeFromIndex(opCtx, sorted, {{key1, loc1}});
    insertToIndex(opCtx, sorted, {{key1, loc2}});
    cursor->restore(opCtx.get());  // Lands on inserted key.

    ASSERT_EQ(cursor->next(), IndexKeyEntry(key1, loc2));

    cursor->savePositioned();
    removeFromIndex(opCtx, sorted, {{key1, loc2}});
    insertToIndex(opCtx, sorted, {{key1, loc1}});
    cursor->restore(opCtx.get());  // Lands after inserted.

    ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc1));

    cursor->savePositioned();
    removeFromIndex(opCtx, sorted, {{key2, loc1}});
    cursor->restore(opCtx.get());

    cursor->savePositioned();
    insertToIndex(opCtx, sorted, {{key2, loc1}});
    cursor->restore(opCtx.get());  // Lands at same point as initial save.

    // Advances from restore point since restore didn't move position.
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key3, loc1));
}
TEST(SortedDataInterface, SaveAndRestorePositionConsidersRecordId_Forward_Standard) {
    testSaveAndRestorePositionConsidersRecordId_Forward(false);
}
TEST(SortedDataInterface, SaveAndRestorePositionConsidersRecordId_Forward_Unique) {
    testSaveAndRestorePositionConsidersRecordId_Forward(true);
}

// Make sure we restore to a RecordId at or ahead of save point if same key on reverse cursor.
void testSaveAndRestorePositionConsidersRecordId_Reverse(bool unique) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(unique,
                                                        {
                                                         {key0, loc1}, {key1, loc1}, {key2, loc2},
                                                        });

    auto cursor = sorted->newCursor(opCtx.get(), false);

    ASSERT_EQ(cursor->seek(key2, true), IndexKeyEntry(key2, loc2));

    cursor->savePositioned();
    removeFromIndex(opCtx, sorted, {{key2, loc2}});
    insertToIndex(opCtx, sorted, {{key2, loc1}});
    cursor->restore(opCtx.get());

    ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc1));

    cursor->savePositioned();
    removeFromIndex(opCtx, sorted, {{key2, loc1}});
    insertToIndex(opCtx, sorted, {{key2, loc2}});
    cursor->restore(opCtx.get());

    ASSERT_EQ(cursor->next(), IndexKeyEntry(key1, loc1));

    cursor->savePositioned();
    removeFromIndex(opCtx, sorted, {{key1, loc1}});
    cursor->restore(opCtx.get());

    cursor->savePositioned();
    insertToIndex(opCtx, sorted, {{key1, loc1}});
    cursor->restore(opCtx.get());  // Lands at same point as initial save.

    // Advances from restore point since restore didn't move position.
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key0, loc1));
}
TEST(SortedDataInterface, SaveAndRestorePositionConsidersRecordId_Reverse_Standard) {
    testSaveAndRestorePositionConsidersRecordId_Reverse(false);
}
TEST(SortedDataInterface, SaveAndRestorePositionConsidersRecordId_Reverse_Unique) {
    testSaveAndRestorePositionConsidersRecordId_Reverse(true);
}

// Ensure that SaveUnpositioned allows later use of the cursor.
TEST(SortedDataInterface, SaveUnpositionedAndRestore) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(false,
                                                        {
                                                         {key1, loc1}, {key2, loc1}, {key3, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx.get());

    ASSERT_EQ(cursor->seek(key2, true), IndexKeyEntry(key2, loc1));

    cursor->saveUnpositioned();
    removeFromIndex(opCtx, sorted, {{key2, loc1}});
    cursor->restore(opCtx.get());

    ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));

    cursor->saveUnpositioned();
    cursor->restore(opCtx.get());

    ASSERT_EQ(cursor->seek(key3, true), IndexKeyEntry(key3, loc1));
}

}  // namespace mongo
