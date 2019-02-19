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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/schema/json_pointer.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

template <class T>
void assertPointerEvaluatesTo(std::string pointerStr,
                              BSONObj toTest,
                              std::string testKey,
                              T testValue) {
    auto testObj = BSON(testKey << testValue);
    auto elt = testObj[testKey];
    auto pointer = JSONPointer(pointerStr);
    ASSERT_BSONELT_EQ(pointer.evaluate(toTest), elt);
}

TEST(JSONPointerTest, ParseInterestingCharacterFields) {
    BSONObj obj = BSON(
        "" << 1 << "c%d" << 2 << "e^f" << 3 << "g|h" << 4 << "i\\\\j" << 5 << "k\"l" << 6 << " "
           << 7);
    assertPointerEvaluatesTo("/", obj, "", 1);
    assertPointerEvaluatesTo("/c%d", obj, "c%d", 2);
    assertPointerEvaluatesTo("/e^f", obj, "e^f", 3);
    assertPointerEvaluatesTo("/g|h", obj, "g|h", 4);
    assertPointerEvaluatesTo("/i\\\\j", obj, "i\\\\j", 5);
    assertPointerEvaluatesTo("/k\"l", obj, "k\"l", 6);
    assertPointerEvaluatesTo("/ ", obj, " ", 7);
}

TEST(JSONPointerTest, EscapeCharacterParse) {
    BSONObj obj =
        BSON("a/b" << 1 << "m~n" << 2 << "o~/~p" << 3 << "~1" << 4 << "~" << 5 << "test~~/~" << 6);
    assertPointerEvaluatesTo("/a~1b", obj, "a/b", 1);
    assertPointerEvaluatesTo("/m~0n", obj, "m~n", 2);
    assertPointerEvaluatesTo("/o~0~1~0p", obj, "o~/~p", 3);
    assertPointerEvaluatesTo("/~01", obj, "~1", 4);
    assertPointerEvaluatesTo("/~000", obj, "~", 5);
    assertPointerEvaluatesTo("/test~00~0~1~00", obj, "test~~/~", 6);
}

TEST(JSONPointerTest, EmptyKeyTest) {
    BSONObj blankKeysObj = BSON("transit" << BSON("" << 1) << "" << BSON("" << 2));
    assertPointerEvaluatesTo("/transit/", blankKeysObj, "", 1);
    assertPointerEvaluatesTo("//", blankKeysObj, "", 2);
}

TEST(JSONPointerTest, MissingKeyReturnsEOO) {
    BSONObj obj = BSON("test" << 1);
    auto pointer = JSONPointer("/");
    ASSERT_FALSE(pointer.evaluate(obj));
    pointer = JSONPointer("/nonsense");
    ASSERT_FALSE(pointer.evaluate(obj));
    pointer = JSONPointer("/nonsense/");
    ASSERT_FALSE(pointer.evaluate(obj));
}

TEST(JSONPointerTest, InvalidPointerThrows) {
    ASSERT_THROWS_CODE(JSONPointer(""), AssertionException, 51064);
    ASSERT_THROWS_CODE(JSONPointer("random"), AssertionException, 51065);
    ASSERT_THROWS_CODE(JSONPointer("/random~"), AssertionException, 51063);
    ASSERT_THROWS_CODE(JSONPointer("/valid/ran~dom"), AssertionException, 51063);
    ASSERT_THROWS_CODE(JSONPointer("/ran~dom/valid"), AssertionException, 51063);
    ASSERT_THROWS_CODE(JSONPointer("/ran~0~dom/random"), AssertionException, 51063);
    ASSERT_THROWS_CODE(JSONPointer("/ran~1~dom/random"), AssertionException, 51063);
}

TEST(JSONPointerTest, NestedFields) {
    BSONObj bottomLevel = BSON("bottomLevel" << 1);
    BSONObj midLevel = BSON("midSide" << 4 << "transit" << bottomLevel);
    BSONObj topLevel = BSON("topSide" << 5 << "topObj" << midLevel);
    assertPointerEvaluatesTo("/topSide", topLevel, "topSide", 5);
    assertPointerEvaluatesTo("/topObj/midSide", topLevel, "midSide", 4);
    assertPointerEvaluatesTo("/topObj/transit/bottomLevel", topLevel, "bottomLevel", 1);
    auto pointer = JSONPointer("/topObj/notPresent/bottomLevel");
    ASSERT_FALSE(pointer.evaluate(topLevel));
    pointer = JSONPointer("/topObj/transit/bottomLevel/");
    ASSERT_FALSE(pointer.evaluate(topLevel));
}

TEST(JSONPointerTest, NestedFieldsWithEscapeCharacters) {
    BSONObj bottomLevel = BSON("bottomLevel" << 1 << "m~n" << 2);
    BSONObj topLevel = BSON("topObj" << BSON("esc/pe" << bottomLevel << "transit" << bottomLevel));
    assertPointerEvaluatesTo("/topObj/transit/m~0n", topLevel, "m~n", 2);
    assertPointerEvaluatesTo("/topObj/esc~1pe/m~0n", topLevel, "m~n", 2);
}

TEST(JSONPointerTest, ArrayTraversalTest) {
    auto arrBottom = BSON_ARRAY(0 << 1 << 2 << 3 << 4 << 5);
    auto arrTop = BSON_ARRAY(6 << 7 << 8 << 9 << 10);
    auto bsonArray = BSON_ARRAY(BSON("builder0"
                                     << "value0")
                                << BSON("builder1"
                                        << "value1")
                                << BSON("builder2"
                                        << "value2")
                                << BSON("builder3"
                                        << "value3"));
    auto topLevel =
        BSON("transit" << BSON("arrBottom" << arrBottom) << "arrTop" << arrTop << "toBSONArray"
                       << bsonArray);
    assertPointerEvaluatesTo("/transit/arrBottom/0", topLevel, "0", 0);
    assertPointerEvaluatesTo("/toBSONArray/0/builder0", topLevel, "builder0", "value0");
    assertPointerEvaluatesTo("/toBSONArray/3/builder3", topLevel, "builder3", "value3");
    assertPointerEvaluatesTo("/arrTop/0", topLevel, "0", 6);
}

TEST(JSONPointerTest, NumericFieldName) {
    auto topLevel = BSON("2"
                         << "text"
                         << "3"
                         << BSON_ARRAY("first"
                                       << "second"
                                       << "third"));
    assertPointerEvaluatesTo("/2", topLevel, "2", "text");
    assertPointerEvaluatesTo("/3/1", topLevel, "1", "second");
}

TEST(JSONPointerTest, DashCharacterBehavior) {
    auto topLevel =
        BSON("-" << 1 << "transit" << BSON("-" << 2) << "arr" << BSON_ARRAY(1 << 2 << 3 << 4 << 5));
    auto pointer = JSONPointer("/arr/-");
    ASSERT_FALSE(pointer.evaluate(topLevel));
    assertPointerEvaluatesTo("/-", topLevel, "-", 1);
    assertPointerEvaluatesTo("/transit/-", topLevel, "-", 2);
}

TEST(JSONPointerTest, ToStringParsesToSamePointer) {
    auto small = BSON("top" << BSON("fi~eld"
                                    << "second"));
    JSONPointer pointer{"/top/fi~0eld"};
    auto reclaimedString = pointer.toString();
    JSONPointer secondPointer{reclaimedString};
    ASSERT_BSONELT_EQ(pointer.evaluate(small), secondPointer.evaluate(small));
    ASSERT_EQ(reclaimedString, "/top/fi~0eld");
}

}  // namespace
}  // namespace mongo
