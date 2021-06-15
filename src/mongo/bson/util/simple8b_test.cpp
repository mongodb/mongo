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

void assertVectorsEqual(const std::vector<uint64_t>& actualVector,
                        const std::vector<uint64_t>& expectedVector) {
    ASSERT_EQ(actualVector.size(), expectedVector.size());

    for (size_t i = 0; i < actualVector.size(); ++i) {
        ASSERT_EQ(actualVector[i], expectedVector[i]);
    }
}

TEST(Simple8b, NoValues) {
    Simple8b s8b;

    std::vector<uint64_t> values = s8b.getAllInts();
    std::vector<uint64_t> expectedValues = {};

    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, OneValueTemporaryVector) {
    Simple8b s8b;

    ASSERT_TRUE(s8b.append(1));
    std::vector<uint64_t> values = s8b.getAllInts();
    std::vector<uint64_t> expectedValues = {1};

    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, MultipleValuesTemporaryVector) {
    Simple8b s8b;

    std::vector<uint64_t> expectedValues = {1, 2, 3};
    for (size_t i = 0; i < expectedValues.size(); ++i) {
        ASSERT_TRUE(s8b.append(expectedValues[i]));
    }
    std::vector<uint64_t> values = s8b.getAllInts();

    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, MaxValuesTemporaryVector) {
    Simple8b s8b;

    std::vector<uint64_t> expectedValues(60, 1);
    for (size_t i = 0; i < expectedValues.size(); ++i) {
        ASSERT_TRUE(s8b.append(expectedValues[i]));
    }
    std::vector<uint64_t> values = s8b.getAllInts();

    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, EncodeWithTrailingDirtyBits) {
    Simple8b s8b;

    std::vector<uint64_t> expectedValues(7, 1);
    for (size_t i = 0; i < expectedValues.size(); ++i) {
        ASSERT_TRUE(s8b.append(expectedValues[i]));
    }
    std::vector<uint64_t> values = s8b.getAllInts();

    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, FullBufferAndVector) {
    Simple8b s8b;

    std::vector<uint64_t> expectedValues(120, 1);
    for (size_t i = 0; i < expectedValues.size(); ++i) {
        ASSERT_TRUE(s8b.append(expectedValues[i]));
    }
    std::vector<uint64_t> values = s8b.getAllInts();

    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, TwoFullBuffersAndVector) {
    Simple8b s8b;

    std::vector<uint64_t> expectedValues(180, 1);
    for (size_t i = 0; i < expectedValues.size(); ++i) {
        ASSERT_TRUE(s8b.append(expectedValues[i]));
    }
    std::vector<uint64_t> values = s8b.getAllInts();

    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, BreakVectorIntoMultipleSimple8bBlocks) {
    Simple8b s8b;

    std::vector<uint64_t> expectedValues(58, 1);
    // 7 is 0b111 and can not be added to the current word because it would overflow.
    // We can not form a 58 bit word because we would be unable to determine
    // if the last 2 bits are empty or unused.
    // Therefore, we must form a word with 30 integers of 1's, 20 integers of 1's
    // and the current vector would have eight 1's and one 7.
    expectedValues.push_back(7);
    for (size_t i = 0; i < expectedValues.size(); ++i) {
        ASSERT_TRUE(s8b.append(expectedValues[i]));
    }

    std::vector<uint64_t> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}
