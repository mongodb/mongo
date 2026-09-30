// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/matcher_typing.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/tassert_guard.h"
#include "mongo/unittest/unittest.h"

namespace mongo::pipeline::type_system {
namespace {

namespace exec_matcher = ::mongo::exec::matcher;

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

/// Narrows the 'inputType' by the $match expression in 'query' and returns the result.
std::string narrowedDebugString(const std::string& query, Type inputType, bool assumeTrue = true) {
    auto expr = parseMatchExpr(query);
    return narrowType(std::move(inputType), expr.get(), assumeTrue).toDebugString();
}

/**
 * Asserts that every document the predicate leaves possible under 'assumeTrue' holds a value of
 * 'x' the narrowed type admits. Runs the matcher to decide which documents those are.
 */
void assertNarrowingAdmitsEveryMatchingDocument(const std::string& query, bool assumeTrue) {
    static const std::vector<BSONObj> kDocuments = {
        fromjson("{x: 1}"),
        fromjson("{x: 'str'}"),
        fromjson("{x: null}"),
        fromjson("{x: true}"),
        fromjson("{x: {a: 1}}"),
        fromjson("{x: []}"),
        fromjson("{x: [1, 2]}"),
        fromjson("{x: ['str']}"),
        fromjson("{x: [[1]]}"),
        fromjson("{x: [{a: 1}]}"),
        fromjson("{x: {}}"),
        fromjson("{x: [null]}"),
        fromjson("{}"),
    };

    auto expr = parseMatchExpr(query);
    const Type narrowed = narrowType(Type::anyObject(), expr.get(), assumeTrue);
    for (const auto& document : kDocuments) {
        if (exec_matcher::matchesBSON(expr.get(), document) != assumeTrue) {
            continue;
        }
        ASSERT_TRUE(narrowed.getField("x").hasType(document["x"].type()))
            << query << " assumeTrue " << assumeTrue << " admits " << document.toString()
            << " but inferred " << narrowed.toDebugString();
    }
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

TEST(MatcherTypingTest, TypeNarrowsFieldToTypeOrArrayHoldingIt) {
    ASSERT_EQ(narrowedDebugString("{x: {$type: 'number'}}", Type::anyObject()),
              "{x: number|array(S), ...}");
}

TEST(MatcherTypingTest, TypeNarrowsNonArrayFieldToTypeAlone) {
    ASSERT_EQ(narrowedDebugString("{x: {$type: 'number'}}",
                                  openObject({{"x", complement(allValues(BSONType::array))}})),
              "{x: number, ...}");
}

TEST(MatcherTypingTest, NegatedTypeRemovesTypeButLeavesArrays) {
    ASSERT_EQ(narrowedDebugString("{x: {$not: {$type: 'number'}}}", Type::anyObject()),
              "{x: ~(number|array(S)), ...}");
}

TEST(MatcherTypingTest, NegatedTypeOnNonArrayFieldRemovesTypeAndArrays) {
    ASSERT_EQ(narrowedDebugString("{x: {$not: {$type: 'number'}}}",
                                  openObject({{"x", complement(allValues(BSONType::array))}})),
              "{x: ~(number|array), ...}");
}

TEST(MatcherTypingTest, NegatedTypeArrayRemovesArrayBracket) {
    ASSERT_EQ(narrowedDebugString("{x: {$not: {$type: 'array'}}}", Type::anyObject()),
              "{x: ~array, ...}");
}

TEST(MatcherTypingTest, TypeUnionNarrowsFieldToEitherTypeOrArray) {
    ASSERT_EQ(narrowedDebugString("{x: {$type: ['number', 'string']}}", Type::anyObject()),
              "{x: number|string|array(S), ...}");
}

TEST(MatcherTypingTest, TypeArrayNarrowsFieldToArray) {
    ASSERT_EQ(narrowedDebugString("{x: {$type: 'array'}}", Type::anyObject()), "{x: array, ...}");
}

TEST(MatcherTypingTest, TypeNullExcludesMissing) {
    ASSERT_EQ(narrowedDebugString("{x: {$type: 'null'}}", Type::anyObject()),
              "{x: null|array(S), ...}");
}

TEST(MatcherTypingTest, TypeNullDoesNotMatchMissingFieldAndNeitherAdmitsIt) {
    auto expr = parseMatchExpr("{x: {$type: 'null'}}");
    ASSERT_FALSE(exec_matcher::matchesBSON(expr.get(), fromjson("{}")));
    ASSERT_TRUE(exec_matcher::matchesBSON(expr.get(), fromjson("{x: null}")));

    const Type field = narrowType(Type::anyObject(), expr.get(), true).getField("x");
    ASSERT_FALSE(field.hasType(BSONType::eoo));
    ASSERT_TRUE(field.hasType(BSONType::null));
}

TEST(MatcherTypingTest, NegatedTypeNullMatchesMissingFieldAndBothAdmitIt) {
    auto expr = parseMatchExpr("{x: {$not: {$type: 'null'}}}");
    ASSERT_TRUE(exec_matcher::matchesBSON(expr.get(), fromjson("{}")));
    ASSERT_FALSE(exec_matcher::matchesBSON(expr.get(), fromjson("{x: null}")));

    const Type field = narrowType(Type::anyObject(), expr.get(), true).getField("x");
    ASSERT_TRUE(field.hasType(BSONType::eoo));
    ASSERT_FALSE(field.hasType(BSONType::null));
}

TEST(MatcherTypingTest, TypeNullByNumericCodeBehavesLikeTheAlias) {
    auto expr = parseMatchExpr("{x: {$type: 10}}");
    ASSERT_FALSE(exec_matcher::matchesBSON(expr.get(), fromjson("{}")));
    ASSERT_EQ(narrowType(Type::anyObject(), expr.get(), true).toDebugString(),
              "{x: null|array(S), ...}");
}

TEST(MatcherTypingTest, MissingIsNotExpressibleAsAType) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    for (auto query : {"{x: {$type: 'missing'}}", "{x: {$type: 0}}"}) {
        auto status = MatchExpressionParser::parse(fromjson(query),
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus();
        ASSERT_EQ(status.code(), ErrorCodes::BadValue) << query;
        ASSERT_STRING_CONTAINS(status.reason(), "$exists:false");
    }
}

TEST(MatcherTypingTest, TypeObjectKeepsKnownFieldsOfField) {
    const Type input = openObject({{"x", openObject({{"a", allValues(BSONType::numberInt)}})}});
    ASSERT_EQ(narrowedDebugString("{x: {$type: 'object'}}", input), "{x: {a: int, ...}, ...}");
}

TEST(MatcherTypingTest, TypeDisjointFromFieldLeavesNoDocument) {
    ASSERT_EQ(narrowedDebugString("{x: {$type: 'number'}}",
                                  openObject({{"x", allValues(BSONType::string)}})),
              "never");
}

TEST(MatcherTypingTest, TypeNumberOnIntInputFieldKeepsInt) {
    ASSERT_EQ(narrowedDebugString("{x: {$type: 'number'}}",
                                  openObject({{"x", allValues(BSONType::numberInt)}})),
              "{x: int, ...}");
}

TEST(MatcherTypingTest, TypeIntOnNumericInputFieldNarrowsToInt) {
    ASSERT_EQ(narrowedDebugString("{x: {$type: 'int'}}",
                                  openObject({{"x", Type(TypeSet::numericTypes(Extent::kAll))}})),
              "{x: int, ...}");
}

TEST(MatcherTypingTest, TypeIntOnArrayCapableInputFieldKeepsArraysHoldingMatch) {
    const Type input = openObject(
        {{"x", unionType(Type(TypeSet::numericTypes(Extent::kAll)), allValues(BSONType::array))}});
    ASSERT_EQ(narrowedDebugString("{x: {$type: 'int'}}", input), "{x: int|array(S), ...}");
}

TEST(MatcherTypingTest, NegatedTypeNumberLeavesNoDocumentForNumericField) {
    const Type input = openObject({{"x", allValues(BSONType::numberLong)}});
    ASSERT_EQ(narrowedDebugString("{x: {$not: {$type: 'number'}}}", input), "never");
}

TEST(MatcherTypingTest, TypeNumberOrArrayCoversBothBracketsExactly) {
    ASSERT_EQ(narrowedDebugString("{x: {$type: ['number', 'array']}}", Type::anyObject()),
              "{x: number|array, ...}");
}

TEST(MatcherTypingTest, NegatedTypeNumberOrArrayRemovesBothBrackets) {
    ASSERT_EQ(narrowedDebugString("{x: {$not: {$type: ['number', 'array']}}}", Type::anyObject()),
              "{x: ~(number|array), ...}");
}

TEST(MatcherTypingTest, TypeNarrowsOnlyItsOwnField) {
    ASSERT_EQ(narrowedDebugString("{y: {$type: 'number'}}",
                                  openObject({{"x", allValues(BSONType::numberInt)}})),
              "{x: int, y: number|array(S), ...}");
}

TEST(MatcherTypingTest, NegatedTypeKeepsMissingFieldMissing) {
    ASSERT_EQ(narrowedDebugString("{x: {$not: {$type: 'number'}}}", closedObject({})), "{}");
}

TEST(MatcherTypingTest, SuccessiveNarrowingRemovesArrayLeftByTraversal) {
    auto typeNumber = parseMatchExpr("{x: {$type: 'number'}}");
    auto notTypeArray = parseMatchExpr("{x: {$not: {$type: 'array'}}}");
    const Type narrowed = narrowType(Type::anyObject(), typeNumber.get(), true);
    ASSERT_EQ(narrowType(narrowed, notTypeArray.get(), true).toDebugString(), "{x: number, ...}");
}

TEST(MatcherTypingTest, SuccessiveNarrowingToDisjointTypesLeavesNoDocument) {
    auto typeNumber = parseMatchExpr("{x: {$type: 'number'}}");
    auto typeString = parseMatchExpr("{x: {$type: 'string'}}");
    auto notTypeArray = parseMatchExpr("{x: {$not: {$type: 'array'}}}");
    Type narrowed = narrowType(Type::anyObject(), typeNumber.get(), true);
    narrowed = narrowType(narrowed, typeString.get(), true);
    ASSERT_EQ(narrowType(narrowed, notTypeArray.get(), true).toDebugString(), "never");
}

TEST(MatcherTypingTest, NarrowingAdmitsEveryDocumentMatcherLeavesPossible) {
    for (auto query : {"{x: {$type: 'number'}}",
                       "{x: {$type: 'array'}}",
                       "{x: {$type: 'object'}}",
                       "{x: {$type: 'null'}}",
                       "{x: {$type: ['number', 'string']}}",
                       "{x: {$not: {$type: 'number'}}}",
                       "{x: {$not: {$type: 'array'}}}",
                       "{x: {$type: ['number', 'array']}}",
                       "{x: {$not: {$type: ['number', 'array']}}}",
                       "{x: {$type: 'string'}}",
                       "{x: {$not: {$type: 'object'}}}"}) {
        assertNarrowingAdmitsEveryMatchingDocument(query, true);
        assertNarrowingAdmitsEveryMatchingDocument(query, false);
    }
}

}  // namespace
}  // namespace mongo::pipeline::type_system
