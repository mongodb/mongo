// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/matcher_typing.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/server_parameter_guard.h"
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

ElementPath noLeafTraversalPath(std::string_view path) {
    return ElementPath(path, ElementPath::LeafArrayBehavior::kNoTraversal);
}

/// Returns boolean indicating whether 'object' is one of the values 'type' covers.
bool admitsObject(const Type& type, const BSONObj& object) {
    if (!type.hasType(BSONType::object)) {
        return false;
    }

    const auto& shape = type.getShape_forTest();
    for (const auto& [fieldName, fieldType] : shape.fields) {
        const BSONElement fieldValue = object[fieldName];
        if (!fieldType.hasType(fieldValue.type())) {
            return false;
        }
        if (fieldValue.type() == BSONType::object && !admitsObject(fieldType, fieldValue.Obj())) {
            return false;
        }
    }

    if (!isOpen(shape.open)) {
        for (auto&& field : object) {
            if (!shape.fields.find(field.fieldNameStringData())) {
                return false;
            }
        }
    }

    return true;
}

/**
 * Generates all nested arrays/objects up to the given depth that contain field names 'b', 'c', '0'
 * or '', and values 1, null, {} or [].
 *
 * For example, depth=3 generates documents like {a: [{b: [1]}]}, {a: {'': {c: []}}} and
 * {a: {0: [null]}}.
 */
std::vector<BSONObj> nestedDocuments(size_t depth) {
    // Each value is held as the only element of a wrapping object.
    std::vector<BSONObj> values = {
        BSON("" << 1), BSON("" << BSONNULL), BSON("" << BSONObj()), BSON("" << BSONArray())};
    for (size_t level = 0; level < depth; ++level) {
        std::vector<BSONObj> nested = values;
        for (auto&& wrapped : values) {
            const BSONElement value = wrapped.firstElement();
            nested.push_back(BSON("" << BSON_ARRAY(value)));
            for (std::string_view fieldName : {"b", "c", "0", ""}) {
                nested.push_back(BSON("" << BSON(fieldName << value)));
            }
        }
        values = std::move(nested);
    }

    std::vector<BSONObj> documents = {BSONObj()};
    for (auto&& wrapped : values) {
        documents.push_back(BSON("a" << wrapped.firstElement()));
    }
    return documents;
}

/// Returns true if the matcher's traversal of 'path' produces a value of a type in 'constraint'.
bool traversalProducesMatch(const ElementPath& path,
                            const BSONObj& document,
                            const Type& constraint) {
    BSONElementIterator cursor(&path, document);
    while (cursor.more()) {
        if (constraint.hasType(cursor.next().element().type())) {
            return true;
        }
    }
    return false;
}

/**
 * Asserts that the narrowed type admits every document for which traversalProducesMatch() returns
 * 'assumeTrue'.
 */
void assertNarrowPathAdmitsEveryMatchingDocument(const std::vector<BSONObj>& documents,
                                                 const ElementPath& path,
                                                 const Type& constraint,
                                                 bool assumeTrue) {
    const Type narrowed = matcher::narrowPath(Type::anyObject(), path, constraint, assumeTrue);
    for (const auto& document : documents) {
        if (traversalProducesMatch(path, document, constraint) != assumeTrue) {
            continue;
        }
        ASSERT_TRUE(admitsObject(narrowed, document))
            << path.fieldRef().dottedField() << " constraint " << constraint.toDebugString()
            << " assumeTrue " << assumeTrue << " leaf traversal "
            << (path.leafArrayBehavior() == ElementPath::LeafArrayBehavior::kTraverse)
            << " legacy null semantics " << path.legacyDottedPathNullSemantics() << " admits "
            << document.toString() << " but inferred " << narrowed.toDebugString();
    }
}

/// Narrows the 'inputType' by 'expr' and returns the result.
std::string narrowedDebugString(const MatchExpression* expr,
                                Type inputType,
                                bool assumeTrue = true) {
    return narrowType(std::move(inputType), expr, assumeTrue).toDebugString();
}

/// Narrows the 'inputType' by the $match expression in 'query' and returns the result.
std::string narrowedDebugString(const std::string& query, Type inputType, bool assumeTrue = true) {
    return narrowedDebugString(parseMatchExpr(query).get(), std::move(inputType), assumeTrue);
}

/// Returns whether the matcher accepts 'document' for 'expr'.
bool matches(const MatchExpression* expr, BSONObj document) {
    return exec_matcher::matchesBSON(expr, document);
}

/// Returns whether the matcher accepts 'document' for the $match expression in 'query'.
bool matches(const std::string& query, const std::string& document) {
    return matches(parseMatchExpr(query).get(), fromjson(document));
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
        fromjson("{x: [1, 'str']}"),
        fromjson("{x: [[1]]}"),
        fromjson("{x: [{a: 1}]}"),
        fromjson("{x: {}}"),
        fromjson("{x: [null]}"),
        fromjson("{}"),
        fromjson("{x: 1, y: 'str', z: {a: 1}}"),
        fromjson("{x: [1, 'str'], y: ['str'], z: [{a: 1}]}"),
    };

    const auto expr = parseMatchExpr(query);
    const Type narrowed = narrowType(Type::anyObject(), expr.get(), assumeTrue);
    for (const auto& document : kDocuments) {
        if (matches(expr.get(), document) != assumeTrue) {
            continue;
        }
        ASSERT_TRUE(narrowed.getField("x").hasType(document["x"].type()))
            << query << " assumeTrue " << assumeTrue << " admits " << document << " but inferred "
            << narrowed.toDebugString();
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
    for (std::string_view dottedPath : {"x", "x.y"}) {
        auto path = ElementPath(dottedPath, ElementPath::LeafArrayBehavior::kTraverseOmitArray);
        ASSERT_EQ(matcher::narrowPath(input, path, constraint, true).toDebugString(),
                  input.toDebugString());
        ASSERT_EQ(matcher::narrowPath(input, path, constraint, false).toDebugString(),
                  input.toDebugString());
    }
}

TEST(MatcherPathTypingTest, UnsupportedNonLeafArrayNarrowsNothing) {
    auto input = openObject({{"a", allValues(BSONType::numberInt)}});
    auto constraint = allValues(BSONType::string);
    for (auto nonLeafArrayBehavior : {ElementPath::NonLeafArrayBehavior::kNoTraversal,
                                      ElementPath::NonLeafArrayBehavior::kMatchSubpath}) {
        for (std::string_view dottedPath : {"a", "a.b"}) {
            auto path = ElementPath(
                dottedPath, ElementPath::LeafArrayBehavior::kTraverse, nonLeafArrayBehavior);
            ASSERT_EQ(matcher::narrowPath(input, path, constraint, true).toDebugString(),
                      input.toDebugString());
            ASSERT_EQ(matcher::narrowPath(input, path, constraint, false).toDebugString(),
                      input.toDebugString());
        }
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

TEST(MatcherPathTypingTest, NarrowDottedPathTraversesObjectsAndArrays) {
    ASSERT_EQ(matcher::narrowPath(
                  Type::anyObject(), traversedPath("a.b"), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              "{a: {b: int|array(S), ...}|array(S), ...}");
}

TEST(MatcherPathTypingTest, NarrowLongerDottedPathTraversesEveryComponent) {
    ASSERT_EQ(matcher::narrowPath(
                  Type::anyObject(), traversedPath("a.b.c"), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              "{a: {b: {c: int|array(S), ...}|array(S), ...}|array(S), ...}");
}

// On an object, numeric path components are treated as normal field names. On an array, they
// refer to array indexes.
TEST(MatcherPathTypingTest, NarrowDottedPathTreatsNumericComponentAsFieldOfObjectAndAsArrayIndex) {
    ASSERT_EQ(matcher::narrowPath(
                  Type::anyObject(), traversedPath("a.0"), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              "{a: {0: int|array(S), ...}|array(S), ...}");
}

TEST(MatcherPathTypingTest, NarrowDottedPathWithoutLeafTraversalKeepsArraysOnPrefix) {
    ASSERT_EQ(
        matcher::narrowPath(
            Type::anyObject(), noLeafTraversalPath("a.b"), allValues(BSONType::numberInt), true)
            .toDebugString(),
        "{a: {b: int, ...}|array(S), ...}");
}

TEST(MatcherPathTypingTest, NegatedNarrowDottedPathKeepsScalarsOnPrefix) {
    ASSERT_EQ(matcher::narrowPath(
                  Type::anyObject(), traversedPath("a.b"), allValues(BSONType::numberInt), false)
                  .toDebugString(),
              "{a: ~(object|array(S))|{b: ~(int|array(S)), ...}, ...}");
}

TEST(MatcherPathTypingTest, NarrowDottedPathToMissingKeepsScalarsOnPrefix) {
    // This is the same as narrowing {'a.b': null}, which matches any document where 'a' is a
    // scalar value.
    auto constraint = unionType(Type::missing(), allValues(BSONType::null));
    // The array on 'a' can't be e.g. [{b: 1}]. Hence only a subset of arrays is covered.
    ASSERT_EQ(matcher::narrowPath(Type::anyObject(), traversedPath("a.b"), constraint, true)
                  .toDebugString(),
              "{a: ~(object|array(S))|{b: missing|null|array(S), ...}, ...}");
}

TEST(MatcherPathTypingTest, NarrowDottedPathOnScalarPrefixLeavesNoDocument) {
    auto input = openObject({{"a", allValues(BSONType::numberInt)}});
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("a.b"), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              "never");
}

TEST(MatcherPathTypingTest, NarrowDottedPathToMissingOnScalarPrefixKeepsTheOriginalType) {
    auto input = openObject({{"a", allValues(BSONType::numberInt)}});
    auto constraint = unionType(Type::missing(), allValues(BSONType::null));
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("a.b"), constraint, true).toDebugString(),
              "{a: int, ...}");
}

TEST(MatcherPathTypingTest, NegatedNarrowDottedPathOnScalarPrefixKeepsTheOriginalType) {
    auto input = openObject({{"a", allValues(BSONType::numberInt)}});
    ASSERT_EQ(
        matcher::narrowPath(input, traversedPath("a.b"), allValues(BSONType::numberInt), false)
            .toDebugString(),
        "{a: int, ...}");
}

TEST(MatcherPathTypingTest, NegatedNarrowDottedPathToMissingOnScalarPrefixLeavesNoDocument) {
    auto input = openObject({{"a", allValues(BSONType::numberInt)}});
    auto constraint = unionType(Type::missing(), allValues(BSONType::null));
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("a.b"), constraint, false).toDebugString(),
              "never");
}

TEST(MatcherPathTypingTest, NarrowDottedPathOnMissingPrefix) {
    auto input = closedObject({});
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("a.b"), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              "never");
    auto constraint = unionType(Type::missing(), allValues(BSONType::null));
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("a.b"), constraint, true).toDebugString(),
              "{}");
}

TEST(MatcherPathTypingTest, NarrowDottedPathOnNonArrayPrefixLeavesOnlyObjects) {
    auto input = openObject({{"a", complement(Type::anyArray())}});
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("a.b"), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              "{a: {b: int|array(S), ...}, ...}");
}

TEST(MatcherPathTypingTest, NarrowDottedPathKeepsKnownFieldsOfPrefix) {
    auto input = openObject({{"a", openObject({{"x", allValues(BSONType::string)}})}});
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("a.b"), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              "{a: {b: int|array(S), x: string, ...}, ...}");
}

TEST(MatcherPathTypingTest, NarrowDottedPathToDisjointTypeLeavesNoDocument) {
    auto input = openObject({{"a", openObject({{"b", allValues(BSONType::string)}})}});
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("a.b"), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              "never");
}

TEST(MatcherPathTypingTest, NarrowDottedPathOnClosedPrefix) {
    auto input = openObject({{"a", closedObject({})}});
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("a.b"), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              "never");
    auto constraint = unionType(Type::missing(), allValues(BSONType::null));
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("a.b"), constraint, true).toDebugString(),
              "{a: {}, ...}");
}

TEST(MatcherPathTypingTest, NarrowPathAdmitsEveryDocumentTheTraversalLeavesPossible) {
    const auto documents = nestedDocuments(3);
    const std::vector<Type> constraints = {
        allValues(BSONType::numberInt),
        allValues(BSONType::null),
        Type::missing(),
        unionType(Type::missing(), allValues(BSONType::null)),
        Type::anyArray(),
        Type::anyObject(),
        unionType(allValues(BSONType::numberInt), Type::anyArray()),
        complement(Type::missing()),
        Type::any(),
    };
    for (bool legacyNullSemantics : {true, false}) {
        unittest::ServerParameterGuard legacyGuard{"internalQueryLegacyDottedPathNullSemantics",
                                                   legacyNullSemantics};
        for (std::string_view dottedPath :
             {"a", "a.b", "a.b.c", "a.0", "a.0.b", "a.b.0", "a.", "a..b"}) {
            for (auto leafBehavior : {ElementPath::LeafArrayBehavior::kTraverse,
                                      ElementPath::LeafArrayBehavior::kNoTraversal}) {
                const ElementPath path(dottedPath, leafBehavior);
                for (const auto& constraint : constraints) {
                    assertNarrowPathAdmitsEveryMatchingDocument(documents, path, constraint, true);
                    assertNarrowPathAdmitsEveryMatchingDocument(documents, path, constraint, false);
                }
            }
        }
    }
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
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("x"), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              "never");
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("x"), Type::missing(), true).toDebugString(),
              "string");
}

TEST(MatcherPathTypingTest, NarrowPathOnObjectOrNonObjectTypeKeepsOnlyObjects) {
    auto input = unionType(Type::anyObject(), allValues(BSONType::string));
    ASSERT_EQ(matcher::narrowPath(input, traversedPath("x"), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              "{x: int|array(S), ...}");
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

TEST(MatcherPathTypingTest, NarrowPathWithoutLeafTraversalAddsNoArrays) {
    ASSERT_EQ(matcher::narrowPath(
                  Type::anyObject(), noLeafTraversalPath("x"), allValues(BSONType::numberInt), true)
                  .toDebugString(),
              "{x: int, ...}");
}

TEST(MatcherPathTypingTest, NegatedNarrowPathWithoutLeafTraversalKeepsArrays) {
    ASSERT_EQ(
        matcher::narrowPath(
            Type::anyObject(), noLeafTraversalPath("x"), allValues(BSONType::numberInt), false)
            .toDebugString(),
        "{x: ~int, ...}");
}

TEST(MatcherPathTypingTest, NarrowPathWithoutLeafTraversalToArrayCoversWholeBracket) {
    ASSERT_EQ(matcher::narrowPath(
                  Type::anyObject(), noLeafTraversalPath("x"), allValues(BSONType::array), true)
                  .toDebugString(),
              "{x: array, ...}");
}

TEST(MatcherPathTypingTest, NegatedNarrowPathWithoutLeafTraversalToArrayRemovesWholeBracket) {
    ASSERT_EQ(matcher::narrowPath(
                  Type::anyObject(), noLeafTraversalPath("x"), allValues(BSONType::array), false)
                  .toDebugString(),
              "{x: ~array, ...}");
}

TEST(MatcherPathTypingTest, NarrowPathWithoutLeafTraversalToMissingOrValueAddsNoArrays) {
    auto constraint = unionType(Type::missing(), allValues(BSONType::numberInt));
    ASSERT_EQ(matcher::narrowPath(Type::anyObject(), noLeafTraversalPath("x"), constraint, true)
                  .toDebugString(),
              "{x: missing|int, ...}");
}

TEST(MatcherPathTypingTest, NarrowPathWithoutLeafTraversalToNeverLeavesNoDocument) {
    ASSERT_EQ(matcher::narrowPath(Type::anyObject(), noLeafTraversalPath("x"), Type::never(), true)
                  .toDebugString(),
              "never");
}

TEST(MatcherPathTypingTest, NarrowPathWithoutLeafTraversalDropsArrayFromField) {
    auto input =
        openObject({{"x", unionType(allValues(BSONType::numberInt), allValues(BSONType::array))}});
    ASSERT_EQ(
        matcher::narrowPath(input, noLeafTraversalPath("x"), allValues(BSONType::numberInt), true)
            .toDebugString(),
        "{x: int, ...}");
}

TEST(MatcherPathTypingTest, NarrowPathWithoutLeafTraversalOnArrayFieldLeavesNoDocument) {
    auto input = openObject({{"x", allValues(BSONType::array)}});
    ASSERT_EQ(
        matcher::narrowPath(input, noLeafTraversalPath("x"), allValues(BSONType::numberInt), true)
            .toDebugString(),
        "never");
}

TEST(MatcherPathTypingTest, NegatedNarrowPathWithoutLeafTraversalKeepsArrayField) {
    auto input =
        openObject({{"x", unionType(allValues(BSONType::numberInt), allValues(BSONType::array))}});
    ASSERT_EQ(
        matcher::narrowPath(input, noLeafTraversalPath("x"), allValues(BSONType::numberInt), false)
            .toDebugString(),
        "{x: array, ...}");
}

TEST(MatcherPathTypingTest, NarrowPathWithoutLeafTraversalToDisjointTypeLeavesNoDocument) {
    auto input = openObject({{"x", allValues(BSONType::string)}});
    ASSERT_EQ(
        matcher::narrowPath(input, noLeafTraversalPath("x"), allValues(BSONType::numberInt), true)
            .toDebugString(),
        "never");
}

TEST(MatcherPathTypingTest, NarrowPathWithoutLeafTraversalKeepsKnownFieldsOfNarrowedField) {
    auto input = openObject({{"x", openObject({{"a", allValues(BSONType::numberInt)}})}});
    ASSERT_EQ(matcher::narrowPath(input, noLeafTraversalPath("x"), Type::anyObject(), true)
                  .toDebugString(),
              "{x: {a: int, ...}, ...}");
}

TEST(MatcherPathTypingTest, NarrowPathWithoutLeafTraversalOnMissingFieldLeavesNoDocument) {
    auto input = closedObject({});
    ASSERT_EQ(
        matcher::narrowPath(input, noLeafTraversalPath("x"), allValues(BSONType::numberInt), true)
            .toDebugString(),
        "never");
}

TEST(MatcherPathTypingTest, NarrowPathWithoutLeafTraversalToMissingOnPresentFieldLeavesNoDocument) {
    auto input = openObject({{"x", allValues(BSONType::numberInt)}});
    ASSERT_EQ(
        matcher::narrowPath(input, noLeafTraversalPath("x"), Type::missing(), true).toDebugString(),
        "never");
}

TEST(MatcherPathTypingTest, NarrowPathWithoutLeafTraversalToNumberSetKeepsOnlyOverlappingType) {
    auto input = openObject(
        {{"x", unionType(allValues(BSONType::numberDouble), allValues(BSONType::string))}});
    auto numbers = Type(TypeSet::numericTypes(Extent::kAll));
    ASSERT_EQ(matcher::narrowPath(input, noLeafTraversalPath("x"), numbers, true).toDebugString(),
              "{x: double, ...}");
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

TEST(MatcherTypingTest, EmptyPredicateNarrowsNothing) {
    auto input = Type::anyObject();
    auto predicate = "{}";
    auto expected = "object";
    ASSERT_TRUE(matches(predicate, "{}"));
    ASSERT_TRUE(matches(predicate, "{x: 1}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NegatedEmptyPredicateLeavesNoDocument) {
    auto input = Type::anyObject();
    auto predicate = "{$nor: [{}]}";
    // $or of one child is that child, the {} is always true and the negation is always false.
    auto expected = "never";
    ASSERT_FALSE(matches(predicate, "{}"));
    ASSERT_FALSE(matches(predicate, "{x: 1}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NegatedEmptyPredicateInAndLeavesNoDocument) {
    auto input = Type::anyObject();
    auto predicate = "{$nor: [{$and: [{}]}]}";
    // $and of one is just that child, {} is always true and the negation is always false.
    auto expected = "never";
    ASSERT_FALSE(matches(predicate, "{x: 1}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NegatedEmptyPredicateInOrLeavesNoDocument) {
    auto input = Type::anyObject();
    auto predicate = "{$nor: [{$or: [{}]}]}";
    auto expected = "never";
    ASSERT_FALSE(matches(predicate, "{x: 1}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, AlwaysTrueMatchesEveryDocument) {
    auto input = Type::anyObject();
    auto predicate = "{$alwaysTrue: 1}";
    auto expected = "object";
    ASSERT_TRUE(matches(predicate, "{x: 1}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NegatedAlwaysTrueMatchesNoDocument) {
    auto input = Type::anyObject();
    auto predicate = "{$nor: [{$alwaysTrue: 1}]}";
    // Negation of everything matches is nothing matches.
    auto expected = "never";
    ASSERT_FALSE(matches(predicate, "{x: 1}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, AlwaysFalseMatchesNoDocument) {
    auto input = Type::anyObject();
    auto predicate = "{$alwaysFalse: 1}";
    auto expected = "never";
    ASSERT_FALSE(matches(predicate, "{x: 1}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NegatedAlwaysFalseMatchesEveryDocument) {
    auto input = Type::anyObject();
    auto predicate = "{$nor: [{$alwaysFalse: 1}]}";
    // Negation of nothing matches is everything matches (every object).
    auto expected = "object";
    ASSERT_TRUE(matches(predicate, "{x: 1}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, EmptyAndMatchesEveryDocument) {
    // $and: [] doesn't directly parse, but can be created from rewrites and {} the empty predicate
    // is just an empty AndMatchExpression too, so we need to handle these cases.
    auto input = Type::anyObject();
    AndMatchExpression predicate;
    // And $and: [], same as {}, same as $alwaysTrue matches everything.
    auto expected = "object";
    ASSERT_TRUE(matches(&predicate, fromjson("{}")));
    ASSERT_EQ(narrowedDebugString(&predicate, input), expected);
}

TEST(MatcherTypingTest, NegatedEmptyAndMatchesNoDocument) {
    auto input = Type::anyObject();
    auto predicate = NorMatchExpression(std::make_unique<AndMatchExpression>());
    // Empty $and under negation (with $nor) matching nothing.
    auto expected = "never";
    ASSERT_FALSE(matches(&predicate, fromjson("{}")));
    ASSERT_EQ(narrowedDebugString(&predicate, input), expected);
}

TEST(MatcherTypingTest, EmptyOrMatchesNoDocument) {
    auto input = Type::anyObject();
    OrMatchExpression predicate;
    // $or requires at least one child to be true, an empty $or never matches.
    auto expected = "never";
    ASSERT_FALSE(matches(&predicate, fromjson("{}")));
    ASSERT_EQ(narrowedDebugString(&predicate, input), expected);
}

TEST(MatcherTypingTest, NegatedEmptyOrMatchesEveryDocument) {
    auto input = Type::anyObject();
    auto predicate = NorMatchExpression(std::make_unique<OrMatchExpression>());
    // $nor: [{$or: []}] negates the empty $or (matches nothing) to match everything.
    auto expected = "object";
    ASSERT_TRUE(matches(&predicate, fromjson("{}")));
    ASSERT_EQ(narrowedDebugString(&predicate, input), expected);
}

TEST(MatcherTypingTest, EmptyNorMatchesEveryDocument) {
    auto input = Type::anyObject();
    // $nor: [] not parsable directly, but we know that empty $or matches nothing, so negation
    // matches everything.
    NorMatchExpression predicate;
    auto expected = "object";
    ASSERT_TRUE(matches(&predicate, fromjson("{}")));
    ASSERT_EQ(narrowedDebugString(&predicate, input), expected);
}

TEST(MatcherTypingTest, NegatedEmptyNorMatchesNoDocument) {
    auto input = Type::anyObject();
    auto predicate = NorMatchExpression(std::make_unique<NorMatchExpression>());
    // $nor: [{$nor: []}] same as $not: {$or: [{$not: {$or: []}}]], so "not not nothing" cancels out
    // to match nothing.
    auto expected = "never";
    ASSERT_FALSE(matches(&predicate, fromjson("{}")));
    ASSERT_EQ(narrowedDebugString(&predicate, input), expected);
}

TEST(MatcherTypingTest, AndCombinesTheConstraintsOfEveryChild) {
    auto input = Type::anyObject();
    auto predicate =
        "{$and: [{x: {$type: 'number'}}, "
        "        {x: {$not: {$type: 'array'}}}]}";
    // The not array lets us eliminate the array(S) we normally get from {$type: 'number'}.
    auto expected = "{x: number, ...}";
    ASSERT_TRUE(matches(predicate, "{x: 1}"));
    ASSERT_FALSE(matches(predicate, "{x: [1]}"));
    ASSERT_FALSE(matches(predicate, "{x: 'str'}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, AndNarrowsEveryFieldItsChildrenConstrain) {
    auto input = Type::anyObject();
    // This is just another way of writing: {x: {$type: ...}, y: {$type: ...}} of course.
    auto predicate =
        "{$and: [{x: {$type: 'number'}}, "
        "        {y: {$type: 'string'}}]}";
    auto expected =
        "{x: number|array(S),"
        " y: string|array(S), ...}";
    ASSERT_TRUE(matches(predicate, "{x: 1, y: 'str'}"));
    ASSERT_TRUE(matches(predicate, "{x: [1], y: ['str']}"));
    ASSERT_FALSE(matches(predicate, "{x: 'str', y: 'str'}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, AndOfDisjointChildrenNarrowsToArraysHoldingBoth) {
    auto input = Type::anyObject();
    auto predicate =
        "{$and: [{x: {$type: 'number'}}, "
        "        {x: {$type: 'string'}}]}";
    // Both conjuncts allow only one type and array with that element. And (number and string)
    // cannot hold, so both get cleared. The only thing that could pass is array(S) which holds both
    // a string and a number element.
    auto expected = "{x: array(S), ...}";
    ASSERT_TRUE(matches(predicate, "{x: [1, 'str']}"));
    ASSERT_FALSE(matches(predicate, "{x: 1}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, AndOfDisjointChildrenLeavesNoDocumentForNonArrayField) {
    // Same as above case, but if 'x' is already known to be non-array!
    auto input = openObject({{"x", complement(allValues(BSONType::array))}});
    auto predicate =
        "{$and: [{x: {$type: 'number'}}, "
        "        {x: {$type: 'string'}}]}";
    // 'x' is non-array from input, and (number and string) cannot happen for a non-array.
    // It is logically the same as having AND(not array, number, string).
    auto expected = "never";
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NegatedAndNarrowsByTheNegationOfSomeChild) {
    auto input = Type::anyObject();
    auto predicate =
        "{$nor: [{$and: [{x: {$type: 'number'}}, "
        "                {x: {$not: {$type: 'array'}}}]}]}";
    // NOT(OR(AND(number, not array))), simplifies to NOT(number) OR NOT(NOT(array)) (De Morgan), so
    // NOT(number) OR array.
    auto expected = "{x: ~number, ...}";
    ASSERT_TRUE(matches(predicate, "{x: [1, 2]}"));
    ASSERT_FALSE(matches(predicate, "{x: 1}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NegatedAndOfOneChildNarrowsByThatChild) {
    auto input = Type::anyObject();
    auto predicate = "{$nor: [{$and: [{x: {$type: 'array'}}]}]}";
    // Simplifies to NOT(array).
    auto expected = "{x: ~array, ...}";
    ASSERT_TRUE(matches(predicate, "{x: 1}"));
    ASSERT_FALSE(matches(predicate, "{x: [1]}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, AndWithAlwaysTrueChildNarrowsByOtherChild) {
    auto input = Type::anyObject();
    auto predicate =
        "{$and: [{$alwaysTrue: 1}, "
        "        {x: {$type: 'number'}}]}";
    auto expected = "{x: number|array(S), ...}";
    ASSERT_TRUE(matches(predicate, "{x: 1}"));
    ASSERT_FALSE(matches(predicate, "{x: 'str'}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, AndWithAlwaysFalseChildLeavesNoDocument) {
    auto input = Type::anyObject();
    auto predicate =
        "{$and: [{$alwaysFalse: 1}, "
        "        {x: {$type: 'number'}}]}";
    auto expected = "never";
    ASSERT_FALSE(matches(predicate, "{x: 1}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, TypeOnSeveralPathsNarrowsEveryPath) {
    auto input = Type::anyObject();
    // The {x, y, z} form is just an $and, so same rules apply.
    auto predicate =
        "{x: {$type: 'number'}, "
        " y: {$type: 'string'}, "
        " z: {$type: 'object'}}";
    auto expected =
        "{x: number|array(S),"
        " y: string|array(S),"
        " z: object|array(S), ...}";
    ASSERT_TRUE(matches(predicate, "{x: 1, y: 'str', z: {a: 1}}"));
    ASSERT_TRUE(matches(predicate, "{x: [1], y: ['str'], z: [{a: 1}]}"));
    ASSERT_FALSE(matches(predicate, "{x: 'str', y: 'str', z: {a: 1}}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NegatedTypeOnSeveralPathsRemovesEveryType) {
    auto input = Type::anyObject();
    auto predicate =
        "{x: {$not: {$type: 'number'}}, "
        " y: {$not: {$type: 'array'}}}";
    // Note for 'x' we remove array(S), not all arrays.
    auto expected =
        "{x: ~(number|array(S)),"
        " y: ~array, ...}";
    ASSERT_TRUE(matches(predicate, "{x: 'str', y: 1}"));
    ASSERT_FALSE(matches(predicate, "{x: 1, y: 1}"));
    ASSERT_FALSE(matches(predicate, "{x: 'str', y: [1]}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, TypeOnSeveralPathsLeavesNoDocumentWhenOnePathIsDisjoint) {
    auto input = openObject({{"y", allValues(BSONType::numberInt)}});
    auto predicate =
        "{x: {$type: 'number'}, "
        " y: {$type: 'string'}}";
    // The predicate matches {x: number, y: string}, but our input is {y: number} which is a
    // tautology, nothing matches past that point.
    auto expected = "never";
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, TypeOnDottedAndNonDottedPathsNarrowsBoth) {
    auto input = Type::anyObject();
    auto predicate =
        "{x: {$type: 'number'}, "
        " 'a.b': {$type: 'string'}}";
    auto expected = "{a: {b: string|array(S), ...}|array(S), x: number|array(S), ...}";
    ASSERT_TRUE(matches(predicate, "{x: 1, a: {b: 'str'}}"));
    ASSERT_FALSE(matches(predicate, "{x: 'str', a: {b: 'str'}}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, OrForgetsFieldsWhichNotEveryChildConstrains) {
    auto input = Type::anyObject();
    auto predicate =
        "{$or: [{x: {$type: 'number'}}, "
        "       {y: {$type: 'string'}}]}";
    // We cannot know which alternative applies so there is no more constrained type. All we can say
    // is "some objects" match.
    auto expected = "object(S)";
    ASSERT_TRUE(matches(predicate, "{x: 1}"));
    ASSERT_TRUE(matches(predicate, "{y: 'str'}"));
    ASSERT_FALSE(matches(predicate, "{x: 'str', y: 1}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NegatedTypeOnSeveralPathsForgetsEveryPath) {
    auto input = Type::anyObject();
    auto predicate =
        "{$nor: [{x: {$type: 'number'}, "
        "         y: {$type: 'string'}}]}";
    // NOT(OR(x:number, y:string)) is same as AND(NOT(x:number), NOT(y:string)) (De Morgan), which
    // is uninformative. Some objects are excluded, but in general we know nothing about the shape.
    auto expected = "object(S)";
    ASSERT_TRUE(matches(predicate, "{x: 1}"));
    ASSERT_TRUE(matches(predicate, "{y: 'str'}"));
    ASSERT_FALSE(matches(predicate, "{x: 1, y: 'str'}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, OrNarrowsToTheUnionOfItsChildren) {
    auto input = Type::anyObject();
    auto predicate =
        "{$or: [{x: {$type: 'number'}}, "
        "       {x: {$type: 'string'}}]}";
    auto expected = "{x: number|string|array(S), ...}";
    ASSERT_TRUE(matches(predicate, "{x: 1}"));
    ASSERT_TRUE(matches(predicate, "{x: 'str'}"));
    ASSERT_FALSE(matches(predicate, "{x: true}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, OrOfOneChildNarrowsByThatChild) {
    auto input = Type::anyObject();
    auto predicate = "{$or: [{x: {$type: 'number'}}]}";
    auto expected = "{x: number|array(S), ...}";
    ASSERT_TRUE(matches(predicate, "{x: 1}"));
    ASSERT_TRUE(matches(predicate, "{x: [1]}"));
    ASSERT_FALSE(matches(predicate, "{x: 'str'}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NegatedOrOfOneChildNarrowsByThatChild) {
    auto input = Type::anyObject();
    auto predicate = "{$nor: [{$or: [{x: {$type: 'number'}}]}]}";
    auto expected = "{x: ~(number|array(S)), ...}";
    ASSERT_FALSE(matches(predicate, "{x: 1}"));
    ASSERT_FALSE(matches(predicate, "{x: [1]}"));
    ASSERT_TRUE(matches(predicate, "{x: 'string'}"));
    ASSERT_TRUE(matches(predicate, "{x: ['string']}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, OrWithAlwaysTrueChildNarrowsNothing) {
    auto input = Type::anyObject();
    auto predicate =
        "{$or: [{$alwaysTrue: 1}, "
        "       {x: {$type: 'number'}}]}";
    auto expected = "object";
    ASSERT_TRUE(matches(predicate, "{x: 1}"));
    ASSERT_TRUE(matches(predicate, "{x: 'str'}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, OrWithAlwaysFalseChildNarrowsByOtherChild) {
    auto input = Type::anyObject();
    auto predicate =
        "{$or: [{$alwaysFalse: 1}, "
        "       {x: {$type: 'number'}}]}";
    auto expected = "{x: number|array(S), ...}";
    ASSERT_TRUE(matches(predicate, "{x: 1}"));
    ASSERT_FALSE(matches(predicate, "{x: 'str'}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, OrKeepsTheFieldsNoChildConstrains) {
    auto input = openObject(
        {{"x", complement(allValues(BSONType::array))}, {"y", allValues(BSONType::numberInt)}});
    auto predicate =
        "{$or: [{x: {$type: 'number'}}, "
        "       {x: {$type: 'string'}}]}";
    // We know from the input 'x' is non-array, and we incorporate the number OR string.
    // 'y' remains.
    auto expected =
        "{x: number|string,"
        " y: int, ...}";
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, OrKeepsOnlySatisfiableChildren) {
    auto input = openObject({{"x", complement(allValues(BSONType::array))}});
    auto predicate =
        "{$or: [{$and: [{x: {$type: 'number'}}, "
        "               {x: {$type: 'string'}}]}, "
        "       {x: {$type: 'string'}}]}";
    // First branch AND(number, string) allows nothing (input says non-array), and the second branch
    // allows only string, so we keep the string.
    auto expected = "{x: string, ...}";
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, OrWithoutSatisfiableChildLeavesNoDocument) {
    auto input = openObject({{"x", allValues(BSONType::boolean)}});
    auto predicate =
        "{$or: [{x: {$type: 'number'}}, "
        "       {x: {$type: 'string'}}]}";
    // The predicate allows number|string, but the input says 'x' is boolean, so this is a
    // tautology, nothing can match.
    auto expected = "never";
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NegatedOrNarrowsByTheNegationOfEveryChild) {
    auto input = Type::anyObject();
    auto predicate =
        "{$nor: [{$or: [{x: {$type: 'number'}}, "
        "               {y: {$type: 'string'}}]}]}";
    auto expected =
        "{x: ~(number|array(S)),"
        " y: ~(string|array(S)), ...}";
    ASSERT_TRUE(matches(predicate, "{x: 'str', y: 1}"));
    ASSERT_FALSE(matches(predicate, "{x: 1, y: 1}"));
    ASSERT_FALSE(matches(predicate, "{x: 'str', y: 'str'}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NorOfTypeArrayRemovesArrayBracket) {
    auto input = Type::anyObject();
    auto predicate = "{$nor: [{x: {$type: 'array'}}]}";
    auto expected = "{x: ~array, ...}";
    ASSERT_TRUE(matches(predicate, "{x: 1}"));
    ASSERT_FALSE(matches(predicate, "{x: [1]}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NorRemovesTheTypeOfEveryChild) {
    auto input = Type::anyObject();
    auto predicate =
        "{$nor: [{x: {$type: 'number'}}, "
        "        {y: {$type: 'string'}}]}";
    auto expected =
        "{x: ~(number|array(S)),"
        " y: ~(string|array(S)), ...}";
    ASSERT_TRUE(matches(predicate, "{x: 'str', y: 1}"));
    ASSERT_FALSE(matches(predicate, "{x: 1, y: 1}"));
    ASSERT_FALSE(matches(predicate, "{x: 'str', y: 'str'}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NegatedNorNarrowsToTheUnionOfItsChildren) {
    auto input = Type::anyObject();
    auto predicate =
        "{$nor: [{$nor: [{x: {$type: 'number'}}, "
        "                {x: {$type: 'string'}}]}]}";
    auto expected = "{x: number|string|array(S), ...}";
    ASSERT_TRUE(matches(predicate, "{x: 1}"));
    ASSERT_TRUE(matches(predicate, "{x: 'str'}"));
    ASSERT_FALSE(matches(predicate, "{x: true}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NegatedNorOfOneChildNarrowsByThatChild) {
    auto input = Type::anyObject();
    auto predicate = "{$nor: [{$nor: [{x: {$type: 'number'}}]}]}";
    auto expected = "{x: number|array(S), ...}";
    ASSERT_TRUE(matches(predicate, "{x: 1}"));
    ASSERT_TRUE(matches(predicate, "{x: [1]}"));
    ASSERT_FALSE(matches(predicate, "{x: 'str'}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NorWithAlwaysTrueChildLeavesNoDocument) {
    auto input = Type::anyObject();
    auto predicate =
        "{$nor: [{$alwaysTrue: 1}, "
        "        {x: {$type: 'number'}}]}";
    auto expected = "never";
    ASSERT_FALSE(matches(predicate, "{x: 1}"));
    ASSERT_FALSE(matches(predicate, "{x: 'str'}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NorWithAlwaysFalseChildRemovesTypeOfOtherChild) {
    auto input = Type::anyObject();
    auto predicate =
        "{$nor: [{$alwaysFalse: 1}, "
        "        {x: {$type: 'number'}}]}";
    auto expected = "{x: ~(number|array(S)), ...}";
    ASSERT_TRUE(matches(predicate, "{x: 'str'}"));
    ASSERT_FALSE(matches(predicate, "{x: 1}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, AndOfOrNarrowsByBoth) {
    auto input = Type::anyObject();
    auto predicate =
        "{$and: [{$or: [{x: {$type: 'number'}}, "
        "               {x: {$type: 'string'}}]}, "
        "        {x: {$not: {$type: 'array'}}}]}";
    auto expected = "{x: number|string, ...}";
    ASSERT_TRUE(matches(predicate, "{x: 1}"));
    ASSERT_TRUE(matches(predicate, "{x: 'str'}"));
    ASSERT_FALSE(matches(predicate, "{x: [1]}"));
    ASSERT_FALSE(matches(predicate, "{x: true}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, OrOfNorNarrowsByBoth) {
    auto input = Type::anyObject();
    auto predicate =
        "{$or: [{$nor: [{x: {$type: 'array'}}]}, "
        "       {$nor: [{x: {$type: 'string'}}]}]}";
    auto expected = "{x: ~array(S), ...}";
    ASSERT_TRUE(matches(predicate, "{x: 1}"));
    ASSERT_TRUE(matches(predicate, "{x: [1]}"));
    ASSERT_FALSE(matches(predicate, "{x: ['str']}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, TypeOnDottedPathNarrowsEveryComponent) {
    auto input = Type::anyObject();
    auto predicate = "{'a.b': {$type: 'number'}}";
    auto expected = "{a: {b: number|array(S), ...}|array(S), ...}";
    ASSERT_TRUE(matches(predicate, "{a: {b: 1}}"));
    ASSERT_TRUE(matches(predicate, "{a: [{b: 1}]}"));
    ASSERT_TRUE(matches(predicate, "{a: {b: [1]}}"));
    ASSERT_FALSE(matches(predicate, "{a: 1}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NegatedTypeArrayOnDottedPathNarrowsOnlyObjectsOnPrefix) {
    auto predicate = "{'a.b': {$not: {$type: 'array'}}}";
    ASSERT_TRUE(matches(predicate, "{a: 1}"));
    ASSERT_TRUE(matches(predicate, "{a: [{b: 1}]}"));
    ASSERT_FALSE(matches(predicate, "{a: {b: [1]}}"));
    // 'a' may hold any value, but an object holds no array at 'b'.
    ASSERT_EQ(narrowedDebugString(predicate, Type::anyObject()),
              "{a: ~(object|array(S))|{b: ~array, ...}, ...}");
}

TEST(MatcherTypingTest, TypeNullOnDottedPathLeavesNoScalarOnPrefix) {
    auto input = Type::anyObject();
    auto predicate = "{'a.b': {$type: 'null'}}";
    // $type: 'null' does not match missing, which is what a scalar produces for 'a.b'.
    auto expected = "{a: {b: null|array(S), ...}|array(S), ...}";
    ASSERT_TRUE(matches(predicate, "{a: {b: null}}"));
    ASSERT_FALSE(matches(predicate, "{a: 5}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NegatedTypeNullOnDottedPathKeepsScalarsOnPrefix) {
    auto predicate = "{'a.b': {$not: {$type: 'null'}}}";
    ASSERT_TRUE(matches(predicate, "{a: 5}"));
    ASSERT_FALSE(matches(predicate, "{a: {b: null}}"));
    ASSERT_EQ(narrowedDebugString(predicate, Type::anyObject()),
              "{a: ~(object|array(S))|{b: ~(null|array(S)), ...}, ...}");
}

TEST(MatcherTypingTest, SuccessiveNarrowingOfEveryPrefixRemovesEveryArray) {
    Type narrowed = Type::anyObject();
    for (auto query : {"{'a.b.c': {$type: 'number'}}",
                       "{a: {$not: {$type: 'array'}}}",
                       "{'a.b': {$not: {$type: 'array'}}}",
                       "{'a.b.c': {$not: {$type: 'array'}}}"}) {
        narrowed = narrowType(std::move(narrowed), parseMatchExpr(query).get(), true);
    }
    ASSERT_EQ(narrowed.toDebugString(), "{a: {b: {c: number, ...}, ...}, ...}");
}

TEST(MatcherTypingTest, AndOfNonArrayPrefixAndDottedPathLeavesOnlyObjectsOnPrefix) {
    auto input = Type::anyObject();
    auto predicate =
        "{$and: [{a: {$not: {$type: 'array'}}}, "
        "        {'a.b': {$type: 'number'}}]}";
    auto expected = "{a: {b: number|array(S), ...}, ...}";
    ASSERT_TRUE(matches(predicate, "{a: {b: 1}}"));
    ASSERT_FALSE(matches(predicate, "{a: [{b: 1}]}"));
    ASSERT_FALSE(matches(predicate, "{a: 1}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, OrOfDottedPathsNarrowsToTheUnionOfTheirLeaves) {
    auto input = Type::anyObject();
    auto predicate =
        "{$or: [{'a.b': {$type: 'number'}}, "
        "       {'a.b': {$type: 'string'}}]}";
    auto expected = "{a: {b: number|string|array(S), ...}|array(S), ...}";
    ASSERT_TRUE(matches(predicate, "{a: {b: 1}}"));
    ASSERT_TRUE(matches(predicate, "{a: [{b: 'str'}]}"));
    ASSERT_FALSE(matches(predicate, "{a: {b: true}}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NorOfDottedPathsRemovesTheTypeOfEveryLeaf) {
    auto input = openObject({{"a", Type::anyObject()}});
    auto predicate =
        "{$nor: [{'a.b': {$type: 'number'}}, "
        "        {'a.c': {$type: 'string'}}]}";
    auto expected = "{a: {b: ~(number|array(S)), c: ~(string|array(S)), ...}, ...}";
    ASSERT_TRUE(matches(predicate, "{a: {b: 'str', c: 1}}"));
    ASSERT_TRUE(matches(predicate, "{a: {}}"));
    ASSERT_FALSE(matches(predicate, "{a: {b: 1}}"));
    ASSERT_FALSE(matches(predicate, "{a: {c: ['str']}}"));
    ASSERT_EQ(narrowedDebugString(predicate, input), expected);
}

TEST(MatcherTypingTest, NarrowingOnDottedPathAdmitsEveryDocumentMatcherLeavesPossible) {
    const auto documents = nestedDocuments(3);
    for (auto query : {"{'a.b': {$type: 'number'}}",
                       "{'a.b': {$type: 'null'}}",
                       "{'a.b': {$type: 'array'}}",
                       "{'a.b': {$type: 'object'}}",
                       "{'a.b.c': {$type: 'number'}}",
                       "{'a.0': {$type: 'number'}}",
                       "{'a.0.b': {$type: 'number'}}",
                       "{'a.b.0': {$type: ['null', 'array']}}",
                       "{'a.b': {$not: {$type: 'number'}}}",
                       "{'a.b': {$not: {$type: 'array'}}}",
                       "{'a.b.c': {$not: {$type: 'null'}}}"}) {
        const auto expr = parseMatchExpr(query);
        for (bool assumeTrue : {true, false}) {
            const Type narrowed = narrowType(Type::anyObject(), expr.get(), assumeTrue);
            for (const auto& document : documents) {
                if (matches(expr.get(), document) != assumeTrue) {
                    continue;
                }
                ASSERT_TRUE(admitsObject(narrowed, document))
                    << query << " assumeTrue " << assumeTrue << " admits " << document
                    << " but inferred " << narrowed.toDebugString();
            }
        }
    }
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
