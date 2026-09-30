// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/matcher_typing.h"

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/util/assert_util.h"

#include <utility>

namespace mongo::pipeline::type_system {
namespace {

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

    void visit(const AndMatchExpression* expr) override {
        if (expr->numChildren() != 1) {
            // TODO(SERVER-134934): Handle this case.
            return;
        }
        type = narrowType(std::move(type), expr->getChild(0), assumeTrue);
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
}  // namespace

Type narrowPath(Type inputType, const ElementPath& path, Type constraint, bool assumeTrue) {
    // A document which cannot exist cannot be narrowed any further.
    if (inputType.isNever()) {
        return inputType;
    }
    if (path.fieldRef().numParts() != 1) {
        // TODO(SERVER-134936): Handle dotted paths.
        return inputType;
    }
    if (!inputType.hasOnlyType(BSONType::object)) {
        // TODO(SERVER-134936): Handle path traversal into non-object.
        return inputType;
    }

    Type matched = std::move(constraint);
    switch (path.leafArrayBehavior()) {
        case ElementPath::LeafArrayBehavior::kTraverse:
            // In this mode, arrays are traversed, and we may be matching a value inside the array.
            matched = addMatchingArrays(std::move(matched));
            break;
        case ElementPath::LeafArrayBehavior::kNoTraversal:
        case ElementPath::LeafArrayBehavior::kTraverseOmitArray:
            // TODO(SERVER-134936): Handle these LeafArrayBehavior cases.
            return inputType;
    }

    // The final field type depends on whether we matched or not.
    Type fieldType = assumeTrue ? std::move(matched) : complement(matched);
    return narrowField(std::move(inputType), path.fieldRef().getPart(0), std::move(fieldType));
}

}  // namespace matcher

}  // namespace mongo::pipeline::type_system
