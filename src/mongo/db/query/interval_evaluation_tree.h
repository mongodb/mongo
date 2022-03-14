/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <stack>

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"

namespace mongo {

class ConstInterval;
class EvalInterval;
class IntersectInterval;
class UnionInterval;
class ComplementInterval;

/**
 *  A polyvalue which represents a node of Interval Evalution Tree.
 */
using IET = optimizer::algebra::
    PolyValue<ConstInterval, EvalInterval, IntersectInterval, UnionInterval, ComplementInterval>;

/**
 *  Represents an interval with the constant bounds, such as (MinKey, MaxKey)
 */
class ConstInterval : public optimizer::algebra::OpSpecificArity<IET, ConstInterval, 0> {
public:
    explicit ConstInterval(const std::vector<Interval>& intervals) : intervals{intervals} {}

    std::vector<Interval> intervals;
};

/**
 * Evaluates an interval from a simple predicate such as {$gt: p1} where p1 is a parameter value
 * known at runtime.
 */
class EvalInterval : public optimizer::algebra::OpSpecificArity<IET, EvalInterval, 0> {
public:
    using InputParamId = MatchExpression::InputParamId;

    EvalInterval(InputParamId inputParamId, MatchExpression::MatchType matchType)
        : _inputParamId{inputParamId}, _matchType{matchType} {}

    InputParamId inputParamId() const {
        return _inputParamId;
    }

    MatchExpression::MatchType matchType() const {
        return _matchType;
    }

private:
    const InputParamId _inputParamId;
    const MatchExpression::MatchType _matchType;
};

/**
 * Intersects two intervals.
 */
class IntersectInterval : public optimizer::algebra::OpSpecificArity<IET, IntersectInterval, 2> {
public:
    using Base = optimizer::algebra::OpSpecificArity<IET, IntersectInterval, 2>;

    IntersectInterval(IET lhs, IET rhs) : Base(std::move(lhs), std::move(rhs)) {}
};

/**
 * Unions two intervals.
 */
class UnionInterval : public optimizer::algebra::OpSpecificArity<IET, UnionInterval, 2> {
public:
    using Base = optimizer::algebra::OpSpecificArity<IET, UnionInterval, 2>;

    UnionInterval(IET lhs, IET rhs) : Base(std::move(lhs), std::move(rhs)) {}
};

/**
 * ComplementInterval represent operand $not on the child.
 */
class ComplementInterval : public optimizer::algebra::OpSpecificArity<IET, ComplementInterval, 1> {
public:
    using Base = optimizer::algebra::OpSpecificArity<IET, ComplementInterval, 1>;

    ComplementInterval(IET child) : Base(std::move(child)) {}
};

std::string ietToString(const IET& iet);
std::string ietsToString(const IndexEntry& index, const std::vector<IET>& iets);

class IETBuilder {
public:
    void intersectIntervals();
    void unionIntervals();
    void complementInterval();
    void translate(const MatchExpression& expr, const OrderedIntervalList& oil);
    void appendIntervalList(const OrderedIntervalList& oil);

    boost::optional<IET> done() const;

private:
    std::stack<IET> _intervals;
};
}  // namespace mongo
