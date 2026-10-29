// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/path.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * InternalExprComparisonMatchExpression consists of comparison expressions with similar semantics
 * to the correlating $eq, $gt, $gte, $lt, and $lte aggregation expression. They differ from the
 * regular comparison match expressions in the following ways:
 *
 * - There will be no type bracketing. For instance, null is considered less than 2, and objects
 *   are considered greater than 2.
 *
 * - The document will match if there is an array anywhere along the path. By always returning true
 *   in such cases, we match a superset of documents that the related aggregation expression would
 *   match. This sidesteps us having to implement field path expression evaluation as part of this
 *   match expression.
 *
 * - Comparison to NaN will consider NaN as the less than all numbers.
 *
 * - Comparison to an array is illegal. It is invalid usage to construct a
 *   InternalExprComparisonMatchExpression node which compares to an array.
 *
 * These expressions are always treated as "imprecise" and are to be used as the first level in a
 * two-level filtering scheme. They must always be accompanied by a later filter which is precise.
 */
template <typename T>
class InternalExprComparisonMatchExpression : public ComparisonMatchExpressionBase {
public:
    InternalExprComparisonMatchExpression(MatchType type,
                                          boost::optional<std::string_view> path,
                                          BSONElement value)
        : ComparisonMatchExpressionBase(type,
                                        path,
                                        Value(value),
                                        ElementPath::LeafArrayBehavior::kNoTraversal,
                                        ElementPath::NonLeafArrayBehavior::kMatchSubpath) {
        tassert(11052405, "_rhs cannot be undefined", _rhs.type() != BSONType::undefined);
        tassert(11052406, "_rhs cannot be an array", _rhs.type() != BSONType::array);
    }

    ~InternalExprComparisonMatchExpression() override = default;

    std::unique_ptr<MatchExpression> clone() const final {
        auto clone = std::make_unique<T>(path(), _rhs);
        clone->setCollator(_collator);
        if (getTag()) {
            clone->setTag(getTag()->clone());
        }
        return clone;
    }

    std::string_view name() const final {
        return T::kName;
    };
};


class InternalExprEqMatchExpression final
    : public InternalExprComparisonMatchExpression<InternalExprEqMatchExpression> {
public:
    static constexpr std::string_view kName = "$_internalExprEq"sv;

    InternalExprEqMatchExpression(boost::optional<std::string_view> path, BSONElement value)
        : InternalExprComparisonMatchExpression<InternalExprEqMatchExpression>(
              MatchType::INTERNAL_EXPR_EQ, path, value) {}

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

class InternalExprGTMatchExpression final
    : public InternalExprComparisonMatchExpression<InternalExprGTMatchExpression> {
public:
    static constexpr std::string_view kName = "$_internalExprGt"sv;

    InternalExprGTMatchExpression(boost::optional<std::string_view> path, BSONElement value)
        : InternalExprComparisonMatchExpression<InternalExprGTMatchExpression>(
              MatchType::INTERNAL_EXPR_GT, path, value) {}


    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

class InternalExprGTEMatchExpression final
    : public InternalExprComparisonMatchExpression<InternalExprGTEMatchExpression> {
public:
    static constexpr std::string_view kName = "$_internalExprGte"sv;

    InternalExprGTEMatchExpression(boost::optional<std::string_view> path, BSONElement value)
        : InternalExprComparisonMatchExpression<InternalExprGTEMatchExpression>(
              MatchType::INTERNAL_EXPR_GTE, path, value) {}

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

class InternalExprLTMatchExpression final
    : public InternalExprComparisonMatchExpression<InternalExprLTMatchExpression> {
public:
    static constexpr std::string_view kName = "$_internalExprLt"sv;

    InternalExprLTMatchExpression(boost::optional<std::string_view> path, BSONElement value)
        : InternalExprComparisonMatchExpression<InternalExprLTMatchExpression>(
              MatchType::INTERNAL_EXPR_LT, path, value) {}

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

class InternalExprLTEMatchExpression final
    : public InternalExprComparisonMatchExpression<InternalExprLTEMatchExpression> {
public:
    static constexpr std::string_view kName = "$_internalExprLte"sv;

    InternalExprLTEMatchExpression(boost::optional<std::string_view> path, BSONElement value)
        : InternalExprComparisonMatchExpression<InternalExprLTEMatchExpression>(
              MatchType::INTERNAL_EXPR_LTE, path, value) {}

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

}  // namespace mongo
