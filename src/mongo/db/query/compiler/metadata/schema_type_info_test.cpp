// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/metadata/schema_type_info.h"

#include "mongo/bson/json.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <string_view>

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

    // The predicate is narrowed to 'string', but 'object' is definitely excluded.
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

TEST(SchemaTypeInfoTest, PopulateFromValidatorLeavesFieldUnknownForNonTypePredicate) {
    auto expr = parseMatchExpr("{x: {$gt: 5}}");

    SchemaTypeInfo info;
    info.populateFromValidator(expr.get());

    // Narrowing doesn't understand $gt, so nothing can be determined about 'x': it's still
    // possible for 'x' to hold any type, including an array.
    ASSERT_TRUE(info.getRootType().getField("x") == Type::any());
    ASSERT_TRUE(info.canPathBeArray(FieldRef("x")));
}

// TODO SERVER-134937: $jsonSchema's "bsonType" keyword translates to InternalSchemaTypeExpression,
// which narrowType does not yet handle, so nothing is narrowed.
TEST(SchemaTypeInfoTest, PopulateFromValidatorLeavesFieldUnknownForJSONSchemaBsonType) {
    auto expr = parseMatchExpr("{$jsonSchema: {properties: {x: {bsonType: 'string'}}}}");

    SchemaTypeInfo info;
    info.populateFromValidator(expr.get());

    ASSERT_TRUE(info.getRootType().getField("x") == Type::any());
    ASSERT_TRUE(info.canPathBeArray(FieldRef("x")));
}

// TODO SERVER-134938: $jsonSchema's "required" keyword translates to
// InternalSchemaAllElemMatchFromIndexMatchExpression/ExistsMatchExpression nodes that narrowType
// does not yet handle either, so the field's type still isn't narrowed even though it's
// guaranteed to be present.
TEST(SchemaTypeInfoTest, PopulateFromValidatorLeavesFieldUnknownForJSONSchemaRequired) {
    auto expr =
        parseMatchExpr("{$jsonSchema: {required: ['x'], properties: {x: {bsonType: 'string'}}}}");

    SchemaTypeInfo info;
    info.populateFromValidator(expr.get());

    ASSERT_TRUE(info.getRootType().getField("x") == Type::any());
    ASSERT_TRUE(info.canPathBeArray(FieldRef("x")));
}

TEST(SchemaTypeInfoTest, CanPathBeArrayIsTrueByDefault) {
    SchemaTypeInfo info;
    ASSERT_TRUE(info.canPathBeArray(FieldRef("x")));
    ASSERT_TRUE(info.canPathBeArray(FieldRef("x.y")));
}

TEST(SchemaTypeInfoTest, CanPathBeArrayWithEmptyPath) {
    ASSERT_FALSE(SchemaTypeInfo().canPathBeArray(FieldRef("")));
    ASSERT_FALSE(SchemaTypeInfo(Type::never()).canPathBeArray(FieldRef("")));
}

TEST(SchemaTypeInfoTest, CanPathBeArrayWithUnusualFieldNames) {
    SchemaTypeInfo unknown;
    SchemaTypeInfo known(Type::object({{"a", Type(BSONType::string, Extent::kAll)}}, Open::kNo));

    for (const auto* path : {"a.", "a..b", ".a", "$invalidField", "a.$invalidField"}) {
        ASSERT_TRUE(unknown.canPathBeArray(FieldRef(path))) << path;
        ASSERT_FALSE(known.canPathBeArray(FieldRef(path))) << path;
    }

    Type inner = Type::object({{"", Type(BSONType::array, Extent::kAll)},
                               {"$invalidField", Type(BSONType::array, Extent::kAll)}},
                              Open::kNo);
    SchemaTypeInfo named(Type::object({{"a", inner},
                                       {"", Type(BSONType::array, Extent::kAll)},
                                       {"$invalidField", Type(BSONType::array, Extent::kAll)}},
                                      Open::kNo));
    for (const auto* path : {"a.", "a..b", ".a", "$invalidField", "a.$invalidField"}) {
        ASSERT_TRUE(named.canPathBeArray(FieldRef(path))) << path;
    }
}

TEST(SchemaTypeInfoTest, CanPathBeArrayWithOverDeepPath) {
    std::string path = "a";
    for (int i = 0; i < BSONDepth::kDefaultMaxAllowableDepth; ++i) {
        path += ".b";
    }

    ASSERT_TRUE(SchemaTypeInfo().canPathBeArray(FieldRef(path)));
    SchemaTypeInfo known(Type::object({{"a", Type(BSONType::string, Extent::kAll)}}, Open::kNo));
    ASSERT_FALSE(known.canPathBeArray(FieldRef(path)));
}

TEST(SchemaTypeInfoTest, EmbeddedNullCannotBePassedAsFieldRef) {
    ASSERT_THROWS_CODE(FieldRef(std::string_view("a\0b", 3)), AssertionException, 9867600);
}

TEST(SchemaTypeInfoTest, CanPathBeArrayIsFalseWhenFieldTypeExcludesArray) {
    Type root = Type::object({{"x", Type(BSONType::string, Extent::kAll)}}, Open::kYes);
    SchemaTypeInfo info(root);

    ASSERT_FALSE(info.canPathBeArray(FieldRef("x")));
}

TEST(SchemaTypeInfoTest, CanPathBeArrayStopsAtScalarField) {
    Type root = Type::object({{"x", Type(BSONType::string, Extent::kAll)}}, Open::kYes);
    SchemaTypeInfo info(root);

    ASSERT_FALSE(info.canPathBeArray(FieldRef("x.y")));
}

TEST(SchemaTypeInfoTest, CanPathBeArrayStopsAtNever) {
    SchemaTypeInfo info(Type::never());

    ASSERT_FALSE(info.canPathBeArray(FieldRef("x.y")));
}

TEST(SchemaTypeInfoTest, CanPathBeArrayIsTrueWhenFieldTypeIsArray) {
    auto expr = parseMatchExpr("{x: {$type: 'array'}}");

    SchemaTypeInfo info;
    info.populateFromValidator(expr.get());

    ASSERT_TRUE(info.canPathBeArray(FieldRef("x")));
}

TEST(SchemaTypeInfoTest, CanPathBeArrayIsFalseWhenFieldTypeIsNotArray) {
    auto expr = parseMatchExpr("{x: {$not: {$type: 'array'}}}");

    SchemaTypeInfo info;
    info.populateFromValidator(expr.get());

    ASSERT_FALSE(info.canPathBeArray(FieldRef("x")));
}

TEST(SchemaTypeInfoTest, CanPathBeArrayChecksEveryPathComponent) {
    Type inner = Type::object({{"b", Type(BSONType::array, Extent::kAll)}}, Open::kYes);
    Type root = Type::object({{"a", inner}}, Open::kYes);
    SchemaTypeInfo info(root);

    // 'a' itself is never an array, only its 'b' field is.
    ASSERT_FALSE(info.canPathBeArray(FieldRef("a")));
    ASSERT_TRUE(info.canPathBeArray(FieldRef("a.b")));
    // 'a.b.c' traverses through the array at 'a.b', so it can also be an array.
    ASSERT_TRUE(info.canPathBeArray(FieldRef("a.b.c")));
}

}  // namespace
}  // namespace mongo
