// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/matcher_typing.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/tassert_guard.h"
#include "mongo/unittest/unittest.h"

namespace mongo::pipeline::type_system {
namespace {

std::unique_ptr<MatchExpression> parseMatchExpr(const std::string& query) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto result = MatchExpressionParser::parse(fromjson(query),
                                               expCtx,
                                               ExtensionsCallbackNoop(),
                                               MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_OK(result.getStatus()) << query;
    return std::move(result.getValue());
}

Type allValues(BSONType type) {
    return Type(type, Extent::kAll);
}

Type openObject(const StringMap<Type>& fields) {
    return Type::object(fields, Open::kYes);
}

Type closedObject(const StringMap<Type>& fields) {
    return Type::object(fields, Open::kNo);
}

ElementPath traversedPath(std::string_view path) {
    return ElementPath(path, ElementPath::LeafArrayBehavior::kTraverse);
}

TEST(MatcherTypingTest, ObjectTypeIsReturnedUnchanged) {
    auto expr = parseMatchExpr("{a: 1}");
    auto input = Type::anyObject();
    ASSERT_EQ(narrowType(input, expr.get(), true), input);
    ASSERT_EQ(narrowType(input, expr.get(), false), input);
}

TEST(MatcherTypingTest, KnownFieldsAreReturnedUnchanged) {
    auto expr = parseMatchExpr("{a: 1}");
    auto input = openObject({{"x", allValues(BSONType::numberInt)}});
    ASSERT_EQ(narrowType(input, expr.get(), true), input);
    ASSERT_EQ(narrowType(input, expr.get(), false), input);
}

TEST(MatcherTypingTest, UnsatisfiableInputIsReturnedUnchanged) {
    auto expr = parseMatchExpr("{a: 1}");
    auto input = Type::never();
    ASSERT_EQ(narrowType(input, expr.get(), true), input);
    ASSERT_EQ(narrowType(input, expr.get(), false), input);
}

TEST(MatcherTypingTest, InputCoveringAnyTypeIsRejected) {
    auto expr = parseMatchExpr("{a: 1}");
    ASSERT_TASSERT_CODE(narrowType(Type::any(), expr.get(), true), 13459201);
}

TEST(MatcherTypingTest, InputCoveringNonObjectTypeIsRejected) {
    auto expr = parseMatchExpr("{a: 1}");
    ASSERT_TASSERT_CODE(narrowType(allValues(BSONType::string), expr.get(), true), 13459201);
    ASSERT_TASSERT_CODE(
        narrowType(unionType(Type::anyObject(), allValues(BSONType::array)), expr.get(), true),
        13459201);
}

TEST(MatcherPathTypingTest, UnsupportedLeafArrayNarrowsNothing) {
    auto input =
        openObject({{"x", unionType(allValues(BSONType::numberInt), allValues(BSONType::string))}});
    auto constraint = allValues(BSONType::numberInt);
    for (auto leafBehaviour : {ElementPath::LeafArrayBehavior::kNoTraversal,
                               ElementPath::LeafArrayBehavior::kTraverseOmitArray}) {
        auto path = ElementPath("x", leafBehaviour);
        ASSERT_EQ(matcher::narrowPath(input, path, constraint, true).toDebugString(),
                  input.toDebugString());
        ASSERT_EQ(matcher::narrowPath(input, path, constraint, false).toDebugString(),
                  input.toDebugString());
    }
}

TEST(MatcherPathTypingTest, UnsupportedDottedPathNarrowsNothing) {
    auto input =
        openObject({{"x", unionType(allValues(BSONType::numberInt), allValues(BSONType::string))}});
    auto constraint = allValues(BSONType::numberInt);
    for (auto leafBehaviour : {ElementPath::LeafArrayBehavior::kTraverse,
                               ElementPath::LeafArrayBehavior::kNoTraversal,
                               ElementPath::LeafArrayBehavior::kTraverseOmitArray}) {
        auto path = ElementPath("x.y", leafBehaviour);
        ASSERT_EQ(matcher::narrowPath(input, path, constraint, true).toDebugString(),
                  input.toDebugString());
        ASSERT_EQ(matcher::narrowPath(input, path, constraint, false).toDebugString(),
                  input.toDebugString());
    }
}

TEST(MatcherPathTypingTest, NarrowPathAddsArraysHoldingMatch) {
    ASSERT_EQ(matcher::narrowPath(
                  Type::anyObject(), traversedPath("x"), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              "{x: int|array(S), ...}");
}

TEST(MatcherPathTypingTest, NarrowPathOnNonArrayFieldLeavesOnlyConstraint) {
    auto input = openObject({{"x", complement(allValues(BSONType::array))}});
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("x"), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              "{x: int, ...}");
}

TEST(MatcherPathTypingTest, NegatedNarrowPathKeepsArraysHoldingNoMatch) {
    ASSERT_EQ(matcher::narrowPath(
                  Type::anyObject(), traversedPath("x"), allValues(BSONType::numberInt), false)
                  .toDebugString(),
              "{x: ~(int|array(S)), ...}");
}

TEST(MatcherPathTypingTest, NegatedNarrowPathOnNonArrayFieldRemovesConstraintAndArrays) {
    auto input = openObject({{"x", complement(allValues(BSONType::array))}});
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("x"), allValues(BSONType::numberInt), false)
                  .toDebugString(),
              "{x: ~(int|array), ...}");
}

TEST(MatcherPathTypingTest, NarrowPathToArrayCoversWholeBracket) {
    ASSERT_EQ(
        matcher::narrowPath(Type::anyObject(), traversedPath("x"), allValues(BSONType::array), true)
            .toDebugString(),
        "{x: array, ...}");
}

TEST(MatcherPathTypingTest, NegatedNarrowPathToArrayRemovesWholeBracket) {
    ASSERT_EQ(matcher::narrowPath(
                  Type::anyObject(), traversedPath("x"), allValues(BSONType::array), false)
                  .toDebugString(),
              "{x: ~array, ...}");
}

TEST(MatcherPathTypingTest, NarrowPathToMissingAddsNoArrays) {
    ASSERT_EQ(matcher::narrowPath(Type::anyObject(), traversedPath("x"), Type::missing(), true)
                  .toDebugString(),
              "{x: missing, ...}");
}

TEST(MatcherPathTypingTest, NegatedNarrowPathToMissingKeepsWholeArrayBracket) {
    ASSERT_EQ(matcher::narrowPath(Type::anyObject(), traversedPath("x"), Type::missing(), false)
                  .toDebugString(),
              "{x: ~missing, ...}");
}

TEST(MatcherPathTypingTest, NarrowPathToNeverLeavesNoDocument) {
    ASSERT_EQ(matcher::narrowPath(Type::anyObject(), traversedPath("x"), Type::never(), true)
                  .toDebugString(),
              "never");
}

TEST(MatcherPathTypingTest, NarrowPathToMissingOrValueAddsArrays) {
    auto constraint = unionType(Type::missing(), allValues(BSONType::numberInt));
    ASSERT_EQ(matcher::narrowPath(Type::anyObject(), traversedPath("x"), constraint, true)
                  .toDebugString(),
              "{x: missing|int|array(S), ...}");
}

TEST(MatcherPathTypingTest, NarrowPathToDisjointTypeLeavesNoDocument) {
    auto input = openObject({{"x", allValues(BSONType::string)}});
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("x"), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              "never");
}

TEST(MatcherPathTypingTest, NarrowPathKeepsKnownFieldsOfNarrowedField) {
    auto input = openObject({{"x", openObject({{"a", allValues(BSONType::numberInt)}})}});
    ASSERT_EQ(
        matcher::narrowPath(input, traversedPath("x"), Type::anyObject(), true).toDebugString(),
        "{x: {a: int, ...}, ...}");
}

TEST(MatcherPathTypingTest, NarrowPathOnMissingFieldLeavesNoDocument) {
    ASSERT_EQ(matcher::narrowPath(
                  closedObject({}), traversedPath("x"), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              "never");
}

TEST(MatcherPathTypingTest, NarrowPathOnDottedPathDoesNotNarrow) {
    auto input = Type::anyObject();
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("a.b"), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              input.toDebugString());
}

TEST(MatcherPathTypingTest, NarrowPathOnEmptyPathDoesNotNarrow) {
    auto input = Type::anyObject();
    ASSERT_EQ(matcher::narrowPath(input, traversedPath(""), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              input.toDebugString());
}

TEST(MatcherPathTypingTest, NarrowPathOnUnsatisfiableInputIsReturnedUnchanged) {
    ASSERT_EQ(
        matcher::narrowPath(Type::never(), traversedPath("x"), allValues(BSONType::numberInt), true)
            .toDebugString(),
        "never");
}

TEST(MatcherPathTypingTest, NarrowPathOnNonObjectType) {
    auto input = allValues(BSONType::string);
    // TODO(SERVER-134936): Implement narrowing on non-object (does nothing now).
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("x"), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              "string");
}

TEST(MatcherPathTypingTest, NarrowPathToNumberSetKeepsOnlyOverlappingNumericType) {
    auto input = openObject(
        {{"x", unionType(allValues(BSONType::numberDouble), allValues(BSONType::string))}});
    auto numbers = Type(TypeSet::numericTypes(Extent::kAll));
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("x"), numbers, true).toDebugString(),
              "{x: double, ...}");
}

TEST(MatcherPathTypingTest, NegatedNarrowPathToNumberSetLeavesNoDocumentForNumericField) {
    auto input = openObject({{"x", allValues(BSONType::numberLong)}});
    auto numbers = Type(TypeSet::numericTypes(Extent::kAll));
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("x"), numbers, false).toDebugString(),
              "never");
}

}  // namespace
}  // namespace mongo::pipeline::type_system
