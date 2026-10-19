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

}  // namespace mongo::pipeline::type_system
