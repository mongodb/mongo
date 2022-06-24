/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

/**
 * Tests for columnar/SBE integration.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/values/columnar.h"
#include "mongo/db/storage/column_store.h"

namespace mongo::sbe {
void makeObjFromColumns(std::vector<MockTranslatedCell>& cells, value::Object& out) {
    for (auto& cell : cells) {
        addCellToObject(cell, out);
    }
}

void compareMakeObjWithExpected(std::vector<MockTranslatedCell>& cells, const BSONObj& expected) {
    auto [expectedTag, expectedVal] = stage_builder::makeValue(expected);
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    value::Object result;
    makeObjFromColumns(cells, result);
    PlanStageTestFixture::assertValuesEqual(value::TypeTags::Object,
                                            value::bitcastFrom<value::Object*>(&result),
                                            expectedTag,
                                            expectedVal);
}

MockTranslatedCell makeCellOfIntegers(StringData path,
                                      StringData arrInfo,
                                      std::vector<unsigned int> vals) {
    return MockTranslatedCell{
        arrInfo,
        path,
        std::vector<value::TypeTags>(vals.size(), value::TypeTags::NumberInt32),
        std::vector<value::Value>(vals.begin(), vals.end())};
}

TEST(ColumnarObjTest, MakeObjNoArrTest) {
    std::vector<MockTranslatedCell> cells{makeCellOfIntegers("a.b", "", {32}),
                                          makeCellOfIntegers("a.d", "", {36})};
    compareMakeObjWithExpected(cells, fromjson("{a: {b: 32, d: 36}}"));
}

TEST(ColumnarObjTest, MakeObjArrOfScalarsTest) {
    std::vector<MockTranslatedCell> cells{makeCellOfIntegers("a", "[", {32, 33, 34, 35, 36})};
    compareMakeObjWithExpected(cells, fromjson("{a: [32, 33, 34, 35, 36]}"));
}

TEST(ColumnarObjTest, MakeObjSingletonArrayOfObj) {
    std::vector<MockTranslatedCell> cells{makeCellOfIntegers("a.b", "[", {32})};
    compareMakeObjWithExpected(cells, fromjson("{a: [{b:32}]}"));
}

TEST(ColumnarObjTest, MakeObjArrOfObjectsTest) {
    std::vector<MockTranslatedCell> cells{makeCellOfIntegers("a.b", "[", {1, 2, 3})};

    compareMakeObjWithExpected(
        cells, BSON("a" << BSON_ARRAY(BSON("b" << 1) << BSON("b" << 2) << BSON("b" << 3))));
}

TEST(ColumnarObjTest, MakeObjBasicArrOfObjectsWithMultipleFields) {
    std::vector<MockTranslatedCell> cells{makeCellOfIntegers("a.b", "[", {1, 2, 3}),
                                          makeCellOfIntegers("a.c", "[", {101, 102, 103})};
    compareMakeObjWithExpected(cells,
                               fromjson("{a: [{b:1, c:101}, {b:2, c: 102}, {b:3, c: 103}]}"));
}

TEST(ColumnarObjTest, MakeObjComplexLeafArray) {
    std::vector<MockTranslatedCell> cells{makeCellOfIntegers("a", "", {}),
                                          makeCellOfIntegers("a.b", "", {}),
                                          makeCellOfIntegers("a.b.c", "{{[[|1][|]", {1, 2, 3, 4})};
    compareMakeObjWithExpected(cells, fromjson("{a:{b:{c:[[1,2],[3],4]}}}"));
}

TEST(ColumnarObjTest, MakeObjArrayOfArrays) {
    std::vector<MockTranslatedCell> cells{makeCellOfIntegers("a", "[[|1][[|]", {1, 2, 3, 4})};
    compareMakeObjWithExpected(cells, fromjson("{a: [[1,2], [[3], 4]]}"));
}

TEST(ColumnarObjTest, MakeObjArrayOfMixed) {
    std::vector<MockTranslatedCell> cells{makeCellOfIntegers("a", "[|+2", {1, 4}),
                                          makeCellOfIntegers("a.b", "[1", {2}),
                                          makeCellOfIntegers("a.c", "[2", {3})};
    compareMakeObjWithExpected(cells, fromjson("{a: [1, {b:2}, {c: 3}, 4]}"));
}

TEST(ColumnarObjTest, MakeObjArrayOfMixed2) {
    std::vector<MockTranslatedCell> cells{makeCellOfIntegers("a", "[|o", {1, 3}),
                                          makeCellOfIntegers("a.b", "[1{[", {2})};
    compareMakeObjWithExpected(cells, fromjson("{a:[1,{b:[2]},3]}"));
}

TEST(ColumnarObjTest, MakeObjTopLevelArrayOfMixed) {
    std::vector<MockTranslatedCell> cells{
        makeCellOfIntegers("a", "[o1|o5", {99}),
        makeCellOfIntegers("a.b", "[{[|3]+2{[[|]]{[", {101, 0, 101, 1, 2, 3, 4, 5, 6}),
        makeCellOfIntegers("a.c", "[1|+1", {0, 1}),
        makeCellOfIntegers("a.d", "[5", {0, 1, 2, 3})};
    compareMakeObjWithExpected(cells,
                               fromjson("{a: [{b: [101, 0, 101, 1]},"
                                        "{c: 0},"
                                        "99,"
                                        "{b: [[2]], c: 1},"
                                        "{b: [3, 4, 5, 6]},"
                                        "{d: 0},"
                                        "{d: 1},"
                                        "{d: 2},"
                                        "{d: 3}"
                                        "]}"));
}

TEST(ColumnarObjTest, MakeObjTopLevelObjWithMixedArrays) {
    std::vector<MockTranslatedCell> cells{
        makeCellOfIntegers("a", "", {}),
        makeCellOfIntegers("a.b", "{[[|1][o]", {1, 2, 2}),
        makeCellOfIntegers("a.b.c", "{[1[{[[|1][|]", {1, 2, 99, 2}),
        makeCellOfIntegers("a.x", "", {1})};
    compareMakeObjWithExpected(cells, fromjson("{a:{b:[[1,2],[{c:[[1,2],[99],2]}],2], x:1}}"));
}

TEST(ColumnarObjTest, MakeObjTopLevelArrayWithSubArrays) {
    std::vector<MockTranslatedCell> cells{
        makeCellOfIntegers("a", "[[|1][o]", {1, 2, 2}),
        makeCellOfIntegers("a.b", "[1[{[[|1][o]", {1, 2, 2}),
        makeCellOfIntegers("a.b.c", "[1[{[1[{[[|1][|]", {1, 2, 3, 4})};
    compareMakeObjWithExpected(cells,
                               fromjson("{a:[[1,2],[{b:[[1,2],[{c:[[1,2],[3],4]}],2]}],2]}"));
}

TEST(ColumnarObjTest, MakeTopLevelArrayOfObjsSparse) {
    std::vector<MockTranslatedCell> cells{makeCellOfIntegers("a", "[o1", {}),
                                          makeCellOfIntegers("a.b", "[o", {1}),
                                          makeCellOfIntegers("a.b.c", "[", {1})};
    compareMakeObjWithExpected(cells, fromjson("{a:[{b:{c:1}},{b:1}]}"));
}

TEST(ColumnarObjTest, MakeObjEmptyTopLevelField) {
    std::vector<MockTranslatedCell> cells{makeCellOfIntegers("", "[", {1, 2, 3})};
    compareMakeObjWithExpected(cells, fromjson("{'': [1,2,3]}"));
}

TEST(ColumnarObjTest, MakeObjEmptyFieldWhichIsObject) {
    std::vector<MockTranslatedCell> cells{makeCellOfIntegers("..a", "{{[", {1, 2, 3})};
    compareMakeObjWithExpected(cells, fromjson("{'': {'': {a: [1,2,3]}}}"));
}

TEST(ColumnarObjTest, MakeObjEmptyFieldWhichIsLeaf) {
    std::vector<MockTranslatedCell> cells{makeCellOfIntegers("a.", "{[", {1, 2, 3})};
    compareMakeObjWithExpected(cells, fromjson("{a: {'': [1,2,3]}}"));
}

TEST(ColumnarObjTest, AddTopLevelNonLeafCellWithoutArrayInfoToObject) {
    // Cell with no array info or values indicates an object.
    std::vector<MockTranslatedCell> cells{makeCellOfIntegers("a", "", {})};
    compareMakeObjWithExpected(cells, fromjson("{a: {}}"));
}

TEST(ColumnarObjTest, AddNonLeafCellWithoutArrayInfoToObject) {
    // Cell with no array info or values indicates an object.
    std::vector<MockTranslatedCell> cells{makeCellOfIntegers("a.b", "", {})};
    compareMakeObjWithExpected(cells, fromjson("{a: {b: {}}}"));
}

TEST(ColumnarObjTest, AddTopLevelNonLeafCellWithArrayInfoToObject) {
    std::vector<MockTranslatedCell> cells{makeCellOfIntegers("a", "[o1", {})};
    compareMakeObjWithExpected(cells, fromjson("{a: [{}, {}]}"));
}

TEST(ColumnarObjTest, AddNonLeafCellWithArrayInfoToObject) {
    std::vector<MockTranslatedCell> cells{makeCellOfIntegers("a.b", "{[o1", {})};
    compareMakeObjWithExpected(cells, fromjson("{a: {b: [{}, {}]}}"));
}

TEST(ColumnarObjTest, AddLeafCellThenAddSparseSibling) {
    std::vector<MockTranslatedCell> cells{makeCellOfIntegers("a.b", "[", {1, 2}),
                                          makeCellOfIntegers("a", "[o1", {}),
                                          makeCellOfIntegers("a.c", "[1", {3})};
    compareMakeObjWithExpected(cells, fromjson("{a: [{b: 1}, {b: 2, c: 3}]}"));
}
}  // namespace mongo::sbe
