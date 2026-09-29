// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/matcher_typing.h"

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/util/assert_util.h"

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

}  // namespace mongo::pipeline::type_system
