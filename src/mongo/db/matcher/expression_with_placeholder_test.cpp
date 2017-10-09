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
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

using unittest::assertGet;

TEST(ExpressionWithPlaceholderTest, ParseBasic) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{i: 0}");
    auto parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto filter = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT(filter->getPlaceholder());
    ASSERT_EQ(*filter->getPlaceholder(), "i");
    ASSERT_TRUE(filter->getFilter()->matchesBSON(fromjson("{i: 0}")));
    ASSERT_FALSE(filter->getFilter()->matchesBSON(fromjson("{i: 1}")));
}

TEST(ExpressionWithPlaceholderTest, ParseDottedField) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{'i.a': 0, 'i.b': 1}");
    auto parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto filter = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT(filter->getPlaceholder());
    ASSERT_EQ(*filter->getPlaceholder(), "i");
    ASSERT_TRUE(filter->getFilter()->matchesBSON(fromjson("{i: {a: 0, b: 1}}")));
    ASSERT_FALSE(filter->getFilter()->matchesBSON(fromjson("{i: {a: 0, b: 0}}")));
}

TEST(ExpressionWithPlaceholderTest, ParseLogicalQuery) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{$and: [{i: {$gte: 0}}, {i: {$lte: 0}}]}");
    auto parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto filter = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT(filter->getPlaceholder());
    ASSERT_EQ(*filter->getPlaceholder(), "i");
    ASSERT_TRUE(filter->getFilter()->matchesBSON(fromjson("{i: 0}")));
    ASSERT_FALSE(filter->getFilter()->matchesBSON(fromjson("{i: 1}")));
}

TEST(ExpressionWithPlaceholderTest, ParseElemMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{i: {$elemMatch: {a: 0}}}");
    auto parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto filter = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT(filter->getPlaceholder());
    ASSERT_EQ(*filter->getPlaceholder(), "i");
    ASSERT_TRUE(filter->getFilter()->matchesBSON(fromjson("{i: [{a: 0}]}")));
    ASSERT_FALSE(filter->getFilter()->matchesBSON(fromjson("{i: [{a: 1}]}")));
}

TEST(ExpressionWithPlaceholderTest, ParseCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(&collator);
    auto rawFilter = fromjson("{i: 'abc'}");
    auto parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto filter = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT(filter->getPlaceholder());
    ASSERT_EQ(*filter->getPlaceholder(), "i");
    ASSERT_TRUE(filter->getFilter()->matchesBSON(fromjson("{i: 'cba'}")));
    ASSERT_FALSE(filter->getFilter()->matchesBSON(fromjson("{i: 0}")));
}

TEST(ExpressionWithPlaceholderTest, ParseIdContainsNumbersAndCapitals) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{iA3: 0}");
    auto parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto filter = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT(filter->getPlaceholder());
    ASSERT_EQ(*filter->getPlaceholder(), "iA3");
    ASSERT_TRUE(filter->getFilter()->matchesBSON(fromjson("{'iA3': 0}")));
    ASSERT_FALSE(filter->getFilter()->matchesBSON(fromjson("{'iA3': 1}")));
}

TEST(ExpressionWithPlaceholderTest, EmptyMatchExpressionParsesSuccessfully) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{}");
    auto parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto result = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT_FALSE(result->getPlaceholder());
}

TEST(ExpressionWithPlaceholderTest, NestedEmptyMatchExpressionParsesSuccessfully) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{$or: [{$and: [{}]}]}");
    auto parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto result = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT_FALSE(result->getPlaceholder());
}

TEST(ExpressionWithPlaceholderTest,
     NestedMatchExpressionParsesSuccessfullyWhenSomeClausesHaveNoFieldName) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{$or: [{$and: [{}]}, {i: 0}, {i: 1}, {$and: [{}]}]}");
    auto parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto result = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT(result->getPlaceholder());
    ASSERT_EQ(*result->getPlaceholder(), "i"_sd);
}

TEST(ExpressionWithPlaceholderTest, SuccessfullyParsesExpressionsWithTypeOther) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter =
        fromjson("{a: {$_internalSchemaObjectMatch: {$_internalSchemaMinProperties: 5}}}");
    auto parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto result = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT(result->getPlaceholder());
    ASSERT_EQ(*result->getPlaceholder(), "a"_sd);

    rawFilter = fromjson("{a: {$_internalSchemaType: 'string'}}");
    parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    result = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT(result->getPlaceholder());
    ASSERT_EQ(*result->getPlaceholder(), "a"_sd);

    rawFilter = fromjson("{$_internalSchemaMinProperties: 1}}");
    parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    result = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT_FALSE(result->getPlaceholder());

    rawFilter = fromjson("{$_internalSchemaCond: [{a: {$exists: true}}, {b: 1}, {c: 1}]}");
    parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    result = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT_FALSE(result->getPlaceholder());
}

TEST(ExpressionWithPlaceholderTest, SuccessfullyParsesAlwaysTrue) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = BSON(AlwaysTrueMatchExpression::kName << 1);
    auto parsedFilter =
        assertGet(MatchExpressionParser::parse(rawFilter,
                                               expCtx,
                                               ExtensionsCallbackNoop(),
                                               MatchExpressionParser::kBanAllSpecialFeatures));
    auto result = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT_FALSE(result->getPlaceholder());
}

TEST(ExpressionWithPlaceholderTest, SuccessfullyParsesAlwaysFalse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = BSON(AlwaysFalseMatchExpression::kName << 1);
    auto parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto result = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT_FALSE(result->getPlaceholder());
}

TEST(ExpressionWithPlaceholderTest, EmptyFieldNameFailsToParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{'': 0}");
    auto parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto status = ExpressionWithPlaceholder::make(std::move(parsedFilter));
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ExpressionWithPlaceholderTest, EmptyElemMatchFieldNameFailsToParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{'': {$elemMatch: {a: 0}}}");
    auto parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto status = ExpressionWithPlaceholder::make(std::move(parsedFilter));
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ExpressionWithPlaceholderTest, EmptyTopLevelFieldNameFailsToParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{'.i': 0}");
    auto parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto status = ExpressionWithPlaceholder::make(std::move(parsedFilter));
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ExpressionWithPlaceholderTest, MultipleTopLevelFieldsFailsToParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{$and: [{i: 0}, {j: 0}]}");
    auto parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto status = ExpressionWithPlaceholder::make(std::move(parsedFilter));
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ExpressionWithPlaceholderTest, SpecialCharactersInFieldNameFailsToParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{'i&': 0}");
    auto parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto status = ExpressionWithPlaceholder::make(std::move(parsedFilter));
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ExpressionWithPlaceholderTest, FieldNameStartingWithNumberFailsToParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{'3i': 0}");
    auto parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto status = ExpressionWithPlaceholder::make(std::move(parsedFilter));
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ExpressionWithPlaceholderTest, FieldNameStartingWithCapitalFailsToParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{'Ai': 0}");
    auto parsedFilter = assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto status = ExpressionWithPlaceholder::make(std::move(parsedFilter));
    ASSERT_NOT_OK(status.getStatus());
    ASSERT_EQ(status.getStatus().code(), ErrorCodes::BadValue);
}

TEST(ExpressionWithPlaceholderTest, EquivalentIfPlaceholderAndExpressionMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter1 = fromjson("{i: 5}}");
    auto parsedFilter1 = assertGet(MatchExpressionParser::parse(rawFilter1, expCtx));
    auto expressionWithPlaceholder1 = ExpressionWithPlaceholder::make(std::move(parsedFilter1));
    ASSERT_OK(expressionWithPlaceholder1.getStatus());

    auto rawFilter2 = fromjson("{i: 5}");
    auto parsedFilter2 = assertGet(MatchExpressionParser::parse(rawFilter2, expCtx));
    auto expressionWithPlaceholder2 = ExpressionWithPlaceholder::make(std::move(parsedFilter2));
    ASSERT_OK(expressionWithPlaceholder2.getStatus());
    ASSERT_TRUE(expressionWithPlaceholder1.getValue()->equivalent(
        expressionWithPlaceholder2.getValue().get()));
}

TEST(ExpressionWithPlaceholderTest, EmptyMatchExpressionsAreEquivalent) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter1 = fromjson("{}");
    auto parsedFilter1 = assertGet(MatchExpressionParser::parse(rawFilter1, expCtx));
    auto expressionWithPlaceholder1 = ExpressionWithPlaceholder::make(std::move(parsedFilter1));
    ASSERT_OK(expressionWithPlaceholder1.getStatus());

    auto rawFilter2 = fromjson("{}");
    auto parsedFilter2 = assertGet(MatchExpressionParser::parse(rawFilter2, expCtx));
    auto expressionWithPlaceholder2 = ExpressionWithPlaceholder::make(std::move(parsedFilter2));
    ASSERT_OK(expressionWithPlaceholder2.getStatus());
    ASSERT_TRUE(expressionWithPlaceholder1.getValue()->equivalent(
        expressionWithPlaceholder2.getValue().get()));
}

TEST(ExpressionWithPlaceholderTest, NestedEmptyMatchExpressionsAreEquivalent) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter1 = fromjson("{$or: [{$and: [{}]}]}");
    auto parsedFilter1 = assertGet(MatchExpressionParser::parse(rawFilter1, expCtx));
    auto expressionWithPlaceholder1 = ExpressionWithPlaceholder::make(std::move(parsedFilter1));
    ASSERT_OK(expressionWithPlaceholder1.getStatus());

    auto rawFilter2 = fromjson("{$or: [{$and: [{}]}]}");
    auto parsedFilter2 = assertGet(MatchExpressionParser::parse(rawFilter2, expCtx));
    auto expressionWithPlaceholder2 = ExpressionWithPlaceholder::make(std::move(parsedFilter2));
    ASSERT_OK(expressionWithPlaceholder2.getStatus());
    ASSERT_TRUE(expressionWithPlaceholder1.getValue()->equivalent(
        expressionWithPlaceholder2.getValue().get()));
}

TEST(ExpressionWithPlaceholderTest, SameObjectMatchesAreEquivalent) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter1 =
        fromjson("{a: {$_internalSchemaObjectMatch: {$_internalSchemaMaxProperties: 2}}}");
    auto parsedFilter1 = assertGet(MatchExpressionParser::parse(rawFilter1, expCtx));
    auto expressionWithPlaceholder1 = ExpressionWithPlaceholder::make(std::move(parsedFilter1));
    ASSERT_OK(expressionWithPlaceholder1.getStatus());

    auto rawFilter2 =
        fromjson("{a: {$_internalSchemaObjectMatch: {$_internalSchemaMaxProperties: 2}}}");
    auto parsedFilter2 = assertGet(MatchExpressionParser::parse(rawFilter2, expCtx));
    auto expressionWithPlaceholder2 = ExpressionWithPlaceholder::make(std::move(parsedFilter2));
    ASSERT_OK(expressionWithPlaceholder2.getStatus());
    ASSERT_TRUE(expressionWithPlaceholder1.getValue()->equivalent(
        expressionWithPlaceholder2.getValue().get()));
}

TEST(ExpressionWithPlaceholderTest, AlwaysTruesAreEquivalent) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter1 = BSON(AlwaysTrueMatchExpression::kName << 1);
    auto parsedFilter1 = assertGet(MatchExpressionParser::parse(rawFilter1, expCtx));
    auto expressionWithPlaceholder1 = ExpressionWithPlaceholder::make(std::move(parsedFilter1));
    ASSERT_OK(expressionWithPlaceholder1.getStatus());

    auto rawFilter2 = BSON(AlwaysTrueMatchExpression::kName << 1);
    auto parsedFilter2 = assertGet(MatchExpressionParser::parse(rawFilter2, expCtx));
    auto expressionWithPlaceholder2 = ExpressionWithPlaceholder::make(std::move(parsedFilter2));
    ASSERT_OK(expressionWithPlaceholder2.getStatus());
    ASSERT_TRUE(expressionWithPlaceholder1.getValue()->equivalent(
        expressionWithPlaceholder2.getValue().get()));
}

TEST(ExpressionWithPlaceholderTest, NotEquivalentIfPlaceholderDoesNotMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter1 = fromjson("{i: {$type: 'array'}}");
    auto parsedFilter1 = assertGet(MatchExpressionParser::parse(rawFilter1, expCtx));
    auto expressionWithPlaceholder1 = ExpressionWithPlaceholder::make(std::move(parsedFilter1));
    ASSERT_OK(expressionWithPlaceholder1.getStatus());

    auto rawFilter2 = fromjson("{j: {$type: 'array'}}");
    auto parsedFilter2 = assertGet(MatchExpressionParser::parse(rawFilter2, expCtx));
    auto expressionWithPlaceholder2 = ExpressionWithPlaceholder::make(std::move(parsedFilter2));
    ASSERT_OK(expressionWithPlaceholder2.getStatus());
    ASSERT_FALSE(expressionWithPlaceholder1.getValue()->equivalent(
        expressionWithPlaceholder2.getValue().get()));
}

TEST(ExpressionWithPlaceholder, NotEquivalentIfOnePlaceholderIsEmpty) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter1 = fromjson("{}");
    auto parsedFilter1 = assertGet(MatchExpressionParser::parse(rawFilter1, expCtx));
    auto expressionWithPlaceholder1 = ExpressionWithPlaceholder::make(std::move(parsedFilter1));
    ASSERT_OK(expressionWithPlaceholder1.getStatus());

    auto rawFilter2 = fromjson("{i: 5}");
    auto parsedFilter2 = assertGet(MatchExpressionParser::parse(rawFilter2, expCtx));
    auto expressionWithPlaceholder2 = ExpressionWithPlaceholder::make(std::move(parsedFilter2));
    ASSERT_OK(expressionWithPlaceholder2.getStatus());
    ASSERT_FALSE(expressionWithPlaceholder1.getValue()->equivalent(
        expressionWithPlaceholder2.getValue().get()));
}

TEST(ExpressionWithPlaceholderTest, NotEquivalentIfExpressionDoesNotMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter1 = fromjson("{i: {$lte: 5}}");
    auto parsedFilter1 = assertGet(MatchExpressionParser::parse(rawFilter1, expCtx));
    auto expressionWithPlaceholder1 = ExpressionWithPlaceholder::make(std::move(parsedFilter1));
    ASSERT_OK(expressionWithPlaceholder1.getStatus());

    auto rawFilter2 = fromjson("{i: {$gte: 5}}");
    auto parsedFilter2 = assertGet(MatchExpressionParser::parse(rawFilter2, expCtx));
    auto expressionWithPlaceholder2 = ExpressionWithPlaceholder::make(std::move(parsedFilter2));
    ASSERT_OK(expressionWithPlaceholder2.getStatus());
    ASSERT_FALSE(expressionWithPlaceholder1.getValue()->equivalent(
        expressionWithPlaceholder2.getValue().get()));
}
}  // namespace
}  // namespace mongo
