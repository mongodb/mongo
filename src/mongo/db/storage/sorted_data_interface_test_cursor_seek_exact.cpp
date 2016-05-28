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
// Tests seekExact when it hits something.
void testSeekExact_Hit(bool unique, bool forward) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted =
        harnessHelper->newSortedDataInterface(unique,
                                              {
                                                  {key1, loc1}, {key2, loc1}, {key3, loc1},
                                              });

    auto cursor = sorted->newCursor(opCtx.get(), forward);

    ASSERT_EQ(cursor->seekExact(key2), IndexKeyEntry(key2, loc1));

    // Make sure iterating works. We may consider loosening this requirement if it is a hardship
    // for some storage engines.
    ASSERT_EQ(cursor->next(), IndexKeyEntry(forward ? key3 : key1, loc1));
    ASSERT_EQ(cursor->next(), boost::none);
}
TEST(SortedDataInterface, SeekExact_Hit_Unique_Forward) {
    testSeekExact_Hit(true, true);
}
TEST(SortedDataInterface, SeekExact_Hit_Unique_Reverse) {
    testSeekExact_Hit(true, false);
}
TEST(SortedDataInterface, SeekExact_Hit_Standard_Forward) {
    testSeekExact_Hit(false, true);
}
TEST(SortedDataInterface, SeekExact_Hit_Standard_Reverse) {
    testSeekExact_Hit(false, false);
}

// Tests seekExact when it doesn't hit the query.
void testSeekExact_Miss(bool unique, bool forward) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(unique,
                                                        {
                                                            {key1, loc1},
                                                            // No key2.
                                                            {key3, loc1},
                                                        });

    auto cursor = sorted->newCursor(opCtx.get(), forward);

    ASSERT_EQ(cursor->seekExact(key2), boost::none);

    // Not testing iteration since the cursors position following a failed seekExact is
    // undefined. However, you must be able to seek somewhere else.
    ASSERT_EQ(cursor->seekExact(key1), IndexKeyEntry(key1, loc1));
}
TEST(SortedDataInterface, SeekExact_Miss_Unique_Forward) {
    testSeekExact_Miss(true, true);
}
TEST(SortedDataInterface, SeekExact_Miss_Unique_Reverse) {
    testSeekExact_Miss(true, false);
}
TEST(SortedDataInterface, SeekExact_Miss_Standard_Forward) {
    testSeekExact_Miss(false, true);
}
TEST(SortedDataInterface, SeekExact_Miss_Standard_Reverse) {
    testSeekExact_Miss(false, false);
}

// Tests seekExact on forward cursor when it hits something with dup keys. Doesn't make sense
// for unique indexes.
TEST(SortedDataInterface, SeekExact_HitWithDups_Forward) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(
        false,
        {
            {key1, loc1}, {key2, loc1}, {key2, loc2}, {key3, loc1},
        });

    auto cursor = sorted->newCursor(opCtx.get());

    ASSERT_EQ(cursor->seekExact(key2), IndexKeyEntry(key2, loc1));
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc2));
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key3, loc1));
    ASSERT_EQ(cursor->next(), boost::none);
}

// Tests seekExact on reverse cursor when it hits something with dup keys. Doesn't make sense
// for unique indexes.
TEST(SortedDataInterface, SeekExact_HitWithDups_Reverse) {
    auto harnessHelper = newHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(
        false,
        {
            {key1, loc1}, {key2, loc1}, {key2, loc2}, {key3, loc1},
        });

    auto cursor = sorted->newCursor(opCtx.get(), false);

    ASSERT_EQ(cursor->seekExact(key2), IndexKeyEntry(key2, loc2));
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key2, loc1));
    ASSERT_EQ(cursor->next(), IndexKeyEntry(key1, loc1));
    ASSERT_EQ(cursor->next(), boost::none);
}
}  // namespace mongo
