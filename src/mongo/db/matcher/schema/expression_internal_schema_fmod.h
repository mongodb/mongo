// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * MatchExpression for $_internalSchemaFmod keyword. Same as ModMatchExpression but works on
 * decimals.
 */
class InternalSchemaFmodMatchExpression final : public LeafMatchExpression {
public:
    InternalSchemaFmodMatchExpression(boost::optional<std::string_view> path,
                                      Decimal128 divisor,
                                      Decimal128 remainder,
                                      clonable_ptr<ErrorAnnotation> annotation = nullptr);

    std::unique_ptr<MatchExpression> clone() const final {
        std::unique_ptr<InternalSchemaFmodMatchExpression> m =
            std::make_unique<InternalSchemaFmodMatchExpression>(
                path(), _divisor, _remainder, _errorAnnotation);
        if (getTag()) {
            m->setTag(getTag()->clone());
        }
        return m;
    }

    void debugString(StringBuilder& debug, int indentationLevel) const final;

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const query_shape::SerializationOptions& opts = {},
                                       bool includePath = true) const final;

    bool equivalent(const MatchExpression* other) const final;

    Decimal128 getDivisor() const {
        return _divisor;
    }
    Decimal128 getRemainder() const {
        return _remainder;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

private:
    Decimal128 _divisor;
    Decimal128 _remainder;
};
}  // namespace mongo
