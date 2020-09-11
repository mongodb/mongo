/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <functional>
#include <stack>

#include "mongo/db/query/optimizer/memo.h"
#include "mongo/db/query/optimizer/node.h"

namespace mongo::optimizer {

Node::Node(Context& ctx) : _nodeId(ctx.getNextNodeId()) {}

void Node::generateMemoBase(std::ostringstream& os) const {
    os << "NodeId: " << _nodeId << "\n";
}

ScanNode::ScanNode(Context& ctx, CollectionNameType collectionName)
    : Base(), Node(ctx), _collectionName(std::move(collectionName)) {}

void ScanNode::generateMemo(std::ostringstream& os) const {
    Node::generateMemoBase(os);
    os << "Scan"
       << "\n";
}

MultiJoinNode::MultiJoinNode(Context& ctx,
                             FilterSet filterSet,
                             ProjectionMap projectionMap,
                             PolymorphicNodeVector children)
    : Base(std::move(children)),
      Node(ctx),
      _filterSet(std::move(filterSet)),
      _projectionMap(std::move(projectionMap)) {}

void MultiJoinNode::generateMemo(std::ostringstream& os) const {
    Node::generateMemoBase(os);
    os << "MultiJoin"
       << "\n";
}

UnionNode::UnionNode(Context& ctx, PolymorphicNodeVector children)
    : Base(std::move(children)), Node(ctx) {}

void UnionNode::generateMemo(std::ostringstream& os) const {
    Node::generateMemoBase(os);
    os << "Union"
       << "\n";
}

GroupByNode::GroupByNode(Context& ctx,
                         GroupByNode::GroupByVector groupByVector,
                         GroupByNode::ProjectionMap projectionMap,
                         PolymorphicNode child)
    : Base(std::move(child)),
      Node(ctx),
      _groupByVector(std::move(groupByVector)),
      _projectionMap(std::move(projectionMap)) {}

void GroupByNode::generateMemo(std::ostringstream& os) const {
    Node::generateMemoBase(os);
    os << "GroupBy"
       << "\n";
}

UnwindNode::UnwindNode(Context& ctx,
                       ProjectionName projectionName,
                       const bool retainNonArrays,
                       PolymorphicNode child)
    : Base(std::move(child)),
      Node(ctx),
      _projectionName(std::move(projectionName)),
      _retainNonArrays(retainNonArrays) {}

void UnwindNode::generateMemo(std::ostringstream& os) const {
    Node::generateMemoBase(os);
    os << "Unwind"
       << "\n";
}

WindNode::WindNode(Context& ctx, ProjectionName projectionName, PolymorphicNode child)
    : Base(std::move(child)), Node(ctx), _projectionName(std::move(projectionName)) {}

void WindNode::generateMemo(std::ostringstream& os) const {
    Node::generateMemoBase(os);
    os << "Wind"
       << "\n";
}

}  // namespace mongo::optimizer
