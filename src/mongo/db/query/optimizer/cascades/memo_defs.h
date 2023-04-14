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

#include "mongo/db/query/optimizer/cascades/rewriter_rules.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/node_defs.h"


namespace mongo::optimizer::cascades {

/**
 * This file contains auxiliary memo structures. Those can be explained without needing access to
 * the memo itself.
 */

/**
 * Deep hashing, compatible with deep equality comparison.
 */
struct MemoNodeRefHash {
    size_t operator()(const ABT::reference_type& nodeRef) const;
};

/**
 * Deep equality comparison.
 */
struct MemoNodeRefCompare {
    bool operator()(const ABT::reference_type& left, const ABT::reference_type& right) const;
};

/**
 * A set of ABT which keeps track of the order in which we inserted them.
 *
 * Compares ABTs using deep equality.
 */
class OrderPreservingABTSet {
public:
    OrderPreservingABTSet() = default;
    OrderPreservingABTSet(const OrderPreservingABTSet&) = delete;
    OrderPreservingABTSet(OrderPreservingABTSet&&) = default;

    OrderPreservingABTSet& operator=(const OrderPreservingABTSet&) = delete;
    OrderPreservingABTSet& operator=(OrderPreservingABTSet&&) = default;


    ABT::reference_type at(size_t index) const;
    std::pair<size_t, bool> emplace_back(ABT node);
    boost::optional<size_t> find(ABT::reference_type node) const;

    void clear();

    size_t size() const;
    const ABTVector& getVector() const;

private:
    opt::unordered_map<ABT::reference_type, size_t, MemoNodeRefHash, MemoNodeRefCompare> _map;
    ABTVector _vector;
};

/**
 * Maintains currently best physical node and its associated cost.
 */
struct PhysNodeInfo {
    ABT _node;

    // Total cost for the entire subtree.
    CostType _cost;

    // Operator cost (without including the subtree).
    CostType _localCost;

    // For display purposes, adjusted cardinality based on physical properties (e.g. Repetition and
    // Limit-Skip).
    CEType _adjustedCE;

    // Rule that triggered the creation of this node.
    PhysicalRewriteType _rule;

    // Node-specific cardinality estimates, for explain.
    NodeCEMap _nodeCEMap;
};

/**
 * Maintains result of optimization under particular physical properties and a cost limit.
 */
struct PhysOptimizationResult {
    PhysOptimizationResult();
    PhysOptimizationResult(size_t index, properties::PhysProps physProps, CostType costLimit);

    const size_t _index;
    const properties::PhysProps _physProps;

    CostType _costLimit;
    // If set, we have successfully optimized.
    boost::optional<PhysNodeInfo> _nodeInfo;
    // Rejected physical plans.
    std::vector<PhysNodeInfo> _rejectedNodeInfo;
};

using PhysNodeVector = std::vector<std::unique_ptr<PhysOptimizationResult>>;

}  // namespace mongo::optimizer::cascades
