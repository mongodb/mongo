/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/optime_list.h"
#include "mongo/stdx/thread.h"

/**
 * Test cases
 * - insert
 *   - to the end
 *   - to the head
 *   - to other pos
 * - erase
 *   - from the end
 *   - from the head
 *   - from other pos
 * - operation patterns
 *   - FIFO
 *   - LIFO
 *   - Exhaustive insertion
 *   - Exhaustive removal
 *   - front / back
 *   - eraseTrueIfFirst
 *   - concurrent access
 */
namespace mongo {
namespace repl {
namespace {
constexpr int maxNumElement = 128;

/**
 * FIFO tests
 */
void OpTimeListInsertTailFIFOTest(OpTimeList& list) {
    std::vector<std::list<Timestamp>::iterator> itors;

    for (int i = 0; i < maxNumElement; i++) {
        auto it = list.insert(list.end(), Timestamp(i, i + 1));
        ASSERT_FALSE(list.empty());
        itors.push_back(it);
    }

    // Compare list content
    auto ts = 0, count = 1;
    for (auto v : list.getVector_forTest()) {
        ASSERT_EQ(v, Timestamp(ts, count));
        ts++;
        count++;
    }

    // Verify front and back
    ASSERT_EQ(list.front(), Timestamp(0, 1));
    ASSERT_EQ(list.back(), Timestamp(127, 128));

    // Exhaustive removal FIFO
    ASSERT_EQ(itors[0], list.begin());
    for (auto it : itors) {
        ASSERT_FALSE(list.empty());
        list.erase(it);
    }
    ASSERT_TRUE(list.empty());
}

TEST(OpTimeList, OpTimeListFIFO) {
    OpTimeList list;

    OpTimeListInsertTailFIFOTest(list);
}

/**
 * LIFO tests
 */
void OpTimeListInsertHeadLIFOTest(OpTimeList& list) {
    std::vector<std::list<Timestamp>::iterator> itors;

    for (int i = 0; i < maxNumElement; i++) {
        auto it = list.insert(list.begin(), Timestamp(i, i + 1));
        ASSERT_FALSE(list.empty());
        itors.push_back(it);
    }

    // Compare list content
    auto ts = maxNumElement, count = ts + 1;
    for (auto v : list.getVector_forTest()) {
        ts--;
        count--;
        ASSERT_EQ(v, Timestamp(ts, count));
    }

    // Verify front and back
    ASSERT_EQ(list.front(), Timestamp(127, 128));
    ASSERT_EQ(list.back(), Timestamp(0, 1));

    // Exhaustive removal LIFO
    ASSERT_EQ(std::next(itors[0]), list.end());
    for (auto it : itors) {
        ASSERT_FALSE(list.empty());
        list.erase(it);
    }
    ASSERT_TRUE(list.empty());
}

TEST(OpTimeList, OpTimeListLIFO) {
    OpTimeList list;

    OpTimeListInsertHeadLIFOTest(list);
}

/**
 * Middle insert/erase test
 * Start from begin() and end() of the list and
 * interleavely add behind begin() and before end()
 * The resulted list contains [0, 2...126, 127...3, 1]
 */
void OpTimeListInsertEraseMiddle(OpTimeList& list) {
    ASSERT_TRUE(list.empty());
    auto it = list.end();
    for (int i = 0; i < maxNumElement; i += 2) {
        list.insert(it, Timestamp(i, i + 1));
        it = list.insert(it, Timestamp(i + 1, i + 2));
        ASSERT_FALSE(list.empty());
    }

    auto v = list.getVector_forTest();

    for (int i = 0; i < maxNumElement / 2; i++) {
        ASSERT_EQ(v[i].getInc() + v[i + maxNumElement / 2].getSecs(), maxNumElement);
    }

    // Verify front and back
    ASSERT_EQ(list.front(), Timestamp(0, 1));
    ASSERT_EQ(list.back(), Timestamp(1, 2));

    // Verify middle element
    ASSERT_EQ(*it, Timestamp(127, 128));
    auto itMid = it;
    it++;

    // Remove middle element and verify
    list.erase(itMid);
    ASSERT_EQ(*(--it), Timestamp(126, 127));

    // Check eraseTrueIfFirst
    auto firstIt = list.begin();
    ASSERT_TRUE(list.eraseTrueIfFirst(firstIt));
    ASSERT_FALSE(list.eraseTrueIfFirst(it));

    list.clear_forTest();
    ASSERT_TRUE(list.empty());
}

TEST(OpTimeList, OpTimeListInsertMiddle) {
    OpTimeList list;

    OpTimeListInsertEraseMiddle(list);
}

/**
 * Multiple insert/erase test
 */
void OpTimeListMultiAccess(OpTimeList& list) {
    AtomicWord<int> inserterCount{2};

    stdx::thread insertionThreadFIFO([&]() {
        std::vector<std::list<Timestamp>::iterator> itors;
        for (int i = 0; i < maxNumElement; i++) {
            auto it = list.insert(list.end(), Timestamp(i, i + 1));
            itors.push_back(it);
        }
        // memory barrier implied
        inserterCount.fetchAndSubtract(1);
    });

    stdx::thread insertionThreadLIFO([&]() {
        std::vector<std::list<Timestamp>::iterator> itors;
        for (int i = 0; i < maxNumElement; i++) {
            auto it = list.insert(list.begin(), Timestamp(i, i + 1));
            itors.push_back(it);
        }
        // memory barrier implied
        inserterCount.fetchAndSubtract(1);
    });

    // Single remover
    stdx::thread removalThread([&]() {
        while (inserterCount.load() > 0 || !list.empty()) {
            while (!list.empty()) {
                list.erase(list.begin());
            }
        }
        ASSERT_TRUE(list.empty());
    });
    insertionThreadFIFO.join();
    insertionThreadLIFO.join();
    removalThread.join();
}

TEST(OpTimeList, OpTimeListMultiAccess) {
    OpTimeList list;

    OpTimeListMultiAccess(list);
    ASSERT_TRUE(list.empty());
}
}  // namespace
}  // namespace repl
}  // namespace mongo
