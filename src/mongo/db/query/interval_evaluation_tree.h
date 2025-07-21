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

#include "mongo/bson/bsonelement.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/algebra/operator.h"
#include "mongo/db/query/algebra/polyvalue.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"

#include <map>
#include <stack>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::interval_evaluation_tree {
class ConstNode;
class EvalNode;
class IntersectNode;
class UnionNode;
class ComplementNode;
class ExplodeNode;

/**
 *  IET is a polyvalue that represents a node of Interval Evaluation Tree.
 */
using IET =
    algebra::PolyValue<ConstNode, EvalNode, IntersectNode, UnionNode, ComplementNode, ExplodeNode>;

/**
 *  ConstNode is a node that represents an interval with constant bounds, such as (MinKey,
 * MaxKey).
 */
class ConstNode : public algebra::OpFixedArity<IET, 0> {
public:
    explicit ConstNode(const OrderedIntervalList& oil) : oil{oil} {}

    bool operator==(const ConstNode& other) const {
        return oil == other.oil;
    }

    const OrderedIntervalList oil;
};

/**
 * EvalNode is a node that evaluates an interval from a simple predicate such as {$gt: p1} where
 * p1 is a parameter value known at runtime.
 */
class EvalNode : public algebra::OpFixedArity<IET, 0> {
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

    bool operator==(const EvalNode& other) const {
        return _inputParamId == other._inputParamId && _matchType == other._matchType;
    }

private:
    const InputParamId _inputParamId;
    const MatchExpression::MatchType _matchType;
};

/**
 * ExplodeNode expects the child node to produce a union of point intervals, and it picks a single
 * point interval from the union, given the index to pick from. This node is used by the
 * "explode for sort" optimization in the query planner. It also takes a 'cacheKey' that can be used
 * to search in the evaluation cache to avoid re-evaluating child.
 */
class ExplodeNode : public algebra::OpFixedArity<IET, 1> {
public:
    using Base = algebra::OpFixedArity<IET, 1>;
    using CacheKey = std::pair<int, int>;

    /**
     * The 'cacheKey' is a pair of integers ('nodeIndex', 'patternIndex'). The 'nodeIndex'
     * identifies which unexploded index scan this explosion originates from, and 'patternIndex'
     * identifies which part of sort pattern this IET is for. 'index' is the index to pick from the
     * list of point intervals.
     */
    ExplodeNode(IET child, CacheKey cacheKey, int index)
        : Base(std::move(child)), _cacheKey(cacheKey), _index(index) {}

    CacheKey cacheKey() const {
        return _cacheKey;
    }

    int index() const {
        return _index;
    }

    bool operator==(const ExplodeNode& other) const {
        return _cacheKey == other._cacheKey && _index == other._index && allChildrenEqual(other);
    }

private:
    const CacheKey _cacheKey;
    const int _index;
};

/**
 * IntersectNode is a node that represents an intersection of two intervals.
 */
class IntersectNode : public algebra::OpFixedArity<IET, 2> {
public:
    using Base = algebra::OpFixedArity<IET, 2>;

    IntersectNode(IET lhs, IET rhs) : Base(std::move(lhs), std::move(rhs)) {}

    bool operator==(const IntersectNode& other) const {
        return allChildrenEqual(other);
    }
};

/**
 * UnionNode is a node that represents a union of two intervals.
 */
class UnionNode : public algebra::OpFixedArity<IET, 2> {
public:
    using Base = algebra::OpFixedArity<IET, 2>;

    UnionNode(IET lhs, IET rhs) : Base(std::move(lhs), std::move(rhs)) {}

    bool operator==(const UnionNode& other) const {
        return allChildrenEqual(other);
    }
};

/**
 * ComplementNode is a node that complements its child.
 */
class ComplementNode : public algebra::OpFixedArity<IET, 1> {
public:
    using Base = algebra::OpFixedArity<IET, 1>;

    ComplementNode(IET child) : Base(std::move(child)) {}

    bool operator==(const ComplementNode& other) const {
        return allChildrenEqual(other);
    }
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
    void addExplode(ExplodeNode::CacheKey cacheKey, int index);

    bool isEmpty() const;
    void pop();

    boost::optional<IET> done();

private:
    std::stack<IET> _intervals;
    bool _doneHasBeenCalled{false};
};

/**
 * A cache used by 'ExplodeNode' to avoid recomputing common IET evaluation results.
 */
struct IndexBoundsEvaluationCache {
    std::map<ExplodeNode::CacheKey, OrderedIntervalList> unexplodedOils;
};

/**
 * Evaluate OrderedIntervalList for the given MatchExpression tree using pre-built IET.
 *
 * @param iet is Interval Evaluation Tree to evaluate index intervals
 * @param inputParamIdMap is a map from assigned inputParamId to MatchExpression, it is used to
 * evaluate EvalNodes
 * @param elt is the index pattern field for which intervals are evaluated
 * @param index is the index entry for which intervals are evaluated
 * @param cache is the evaluation cache used by the explode nodes to avoid recomputing the common
 * IET evaluation results
 * @return evaluated ordered interval list
 */
OrderedIntervalList evaluateIntervals(const IET& iet,
                                      const std::vector<const MatchExpression*>& inputParamIdMap,
                                      const BSONElement& elt,
                                      const IndexEntry& index,
                                      IndexBoundsEvaluationCache* cache = nullptr);
}  // namespace mongo::interval_evaluation_tree
