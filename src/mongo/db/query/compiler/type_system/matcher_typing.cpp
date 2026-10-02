// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/matcher_typing.h"

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/util/assert_util.h"

#include <utility>

namespace mongo::pipeline::type_system {
namespace {

using LeafArrayBehavior = ElementPath::LeafArrayBehavior;
using NonLeafArrayBehavior = ElementPath::NonLeafArrayBehavior;

/// Returns the type to assume when the predicate is always true.
Type narrowByAlwaysTrue(Type inputType, bool assumeTrue) {
    // Under negation nothing matches.
    return assumeTrue ? std::move(inputType) : Type::never();
}

/// Returns the type to assume when the predicate is always false.
Type narrowByAlwaysFalse(Type inputType, bool assumeTrue) {
    // An always false predicate is the negation of an always true one.
    return narrowByAlwaysTrue(std::move(inputType), !assumeTrue);
}

/**
 * Returns the type to assume when every child matches. This is represented by the intersection of
 * their constraints. 'expr' must have at least 1 child.
 */
Type narrowByEveryChild(Type inputType, const ListOfMatchExpression* expr, bool assumeTrue) {
    tassert(13493401, "Expression must have a child", expr->numChildren() > 0);
    for (size_t i = 0; i < expr->numChildren(); ++i) {
        // Repeatedly narrow the input using every child.
        inputType = narrowType(std::move(inputType), expr->getChild(i), assumeTrue);
    }
    return inputType;
}

/**
 * Returns the type to assume when at least one child matches. This is represented by the union of
 * their constraints. 'expr' must have at least 1 child.
 */
Type narrowBySomeChild(Type inputType, const ListOfMatchExpression* expr, bool assumeTrue) {
    tassert(13493402, "Expression must have a child", expr->numChildren() > 0);
    if (expr->numChildren() == 1) {
        // When we have a single child we can std::move the input to avoid copying.
        return narrowType(std::move(inputType), expr->getChild(0), assumeTrue);
    }
    // We union all of the types of the children here, starting with nothing matching.
    Type narrowed = Type::never();
    for (size_t i = 0; i < expr->numChildren(); ++i) {
        // Independently evaluate the type that would match at this branch.
        Type childType = narrowType(inputType, expr->getChild(i), assumeTrue);
        narrowed = unionType(std::move(narrowed), std::move(childType));
    }
    return narrowed;
}

/// Returns the type to assume when the conjunction of the children of 'expr' is 'assumeTrue'.
Type narrowByConjunction(Type inputType, const ListOfMatchExpression* expr, bool assumeTrue) {
    if (expr->numChildren() == 0) {
        // An empty conjunction matches every document. This is $and: [] and $alwaysTrue which is
        // what the $and case rewrites into.
        return narrowByAlwaysTrue(std::move(inputType), assumeTrue);
    }
    if (assumeTrue) {
        // Under conjunction, every child must match.
        return narrowByEveryChild(std::move(inputType), expr, true);
    }
    // De Morgan: not (A and B) = (not A) or (not B)
    return narrowBySomeChild(std::move(inputType), expr, false);
}

/// Returns the type to assume when the disjunction of the children of 'expr' is 'assumeTrue'.
Type narrowByDisjunction(Type inputType, const ListOfMatchExpression* expr, bool assumeTrue) {
    if (expr->numChildren() == 0) {
        // An empty disjunction matches no document. This is $or: [] and $alwaysFalse which is what
        // the $or case rewrites into.
        return narrowByAlwaysFalse(std::move(inputType), assumeTrue);
    }
    if (assumeTrue) {
        // Under disjunction, at least one child must match, and we do not know which one.
        return narrowBySomeChild(std::move(inputType), expr, true);
    }
    // De Morgan: not (A or B) = (not A) and (not B)
    return narrowByEveryChild(std::move(inputType), expr, false);
}

/**
 * Narrows the input type by every expression which carries type information.
 * An expression left without an override leaves the input type unchanged, which is always valid.
 */
struct NarrowTypeMatchExpressionVisitor final : public SelectiveMatchExpressionVisitorBase<true> {
    using SelectiveMatchExpressionVisitorBase<true>::visit;

    NarrowTypeMatchExpressionVisitor(Type inputType, bool assumeTrue)
        : type(std::move(inputType)), assumeTrue(assumeTrue) {}

    void visit(const TypeMatchExpression* expr) override {
        type = matcher::narrowPath(std::move(type),
                                   *expr->elementPath(),
                                   Type::fromMatcherTypeSet(expr->typeSet()),
                                   assumeTrue);
    }

    void visit(const NotMatchExpression* expr) override {
        type = narrowType(std::move(type), expr->getChild(0), !assumeTrue);
    }

    void visit(const AlwaysTrueMatchExpression* expr) override {
        type = narrowByAlwaysTrue(std::move(type), assumeTrue);
    }

    void visit(const AlwaysFalseMatchExpression* expr) override {
        type = narrowByAlwaysFalse(std::move(type), assumeTrue);
    }

    void visit(const AndMatchExpression* expr) override {
        type = narrowByConjunction(std::move(type), expr, assumeTrue);
    }

    void visit(const OrMatchExpression* expr) override {
        type = narrowByDisjunction(std::move(type), expr, assumeTrue);
    }

    void visit(const NorMatchExpression* expr) override {
        // $nor is just a negated $or.
        type = narrowByDisjunction(std::move(type), expr, !assumeTrue);
    }

    /// The input type, narrowed by every expression which carries type information so far.
    Type type;
    bool assumeTrue;
};

}  // namespace

Type narrowType(Type inputType, const MatchExpression* expr, bool assumeTrue) {
    // A document which cannot exist cannot be narrowed any further.
    if (inputType.isNever()) {
        return inputType;
    }
    tassert(13459201, "Input type must be an object type", inputType.hasOnlyType(BSONType::object));

    NarrowTypeMatchExpressionVisitor visitor(std::move(inputType), assumeTrue);
    expr->acceptVisitor(&visitor);

    tassert(13459202,
            "Narrowed type must be an object type or never",
            visitor.type.hasOnlyType(BSONType::object) || visitor.type.isNever());
    return std::move(visitor.type);
}

namespace matcher {

namespace {
/**
 * Adds the 'array' type to the 'constraint', if an 'array' can contain values matching
 * 'constraint'. Only 'missing' and 'never' cannot be array elements (by construction).
 * In all other cases we always match a subset of some arrays.
 */
Type addMatchingArrays(Type constraint) {
    // 'missing' is the only value that cannot be an array element.
    const auto elementTypes = complement(Type::missing());
    if (intersectType(constraint, elementTypes).isNever()) {
        // If we cannot have any values that is allowable in an array, then we cannot match an
        // element in the array. This propagates 'missing' and 'never' without adding 'array(S)'.
        return constraint;
    }
    // Otherwise, we could have matched a value inside the array. Therefore we either have a value
    // matching the constraint or an array with a value matching the constraint (array with
    // kSubset).
    return unionType(std::move(constraint), Type(BSONType::array, Extent::kSubset));
}

/// Returns a type describing documents where traversing 'fieldRef' can produce 'leaf'.
/// Assumes NonLeafArrayBehavior == kTraverse.
Type documentTypeFromLeaf(const FieldRef& fieldRef, Type leaf) {
    Type prefixTypes = Type::someArray();
    // Traversing a scalar value produces missing. Hence a scalar type along the path prefix is
    // only possible if the leaf can be missing.
    if (leaf.hasType(BSONType::eoo)) {
        prefixTypes = unionType(std::move(prefixTypes), Type::anyScalar());
    }

    // Build the full nested Type based on what we know about the leaf. E.g. for the path "a.b.c",
    // the full type would be {a: prefixTypes | {b: prefixTypes | {c: leaf}}}.
    Type result = std::move(leaf);
    for (auto i = fieldRef.numParts(); i-- > 0;) {
        Type object = Type::anyObject();
        object.setField(fieldRef.getPart(i), std::move(result));
        result = unionType(prefixTypes, std::move(object));
    }
    return result;
}
}  // namespace

Type narrowPath(Type inputType, const ElementPath& path, Type constraint, bool assumeTrue) {
    // A document which cannot exist cannot be narrowed any further.
    if (inputType.isNever()) {
        return inputType;
    }

    const FieldRef& fieldRef = path.fieldRef();
    if (fieldRef.empty()) {
        return inputType;
    }

    switch (path.nonLeafArrayBehavior()) {
        case NonLeafArrayBehavior::kTraverse:
            // Currently the only supported mode.
            break;
        case NonLeafArrayBehavior::kNoTraversal:
        case NonLeafArrayBehavior::kMatchSubpath:
            // Not implemented. The predicates that are currently supported only specify kTraverse.
            return inputType;
    }

    Type matched = std::move(constraint);
    switch (path.leafArrayBehavior()) {
        case LeafArrayBehavior::kTraverse:
            // In this mode, arrays are traversed, and we may be matching a value inside the array.
            matched = addMatchingArrays(std::move(matched));
            break;
        case LeafArrayBehavior::kNoTraversal:
            // Arrays are not traversed, so the value itself is the only candidate.
            break;
        case LeafArrayBehavior::kTraverseOmitArray:
            // Not implemented, since MatchExpressions do not specify it.
            return inputType;
    }

    Type leaf = assumeTrue ? std::move(matched) : complement(matched);
    Type docType = documentTypeFromLeaf(fieldRef, std::move(leaf));
    return intersectType(std::move(inputType), std::move(docType));
}

}  // namespace matcher

}  // namespace mongo::pipeline::type_system
