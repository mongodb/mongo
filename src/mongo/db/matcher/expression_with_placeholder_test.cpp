/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

using unittest::assertGet;

TEST(ExpressionWithPlaceholderTest, ParseBasic) {
    const CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{i: 0}");
    auto filter = assertGet(ExpressionWithPlaceholder::parse(rawFilter, collator));
    ASSERT(filter->getPlaceholder());
    ASSERT_EQ(*filter->getPlaceholder(), "i");
    ASSERT_TRUE(filter->getFilter()->matchesBSON(fromjson("{i: 0}")));
    ASSERT_FALSE(filter->getFilter()->matchesBSON(fromjson("{i: 1}")));
}

TEST(ExpressionWithPlaceholderTest, ParseDottedField) {
    const CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{'i.a': 0, 'i.b': 1}");
    auto filter = assertGet(ExpressionWithPlaceholder::parse(rawFilter, collator));
    ASSERT(filter->getPlaceholder());
    ASSERT_EQ(*filter->getPlaceholder(), "i");
    ASSERT_TRUE(filter->getFilter()->matchesBSON(fromjson("{i: {a: 0, b: 1}}")));
    ASSERT_FALSE(filter->getFilter()->matchesBSON(fromjson("{i: {a: 0, b: 0}}")));
}

TEST(ExpressionWithPlaceholderTest, ParseLogicalQuery) {
    const CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{$and: [{i: {$gte: 0}}, {i: {$lte: 0}}]}");
    auto filter = assertGet(ExpressionWithPlaceholder::parse(rawFilter, collator));
    ASSERT(filter->getPlaceholder());
    ASSERT_EQ(*filter->getPlaceholder(), "i");
    ASSERT_TRUE(filter->getFilter()->matchesBSON(fromjson("{i: 0}")));
    ASSERT_FALSE(filter->getFilter()->matchesBSON(fromjson("{i: 1}")));
}

TEST(ExpressionWithPlaceholderTest, ParseElemMatch) {
    const CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{i: {$elemMatch: {a: 0}}}");
    auto filter = assertGet(ExpressionWithPlaceholder::parse(rawFilter, collator));
    ASSERT(filter->getPlaceholder());
    ASSERT_EQ(*filter->getPlaceholder(), "i");
    ASSERT_TRUE(filter->getFilter()->matchesBSON(fromjson("{i: [{a: 0}]}")));
    ASSERT_FALSE(filter->getFilter()->matchesBSON(fromjson("{i: [{a: 1}]}")));
}

TEST(ExpressionWithPlaceholderTest, ParseCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto rawFilter = fromjson("{i: 'abc'}");
    auto filter = assertGet(ExpressionWithPlaceholder::parse(rawFilter, &collator));
    ASSERT(filter->getPlaceholder());
    ASSERT_EQ(*filter->getPlaceholder(), "i");
    ASSERT_TRUE(filter->getFilter()->matchesBSON(fromjson("{i: 'cba'}")));
    ASSERT_FALSE(filter->getFilter()->matchesBSON(fromjson("{i: 0}")));
}

TEST(ExpressionWithPlaceholderTest, ParseIdContainsNumbersAndCapitals) {
    const CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{iA3: 0}");
    auto filter = assertGet(ExpressionWithPlaceholder::parse(rawFilter, collator));
    ASSERT(filter->getPlaceholder());
    ASSERT_EQ(*filter->getPlaceholder(), "iA3");
    ASSERT_TRUE(filter->getFilter()->matchesBSON(fromjson("{'iA3': 0}")));
    ASSERT_FALSE(filter->getFilter()->matchesBSON(fromjson("{'iA3': 1}")));
}

TEST(ExpressionWithPlaceholderTest, BadMatchExpressionFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{$and: 0}");
    auto status = ExpressionWithPlaceholder::parse(rawFilter, collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ExpressionWithPlaceholderTest, EmptyMatchExpressionParsesSuccessfully) {
    constexpr CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{}");
    auto result = assertGet(ExpressionWithPlaceholder::parse(rawFilter, collator));
    ASSERT_FALSE(result->getPlaceholder());
}

TEST(ExpressionWithPlaceholderTest, NestedEmptyMatchExpressionParsesSuccessfully) {
    constexpr CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{$or: [{$and: [{}]}]}");
    auto result = assertGet(ExpressionWithPlaceholder::parse(rawFilter, collator));
    ASSERT_FALSE(result->getPlaceholder());
}

TEST(ExpressionWithPlaceholderTest,
     NestedMatchExpressionParsesSuccessfullyWhenSomeClausesHaveNoFieldName) {
    constexpr CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{$or: [{$and: [{}]}, {i: 0}, {i: 1}, {$and: [{}]}]}");
    auto result = assertGet(ExpressionWithPlaceholder::parse(rawFilter, collator));
    ASSERT(result->getPlaceholder());
    ASSERT_EQ(*result->getPlaceholder(), "i"_sd);
}

TEST(ExpressionWithPlaceholderTest, SuccessfullyParsesExpressionsWithTypeOther) {
    constexpr CollatorInterface* collator = nullptr;
    auto rawFilter =
        fromjson("{a: {$_internalSchemaObjectMatch: {$_internalSchemaMinProperties: 5}}}");
    auto result = assertGet(ExpressionWithPlaceholder::parse(rawFilter, collator));
    ASSERT(result->getPlaceholder());
    ASSERT_EQ(*result->getPlaceholder(), "a"_sd);

    rawFilter = fromjson("{a: {$_internalSchemaType: 'string'}}");
    result = assertGet(ExpressionWithPlaceholder::parse(rawFilter, collator));
    ASSERT(result->getPlaceholder());
    ASSERT_EQ(*result->getPlaceholder(), "a"_sd);

    rawFilter = fromjson("{$_internalSchemaMinProperties: 1}}");
    result = assertGet(ExpressionWithPlaceholder::parse(rawFilter, collator));
    ASSERT_FALSE(result->getPlaceholder());

    rawFilter = fromjson("{$_internalSchemaCond: [{a: {$exists: true}}, {b: 1}, {c: 1}]}");
    result = assertGet(ExpressionWithPlaceholder::parse(rawFilter, collator));
    ASSERT_FALSE(result->getPlaceholder());
}

TEST(ExpressionWithPlaceholderTest, SuccessfullyParsesAlwaysTrue) {
    constexpr CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{$alwaysTrue: 1}");
    auto result = assertGet(ExpressionWithPlaceholder::parse(rawFilter, collator));
    ASSERT_FALSE(result->getPlaceholder());
}

TEST(ExpressionWithPlaceholderTest, SuccessfullyParsesAlwaysFalse) {
    constexpr CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{$alwaysFalse: 1}");
    auto result = assertGet(ExpressionWithPlaceholder::parse(rawFilter, collator));
    ASSERT_FALSE(result->getPlaceholder());
}

TEST(ExpressionWithPlaceholderTest, EmptyFieldNameFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{'': 0}");
    auto status = ExpressionWithPlaceholder::parse(rawFilter, collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ExpressionWithPlaceholderTest, EmptyElemMatchFieldNameFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{'': {$elemMatch: {a: 0}}}");
    auto status = ExpressionWithPlaceholder::parse(rawFilter, collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ExpressionWithPlaceholderTest, EmptyTopLevelFieldNameFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{'.i': 0}");
    auto status = ExpressionWithPlaceholder::parse(rawFilter, collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ExpressionWithPlaceholderTest, MultipleTopLevelFieldsFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{$and: [{i: 0}, {j: 0}]}");
    auto status = ExpressionWithPlaceholder::parse(rawFilter, collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ExpressionWithPlaceholderTest, SpecialCharactersInFieldNameFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{'i&': 0}");
    auto status = ExpressionWithPlaceholder::parse(rawFilter, collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ExpressionWithPlaceholderTest, FieldNameStartingWithNumberFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{'3i': 0}");
    auto status = ExpressionWithPlaceholder::parse(rawFilter, collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ExpressionWithPlaceholderTest, FieldNameStartingWithCapitalFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{'Ai': 0}");
    auto status = ExpressionWithPlaceholder::parse(rawFilter, collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ExpressionWithPlaceholderTest, TextSearchExpressionFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{$text: {$search: 'search terms'}}");
    auto status = ExpressionWithPlaceholder::parse(rawFilter, collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ExpressionWithPlaceholderTest, WhereExpressionFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{$where: 'sleep(100)'}");
    auto status = ExpressionWithPlaceholder::parse(rawFilter, collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ExpressionWithPlaceholderTest, GeoNearExpressionFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawFilter =
        fromjson("{i: {$nearSphere: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}");
    auto status = ExpressionWithPlaceholder::parse(rawFilter, collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ExpressionWithPlaceholderTest, ExprExpressionFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawFilter = fromjson("{i: {$expr: 5}}");
    auto status = ExpressionWithPlaceholder::parse(rawFilter, collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ExpressionWithPlaceholderTest, EquivalentIfPlaceholderAndExpressionMatch) {
    constexpr auto collator = nullptr;
    auto rawFilter1 = fromjson("{i: 5}}");
    auto expressionWithPlaceholder1 = ExpressionWithPlaceholder::parse(rawFilter1, collator);
    ASSERT_OK(expressionWithPlaceholder1.getStatus());

    auto rawFilter2 = fromjson("{i: 5}");
    auto expressionWithPlaceholder2 = ExpressionWithPlaceholder::parse(rawFilter2, collator);
    ASSERT_OK(expressionWithPlaceholder2.getStatus());
    ASSERT_TRUE(expressionWithPlaceholder1.getValue()->equivalent(
        expressionWithPlaceholder2.getValue().get()));
}

TEST(ExpressionWithPlaceholderTest, EmptyMatchExpressionsAreEquivalent) {
    constexpr auto collator = nullptr;
    auto rawFilter1 = fromjson("{}");
    auto expressionWithPlaceholder1 = ExpressionWithPlaceholder::parse(rawFilter1, collator);
    ASSERT_OK(expressionWithPlaceholder1.getStatus());

    auto rawFilter2 = fromjson("{}");
    auto expressionWithPlaceholder2 = ExpressionWithPlaceholder::parse(rawFilter2, collator);
    ASSERT_OK(expressionWithPlaceholder2.getStatus());
    ASSERT(expressionWithPlaceholder1.getValue()->equivalent(
        expressionWithPlaceholder2.getValue().get()));
}

TEST(ExpressionWithPlaceholderTest, NestedEmptyMatchExpressionsAreEquivalent) {
    constexpr auto collator = nullptr;
    auto rawFilter1 = fromjson("{$or: [{$and: [{}]}]}");
    auto expressionWithPlaceholder1 = ExpressionWithPlaceholder::parse(rawFilter1, collator);
    ASSERT_OK(expressionWithPlaceholder1.getStatus());

    auto rawFilter2 = fromjson("{$or: [{$and: [{}]}]}");
    auto expressionWithPlaceholder2 = ExpressionWithPlaceholder::parse(rawFilter2, collator);
    ASSERT_OK(expressionWithPlaceholder2.getStatus());
    ASSERT(expressionWithPlaceholder1.getValue()->equivalent(
        expressionWithPlaceholder2.getValue().get()));
}

TEST(ExpressionWithPlaceholderTest, SameObjectMatchesAreEquivalent) {
    constexpr auto collator = nullptr;
    auto rawFilter1 =
        fromjson("{a: {$_internalSchemaObjectMatch: {$_internalSchemaMaxProperties: 2}}}");
    auto expressionWithPlaceholder1 = ExpressionWithPlaceholder::parse(rawFilter1, collator);
    ASSERT_OK(expressionWithPlaceholder1.getStatus());

    auto rawFilter2 =
        fromjson("{a: {$_internalSchemaObjectMatch: {$_internalSchemaMaxProperties: 2}}}");
    auto expressionWithPlaceholder2 = ExpressionWithPlaceholder::parse(rawFilter2, collator);
    ASSERT_OK(expressionWithPlaceholder2.getStatus());
    ASSERT(expressionWithPlaceholder1.getValue()->equivalent(
        expressionWithPlaceholder2.getValue().get()));
}

TEST(ExpressionWithPlaceholderTest, AlwaysTruesAreEquivalent) {
    constexpr auto collator = nullptr;
    auto rawFilter1 = fromjson("{$alwaysTrue: 1}");
    auto expressionWithPlaceholder1 = ExpressionWithPlaceholder::parse(rawFilter1, collator);
    ASSERT_OK(expressionWithPlaceholder1.getStatus());

    auto rawFilter2 = fromjson("{$alwaysTrue: 1}");
    auto expressionWithPlaceholder2 = ExpressionWithPlaceholder::parse(rawFilter2, collator);
    ASSERT_OK(expressionWithPlaceholder2.getStatus());
    ASSERT(expressionWithPlaceholder1.getValue()->equivalent(
        expressionWithPlaceholder2.getValue().get()));
}

TEST(ExpressionWithPlaceholderTest, NotEquivalentIfPlaceholderDoesNotMatch) {
    constexpr auto collator = nullptr;
    auto rawFilter1 = fromjson("{i: {$type: 'array'}}");
    auto expressionWithPlaceholder1 = ExpressionWithPlaceholder::parse(rawFilter1, collator);
    ASSERT_OK(expressionWithPlaceholder1.getStatus());

    auto rawFilter2 = fromjson("{j: {$type: 'array'}}");
    auto expressionWithPlaceholder2 = ExpressionWithPlaceholder::parse(rawFilter2, collator);
    ASSERT_OK(expressionWithPlaceholder2.getStatus());
    ASSERT_FALSE(expressionWithPlaceholder1.getValue()->equivalent(
        expressionWithPlaceholder2.getValue().get()));
}

TEST(ExpressionWithPlaceholder, NotEquivalentIfOnePlaceholderIsEmpty) {
    constexpr auto collator = nullptr;
    auto rawFilter1 = fromjson("{}");
    auto expressionWithPlaceholder1 = ExpressionWithPlaceholder::parse(rawFilter1, collator);
    ASSERT_OK(expressionWithPlaceholder1.getStatus());

    auto rawFilter2 = fromjson("{i: 5}");
    auto expressionWithPlaceholder2 = ExpressionWithPlaceholder::parse(rawFilter2, collator);
    ASSERT_OK(expressionWithPlaceholder2.getStatus());
    ASSERT_FALSE(expressionWithPlaceholder1.getValue()->equivalent(
        expressionWithPlaceholder2.getValue().get()));
}

TEST(ExpressionWithPlaceholderTest, NotEquivalentIfExpressionDoesNotMatch) {
    constexpr auto collator = nullptr;
    auto rawFilter1 = fromjson("{i: {$lte: 5}}");
    auto expressionWithPlaceholder1 = ExpressionWithPlaceholder::parse(rawFilter1, collator);
    ASSERT_OK(expressionWithPlaceholder1.getStatus());

    auto rawFilter2 = fromjson("{i: {$gte: 5}}");
    auto expressionWithPlaceholder2 = ExpressionWithPlaceholder::parse(rawFilter2, collator);
    ASSERT_OK(expressionWithPlaceholder2.getStatus());
    ASSERT_FALSE(expressionWithPlaceholder1.getValue()->equivalent(
        expressionWithPlaceholder2.getValue().get()));
}
}  // namespace
}  // namespace mongo
