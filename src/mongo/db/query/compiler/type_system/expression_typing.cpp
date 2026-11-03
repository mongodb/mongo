// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/expression_typing.h"

#include "mongo/db/pipeline/expression.h"
#include "mongo/util/assert_util.h"

#include <utility>

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

    void visit(const ExpressionOr* expr) override {
        returnsBoolean();
    }

    void returnsBoolean() {
        outputType = Type(BSONType::boolean, Extent::kSubset);
    }

    const Type& inputType;
    Type outputType{Type::any()};
};

/**
 * Narrows the input type by every expression which carries type information.
 * An expression left without an override leaves the input type unchanged, which is always valid.
 */
struct NarrowTypeExpressionVisitor final
    : public SelectiveConstExpressionVisitorBase<NarrowTypeExpressionVisitor> {
    using SelectiveConstExpressionVisitorBase<NarrowTypeExpressionVisitor>::visit;

    NarrowTypeExpressionVisitor(Type inputType, bool assumeTrue)
        : type(std::move(inputType)), assumeTrue(assumeTrue) {}

    void visit(const ExpressionOr* expr) override {
        // TODO(SERVER-135985): Implement fully as we do for MatchExpressions.
        if (expr->getChildren().empty() && assumeTrue) {
            // An empty $or evaluates to false, so it matches no document.
            type = Type::never();
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
