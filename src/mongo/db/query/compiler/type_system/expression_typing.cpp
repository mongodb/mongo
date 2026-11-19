// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/expression_typing.h"

#include "mongo/db/pipeline/expression.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo::pipeline::type_system {
namespace {

/**
 * Computes the output type for the given expression.
 * An expression left without an override sets the output type to 'any', which is always valid.
 */
struct GetOutputTypeExpressionVisitor final
    : public SelectiveConstExpressionVisitorBase<GetOutputTypeExpressionVisitor> {
    using SelectiveConstExpressionVisitorBase<GetOutputTypeExpressionVisitor>::visit;

    GetOutputTypeExpressionVisitor(const Type& inputType) : inputType(inputType) {}

    void visit(const ExpressionConstant* expr) override {
        outputType = Type::fromValue(expr->getValue());
    }

    void visit(const ExpressionArray*) override {
        outputType = Type::someArray();
    }

    void visit(const ExpressionObject*) override {
        outputType = Type(BSONType::object, Extent::kSubset);
    }

    void visit(const ExpressionFieldPath* expr) override {
        if (expr->isVariableReference()) {
            // The value of a variable other than ROOT/CURRENT is not modelled.
            return;
        }
        if (expr->isROOT()) {
            outputType = inputType;
            return;
        }
        outputType = expression::traversePath(inputType,
                                              expr->getFieldPathWithoutCurrentPrefix(),
                                              expression::PathSemantics::kFieldPath);
    }

    void visit(const ExpressionNot* expr) override {
        returnsBoolean();
    }

    void visit(const ExpressionOr* expr) override {
        returnsBoolean();
    }

    void visit(const ExpressionAnd* expr) override {
        returnsBoolean();
    }

    void returnsBoolean() {
        outputType = Type(BSONType::boolean, Extent::kSubset);
    }

    const Type& inputType;
    Type outputType{Type::any()};
};

const ExpressionFieldPath* getIfExpressionFieldPath(const Expression* expr) {
    const ExpressionFieldPath* path = nullptr;
    FieldPathVisitor visitor([&](const ExpressionFieldPath* fieldPath) { path = fieldPath; });
    expr->acceptVisitor(&visitor);
    return path;
}

/**
 * Returns 'inputType' narrowed by all children assuming each evaluates to 'assumeTrue'.
 * With no children, returns 'inputType' unchanged.
 */
Type narrowByEveryChild(Type inputType,
                        const Expression::ExpressionVector& children,
                        bool assumeTrue) {
    for (const auto& child : children) {
        inputType = narrowType(std::move(inputType), child.get(), assumeTrue);
    }
    return inputType;
}

/**
 * Returns the union of input types narrowed by each child assuming it evaluates to 'assumeTrue'.
 * This represents inputs for which at least one child has that outcome; with no children, the
 * result is 'never'.
 */
Type narrowBySomeChild(Type inputType,
                       const Expression::ExpressionVector& children,
                       bool assumeTrue) {
    if (children.size() == 1) {
        return narrowType(std::move(inputType), children.front().get(), assumeTrue);
    }
    Type result = Type::never();
    for (const auto& child : children) {
        result = unionType(std::move(result), narrowType(inputType, child.get(), assumeTrue));
    }
    return result;
}

struct ComparisonOperands {
    /// The field path whose type should be narrowed.
    const ExpressionFieldPath* path;
    /// The inferred type of the other comparison operand.
    Type otherType;
    /// Whether 'path' is the first (left-hand) comparison operand; determines the direction of
    /// the ordered type constraint.
    bool pathIsFirst;
};

/**
 * Finds a field-path operand at 'pathIndex' and infers the type of the other operand using
 * 'inputType'.
 */
boost::optional<ComparisonOperands> findComparisonOperands(
    const Expression::ExpressionVector& children, const Type& inputType, size_t pathIndex) {
    const ExpressionFieldPath* path = getIfExpressionFieldPath(children[pathIndex].get());
    if (!path || path->isVariableReference() || path->isROOT()) {
        return boost::none;
    }

    Type otherType = getOutputType(inputType, children[1 - pathIndex].get());
    if (otherType.getTypeSet() == TypeSet::any()) {
        return boost::none;
    }
    return ComparisonOperands{path, std::move(otherType), pathIndex == 0};
}

/**
 * Returns the type constraint for a comparison expression based on the operands and the comparison
 * operator. 'assumeTrue' indicates whether the comparison is assumed to be true or false. Returns a
 * type representing the constraint on the field-path operand based on the comparison.
 */
Type getComparisonConstraint(const ComparisonOperands& operands,
                             ExpressionCompare::CmpOp op,
                             bool assumeTrue) {
    if (op == ExpressionCompare::EQ || op == ExpressionCompare::NE) {
        const bool assumeEqual = op == ExpressionCompare::EQ ? assumeTrue : !assumeTrue;
        Type constraint(sameCanonicalClasses(operands.otherType.getTypeSet()));
        return assumeEqual ? std::move(constraint) : complement(std::move(constraint));
    }

    const bool isLess = op == ExpressionCompare::LT || op == ExpressionCompare::LTE;
    tassert(13918801,
            "Expected an ordered comparison",
            isLess || op == ExpressionCompare::GT || op == ExpressionCompare::GTE);
    // For a false comparison, reverse the operator (e.g. !(a < b) becomes a >= b).
    // Complementing the inferred constraint instead can exclude valid values when the bound is a
    // union: for b of type number|string, the constraint for a < b includes all numbers, but
    // a >= b can also hold for numbers (e.g. a = 5, b = 1).
    const bool less = (isLess == operands.pathIsFirst) == assumeTrue;
    const bool inclusive =
        (op == ExpressionCompare::LTE || op == ExpressionCompare::GTE) == assumeTrue;
    return Type(orderedComparisonTypes(operands.otherType.getTypeSet(), less, inclusive));
}

/**
 * Narrows the input type by every expression which carries type information.
 * An expression left without an override leaves the input type unchanged, which is always valid.
 */
struct NarrowTypeExpressionVisitor final
    : public SelectiveConstExpressionVisitorBase<NarrowTypeExpressionVisitor> {
    using SelectiveConstExpressionVisitorBase<NarrowTypeExpressionVisitor>::visit;

    NarrowTypeExpressionVisitor(Type inputType, bool assumeTrue)
        : type(std::move(inputType)), assumeTrue(assumeTrue) {}

    void visit(const ExpressionConstant* expr) override {
        if (expr->getValue().coerceToBool() != assumeTrue) {
            type = Type::never();
        }
    }

    void visit(const ExpressionNot* expr) override {
        tassert(13918800, "$not must have one operand", expr->getChildren().size() == 1);
        type = narrowType(std::move(type), expr->getChildren().front().get(), !assumeTrue);
    }

    void visit(const ExpressionAnd* expr) override {
        type = assumeTrue ? narrowByEveryChild(std::move(type), expr->getChildren(), true)
                          : narrowBySomeChild(std::move(type), expr->getChildren(), false);
    }

    void visit(const ExpressionOr* expr) override {
        type = assumeTrue ? narrowBySomeChild(std::move(type), expr->getChildren(), true)
                          : narrowByEveryChild(std::move(type), expr->getChildren(), false);
    }

    void visit(const ExpressionCompare* expr) override {
        const auto op = expr->getOp();

        const auto& children = expr->getChildren();
        tassert(13598400, "ExpressionCompare must have two operands", children.size() == 2);

        const bool isEquality = op == ExpressionCompare::EQ || op == ExpressionCompare::NE;
        const bool isInequality = op == ExpressionCompare::LT || op == ExpressionCompare::LTE ||
            op == ExpressionCompare::GT || op == ExpressionCompare::GTE;
        if (!isEquality && !isInequality) {
            return;
        }

        // Under negated equality, only a singleton type identifies the other operand's value.
        const bool requireSingleton =
            isEquality && (op == ExpressionCompare::EQ ? !assumeTrue : assumeTrue);
        for (size_t pathIndex : {0, 1}) {
            auto operands = findComparisonOperands(children, type, pathIndex);
            if (!operands || (requireSingleton && !operands->otherType.isSingletonType())) {
                continue;
            }
            Type constraint = getComparisonConstraint(*operands, op, assumeTrue);
            type = expression::narrowPath(std::move(type),
                                          operands->path->getFieldPathWithoutCurrentPrefix(),
                                          expression::PathSemantics::kFieldPath,
                                          std::move(constraint),
                                          true);
            return;
        }
    }

    /// The input type, narrowed by every expression which carries type information so far.
    Type type;
    bool assumeTrue;
};

}  // namespace

Type getOutputType(const Type& inputType, const Expression* expr) {
    // A document which cannot exist cannot be evaluated on.
    if (inputType.isNever()) {
        return Type::never();
    }
    tassert(13598201, "Input type must be an object type", inputType.hasOnlyType(BSONType::object));

    GetOutputTypeExpressionVisitor visitor(inputType);
    expr->acceptVisitor(&visitor);

    return std::move(visitor.outputType);
}

Type narrowType(Type inputType, const Expression* expr, bool assumeTrue) {
    // A document which cannot exist cannot be narrowed any further.
    if (inputType.isNever()) {
        return inputType;
    }
    tassert(13598202, "Input type must be an object type", inputType.hasOnlyType(BSONType::object));

    NarrowTypeExpressionVisitor visitor(std::move(inputType), assumeTrue);
    expr->acceptVisitor(&visitor);

    tassert(13598203,
            "Narrowed type must be an object type or never",
            visitor.type.hasOnlyType(BSONType::object) || visitor.type.isNever());
    return std::move(visitor.type);
}

namespace expression {
namespace {
Type traverseFieldPath(Type inputType, const FieldPath& path, size_t index);

/// Returns the type of the given component in the path in given object type.
Type traverseObject(Type inputType, const FieldPath& path, size_t index) {
    Type field = inputType.getField(path.getFieldName(index));
    if (++index == path.getPathLength()) {
        return field;
    }
    return traverseFieldPath(std::move(field), path, index);
}

/// Returns the type 'path' (starting from position 'index') evaluates to on inputType when
/// traversed following FieldPath semantics.
Type traverseFieldPath(Type inputType, const FieldPath& path, size_t index) {
    Type result = Type::never();
    if (inputType.hasType(BSONType::object)) {
        result = traverseObject(inputType, path, index);
    }
    if (inputType.hasType(BSONType::array)) {
        // If there is an array along the path, we treat everything beneath it as possibly an array.
        result = unionType(std::move(result), Type::someArray());
    }
    if (!intersectType(std::move(inputType), Type::anyScalar()).isNever()) {
        // If there may be a scalar along the path, the result might be missing.
        result = unionType(std::move(result), Type::missing());
    }
    return result;
}

/// Returns a type describing documents where traversing 'path' following ExpressionFieldPath
/// semantics can produce 'leaf'.
Type documentTypeFromLeaf(const FieldPath& path, Type leaf) {
    // Traversing a path inside an array causes the result to be wrapped in an array. Hence an array
    // along the path prefix is only possible if the leaf can be one.
    Type prefixTypes = intersectType(leaf, Type::anyArray());
    // Traversing a scalar value produces missing. Hence a scalar type along the path prefix is
    // only possible if the leaf can be missing.
    if (leaf.hasType(BSONType::eoo)) {
        prefixTypes = unionType(std::move(prefixTypes), Type::anyScalar());
    }

    // Build the full nested Type based on what we know about the leaf. E.g. for the path "a.b.c",
    // the full type would be {a: prefixTypes | {b: prefixTypes | {c: leaf}}}.
    Type result = std::move(leaf);
    for (auto i = path.getPathLength(); i-- > 0;) {
        Type object = Type::anyObject();
        object.setField(path.getFieldName(i), std::move(result));
        result = unionType(prefixTypes, std::move(object));
    }
    return result;
}
}  // namespace

Type traversePath(Type inputType, const FieldPath& path, PathSemantics semantics) {
    switch (semantics) {
        case PathSemantics::kFieldPath:
            return traverseFieldPath(std::move(inputType), path, 0);
        case PathSemantics::kOther:
            // Not modelled.
            return Type::any();
    }
    MONGO_UNREACHABLE_TASSERT(13597800);
}

Type narrowPath(Type inputType,
                const FieldPath& path,
                PathSemantics semantics,
                Type constraint,
                bool assumeTrue) {
    // A document which cannot exist cannot be narrowed any further.
    if (inputType.isNever()) {
        return inputType;
    }

    switch (semantics) {
        case PathSemantics::kFieldPath: {
            Type leaf = assumeTrue ? std::move(constraint) : complement(std::move(constraint));
            Type docType = documentTypeFromLeaf(path, std::move(leaf));
            return intersectType(std::move(inputType), std::move(docType));
        }
        case PathSemantics::kOther:
            // Not modelled.
            return inputType;
    }
    MONGO_UNREACHABLE_TASSERT(13598100);
}
}  // namespace expression

}  // namespace mongo::pipeline::type_system
