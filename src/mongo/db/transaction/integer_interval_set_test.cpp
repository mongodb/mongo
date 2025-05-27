/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/transaction/integer_interval_set.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
using unittest::match::ElementsAre;
using unittest::match::Eq;

typedef std::pair<int, int> P;

TEST(IntegerIntervalSet, Inserts) {
    IntegerIntervalSet<int> testSet;
    // Insert into empty set
    {
        auto [iter, inserted] = testSet.insert(20);
        ASSERT_EQ(inserted, true);
        ASSERT_EQ(*iter, (P{20, 20}));
        ASSERT_THAT(testSet, ElementsAre(Eq(P{20, 20})));
    }

    // Insert value not part of any range after all current ranges.
    {
        auto [iter, inserted] = testSet.insert(40);
        ASSERT_EQ(inserted, true);
        ASSERT_EQ(*iter, (P{40, 40}));
        ASSERT_THAT(testSet, ElementsAre(Eq(P{20, 20}), Eq(P{40, 40})));
    }

    // Insert value not part of any range before all current ranges.
    {
        auto [iter, inserted] = testSet.insert(10);
        ASSERT_EQ(inserted, true);
        ASSERT_EQ(*iter, (P{10, 10}));
        ASSERT_THAT(testSet, ElementsAre(Eq(P{10, 10}), Eq(P{20, 20}), Eq(P{40, 40})));
    }

    // Insert value not part of any range between current ranges.
    {
        auto [iter, inserted] = testSet.insert(23);
        ASSERT_EQ(inserted, true);
        ASSERT_EQ(*iter, (P{23, 23}));
        ASSERT_THAT(testSet,
                    ElementsAre(Eq(P{10, 10}), Eq(P{20, 20}), Eq(P{23, 23}), Eq(P{40, 40})));
    }

    // Insert value at start of first range
    {
        auto [iter, inserted] = testSet.insert(9);
        ASSERT_EQ(inserted, true);
        ASSERT_EQ(*iter, (P{9, 10}));
        ASSERT_THAT(testSet,
                    ElementsAre(Eq(P{9, 10}), Eq(P{20, 20}), Eq(P{23, 23}), Eq(P{40, 40})));
    }

    // Insert value at end of last range
    {
        auto [iter, inserted] = testSet.insert(41);
        ASSERT_EQ(inserted, true);
        ASSERT_EQ(*iter, (P{40, 41}));
        ASSERT_THAT(testSet,
                    ElementsAre(Eq(P{9, 10}), Eq(P{20, 20}), Eq(P{23, 23}), Eq(P{40, 41})));
    }

    // Insert value at beginning of a middle range
    {
        auto [iter, inserted] = testSet.insert(19);
        ASSERT_EQ(inserted, true);
        ASSERT_EQ(*iter, (P{19, 20}));
        ASSERT_THAT(testSet,
                    ElementsAre(Eq(P{9, 10}), Eq(P{19, 20}), Eq(P{23, 23}), Eq(P{40, 41})));
    }

    // Insert value at end of a middle range
    {
        auto [iter, inserted] = testSet.insert(21);
        ASSERT_EQ(inserted, true);
        ASSERT_EQ(*iter, (P{19, 21}));
        ASSERT_THAT(testSet,
                    ElementsAre(Eq(P{9, 10}), Eq(P{19, 21}), Eq(P{23, 23}), Eq(P{40, 41})));
    }

    // Insert value which will cause ranges to coalesce.
    {
        auto [iter, inserted] = testSet.insert(22);
        ASSERT_EQ(inserted, true);
        ASSERT_EQ(*iter, (P{19, 23}));
        ASSERT_THAT(testSet, ElementsAre(Eq(P{9, 10}), Eq(P{19, 23}), Eq(P{40, 41})));
    }

    // Insert value which will cause ranges to coalesce at end.
    {
        auto [iter0, inserted0] = testSet.insert(38);
        ASSERT_EQ(inserted0, true);
        ASSERT_EQ(*iter0, (P{38, 38}));

        auto [iter, inserted] = testSet.insert(39);
        ASSERT_EQ(inserted, true);
        ASSERT_EQ(*iter, (P{38, 41}));
        ASSERT_THAT(testSet, ElementsAre(Eq(P{9, 10}), Eq(P{19, 23}), Eq(P{38, 41})));
    }

    // Insert value which will cause ranges to coalesce at start.
    {
        auto [iter0, inserted0] = testSet.insert(7);
        ASSERT_EQ(inserted0, true);
        ASSERT_EQ(*iter0, (P{7, 7}));

        auto [iter, inserted] = testSet.insert(8);
        ASSERT_EQ(inserted, true);
        ASSERT_EQ(*iter, (P{7, 10}));
        ASSERT_THAT(testSet, ElementsAre(Eq(P{7, 10}), Eq(P{19, 23}), Eq(P{38, 41})));
    }
    // Check for inserts of existing values at beginning, middle, and end of beginning, middle,
    // and end ranges.
    {
        auto [iter, inserted] = testSet.insert(7);
        ASSERT_EQ(inserted, false);
        ASSERT_EQ(*iter, (P{7, 10}));
    }
    {
        auto [iter, inserted] = testSet.insert(8);
        ASSERT_EQ(inserted, false);
        ASSERT_EQ(*iter, (P{7, 10}));
    }
    {
        auto [iter, inserted] = testSet.insert(10);
        ASSERT_EQ(inserted, false);
        ASSERT_EQ(*iter, (P{7, 10}));
    }
    {
        auto [iter, inserted] = testSet.insert(19);
        ASSERT_EQ(inserted, false);
        ASSERT_EQ(*iter, (P{19, 23}));
    }
    {
        auto [iter, inserted] = testSet.insert(21);
        ASSERT_EQ(inserted, false);
        ASSERT_EQ(*iter, (P{19, 23}));
    }
    {
        auto [iter, inserted] = testSet.insert(23);
        ASSERT_EQ(inserted, false);
        ASSERT_EQ(*iter, (P{19, 23}));
    }
    {
        auto [iter, inserted] = testSet.insert(38);
        ASSERT_EQ(inserted, false);
        ASSERT_EQ(*iter, (P{38, 41}));
    }
    {
        auto [iter, inserted] = testSet.insert(40);
        ASSERT_EQ(inserted, false);
        ASSERT_EQ(*iter, (P{38, 41}));
    }
    {
        auto [iter, inserted] = testSet.insert(41);
        ASSERT_EQ(inserted, false);
        ASSERT_EQ(*iter, (P{38, 41}));
    }
}

TEST(IntegerIntervalSet, Erases) {
    IntegerIntervalSet<int> testSet;
    // Erase from empty set.
    ASSERT_EQ(0, testSet.erase(10));
    ASSERT_TRUE(testSet.empty());

    // Erase only element from set, expect it to become empty.
    testSet.insert(10);
    ASSERT_EQ(1, testSet.erase(10));
    ASSERT_TRUE(testSet.empty());

    testSet.insert(30);
    testSet.insert(31);
    testSet.insert(32);
    testSet.insert(33);
    testSet.insert(34);
    testSet.insert(35);
    testSet.insert(36);

    // Erase from start of interval
    ASSERT_EQ(1, testSet.erase(30));
    ASSERT_THAT(testSet, ElementsAre(Eq(P{31, 36})));

    // Erase again should fail.
    ASSERT_EQ(0, testSet.erase(30));

    // Erase from end of interval
    ASSERT_EQ(1, testSet.erase(36));
    ASSERT_THAT(testSet, ElementsAre(Eq(P{31, 35})));

    // Erase again should fail.
    ASSERT_EQ(0, testSet.erase(36));

    // Erase from middle of interval
    ASSERT_EQ(1, testSet.erase(33));
    ASSERT_THAT(testSet, ElementsAre(Eq(P{31, 32}), Eq(P{34, 35})));

    // Erase again should fail.
    ASSERT_EQ(0, testSet.erase(33));
}

TEST(IntegerIntervalSet, Clear) {
    IntegerIntervalSet<int> testSet;
    // Clear an empty set
    testSet.clear();
    ASSERT_TRUE(testSet.empty());
    ASSERT_EQ(testSet.size(), 0);

    // Clear a one-interval set
    testSet.insert(10);
    ASSERT_FALSE(testSet.empty());
    ASSERT_EQ(testSet.size(), 1);
    testSet.clear();
    ASSERT_TRUE(testSet.empty());
    ASSERT_EQ(testSet.size(), 0);

    // Clear a multiple-interval set
    testSet.insert(10);
    testSet.insert(20);
    testSet.insert(21);
    testSet.insert(30);
    testSet.insert(31);
    testSet.insert(32);
    testSet.insert(33);
    testSet.insert(34);
    testSet.insert(40);
    ASSERT_FALSE(testSet.empty());
    ASSERT_EQ(testSet.size(), 4);
    testSet.clear();
    ASSERT_TRUE(testSet.empty());
    ASSERT_EQ(testSet.size(), 0);
}

TEST(IntegerIntervalSet, MoveAndCopy) {
    IntegerIntervalSet<int> testSet;
    testSet.insert(10);
    testSet.insert(20);
    testSet.insert(21);
    testSet.insert(30);
    testSet.insert(31);
    testSet.insert(32);
    testSet.insert(33);
    testSet.insert(34);
    testSet.insert(40);

    IntegerIntervalSet<int> moveSet1(std::move(testSet));
    ASSERT_THAT(moveSet1, ElementsAre(Eq(P{10, 10}), Eq(P{20, 21}), Eq(P{30, 34}), Eq(P{40, 40})));

    IntegerIntervalSet<int> moveSet2;
    moveSet2.insert(50);
    moveSet2 = std::move(moveSet1);
    ASSERT_THAT(moveSet2, ElementsAre(Eq(P{10, 10}), Eq(P{20, 21}), Eq(P{30, 34}), Eq(P{40, 40})));

    IntegerIntervalSet<int> copySet1(moveSet2);
    ASSERT_THAT(moveSet2, ElementsAre(Eq(P{10, 10}), Eq(P{20, 21}), Eq(P{30, 34}), Eq(P{40, 40})));
    ASSERT_THAT(copySet1, ElementsAre(Eq(P{10, 10}), Eq(P{20, 21}), Eq(P{30, 34}), Eq(P{40, 40})));
    copySet1.erase(10);
    copySet1.erase(21);
    copySet1.insert(39);
    copySet1.erase(33);
    // Copied-from set better not have changed.
    ASSERT_THAT(moveSet2, ElementsAre(Eq(P{10, 10}), Eq(P{20, 21}), Eq(P{30, 34}), Eq(P{40, 40})));
    ASSERT_THAT(copySet1, ElementsAre(Eq(P{20, 20}), Eq(P{30, 32}), Eq(P{34, 34}), Eq(P{39, 40})));

    IntegerIntervalSet<int> copySet2;
    copySet2.insert(90);
    copySet2 = moveSet2;
    ASSERT_THAT(moveSet2, ElementsAre(Eq(P{10, 10}), Eq(P{20, 21}), Eq(P{30, 34}), Eq(P{40, 40})));
    ASSERT_THAT(copySet2, ElementsAre(Eq(P{10, 10}), Eq(P{20, 21}), Eq(P{30, 34}), Eq(P{40, 40})));
    copySet2.erase(40);
    copySet2.erase(20);
    copySet2.insert(11);
    copySet2.erase(31);
    // Copied-from set better not have changed.
    ASSERT_THAT(moveSet2, ElementsAre(Eq(P{10, 10}), Eq(P{20, 21}), Eq(P{30, 34}), Eq(P{40, 40})));
    ASSERT_THAT(copySet2, ElementsAre(Eq(P{10, 11}), Eq(P{21, 21}), Eq(P{30, 30}), Eq(P{32, 34})));
}

}  // namespace mongo
