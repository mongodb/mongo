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

namespace mongo::interval_evaluation_tree {
class ConstNode;
class EvalNode;
class IntersectNode;
class UnionNode;
class ComplementNode;

/**
 *  IET is a polyvalue that represents a node of Interval Evaluation Tree.
 */
using IET =
    optimizer::algebra::PolyValue<ConstNode, EvalNode, IntersectNode, UnionNode, ComplementNode>;

/**
 *  ConstNode is a node that represents an interval with constant bounds, such as (MinKey,
 * MaxKey).
 */
class ConstNode : public optimizer::algebra::OpFixedArity<IET, 0> {
public:
    explicit ConstNode(const OrderedIntervalList& oil) : oil{oil} {}

    const OrderedIntervalList oil;
};

/**
 * EvalNode is a node that evaluates an interval from a simple predicate such as {$gt: p1} where
 * p1 is a parameter value known at runtime.
 */
class EvalNode : public optimizer::algebra::OpFixedArity<IET, 0> {
public:
    using InputParamId = MatchExpression::InputParamId;

    EvalNode(InputParamId inputParamId, MatchExpression::MatchType matchType)
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
 * IntersectNode is a node that represents an intersection of two intervals.
 */
class IntersectNode : public optimizer::algebra::OpFixedArity<IET, 2> {
public:
    using Base = optimizer::algebra::OpFixedArity<IET, 2>;

    IntersectNode(IET lhs, IET rhs) : Base(std::move(lhs), std::move(rhs)) {}
};

/**
 * UnionNode is a node that represents a union of two intervals.
 */
class UnionNode : public optimizer::algebra::OpFixedArity<IET, 2> {
public:
    using Base = optimizer::algebra::OpFixedArity<IET, 2>;

    UnionNode(IET lhs, IET rhs) : Base(std::move(lhs), std::move(rhs)) {}
};

/**
 * ComplementNode is a node that complements its child.
 */
class ComplementNode : public optimizer::algebra::OpFixedArity<IET, 1> {
public:
    using Base = optimizer::algebra::OpFixedArity<IET, 1>;

    ComplementNode(IET child) : Base(std::move(child)) {}
};

std::string ietToString(const IET& iet);
std::string ietsToString(const IndexEntry& index, const std::vector<IET>& iets);

class Builder {
public:
    void addIntersect();
    void addUnion();
    void addComplement();
    void addEval(const MatchExpression& expr, const OrderedIntervalList& oil);
    void addConst(const OrderedIntervalList& oil);

    bool isEmpty() const;
    void pop();

    boost::optional<IET> done() const;

private:
    std::stack<IET> _intervals;
};

/**
 * Evaluate OrderedIntervalList for the given MatchExpression tree using pre-built IET.
 *
 * @param iet is Interval Evaluation Tree to evaluate index intervals
 * @param inputParamIdMap is a map from assigned inputParamId to MatchExpression, it is used to
 * evaluate EvalNodes
 * @param elt is the index pattern field for which intervals are evaluated
 * @param index is the index entry for which intervals are evaluated
 * @return evaluted ordered interval list
 */
OrderedIntervalList evaluateIntervals(const IET& iet,
                                      const std::vector<const MatchExpression*>& inputParamIdMap,
                                      const BSONElement& elt,
                                      const IndexEntry& index);
}  // namespace mongo::interval_evaluation_tree
