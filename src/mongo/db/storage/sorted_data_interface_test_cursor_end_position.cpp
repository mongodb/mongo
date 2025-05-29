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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
// Tests setEndPosition with next().
void testSetEndPosition_Next_Forward(OperationContext* opCtx,
                                     RecoveryUnit& ru,
                                     SortedDataInterfaceHarnessHelper* harnessHelper,
                                     bool unique,
                                     bool inclusive) {
    auto sorted = harnessHelper->newSortedDataInterface(opCtx,
                                                        unique,
                                                        /*partial=*/false,
                                                        {
                                                            {key1, loc1},
                                                            {key2, loc1},
                                                            {key3, loc1},
                                                            {key4, loc1},
                                                            {key5, loc1},
                                                        });
    // Dup key on end point. Illegal for unique indexes.
    if (!unique)
        insertToIndex(opCtx, sorted.get(), {{key3, loc2}});

    auto cursor = sorted->newCursor(opCtx, ru);
    cursor->setEndPosition(key3, inclusive);

    ASSERT_EQ(
        cursor->seek(ru, makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
        IndexKeyEntry(key1, loc1));
    ASSERT_EQ(cursor->next(ru), IndexKeyEntry(key2, loc1));
    if (inclusive) {
        ASSERT_EQ(cursor->next(ru), IndexKeyEntry(key3, loc1));
        if (!unique) {
            ASSERT_EQ(cursor->next(ru), IndexKeyEntry(key3, loc2));
        }
    }
    ASSERT_EQ(cursor->next(ru), boost::none);
    ASSERT_EQ(cursor->next(ru), boost::none);  // don't resurrect.
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Next_Forward_Unique_Inclusive) {
    testSetEndPosition_Next_Forward(opCtx(), recoveryUnit(), harnessHelper(), true, true);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Next_Forward_Unique_Exclusive) {
    testSetEndPosition_Next_Forward(opCtx(), recoveryUnit(), harnessHelper(), true, false);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Next_Forward_Standard_Inclusive) {
    testSetEndPosition_Next_Forward(opCtx(), recoveryUnit(), harnessHelper(), false, true);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Next_Forward_Standard_Exclusive) {
    testSetEndPosition_Next_Forward(opCtx(), recoveryUnit(), harnessHelper(), false, false);
}

void testSetEndPosition_Next_Reverse(OperationContext* opCtx,
                                     RecoveryUnit& ru,
                                     SortedDataInterfaceHarnessHelper* harnessHelper,
                                     bool unique,
                                     bool inclusive) {
    auto sorted = harnessHelper->newSortedDataInterface(opCtx,
                                                        unique,
                                                        /*partial=*/false,
                                                        {
                                                            {key1, loc1},
                                                            {key2, loc1},
                                                            {key3, loc1},
                                                            {key4, loc1},
                                                            {key5, loc1},
                                                        });

    // Dup key on end point. Illegal for unique indexes.
    if (!unique)
        insertToIndex(opCtx, sorted.get(), {{key3, loc2}});

    auto cursor = sorted->newCursor(opCtx, ru, false);
    cursor->setEndPosition(key3, inclusive);

    ASSERT_EQ(cursor->seek(
                  ru, makeKeyStringForSeek(sorted.get(), key5, false, true).finishAndGetBuffer()),
              IndexKeyEntry(key5, loc1));
    ASSERT_EQ(cursor->next(ru), IndexKeyEntry(key4, loc1));
    if (inclusive) {
        if (!unique) {
            ASSERT_EQ(cursor->next(ru), IndexKeyEntry(key3, loc2));
        }
        ASSERT_EQ(cursor->next(ru), IndexKeyEntry(key3, loc1));
    }
    ASSERT_EQ(cursor->next(ru), boost::none);
    ASSERT_EQ(cursor->next(ru), boost::none);  // don't resurrect.
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Next_Reverse_Unique_Inclusive) {
    testSetEndPosition_Next_Reverse(opCtx(), recoveryUnit(), harnessHelper(), true, true);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Next_Reverse_Unique_Exclusive) {
    testSetEndPosition_Next_Reverse(opCtx(), recoveryUnit(), harnessHelper(), true, false);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Next_Reverse_Standard_Inclusive) {
    testSetEndPosition_Next_Reverse(opCtx(), recoveryUnit(), harnessHelper(), false, true);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Next_Reverse_Standard_Exclusive) {
    testSetEndPosition_Next_Reverse(opCtx(), recoveryUnit(), harnessHelper(), false, false);
}

// Tests setEndPosition with seek().
void testSetEndPosition_Seek_Forward(OperationContext* opCtx,
                                     RecoveryUnit& ru,
                                     SortedDataInterfaceHarnessHelper* harnessHelper,
                                     bool unique,
                                     bool inclusive) {
    auto sorted = harnessHelper->newSortedDataInterface(opCtx,
                                                        unique,
                                                        /*partial=*/false,
                                                        {
                                                            {key1, loc1},
                                                            // No key2
                                                            {key3, loc1},
                                                            {key4, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx, ru);
    cursor->setEndPosition(key3, inclusive);

    // Directly seeking past end is considered out of range.
    ASSERT_EQ(
        cursor->seek(
            ru, makeKeyStringForSeek(sorted.get(), key4, true, inclusive).finishAndGetBuffer()),
        boost::none);

    // Seeking to key3 directly or indirectly is only returned if endPosition is inclusive.
    auto maybeKey3 = inclusive ? boost::make_optional(IndexKeyEntry(key3, loc1)) : boost::none;

    // direct
    ASSERT_EQ(
        cursor->seek(
            ru, makeKeyStringForSeek(sorted.get(), key3, true, inclusive).finishAndGetBuffer()),
        maybeKey3);

    // indirect
    ASSERT_EQ(
        cursor->seek(
            ru, makeKeyStringForSeek(sorted.get(), key2, true, inclusive).finishAndGetBuffer()),
        maybeKey3);

    cursor->saveUnpositioned();
    removeFromIndex(opCtx, sorted.get(), {{key3, loc1}});
    cursor->restore(ru);

    ASSERT_EQ(
        cursor->seek(
            ru, makeKeyStringForSeek(sorted.get(), key2, true, inclusive).finishAndGetBuffer()),
        boost::none);
    ASSERT_EQ(
        cursor->seek(
            ru, makeKeyStringForSeek(sorted.get(), key3, true, inclusive).finishAndGetBuffer()),
        boost::none);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Seek_Forward_Unique_Inclusive) {
    testSetEndPosition_Seek_Forward(opCtx(), recoveryUnit(), harnessHelper(), true, true);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Seek_Forward_Unique_Exclusive) {
    testSetEndPosition_Seek_Forward(opCtx(), recoveryUnit(), harnessHelper(), true, false);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Seek_Forward_Standard_Inclusive) {
    testSetEndPosition_Seek_Forward(opCtx(), recoveryUnit(), harnessHelper(), false, true);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Seek_Forward_Standard_Exclusive) {
    testSetEndPosition_Seek_Forward(opCtx(), recoveryUnit(), harnessHelper(), false, false);
}

void testSetEndPosition_Seek_Reverse(OperationContext* opCtx,
                                     RecoveryUnit& ru,
                                     SortedDataInterfaceHarnessHelper* harnessHelper,
                                     bool unique,
                                     bool inclusive) {
    auto sorted = harnessHelper->newSortedDataInterface(opCtx,
                                                        unique,
                                                        /*partial=*/false,
                                                        {
                                                            {key1, loc1},
                                                            {key2, loc1},
                                                            // No key3
                                                            {key4, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx, ru, false);
    cursor->setEndPosition(key2, inclusive);

    // Directly seeking past end is considered out of range.
    ASSERT_EQ(
        cursor->seek(
            ru, makeKeyStringForSeek(sorted.get(), key1, false, inclusive).finishAndGetBuffer()),
        boost::none);

    // Seeking to key2 directly or indirectly is only returned if endPosition is inclusive.
    auto maybeKey2 = inclusive ? boost::make_optional(IndexKeyEntry(key2, loc1)) : boost::none;

    // direct
    ASSERT_EQ(
        cursor->seek(
            ru, makeKeyStringForSeek(sorted.get(), key2, false, inclusive).finishAndGetBuffer()),
        maybeKey2);

    // indirect
    ASSERT_EQ(cursor->seek(
                  ru, makeKeyStringForSeek(sorted.get(), key3, false, true).finishAndGetBuffer()),
              maybeKey2);

    cursor->saveUnpositioned();
    removeFromIndex(opCtx, sorted.get(), {{key2, loc1}});
    cursor->restore(ru);

    ASSERT_EQ(cursor->seek(
                  ru, makeKeyStringForSeek(sorted.get(), key3, false, true).finishAndGetBuffer()),
              boost::none);
    ASSERT_EQ(cursor->seek(
                  ru, makeKeyStringForSeek(sorted.get(), key2, false, true).finishAndGetBuffer()),
              boost::none);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Seek_Reverse_Unique_Inclusive) {
    testSetEndPosition_Seek_Reverse(opCtx(), recoveryUnit(), harnessHelper(), true, true);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Seek_Reverse_Unique_Exclusive) {
    testSetEndPosition_Seek_Reverse(opCtx(), recoveryUnit(), harnessHelper(), true, false);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Seek_Reverse_Standard_Inclusive) {
    testSetEndPosition_Seek_Reverse(opCtx(), recoveryUnit(), harnessHelper(), false, true);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Seek_Reverse_Standard_Exclusive) {
    testSetEndPosition_Seek_Reverse(opCtx(), recoveryUnit(), harnessHelper(), false, false);
}

// Test that restore never lands on the wrong side of the endPosition.
void testSetEndPosition_Restore_Forward(OperationContext* opCtx,
                                        RecoveryUnit& ru,
                                        SortedDataInterfaceHarnessHelper* harnessHelper,
                                        bool unique) {
    auto sorted = harnessHelper->newSortedDataInterface(opCtx,
                                                        unique,
                                                        /*partial=*/false,
                                                        {
                                                            {key1, loc1},
                                                            {key2, loc1},
                                                            {key3, loc1},
                                                            {key4, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx, ru);
    cursor->setEndPosition(key3, false);  // Should never see key3 or key4.

    ASSERT_EQ(
        cursor->seek(ru, makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
        IndexKeyEntry(key1, loc1));

    cursor->save();
    cursor->restore(ru);

    ASSERT_EQ(cursor->next(ru), IndexKeyEntry(key2, loc1));

    cursor->save();
    removeFromIndex(opCtx,
                    sorted.get(),
                    {
                        {key2, loc1},
                        {key3, loc1},
                    });
    cursor->restore(ru);

    ASSERT_EQ(cursor->next(ru), boost::none);
}

TEST_F(SortedDataInterfaceTest, SetEndPosition_Restore_Forward_Unique) {
    testSetEndPosition_Restore_Forward(opCtx(), recoveryUnit(), harnessHelper(), true);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Restore_Forward_Standard) {
    testSetEndPosition_Restore_Forward(opCtx(), recoveryUnit(), harnessHelper(), false);
}

void testSetEndPosition_Restore_Reverse(OperationContext* opCtx,
                                        RecoveryUnit& ru,
                                        SortedDataInterfaceHarnessHelper* harnessHelper,
                                        bool unique) {
    auto sorted = harnessHelper->newSortedDataInterface(opCtx,
                                                        unique,
                                                        /*partial=*/false,
                                                        {
                                                            {key1, loc1},
                                                            {key2, loc1},
                                                            {key3, loc1},
                                                            {key4, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx, ru, false);
    cursor->setEndPosition(key2, false);  // Should never see key1 or key2.

    ASSERT_EQ(cursor->seek(
                  ru, makeKeyStringForSeek(sorted.get(), key4, false, true).finishAndGetBuffer()),
              IndexKeyEntry(key4, loc1));

    cursor->save();
    cursor->restore(ru);

    ASSERT_EQ(cursor->next(ru), IndexKeyEntry(key3, loc1));

    cursor->save();
    removeFromIndex(opCtx,
                    sorted.get(),
                    {
                        {key2, loc1},
                        {key3, loc1},
                    });
    cursor->restore(ru);

    ASSERT_EQ(cursor->next(ru), boost::none);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Restore_Reverse_Unique) {
    testSetEndPosition_Restore_Reverse(opCtx(), recoveryUnit(), harnessHelper(), true);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Restore_Reverse_Standard) {
    testSetEndPosition_Restore_Reverse(opCtx(), recoveryUnit(), harnessHelper(), false);
}

// Test that restore always updates the end cursor if one is used. Some storage engines use a
// cursor positioned at the first out-of-range document and have next() check if the current
// position is the same as the end cursor. End cursor maintenance cannot be directly tested
// (since implementations are free not to use end cursors) but implementations that incorrectly
// restore end cursors would tend to fail this test.
void testSetEndPosition_RestoreEndCursor_Forward(OperationContext* opCtx,
                                                 RecoveryUnit& ru,
                                                 SortedDataInterfaceHarnessHelper* harnessHelper,
                                                 bool unique) {
    auto sorted = harnessHelper->newSortedDataInterface(opCtx,
                                                        unique,
                                                        /*partial=*/false,
                                                        {
                                                            {key1, loc1},
                                                            {key4, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx, ru);
    cursor->setEndPosition(key2, true);

    ASSERT_EQ(
        cursor->seek(ru, makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
        IndexKeyEntry(key1, loc1));

    // A potential source of bugs is not restoring end cursor with saveUnpositioned().
    cursor->saveUnpositioned();
    insertToIndex(opCtx,
                  sorted.get(),
                  {
                      {key2, loc1},  // in range
                      {key3, loc1},  // out of range
                  });
    cursor->restore(ru);

    ASSERT_EQ(
        cursor->seek(ru, makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
        IndexKeyEntry(key1, loc1));
    ASSERT_EQ(cursor->next(ru), IndexKeyEntry(key2, loc1));
    ASSERT_EQ(cursor->next(ru), boost::none);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_RestoreEndCursor_Forward_Unique) {
    testSetEndPosition_RestoreEndCursor_Forward(opCtx(), recoveryUnit(), harnessHelper(), true);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_RestoreEndCursor_Forward_Standard) {
    testSetEndPosition_RestoreEndCursor_Forward(opCtx(), recoveryUnit(), harnessHelper(), false);
}

void testSetEndPosition_RestoreEndCursor_Reverse(OperationContext* opCtx,
                                                 RecoveryUnit& ru,
                                                 SortedDataInterfaceHarnessHelper* harnessHelper,
                                                 bool unique) {
    auto sorted = harnessHelper->newSortedDataInterface(opCtx,
                                                        unique,
                                                        /*partial=*/false,
                                                        {
                                                            {key1, loc1},
                                                            {key4, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx, ru, false);
    cursor->setEndPosition(key3, true);

    ASSERT_EQ(cursor->seek(
                  ru, makeKeyStringForSeek(sorted.get(), key4, false, true).finishAndGetBuffer()),
              IndexKeyEntry(key4, loc1));

    cursor->saveUnpositioned();
    insertToIndex(opCtx,
                  sorted.get(),
                  {
                      {key2, loc1},  // in range
                      {key3, loc1},  // out of range
                  });
    cursor->restore(ru);  // must restore end cursor even with saveUnpositioned().

    ASSERT_EQ(cursor->seek(
                  ru, makeKeyStringForSeek(sorted.get(), key4, false, true).finishAndGetBuffer()),
              IndexKeyEntry(key4, loc1));
    ASSERT_EQ(cursor->next(ru), IndexKeyEntry(key3, loc1));
    ASSERT_EQ(cursor->next(ru), boost::none);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_RestoreEndCursor_Reverse_Standard) {
    testSetEndPosition_RestoreEndCursor_Reverse(opCtx(), recoveryUnit(), harnessHelper(), true);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_RestoreEndCursor_Reverse_Unique) {
    testSetEndPosition_RestoreEndCursor_Reverse(opCtx(), recoveryUnit(), harnessHelper(), false);
}

// setEndPosition with empty BSONObj is supposed to mean "no end position", regardless of
// inclusive flag or direction.
void testSetEndPosition_Empty_Forward(OperationContext* opCtx,
                                      RecoveryUnit& ru,
                                      SortedDataInterfaceHarnessHelper* harnessHelper,
                                      bool unique,
                                      bool inclusive) {
    auto sorted = harnessHelper->newSortedDataInterface(opCtx,
                                                        unique,
                                                        /*partial=*/false,
                                                        {
                                                            {key1, loc1},
                                                            {key2, loc1},
                                                            {key3, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx, ru);
    cursor->setEndPosition(BSONObj(), inclusive);

    ASSERT_EQ(
        cursor->seek(ru, makeKeyStringForSeek(sorted.get(), key1, true, true).finishAndGetBuffer()),
        IndexKeyEntry(key1, loc1));
    ASSERT_EQ(cursor->next(ru), IndexKeyEntry(key2, loc1));
    ASSERT_EQ(cursor->next(ru), IndexKeyEntry(key3, loc1));
    ASSERT_EQ(cursor->next(ru), boost::none);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Empty_Forward_Unique_Inclusive) {
    testSetEndPosition_Empty_Forward(opCtx(), recoveryUnit(), harnessHelper(), true, true);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Empty_Forward_Unique_Exclusive) {
    testSetEndPosition_Empty_Forward(opCtx(), recoveryUnit(), harnessHelper(), true, false);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Empty_Forward_Standard_Inclusive) {
    testSetEndPosition_Empty_Forward(opCtx(), recoveryUnit(), harnessHelper(), false, true);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Empty_Forward_Standard_Exclusive) {
    testSetEndPosition_Empty_Forward(opCtx(), recoveryUnit(), harnessHelper(), false, false);
}

void testSetEndPosition_Empty_Reverse(OperationContext* opCtx,
                                      RecoveryUnit& ru,
                                      SortedDataInterfaceHarnessHelper* harnessHelper,
                                      bool unique,
                                      bool inclusive) {
    auto sorted = harnessHelper->newSortedDataInterface(opCtx,
                                                        unique,
                                                        /*partial=*/false,
                                                        {
                                                            {key1, loc1},
                                                            {key2, loc1},
                                                            {key3, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx, ru, false);
    cursor->setEndPosition(BSONObj(), inclusive);

    ASSERT_EQ(cursor->seek(
                  ru, makeKeyStringForSeek(sorted.get(), key3, false, true).finishAndGetBuffer()),
              IndexKeyEntry(key3, loc1));
    ASSERT_EQ(cursor->next(ru), IndexKeyEntry(key2, loc1));
    ASSERT_EQ(cursor->next(ru), IndexKeyEntry(key1, loc1));
    ASSERT_EQ(cursor->next(ru), boost::none);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Empty_Reverse_Unique_Inclusive) {
    testSetEndPosition_Empty_Reverse(opCtx(), recoveryUnit(), harnessHelper(), true, true);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Empty_Reverse_Unique_Exclusive) {
    testSetEndPosition_Empty_Reverse(opCtx(), recoveryUnit(), harnessHelper(), true, false);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Empty_Reverse_Standard_Inclusive) {
    testSetEndPosition_Empty_Reverse(opCtx(), recoveryUnit(), harnessHelper(), false, true);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Empty_Reverse_Standard_Exclusive) {
    testSetEndPosition_Empty_Reverse(opCtx(), recoveryUnit(), harnessHelper(), false, false);
}

void testSetEndPosition_Character_Limits(OperationContext* opCtx,
                                         RecoveryUnit& ru,
                                         SortedDataInterfaceHarnessHelper* harnessHelper,
                                         bool unique,
                                         bool inclusive) {
    auto sorted = harnessHelper->newSortedDataInterface(opCtx,
                                                        unique,
                                                        /*partial=*/false,
                                                        {
                                                            {key7, loc1},
                                                            {key8, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx, ru);
    cursor->setEndPosition(key7, inclusive);

    if (inclusive) {
        ASSERT_EQ(
            cursor->seek(ru,
                         makeKeyStringForSeek(sorted.get(), key7, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key7, loc1));
        ASSERT_EQ(cursor->next(ru), boost::none);
    } else {
        ASSERT_EQ(
            cursor->seek(ru,
                         makeKeyStringForSeek(sorted.get(), key7, true, true).finishAndGetBuffer()),
            boost::none);
    }

    cursor = sorted->newCursor(opCtx, ru);
    cursor->setEndPosition(key8, inclusive);

    if (inclusive) {
        ASSERT_EQ(
            cursor->seek(ru,
                         makeKeyStringForSeek(sorted.get(), key7, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key7, loc1));
        ASSERT_EQ(cursor->next(ru), IndexKeyEntry(key8, loc1));
        ASSERT_EQ(cursor->next(ru), boost::none);
    } else {
        ASSERT_EQ(
            cursor->seek(ru,
                         makeKeyStringForSeek(sorted.get(), key7, true, true).finishAndGetBuffer()),
            IndexKeyEntry(key7, loc1));
        ASSERT_EQ(cursor->next(ru), boost::none);
    }
}

TEST_F(SortedDataInterfaceTest, SetEndPosition_Character_Limits_Unique_Inclusive) {
    testSetEndPosition_Character_Limits(opCtx(), recoveryUnit(), harnessHelper(), true, true);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Character_Limits_Unique_Exclusive) {
    testSetEndPosition_Character_Limits(opCtx(), recoveryUnit(), harnessHelper(), true, false);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Character_Limits_Standard_Inclusive) {
    testSetEndPosition_Character_Limits(opCtx(), recoveryUnit(), harnessHelper(), false, true);
}
TEST_F(SortedDataInterfaceTest, SetEndPosition_Character_Limits_Standard_Exclusive) {
    testSetEndPosition_Character_Limits(opCtx(), recoveryUnit(), harnessHelper(), false, false);
}

}  // namespace
}  // namespace mongo
