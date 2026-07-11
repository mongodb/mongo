// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/slotted_timestamp_list.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

namespace {

/**
 * Runs several asserts to check the list: correct size, check if empty, correct values, correct
 * front and back, correct capacity.
 */
void checkListValues(const SlottedTimestampList& list, std::vector<std::size_t> expected) {
    auto vec = list.getVector_forTest();
    ASSERT_EQ(expected.size(), list.size());
    ASSERT_EQ(expected.size(), vec.size());
    ASSERT_EQ(expected.size() == 0, list.empty());

    for (std::size_t i = 0; i < vec.size(); i++) {
        ASSERT_EQ(vec[i].asULL(), expected[i]);
    }

    if (expected.size() != 0) {
        ASSERT_EQ(list.front().asULL(), expected.front());
        ASSERT_EQ(list.back().asULL(), expected.back());
    }

    ASSERT_GTE(list.getCapacity_forTest(), expected.size());
}

/**
 * Inserts n timestamps with values starting at the startValue and incrementing by 1.
 * Also asserts to check that the const_iterator returned by insert() points to
 * the timestamp with the correct value. Adds to positions vector after each insert.
 * Precondition: startValue has to be unique because all Timestamps are unique
 */
void insertN(SlottedTimestampList& list,
             size_t startValue,
             int N,
             std::vector<std::list<Timestamp>::const_iterator>& positions) {

    for (int i = 0; i < N; i++) {
        auto it = list.insert(Timestamp(startValue + i));
        positions.push_back(it);
        ASSERT_EQ(*it, Timestamp(startValue + i));
    }
}

/**
 * Erases n timestamps starting at index startIndex. Also erases corresponding const_iterator
 * from the positions vector.
 */
void eraseN(SlottedTimestampList& list,
            int startIndex,
            int N,
            std::vector<std::list<Timestamp>::const_iterator>& positions) {

    for (int i = 0; i < N; i++) {
        list.erase(positions[startIndex]);
        positions.erase(positions.begin() + startIndex);
    }
}

}  // namespace

DEATH_TEST(SlottedTimestampListTestDeathTest, InvariantRequiresNonEmptyListForFront, "invariant") {
    SlottedTimestampList list;
    list.front();
}

DEATH_TEST(SlottedTimestampListTestDeathTest, InvariantRequiresNonEmptyListForBack, "invariant") {
    SlottedTimestampList list;
    list.back();
}

TEST(SlottedTimestampList, EraseMiddleAndReuseFreeSlots) {
    SlottedTimestampList list;
    std::vector<std::list<Timestamp>::const_iterator> positions;

    insertN(list, 0 /*startValue*/, 5 /*N*/, positions);
    checkListValues(list, {0, 1, 2, 3, 4});

    ASSERT_EQ(list.getCapacity_forTest(), 5);

    eraseN(list, 1 /*startIndex*/, 2 /*N*/, positions);
    checkListValues(list, {0, 3, 4});

    ASSERT_EQ(list.getCapacity_forTest(), 5);

    insertN(list, 10 /*startValue*/, 5 /*N*/, positions);
    checkListValues(list, {0, 3, 4, 10, 11, 12, 13, 14});

    ASSERT_EQ(list.getCapacity_forTest(), 8);
}

TEST(SlottedTimestampList, ClearAndEraseTimestamps) {
    SlottedTimestampList list;
    std::vector<std::list<Timestamp>::const_iterator> positions;

    insertN(list, 0 /*startValue*/, 5 /*N*/, positions);
    checkListValues(list, {0, 1, 2, 3, 4});

    ASSERT_EQ(list.getCapacity_forTest(), 5);

    list.clear();
    checkListValues(list, {});

    ASSERT_EQ(list.getCapacity_forTest(), 5);

    insertN(list, 0 /*startValue*/, 3 /*N*/, positions);
    checkListValues(list, {0, 1, 2});

    ASSERT_EQ(list.getCapacity_forTest(), 5);

    eraseN(list, 1 /*startIndex*/, 2 /*N*/, positions);
    checkListValues(list, {0});

    ASSERT_EQ(list.getCapacity_forTest(), 5);

    insertN(list, 20 /*startValue*/, 6 /*N*/, positions);
    checkListValues(list, {0, 20, 21, 22, 23, 24, 25});

    ASSERT_EQ(list.getCapacity_forTest(), 7);
}

TEST(SlottedTimestampListTest, EmptyList) {
    SlottedTimestampList list;

    checkListValues(list, {});
    ASSERT_EQ(list.getCapacity_forTest(), 0);

    list.clear();
    checkListValues(list, {});
    ASSERT_EQ(list.getCapacity_forTest(), 0);
}

TEST(SlottedTimestampListTest, EraseSingleAndReuseSlot) {
    SlottedTimestampList list;

    auto it = list.insert(Timestamp(0));
    ASSERT_EQ(it->asULL(), 0);
    checkListValues(list, {0});
    ASSERT_EQ(list.getCapacity_forTest(), 1);

    list.erase(it);
    checkListValues(list, {});
    ASSERT_EQ(list.getCapacity_forTest(), 1);

    it = list.insert(Timestamp(1));
    checkListValues(list, {1});
    ASSERT_EQ(list.getCapacity_forTest(), 1);

    it = list.insert(Timestamp(2));
    checkListValues(list, {1, 2});
    ASSERT_EQ(list.getCapacity_forTest(), 2);
}

TEST(SlottedTimestampList, EraseAllFIFOAndReuseSlots) {
    SlottedTimestampList list;
    std::vector<SlottedTimestampList::const_iterator> iters;

    insertN(list, 0 /* startValue */, 5 /* N */, iters);
    checkListValues(list, {0, 1, 2, 3, 4});
    ASSERT_EQ(list.getCapacity_forTest(), 5);

    eraseN(list, 0 /* startIndex */, 5 /* N */, iters);
    checkListValues(list, {});
    ASSERT_EQ(list.getCapacity_forTest(), 5);

    insertN(list, 5 /* startValue */, 5 /* N */, iters);
    checkListValues(list, {5, 6, 7, 8, 9});
    ASSERT_EQ(list.getCapacity_forTest(), 5);

    auto it = list.insert(Timestamp(10));
    ASSERT_EQ(it->asULL(), 10);
    ASSERT_EQ(list.getCapacity_forTest(), 6);
}

TEST(SlottedTimestampList, EraseAllLIFOAndReuseSlots) {
    SlottedTimestampList list;
    std::vector<SlottedTimestampList::const_iterator> iters;

    insertN(list, 0 /* startValue */, 5 /* N */, iters);
    checkListValues(list, {0, 1, 2, 3, 4});
    ASSERT_EQ(list.getCapacity_forTest(), 5);

    std::reverse(iters.begin(), iters.end());
    eraseN(list, 0 /* startIndex */, 5 /* N */, iters);
    checkListValues(list, {});
    ASSERT_EQ(list.getCapacity_forTest(), 5);

    insertN(list, 5 /* startValue */, 5 /* N */, iters);
    checkListValues(list, {5, 6, 7, 8, 9});
    ASSERT_EQ(list.getCapacity_forTest(), 5);

    auto it = list.insert(Timestamp(10));
    ASSERT_EQ(it->asULL(), 10);
    ASSERT_EQ(list.getCapacity_forTest(), 6);
}

TEST(SlottedTimestampList, EraseHalfFIFOHalfLIFOAndResuseSlots) {
    SlottedTimestampList list;
    std::vector<SlottedTimestampList::const_iterator> iters;

    insertN(list, 0 /* startValue */, 5 /* N */, iters);
    checkListValues(list, {0, 1, 2, 3, 4});
    ASSERT_EQ(list.getCapacity_forTest(), 5);

    eraseN(list, 0 /* startIndex */, 3 /* N */, iters);
    checkListValues(list, {3, 4});

    std::reverse(iters.begin(), iters.end());
    eraseN(list, 0 /* startIndex */, 2 /* N */, iters);
    checkListValues(list, {});
    ASSERT_EQ(list.getCapacity_forTest(), 5);

    insertN(list, 5 /* startValue */, 5 /* N */, iters);
    checkListValues(list, {5, 6, 7, 8, 9});
    ASSERT_EQ(list.getCapacity_forTest(), 5);

    auto it = list.insert(Timestamp(10));
    ASSERT_EQ(it->asULL(), 10);
    ASSERT_EQ(list.getCapacity_forTest(), 6);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
