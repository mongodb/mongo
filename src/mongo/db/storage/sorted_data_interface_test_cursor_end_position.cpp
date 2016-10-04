/**
 *    Copyright (C) 2015 MongoDB Inc.
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
// Tests setEndPosition with next().
void testSetEndPosition_Next_Forward(bool unique, bool inclusive) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(
        unique,
        {
            {key1, loc1}, {key2, loc1}, {key3, loc1}, {key4, loc1}, {key5, loc1},
        });

    // Dup key on end point. Illegal for unique indexes.
    if (!unique)
        insertToIndex(opCtx, sorted, {{key3, loc2}});

    auto cursor = sorted->newCursor(opCtx.get());
    cursor->setEndPosition(key3, inclusive);

    ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc1));
    if (inclusive) {
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key3, loc1));
        if (!unique)
            ASSERT_EQ(cursor->next(), IndexKeyEntry(key3, loc2));
    }
    ASSERT_EQ(cursor->next(), boost::none);
    ASSERT_EQ(cursor->next(), boost::none);  // don't resurrect.
}
TEST(SortedDataInterface, SetEndPosition_Next_Forward_Unique_Inclusive) {
    testSetEndPosition_Next_Forward(true, true);
}
TEST(SortedDataInterface, SetEndPosition_Next_Forward_Unique_Exclusive) {
    testSetEndPosition_Next_Forward(true, false);
}
TEST(SortedDataInterface, SetEndPosition_Next_Forward_Standard_Inclusive) {
    testSetEndPosition_Next_Forward(false, true);
}
TEST(SortedDataInterface, SetEndPosition_Next_Forward_Standard_Exclusive) {
    testSetEndPosition_Next_Forward(false, false);
}

void testSetEndPosition_Next_Reverse(bool unique, bool inclusive) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(
        unique,
        {
            {key1, loc1}, {key2, loc1}, {key3, loc1}, {key4, loc1}, {key5, loc1},
        });

    // Dup key on end point. Illegal for unique indexes.
    if (!unique)
        insertToIndex(opCtx, sorted, {{key3, loc2}});

    auto cursor = sorted->newCursor(opCtx.get(), false);
    cursor->setEndPosition(key3, inclusive);

    ASSERT_EQ(cursor->seek(key5, true), IndexKeyEntry(key5, loc1));
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key4, loc1));
    if (inclusive) {
        if (!unique)
            ASSERT_EQ(cursor->next(), IndexKeyEntry(key3, loc2));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(key3, loc1));
    }
    ASSERT_EQ(cursor->next(), boost::none);
    ASSERT_EQ(cursor->next(), boost::none);  // don't resurrect.
}
TEST(SortedDataInterface, SetEndPosition_Next_Reverse_Unique_Inclusive) {
    testSetEndPosition_Next_Reverse(true, true);
}
TEST(SortedDataInterface, SetEndPosition_Next_Reverse_Unique_Exclusive) {
    testSetEndPosition_Next_Reverse(true, false);
}
TEST(SortedDataInterface, SetEndPosition_Next_Reverse_Standard_Inclusive) {
    testSetEndPosition_Next_Reverse(false, true);
}
TEST(SortedDataInterface, SetEndPosition_Next_Reverse_Standard_Exclusive) {
    testSetEndPosition_Next_Reverse(false, false);
}

// Tests setEndPosition with seek() and seekExact().
void testSetEndPosition_Seek_Forward(bool unique, bool inclusive) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(unique,
                                                        {
                                                            {key1, loc1},
                                                            // No key2
                                                            {key3, loc1},
                                                            {key4, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx.get());
    cursor->setEndPosition(key3, inclusive);

    // Directly seeking past end is considered out of range.
    ASSERT_EQ(cursor->seek(key4, true), boost::none);
    ASSERT_EQ(cursor->seekExact(key4), boost::none);

    // Seeking to key3 directly or indirectly is only returned if endPosition is inclusive.
    auto maybeKey3 = inclusive ? boost::make_optional(IndexKeyEntry(key3, loc1)) : boost::none;

    // direct
    ASSERT_EQ(cursor->seek(key3, true), maybeKey3);
    ASSERT_EQ(cursor->seekExact(key3), maybeKey3);

    // indirect
    ASSERT_EQ(cursor->seek(key2, true), maybeKey3);

    cursor->saveUnpositioned();
    removeFromIndex(opCtx, sorted, {{key3, loc1}});
    cursor->restore();

    ASSERT_EQ(cursor->seek(key2, true), boost::none);
    ASSERT_EQ(cursor->seek(key3, true), boost::none);
}
TEST(SortedDataInterface, SetEndPosition_Seek_Forward_Unique_Inclusive) {
    testSetEndPosition_Seek_Forward(true, true);
}
TEST(SortedDataInterface, SetEndPosition_Seek_Forward_Unique_Exclusive) {
    testSetEndPosition_Seek_Forward(true, false);
}
TEST(SortedDataInterface, SetEndPosition_Seek_Forward_Standard_Inclusive) {
    testSetEndPosition_Seek_Forward(false, true);
}
TEST(SortedDataInterface, SetEndPosition_Seek_Forward_Standard_Exclusive) {
    testSetEndPosition_Seek_Forward(false, false);
}

void testSetEndPosition_Seek_Reverse(bool unique, bool inclusive) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(unique,
                                                        {
                                                            {key1, loc1},
                                                            {key2, loc1},
                                                            // No key3
                                                            {key4, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx.get(), false);
    cursor->setEndPosition(key2, inclusive);

    // Directly seeking past end is considered out of range.
    ASSERT_EQ(cursor->seek(key1, true), boost::none);
    ASSERT_EQ(cursor->seekExact(key1), boost::none);

    // Seeking to key2 directly or indirectly is only returned if endPosition is inclusive.
    auto maybeKey2 = inclusive ? boost::make_optional(IndexKeyEntry(key2, loc1)) : boost::none;

    // direct
    ASSERT_EQ(cursor->seek(key2, true), maybeKey2);
    ASSERT_EQ(cursor->seekExact(key2), maybeKey2);

    // indirect
    ASSERT_EQ(cursor->seek(key3, true), maybeKey2);

    cursor->saveUnpositioned();
    removeFromIndex(opCtx, sorted, {{key2, loc1}});
    cursor->restore();

    ASSERT_EQ(cursor->seek(key3, true), boost::none);
    ASSERT_EQ(cursor->seek(key2, true), boost::none);
}
TEST(SortedDataInterface, SetEndPosition_Seek_Reverse_Unique_Inclusive) {
    testSetEndPosition_Seek_Reverse(true, true);
}
TEST(SortedDataInterface, SetEndPosition_Seek_Reverse_Unique_Exclusive) {
    testSetEndPosition_Seek_Reverse(true, false);
}
TEST(SortedDataInterface, SetEndPosition_Seek_Reverse_Standard_Inclusive) {
    testSetEndPosition_Seek_Reverse(false, true);
}
TEST(SortedDataInterface, SetEndPosition_Seek_Reverse_Standard_Exclusive) {
    testSetEndPosition_Seek_Reverse(false, false);
}

// Test that restore never lands on the wrong side of the endPosition.
void testSetEndPosition_Restore_Forward(bool unique) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(
        unique,
        {
            {key1, loc1}, {key2, loc1}, {key3, loc1}, {key4, loc1},
        });

    auto cursor = sorted->newCursor(opCtx.get());
    cursor->setEndPosition(key3, false);  // Should never see key3 or key4.

    ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));

    cursor->save();
    cursor->restore();

    ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc1));

    cursor->save();
    removeFromIndex(opCtx,
                    sorted,
                    {
                        {key2, loc1}, {key3, loc1},
                    });
    cursor->restore();

    ASSERT_EQ(cursor->next(), boost::none);
}
TEST(SortedDataInterface, SetEndPosition_Restore_Forward_Unique) {
    testSetEndPosition_Restore_Forward(true);
}
TEST(SortedDataInterface, SetEndPosition_Restore_Forward_Standard) {
    testSetEndPosition_Restore_Forward(false);
}

void testSetEndPosition_Restore_Reverse(bool unique) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(
        unique,
        {
            {key1, loc1}, {key2, loc1}, {key3, loc1}, {key4, loc1},
        });

    auto cursor = sorted->newCursor(opCtx.get(), false);
    cursor->setEndPosition(key2, false);  // Should never see key1 or key2.

    ASSERT_EQ(cursor->seek(key4, true), IndexKeyEntry(key4, loc1));

    cursor->save();
    cursor->restore();

    ASSERT_EQ(cursor->next(), IndexKeyEntry(key3, loc1));

    cursor->save();
    removeFromIndex(opCtx,
                    sorted,
                    {
                        {key2, loc1}, {key3, loc1},
                    });
    cursor->restore();

    ASSERT_EQ(cursor->next(), boost::none);
}
TEST(SortedDataInterface, SetEndPosition_Restore_Reverse_Unique) {
    testSetEndPosition_Restore_Reverse(true);
}
TEST(SortedDataInterface, SetEndPosition_Restore_Reverse_Standard) {
    testSetEndPosition_Restore_Reverse(false);
}

// Test that restore always updates the end cursor if one is used. Some storage engines use a
// cursor positioned at the first out-of-range document and have next() check if the current
// position is the same as the end cursor. End cursor maintenance cannot be directly tested
// (since implementations are free not to use end cursors) but implementations that incorrectly
// restore end cursors would tend to fail this test.
void testSetEndPosition_RestoreEndCursor_Forward(bool unique) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(unique,
                                                        {
                                                            {key1, loc1}, {key4, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx.get());
    cursor->setEndPosition(key2, true);

    ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));

    // A potential source of bugs is not restoring end cursor with saveUnpositioned().
    cursor->saveUnpositioned();
    insertToIndex(opCtx,
                  sorted,
                  {
                      {key2, loc1},  // in range
                      {key3, loc1},  // out of range
                  });
    cursor->restore();

    ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc1));
    ASSERT_EQ(cursor->next(), boost::none);
}
TEST(SortedDataInterface, SetEndPosition_RestoreEndCursor_Forward_Unique) {
    testSetEndPosition_RestoreEndCursor_Forward(true);
}
TEST(SortedDataInterface, SetEndPosition_RestoreEndCursor_Forward_Standard) {
    testSetEndPosition_RestoreEndCursor_Forward(false);
}

void testSetEndPosition_RestoreEndCursor_Reverse(bool unique) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(unique,
                                                        {
                                                            {key1, loc1}, {key4, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx.get(), false);
    cursor->setEndPosition(key3, true);

    ASSERT_EQ(cursor->seek(key4, true), IndexKeyEntry(key4, loc1));

    cursor->saveUnpositioned();
    insertToIndex(opCtx,
                  sorted,
                  {
                      {key2, loc1},  // in range
                      {key3, loc1},  // out of range
                  });
    cursor->restore();  // must restore end cursor even with saveUnpositioned().

    ASSERT_EQ(cursor->seek(key4, true), IndexKeyEntry(key4, loc1));
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key3, loc1));
    ASSERT_EQ(cursor->next(), boost::none);
}
TEST(SortedDataInterface, SetEndPosition_RestoreEndCursor_Reverse_Standard) {
    testSetEndPosition_RestoreEndCursor_Reverse(true);
}
TEST(SortedDataInterface, SetEndPosition_RestoreEndCursor_Reverse_Unique) {
    testSetEndPosition_RestoreEndCursor_Reverse(false);
}

// setEndPosition with empty BSONObj is supposed to mean "no end position", regardless of
// inclusive flag or direction.
void testSetEndPosition_Empty_Forward(bool unique, bool inclusive) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted =
        harnessHelper->newSortedDataInterface(unique,
                                              {
                                                  {key1, loc1}, {key2, loc1}, {key3, loc1},
                                              });

    auto cursor = sorted->newCursor(opCtx.get());
    cursor->setEndPosition(BSONObj(), inclusive);

    ASSERT_EQ(cursor->seek(key1, true), IndexKeyEntry(key1, loc1));
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc1));
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key3, loc1));
    ASSERT_EQ(cursor->next(), boost::none);
}
TEST(SortedDataInterface, SetEndPosition_Empty_Forward_Unique_Inclusive) {
    testSetEndPosition_Empty_Forward(true, true);
}
TEST(SortedDataInterface, SetEndPosition_Empty_Forward_Unique_Exclusive) {
    testSetEndPosition_Empty_Forward(true, false);
}
TEST(SortedDataInterface, SetEndPosition_Empty_Forward_Standard_Inclusive) {
    testSetEndPosition_Empty_Forward(false, true);
}
TEST(SortedDataInterface, SetEndPosition_Empty_Forward_Standard_Exclusive) {
    testSetEndPosition_Empty_Forward(false, false);
}

void testSetEndPosition_Empty_Reverse(bool unique, bool inclusive) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted =
        harnessHelper->newSortedDataInterface(unique,
                                              {
                                                  {key1, loc1}, {key2, loc1}, {key3, loc1},
                                              });

    auto cursor = sorted->newCursor(opCtx.get(), false);
    cursor->setEndPosition(BSONObj(), inclusive);

    ASSERT_EQ(cursor->seek(key3, true), IndexKeyEntry(key3, loc1));
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc1));
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key1, loc1));
    ASSERT_EQ(cursor->next(), boost::none);
}
TEST(SortedDataInterface, SetEndPosition_Empty_Reverse_Unique_Inclusive) {
    testSetEndPosition_Empty_Reverse(true, true);
}
TEST(SortedDataInterface, SetEndPosition_Empty_Reverse_Unique_Exclusive) {
    testSetEndPosition_Empty_Reverse(true, false);
}
TEST(SortedDataInterface, SetEndPosition_Empty_Reverse_Standard_Inclusive) {
    testSetEndPosition_Empty_Reverse(false, true);
}
TEST(SortedDataInterface, SetEndPosition_Empty_Reverse_Standard_Exclusive) {
    testSetEndPosition_Empty_Reverse(false, false);
}
}  // namespace mongo
