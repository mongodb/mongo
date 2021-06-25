/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <vector>

#include "mongo/bson/util/simple8b.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

// TODO: SERVER-58434 fix simple8b big endian and reenable tests.
/*
void assertVectorsEqual(const std::vector<Simple8b::Value>& actualVector,
                        const std::vector<Simple8b::Value>& expectedVector) {
    ASSERT_EQ(actualVector.size(), expectedVector.size());

    for (size_t i = 0; i < actualVector.size(); ++i) {
        ASSERT_EQ(actualVector[i].val, expectedVector[i].val);
        ASSERT_EQ(actualVector[i].index, expectedVector[i].index);
    }
}

TEST(Simple8b, NoValues) {
    Simple8b s8b;

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    std::vector<Simple8b::Value> expectedValues = {};

    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, OnlySkip) {
    Simple8b s8b;

    s8b.skip();
    std::vector<Simple8b::Value> values = s8b.getAllInts();
    std::vector<Simple8b::Value> expectedValues = {};

    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, OneValuePending) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts = {1};
    std::vector<Simple8b::Value> expectedValues;
    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, MaxValuePending) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts = {0xFFFFFFFFE};
    std::vector<Simple8b::Value> expectedValues;
    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, MultipleValuesPending) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts = {1, 2, 3};
    std::vector<Simple8b::Value> expectedValues;
    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, MaxValuesPending) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(60, 1);
    std::vector<Simple8b::Value> expectedValues;
    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, EncodeWithTrailingDirtyBits) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(7, 1);
    std::vector<Simple8b::Value> expectedValues;
    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, FullBufferAndPending) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(120, 1);
    std::vector<Simple8b::Value> expectedValues;
    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, MaxValueBuffer) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(3, 0xFFFFFFFFE);

    std::vector<Simple8b::Value> expectedValues;
    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, TrySomeSmallValues) {
    Simple8b s8b;

    std::vector<Simple8b::Value> expectedValues;
    for (size_t num = 0; num <= 0x0001FFFFF; ++num) {
        expectedValues.push_back({(uint32_t)num, num});
        ASSERT_TRUE(s8b.append(num));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, TrySomeLargeValues) {
    Simple8b s8b;

    std::vector<Simple8b::Value> expectedValues;
    for (size_t num = 0xF00000000; num <= 0xF001FFFFF; ++num) {
        expectedValues.push_back({(uint32_t)num, num});
        ASSERT_TRUE(s8b.append(num));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, TwoFullBuffersAndPending) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(180, 1);
    std::vector<Simple8b::Value> expectedValues;
    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, BreakPendingIntoMultipleSimple8bBlocks) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(58, 1);
    expectedInts.push_back(7);
    // 7 is 0b111 and can not be added to the current word because it would overflow.
    // We can not form a 58 bit word because we would be unable to determine
    // if the last 2 bits are empty or unused.
    // Therefore, we must form a word with 30 integers of 1's, 20 integers of 1's
    // and the current vector would have eight 1's and one 7.
    std::vector<Simple8b::Value> expectedValues;
    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, SkipInPending) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(3, 3);
    std::vector<Simple8b::Value> expectedValues;
    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    s8b.skip();
    int index = expectedInts.size() + 1;
    expectedValues.push_back({(uint32_t)index, 7});
    ASSERT_TRUE(s8b.append(expectedValues.back().val));

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, SkipInBuf) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(50, 1);
    std::vector<Simple8b::Value> expectedValues;
    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    s8b.skip();

    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)(i + expectedInts.size() + 1), expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, TrailingSkipsDoNotShowUp) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(48, 1);
    std::vector<Simple8b::Value> expectedValues;
    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    s8b.skip();
    s8b.skip();

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, LeadingSkips) {
    Simple8b s8b;

    int numSkips = 2;
    s8b.skip();
    s8b.skip();

    std::vector<uint64_t> expectedInts = {3, 8, 13};
    std::vector<Simple8b::Value> expectedValues;
    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i + numSkips, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, WordOfSkips) {
    Simple8b s8b;

    int numSkips = 30;
    for (int i = 0; i < numSkips; ++i)
        s8b.skip();

    uint64_t numWithMoreThanThirtyBits = 1ull << 30;
    std::vector<uint64_t> expectedInts = {numWithMoreThanThirtyBits};
    std::vector<Simple8b::Value> expectedValues;
    expectedValues.push_back({(uint32_t)numSkips, numWithMoreThanThirtyBits});
    ASSERT_TRUE(s8b.append(numWithMoreThanThirtyBits));

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}
*/
