// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/metadata/schema_type_info.h"

#include "mongo/bson/json.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using namespace pipeline::type_system;

std::unique_ptr<MatchExpression> parseMatchExpr(const std::string& query) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto result = MatchExpressionParser::parse(fromjson(query),
                                               expCtx,
                                               ExtensionsCallbackNoop(),
                                               MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_OK(result.getStatus()) << query;
    return std::move(result.getValue());
}

TEST(SchemaTypeInfoTest, DefaultConstructedIsAnyObject) {
    SchemaTypeInfo info;
    ASSERT_TRUE(info.getRootType() == Type::anyObject());
}

TEST(SchemaTypeInfoTest, EmptySchemaTypeInfoIsAnyObject) {
    ASSERT_TRUE(SchemaTypeInfo::empty().getRootType() == Type::anyObject());
}

TEST(SchemaTypeInfoTest, EmptySchemaTypeInfoDefaultsEpochToZero) {
    ASSERT_EQ(SchemaTypeInfo::empty().epoch(), 0ULL);
}

TEST(SchemaTypeInfoTest, IncrementEpochChangesEpoch) {
    SchemaTypeInfo info;
    info.incrementEpoch();
    ASSERT_EQ(info.epoch(), 1ULL);
}

TEST(SchemaTypeInfoTest, PopulateFromNullValidatorLeavesAnyObject) {
    SchemaTypeInfo info;
    info.populateFromValidator(nullptr);
    ASSERT_TRUE(info.getRootType() == Type::anyObject());
}

TEST(SchemaTypeInfoTest, PopulateFromValidatorNarrowsFieldType) {
    auto expr = parseMatchExpr("{x: {$type: 'string'}}");

    SchemaTypeInfo info;
    info.populateFromValidator(expr.get());

    // The predicate also matches an array holding a string element, so 'x' isn't narrowed to
    // 'string' alone, but 'object' is definitely excluded.
    ASSERT_TRUE(info.getRootType().getField("x").hasType(BSONType::string));
    ASSERT_FALSE(info.getRootType().getField("x").hasType(BSONType::object));
}

TEST(SchemaTypeInfoTest, PopulateFromValidatorRebuildsFromScratch) {
    auto stringExpr = parseMatchExpr("{x: {$type: 'string'}}");
    auto numberExpr = parseMatchExpr("{x: {$type: 'number'}}");

    SchemaTypeInfo info;
    info.populateFromValidator(stringExpr.get());
    info.populateFromValidator(numberExpr.get());

    // The second call must not narrow on top of the first, but rebuild from 'anyObject'.
    ASSERT_FALSE(info.getRootType().getField("x").hasType(BSONType::string));
    ASSERT_TRUE(info.getRootType().getField("x").hasType(BSONType::numberInt));
}

}  // namespace
}  // namespace mongo
