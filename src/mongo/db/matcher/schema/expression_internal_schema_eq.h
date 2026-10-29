// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/unordered_fields_bsonelement_comparator.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string_view>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * MatchExpression for $_internalSchemaEq, which behaves similar to $eq except:
 *
 * - leaf arrays are not traversed.
 * - comparisons between objects do not consider field order.
 * - null element values only match the literal null, and not missing or undefined values.
 * - always uses simple string comparison semantics, even if the query has a non-simple collation.
 */
class InternalSchemaEqMatchExpression final : public LeafMatchExpression {
public:
    static constexpr std::string_view kName = "$_internalSchemaEq"sv;

    InternalSchemaEqMatchExpression(boost::optional<std::string_view> path,
                                    BSONElement rhs,
                                    clonable_ptr<ErrorAnnotation> annotation = nullptr);

    std::unique_ptr<MatchExpression> clone() const final;

    void debugString(StringBuilder& debug, int indentationLevel) const final;

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const query_shape::SerializationOptions& opts = {},
                                       bool includePath = true) const final;

    bool equivalent(const MatchExpression* other) const final;

    size_t numChildren() const final {
        return 0;
    }

    MatchExpression* getChild(size_t i) const final {
        MONGO_UNREACHABLE_TASSERT(6400213);
    }

    void resetChild(size_t, MatchExpression*) override {
        MONGO_UNREACHABLE;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    const BSONElement& getRhsElem() const {
        return _rhsElem;
    }

    const UnorderedFieldsBSONElementComparator& getComparator() const {
        return _eltCmp;
    }

private:
    UnorderedFieldsBSONElementComparator _eltCmp;
    BSONElement _rhsElem;
};
}  // namespace mongo
