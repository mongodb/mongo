/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/util/string_util.h"

#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo::query_string_util {

void assertLevenshteinDistanceBetweenStrings(const std::string s1,
                                             const std::string s2,
                                             unsigned int expectedDistance) {
    ASSERT_EQ(levenshteinDistance(s1, s2), expectedDistance);
}

TEST(LevenshteinDistanceTest, EmptyStrings) {
    assertLevenshteinDistanceBetweenStrings("", "", 0);
}

TEST(LevenshteinDistanceTest, EqualString) {
    assertLevenshteinDistanceBetweenStrings("foo", "foo", 0);
}

TEST(LevenshteinDistanceTest, OneStringEmpty) {
    assertLevenshteinDistanceBetweenStrings("foo", "", 3);
    assertLevenshteinDistanceBetweenStrings("", "bar", 3);
}


TEST(LevenshteinDistanceTest, UnequalNonEmptyStrings) {
    assertLevenshteinDistanceBetweenStrings("cat", "cut", 1);
    assertLevenshteinDistanceBetweenStrings("hat", "bat", 1);
    assertLevenshteinDistanceBetweenStrings("hat", "ha", 1);
    assertLevenshteinDistanceBetweenStrings("at", "hat", 1);
    assertLevenshteinDistanceBetweenStrings("h", "ros", 3);
    assertLevenshteinDistanceBetweenStrings("horse", "ros", 3);
    assertLevenshteinDistanceBetweenStrings("kitten", "sitting", 3);
    assertLevenshteinDistanceBetweenStrings("intention", "execution", 5);
    assertLevenshteinDistanceBetweenStrings("supercalifragilisticexpialidocious", "fragile", 27);
}

}  // namespace mongo::query_string_util
