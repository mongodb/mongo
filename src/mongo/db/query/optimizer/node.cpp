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

#include "mongo/db/query/optimizer/node.h"

namespace mongo::optimizer {

Node::Node(NodePtr child) {
    _children.push_back(std::move(child));
}

Node::Node(std::vector<NodePtr> children) : _children(std::move(children)) {}

std::string Node::generateMemo() {
    std::ostringstream os;
    generateMemoInternal(os);
    return os.str();
}

NodePtr ScanNode::create(CollectionNameType collectionName) {
    return NodePtr(new ScanNode(std::move(collectionName)));
}

ScanNode::ScanNode(CollectionNameType collectionName)
    : Node(), _collectionName(std::move(collectionName)) {}

void ScanNode::generateMemoInternal(std::ostringstream& os) {
    os << "Scan";
}

NodePtr MultiJoinNode::create(FilterSet filterSet,
                              ProjectionMap projectionMap,
                              std::vector<NodePtr> children) {
    return NodePtr(
        new MultiJoinNode(std::move(filterSet), std::move(projectionMap), std::move(children)));
}

MultiJoinNode::MultiJoinNode(FilterSet filterSet,
                             ProjectionMap projectionMap,
                             std::vector<NodePtr> children)
    : Node(std::move(children)),
      _filterSet(std::move(filterSet)),
      _projectionMap(std::move(projectionMap)) {}

void MultiJoinNode::generateMemoInternal(std::ostringstream& os) {
    os << "MultiJoin";
}

NodePtr UnionNode::create(std::vector<NodePtr> children) {
    return NodePtr(new UnionNode(std::move(children)));
}

UnionNode::UnionNode(std::vector<NodePtr> children) : Node(std::move(children)) {}

void UnionNode::generateMemoInternal(std::ostringstream& os) {
    os << "Union";
}

NodePtr GroupByNode::create(GroupByNode::GroupByVector groupByVector,
                            GroupByNode::ProjectionMap projectionMap,
                            NodePtr child) {
    return NodePtr(
        new GroupByNode(std::move(groupByVector), std::move(projectionMap), std::move(child)));
}

GroupByNode::GroupByNode(GroupByNode::GroupByVector groupByVector,
                         GroupByNode::ProjectionMap projectionMap,
                         NodePtr child)
    : Node(std::move(child)),
      _groupByVector(std::move(groupByVector)),
      _projectionMap(std::move(projectionMap)) {}

void GroupByNode::generateMemoInternal(std::ostringstream& os) {
    os << "GroupBy";
}

NodePtr UnwindNode::create(ProjectionName projectionName,
                           const bool retainNonArrays,
                           NodePtr child) {
    return NodePtr(new UnwindNode(std::move(projectionName), retainNonArrays, std::move(child)));
}

UnwindNode::UnwindNode(ProjectionName projectionName, const bool retainNonArrays, NodePtr child)
    : Node(std::move(child)),
      _projectionName(std::move(projectionName)),
      _retainNonArrays(retainNonArrays) {}

void UnwindNode::generateMemoInternal(std::ostringstream& os) {
    os << "Unwind";
}

NodePtr WindNode::create(ProjectionName projectionName, NodePtr child) {
    return NodePtr(new WindNode(std::move(projectionName), std::move(child)));
}

WindNode::WindNode(ProjectionName projectionName, NodePtr child)
    : Node(std::move(child)), _projectionName(std::move(projectionName)) {}

void WindNode::generateMemoInternal(std::ostringstream& os) {
    os << "Wind";
}

}  // namespace mongo::optimizer
