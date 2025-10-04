/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cbr_rewrites.h"

#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_hasher.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"

namespace mongo::cost_based_ranker {

namespace {

/**
 * Given a 'path' and an 'interval' on that path, generate a minimal logically equivalent
 * MatchExpression. In principle the expression could be less strict than the interval because it
 * is used for CE, however currently it is strictly equivalent.
 * Example:
 * Consider an open range condition such as {a: {$gt: 42}} and an index on 'a'. This results in an
 * interval [42, inf]. During CE of this interval CBR ends up calling this function to convert it to
 * an equivalent condition, ideally the same one that was used to generate the interval.
 * This function implements careful handling of interval bounds in order to avoid adding
 * logically redundant terms such as the comparison to inf as in this logically equivalent
 * expression: {$and: [{a: {$gt: 42}}, {a: {$lt: inf}}]}
 */
std::unique_ptr<MatchExpression> getMatchExpressionFromInterval(StringData path,
                                                                const Interval& interval) {
    if (interval.isFullyOpen()) {
        // Intervals containing all values of a field can be estimated as True match expression.
        return std::make_unique<AlwaysTrueMatchExpression>();
    }

    if (interval.isNull()) {
        return std::make_unique<AlwaysFalseMatchExpression>();
    }

    if (interval.isUndefined()) {
        // The transformation of undefined interval to match expression is not supported.
        return nullptr;
    }

    if (interval.isPoint()) {
        return std::make_unique<EqualityMatchExpression>(path, interval.start);
    }

    // Create other comparison expressions.
    auto direction = interval.getDirection();
    tassert(10450101,
            "Expected interval with ascending or descending direction",
            direction != Interval::Direction::kDirectionNone);

    std::vector<std::unique_ptr<MatchExpression>> expressions;
    bool isAscending = (direction == Interval::Direction::kDirectionAscending);

    bool gtIncl = (isAscending) ? interval.startInclusive : interval.endInclusive;
    auto& gtVal = (isAscending) ? interval.start : interval.end;
    bool isLB = isLowerBound(gtVal, gtIncl);  // Low type bracket or -inf

    bool ltIncl = (isAscending) ? interval.endInclusive : interval.startInclusive;
    auto& ltVal = (isAscending) ? interval.end : interval.start;
    bool isUB = isUpperBound(ltVal, ltIncl);  // Upper type bracket or inf

    // If this is a type bracket interval create a condition that represents it even if both
    // bounds are minimal/maximal. Such intervals are usually part of an OIL and are complimentary
    // to the other OIL intervals.
    bool isTypeBracketInterval = isLB && isUB;

    if (isTypeBracketInterval || !isLB) {
        // Create an expression for the lower bound only if it is not minimal for the type. This
        // avoids adding redundant always true conditions such as {a: {$gt: -inf}}.
        if (gtIncl) {
            expressions.push_back(std::make_unique<GTEMatchExpression>(path, gtVal));
        } else {
            expressions.push_back(std::make_unique<GTMatchExpression>(path, gtVal));
        };
    }

    if (isTypeBracketInterval || !isUB) {
        // Create an expression for the upper bound only if it is not maximal for the type. This
        // avoids adding redundant always true conditions such as {a: {$lt: inf}}.
        if (ltIncl) {
            expressions.push_back(std::make_unique<LTEMatchExpression>(path, ltVal));
        } else {
            expressions.push_back(std::make_unique<LTMatchExpression>(path, ltVal));
        };
    }

    if (expressions.size() > 1) {
        return std::make_unique<AndMatchExpression>(std::move(expressions));
    }
    if (expressions.size() == 1) {
        return std::move(expressions[0]);
    }
    // The transformation of this interval to match expression is not supported.
    return nullptr;
}

std::unique_ptr<MatchExpression> getMatchExpressionFromOIL(const OrderedIntervalList* oil) {
    if (oil->isFullyOpen()) {
        // Do not create expression for intervals containing all values of a field.
        return std::make_unique<AlwaysTrueMatchExpression>();
    }

    if (oil->intervals.size() == 0) {
        // Edge case when interval intersection is empty. For instance: {$and: [{"a": 1}, {"a" :
        // 5}]}. Create an ALWAYS_FALSE expression.
        return std::make_unique<AlwaysFalseMatchExpression>();
    }

    const auto path = StringData(oil->name);
    std::vector<std::unique_ptr<MatchExpression>> expressions;
    for (const auto& interval : oil->intervals) {
        auto expr = getMatchExpressionFromInterval(path, interval);
        if (!expr) {
            // We found an interval that cannot be transformed to match expression, bail out.
            return nullptr;
        }
        expressions.push_back(std::move(expr));
    }

    if (expressions.size() == 1) {
        return std::move(expressions[0]);
    }
    if (expressions.size() > 1) {
        // Make an OR match expression for the disjunction of intervals.
        return std::make_unique<OrMatchExpression>(std::move(expressions));
    }

    return nullptr;
}

}  // namespace

std::unique_ptr<MatchExpression> getMatchExpressionFromBounds(const IndexBounds& bounds,
                                                              const MatchExpression* filterExpr) {
    std::vector<std::unique_ptr<MatchExpression>> expressions;

    for (auto& oil : bounds.fields) {
        auto disj = getMatchExpressionFromOIL(&oil);
        if (!disj) {
            // We found an OIL with an interval that cannot be transformed to match expression, bail
            // out.
            return nullptr;
        }
        expressions.push_back(std::move(disj));
    }

    if (filterExpr) {
        expressions.push_back(filterExpr->clone());
    }

    if (expressions.size() > 1) {
        // Normalize and simplify the resulting conjunction in order to enable better detection of
        // expression equivalence.
        auto conj = std::make_unique<AndMatchExpression>(std::move(expressions));
        auto simplified = normalizeMatchExpression(std::move(conj), true);
        if (simplified->isTriviallyTrue()) {
            // There are at least two different forms of true expressions. Return the same form.
            return std::make_unique<AlwaysTrueMatchExpression>();
        }
        return simplified;
    }
    if (expressions.size() == 1) {
        return std::move(expressions[0]);
    }
    return nullptr;
}

}  // namespace mongo::cost_based_ranker
