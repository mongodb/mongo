/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/matcher/expression_leaf.h"

namespace mongo {

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
 */
template <typename T>
class InternalExprComparisonMatchExpression : public ComparisonMatchExpressionBase {
public:
    InternalExprComparisonMatchExpression(MatchType type, StringData path, BSONElement value)
        : ComparisonMatchExpressionBase(type,
                                        path,
                                        Value(value),
                                        ElementPath::LeafArrayBehavior::kNoTraversal,
                                        ElementPath::NonLeafArrayBehavior::kMatchSubpath) {
        invariant(_rhs.type() != BSONType::Undefined);
        invariant(_rhs.type() != BSONType::Array);
    }

    virtual ~InternalExprComparisonMatchExpression() = default;

    bool matchesSingleElement(const BSONElement& elem, MatchDetails* details) const final {
        // We use NonLeafArrayBehavior::kMatchSubpath traversal in
        // InternalExprComparisonMatchExpression. This means matchesSinglElement() will be called
        // when an array is found anywhere along the patch we are matching against. When this
        // occurs, we return 'true' and depend on the corresponding ExprMatchExpression node to
        // filter properly.
        if (elem.type() == BSONType::Array) {
            return true;
        }

        auto comp = elem.woCompare(_rhs, BSONElement::ComparisonRulesSet(0), _collator);

        switch (matchType()) {
            case INTERNAL_EXPR_GT:
                return comp > 0;
            case INTERNAL_EXPR_GTE:
                return comp >= 0;
            case INTERNAL_EXPR_LT:
                return comp < 0;
            case INTERNAL_EXPR_LTE:
                return comp <= 0;
            case INTERNAL_EXPR_EQ:
                return comp == 0;
            default:
                // This is a comparison match expression, so it must be either a $eq, $lt, $lte, $gt
                // or $gte expression.
                MONGO_UNREACHABLE_TASSERT(3994308);
        }
    };

    std::unique_ptr<MatchExpression> shallowClone() const final {
        auto clone = std::make_unique<T>(path(), _rhs);
        clone->setCollator(_collator);
        if (getTag()) {
            clone->setTag(getTag()->clone());
        }
        return clone;
    }

    StringData name() const final {
        return T::kName;
    };
};


class InternalExprEqMatchExpression final
    : public InternalExprComparisonMatchExpression<InternalExprEqMatchExpression> {
public:
    static constexpr StringData kName = "$_internalExprEq"_sd;

    InternalExprEqMatchExpression(StringData path, BSONElement value)
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
    static constexpr StringData kName = "$_internalExprGt"_sd;

    InternalExprGTMatchExpression(StringData path, BSONElement value)
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
    static constexpr StringData kName = "$_internalExprGte"_sd;

    InternalExprGTEMatchExpression(StringData path, BSONElement value)
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
    static constexpr StringData kName = "$_internalExprLt"_sd;

    InternalExprLTMatchExpression(StringData path, BSONElement value)
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
    static constexpr StringData kName = "$_internalExprLte"_sd;

    InternalExprLTEMatchExpression(StringData path, BSONElement value)
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
