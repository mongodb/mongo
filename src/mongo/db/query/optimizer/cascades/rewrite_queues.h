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

#include <queue>

#include "mongo/db/query/optimizer/cascades/rewriter_rules.h"
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/utils/rewriter_utils.h"

namespace mongo::optimizer::cascades {

/**
 * Keeps track of candidate physical rewrites.
 */
struct LogicalRewriteEntry {
    LogicalRewriteEntry(double priority, LogicalRewriteType type, MemoLogicalNodeId nodeId);

    LogicalRewriteEntry() = delete;
    LogicalRewriteEntry(const LogicalRewriteEntry& other) = delete;
    LogicalRewriteEntry(LogicalRewriteEntry&& other) = default;

    // Numerically lower priority gets applied first.
    double _priority;

    LogicalRewriteType _type;
    MemoLogicalNodeId _nodeId;
};

struct LogicalRewriteEntryComparator {
    bool operator()(const std::unique_ptr<LogicalRewriteEntry>& x,
                    const std::unique_ptr<LogicalRewriteEntry>& y) const;
};

using LogicalRewriteQueue = std::priority_queue<std::unique_ptr<LogicalRewriteEntry>,
                                                std::vector<std::unique_ptr<LogicalRewriteEntry>>,
                                                LogicalRewriteEntryComparator>;

/**
 * For now all physical rules use the same priority.
 * TODO: use specific priorities (may depend on node parameters).
 */
static constexpr double kDefaultPriority = 10.0;

/**
 * Keeps track of candidate physical rewrites.
 */
struct PhysRewriteEntry {
    PhysRewriteEntry(double priority,
                     PhysicalRewriteType rule,
                     ABT node,
                     ChildPropsType childProps,
                     NodeCEMap nodeCEMap);

    PhysRewriteEntry() = delete;
    PhysRewriteEntry(const PhysRewriteEntry& other) = delete;
    PhysRewriteEntry(PhysRewriteEntry&& other) = default;

    // Numerically lower priority gets applied first.
    double _priority;
    // Rewrite rule that triggered this entry.
    PhysicalRewriteType _rule;

    ABT _node;
    ChildPropsType _childProps;

    NodeCEMap _nodeCEMap;
};

struct PhysRewriteEntryComparator {
    bool operator()(const std::unique_ptr<PhysRewriteEntry>& x,
                    const std::unique_ptr<PhysRewriteEntry>& y) const;
};

using PhysRewriteQueue = std::priority_queue<std::unique_ptr<PhysRewriteEntry>,
                                             std::vector<std::unique_ptr<PhysRewriteEntry>>,
                                             PhysRewriteEntryComparator>;

void optimizeChildrenNoAssert(PhysRewriteQueue& queue,
                              double priority,
                              PhysicalRewriteType rule,
                              ABT node,
                              ChildPropsType childProps,
                              NodeCEMap nodeCEMap);

template <class T, PhysicalRewriteType rule>
static void optimizeChildren(PhysRewriteQueue& queue,
                             double priority,
                             ABT node,
                             ChildPropsType childProps) {
    static_assert(canBePhysicalNode<T>(), "Can only optimize a physical node.");
    optimizeChildrenNoAssert(queue, priority, rule, std::move(node), std::move(childProps), {});
}

template <class T, PhysicalRewriteType rule>
static void optimizeChild(PhysRewriteQueue& queue,
                          double priority,
                          ABT node,
                          properties::PhysProps childProps) {
    ABT& childRef = node.cast<T>()->getChild();
    optimizeChildren<T, rule>(
        queue, priority, std::move(node), ChildPropsType{{&childRef, std::move(childProps)}});
}

template <class T, PhysicalRewriteType rule>
static void optimizeChild(PhysRewriteQueue& queue, const double priority, ABT node) {
    optimizeChildren<T, rule>(queue, priority, std::move(node), {});
}


template <PhysicalRewriteType rule>
void optimizeUnderNewProperties(cascades::PhysRewriteQueue& queue,
                                const double priority,
                                ABT child,
                                properties::PhysProps props) {
    optimizeChild<FilterNode, rule>(
        queue, priority, wrapConstFilter(std::move(child)), std::move(props));
}

template <class T, PhysicalRewriteType rule>
static void optimizeChildren(PhysRewriteQueue& queue,
                             double priority,
                             ABT node,
                             properties::PhysProps leftProps,
                             properties::PhysProps rightProps) {
    ABT& leftChildRef = node.cast<T>()->getLeftChild();
    ABT& rightChildRef = node.cast<T>()->getRightChild();
    optimizeChildren<T, rule>(
        queue,
        priority,
        std::move(node),
        {{&leftChildRef, std::move(leftProps)}, {&rightChildRef, std::move(rightProps)}});
}

}  // namespace mongo::optimizer::cascades
