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

#include "mongo/bson/json.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
TEST(InternalSchemaUniqueItemsMatchExpression, RejectsNonArrays) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo");
    ASSERT_FALSE(uniqueItems.matchesBSON(BSON("foo" << 1)));
    ASSERT_FALSE(uniqueItems.matchesBSON(BSON("foo" << BSONObj())));
    ASSERT_FALSE(uniqueItems.matchesBSON(BSON("foo"
                                              << "string")));
}

TEST(InternalSchemaUniqueItemsMatchExpression, MatchesEmptyArray) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo");
    ASSERT_TRUE(uniqueItems.matchesBSON(BSON("foo" << BSONArray())));
}

TEST(InternalSchemaUniqueItemsMatchExpression, MatchesOneElementArray) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo");
    ASSERT_TRUE(uniqueItems.matchesBSON(BSON("foo" << BSON_ARRAY(1))));
    ASSERT_TRUE(uniqueItems.matchesBSON(BSON("foo" << BSON_ARRAY(BSONObj()))));
    ASSERT_TRUE(uniqueItems.matchesBSON(BSON("foo" << BSON_ARRAY(BSON_ARRAY(9 << "bar")))));
}

TEST(InternalSchemaUniqueItemsMatchExpression, MatchesArrayOfUniqueItems) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo");
    ASSERT_TRUE(uniqueItems.matchesBSON(fromjson("{foo: [1, 'bar', {}, [], null]}")));
    ASSERT_TRUE(uniqueItems.matchesBSON(fromjson("{foo: [{x: 1}, {x: 2}, {x: 2, y: 3}]}")));
    ASSERT_TRUE(uniqueItems.matchesBSON(fromjson("{foo: [[1], [1, 2], 1]}")));
    ASSERT_TRUE(uniqueItems.matchesBSON(fromjson("{foo: [['a', 'b'], ['b', 'a']]}")));
}

TEST(InternalSchemaUniqueItemsMatchExpression, MatchesNestedArrayOfUniqueItems) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo.bar");
    ASSERT_TRUE(uniqueItems.matchesBSON(fromjson("{foo: {bar: [1, 'bar', {}, [], null]}}")));
    ASSERT_TRUE(uniqueItems.matchesBSON(fromjson("{foo: {bar: [{x: 1}, {x: 2}, {x: 2, y: 3}]}}")));
    ASSERT_TRUE(uniqueItems.matchesBSON(fromjson("{foo: {bar: [[1], [1, 2], 1]}}")));
    ASSERT_TRUE(uniqueItems.matchesBSON(fromjson("{foo: {bar: [['a', 'b'], ['b', 'a']]}}")));
}

TEST(InternalSchemaUniqueItemsMatchExpression, RejectsArrayWithDuplicates) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo");
    ASSERT_FALSE(uniqueItems.matchesBSON(fromjson("{foo: [1, 1, 1]}")));
    ASSERT_FALSE(uniqueItems.matchesBSON(fromjson("{foo: [['bar'], ['bar']]}")));
    ASSERT_FALSE(
        uniqueItems.matchesBSON(fromjson("{foo: [{x: 'a', y: [1, 2]}, {y: [1, 2], x: 'a'}]}")));
}

TEST(InternalSchemaUniqueItemsMatchExpression, RejectsNestedArrayWithDuplicates) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo");
    ASSERT_FALSE(uniqueItems.matchesBSON(fromjson("{foo: {bar: [1, 1, 1]}}")));
    ASSERT_FALSE(uniqueItems.matchesBSON(fromjson("{foo: {bar: [['baz'], ['baz']]}}")));
    ASSERT_FALSE(uniqueItems.matchesBSON(
        fromjson("{foo: {bar: [{x: 'a', y: [1, 2]}, {y: [1, 2], x: 'a'}]}}")));
}

TEST(InternalSchemaUniqueItemsMatchExpression, FieldNameSignificantWhenComparingNestedObjects) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo");
    ASSERT_TRUE(uniqueItems.matchesBSON(fromjson("{foo: [{x: 7}, {y: 7}]}")));
    ASSERT_TRUE(uniqueItems.matchesBSON(fromjson("{foo: [{a: 'bar'}, {b: 'bar'}]}")));
    ASSERT_FALSE(uniqueItems.matchesBSON(fromjson("{foo: [{a: 'bar'}, {a: 'bar'}]}")));
    ASSERT_TRUE(uniqueItems.matchesBSON(fromjson("{foo: [[1, 2, 3], [1, 3, 2]]}")));
}

TEST(InternalSchemaUniqueItemsMatchExpression, AlwaysUsesBinaryComparisonRegardlessOfCollator) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    uniqueItems.setCollator(&collator);

    ASSERT_TRUE(uniqueItems.matchesBSON(fromjson("{foo: ['one', 'OnE', 'ONE']}")));
    ASSERT_TRUE(uniqueItems.matchesBSON(fromjson("{foo: [{x: 'two'}, {y: 'two'}]}")));
    ASSERT_TRUE(uniqueItems.matchesBSON(fromjson("{foo: [{a: 'three'}, {a: 'THREE'}]}")));
}

TEST(InternalSchemaUniqueItemsMatchExpression, FindsFirstDuplicateValue) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("");
    auto inputArray = fromjson("[1, 2, 2, 1]}");
    auto result = uniqueItems.findFirstDuplicateValue(inputArray);
    ASSERT_TRUE(result);
    ASSERT_EQUALS(result.Int(), 2);
    ASSERT_FALSE(uniqueItems.findFirstDuplicateValue(fromjson("[1, 2]}")));
    ASSERT_FALSE(uniqueItems.findFirstDuplicateValue(fromjson("[]}")));
}

DEATH_TEST_REGEX(InternalSchemaUniqueItemsMatchExpression,
                 GetChildFailsIndexLargerThanZero,
                 "Tripwire assertion.*6400219") {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo");

    ASSERT_EQ(uniqueItems.numChildren(), 0);
    ASSERT_THROWS_CODE(uniqueItems.getChild(0), AssertionException, 6400219);
}

}  // namespace
}  // namespace mongo
