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
#include "mongo/db/query/optimizer/visitor.h"

namespace mongo::optimizer {

Node::Node(Context& ctx) : _nodeId(ctx.getNextNodeId()), _children() {}

Node::Node(Context& ctx, NodePtr child) : _nodeId(ctx.getNextNodeId()) {
    _children.push_back(std::move(child));
}

Node::Node(Context& ctx, std::vector<NodePtr> children)
    : _nodeId(ctx.getNextNodeId()), _children(std::move(children)) {}

void Node::generateMemoBase(std::ostringstream& os) {
    os << "NodeId: " << _nodeId << "\n";
}

std::string Node::generateMemo() {
    std::ostringstream os;

    class MemoVisitor: public AbstractVisitor
    {
    protected:
        void visit(ScanNode& node) override {
            node.generateMemoInternal(os);
        }
        void visit(MultiJoinNode& node) override {
            node.generateMemoInternal(os);
        }
        void visit(UnionNode& node) override {
            node.generateMemoInternal(os);
        }
        void visit(GroupByNode& node) override {
            node.generateMemoInternal(os);
        }
        void visit(UnwindNode& node) override {
            node.generateMemoInternal(os);
        }
        void visit(WindNode& node) override {
            node.generateMemoInternal(os);
        }
    public:
        std::ostringstream os;
    };

    MemoVisitor visitor;
    visitPreOrder(visitor);
    return visitor.os.str();
}

void Node::visitPreOrder(AbstractVisitor& visitor) {
    visit(visitor);
    for (const NodePtr& ptr: _children) {
        ptr->visitPreOrder(visitor);
    }
}

NodePtr ScanNode::create(Context& ctx, CollectionNameType collectionName) {
    return NodePtr(new ScanNode(ctx, std::move(collectionName)));
}

ScanNode::ScanNode(Context& ctx, CollectionNameType collectionName)
    : Node(ctx), _collectionName(std::move(collectionName)) {}

void ScanNode::generateMemoInternal(std::ostringstream& os) {
    Node::generateMemoBase(os);
    os << "Scan" << "\n";
}

void ScanNode::visit(AbstractVisitor& visitor) {
    visitor.visit(*this);
}

NodePtr MultiJoinNode::create(Context& ctx,
                              FilterSet filterSet,
                              ProjectionMap projectionMap,
                              std::vector<NodePtr> children) {
    return NodePtr(new MultiJoinNode(
        ctx, std::move(filterSet), std::move(projectionMap), std::move(children)));
}

MultiJoinNode::MultiJoinNode(Context& ctx,
                             FilterSet filterSet,
                             ProjectionMap projectionMap,
                             std::vector<NodePtr> children)
    : Node(ctx, std::move(children)),
      _filterSet(std::move(filterSet)),
      _projectionMap(std::move(projectionMap)) {}

void MultiJoinNode::generateMemoInternal(std::ostringstream& os) {
    Node::generateMemoBase(os);
    os << "MultiJoin" << "\n";
}

void MultiJoinNode::visit(AbstractVisitor& visitor) {
    visitor.visit(*this);
}

NodePtr UnionNode::create(Context& ctx, std::vector<NodePtr> children) {
    return NodePtr(new UnionNode(ctx, std::move(children)));
}

UnionNode::UnionNode(Context& ctx, std::vector<NodePtr> children)
    : Node(ctx, std::move(children)) {}

void UnionNode::generateMemoInternal(std::ostringstream& os) {
    Node::generateMemoBase(os);
    os << "Union" << "\n";
}

void UnionNode::visit(AbstractVisitor& visitor) {
    visitor.visit(*this);
}

NodePtr GroupByNode::create(Context& ctx,
                            GroupByNode::GroupByVector groupByVector,
                            GroupByNode::ProjectionMap projectionMap,
                            NodePtr child) {
    return NodePtr(
        new GroupByNode(ctx, std::move(groupByVector), std::move(projectionMap), std::move(child)));
}

GroupByNode::GroupByNode(Context& ctx,
                         GroupByNode::GroupByVector groupByVector,
                         GroupByNode::ProjectionMap projectionMap,
                         NodePtr child)
    : Node(ctx, std::move(child)),
      _groupByVector(std::move(groupByVector)),
      _projectionMap(std::move(projectionMap)) {}

void GroupByNode::generateMemoInternal(std::ostringstream& os) {
    Node::generateMemoBase(os);
    os << "GroupBy" << "\n";
}

void GroupByNode::visit(AbstractVisitor& visitor) {
    visitor.visit(*this);
}

NodePtr UnwindNode::create(Context& ctx,
                           ProjectionName projectionName,
                           const bool retainNonArrays,
                           NodePtr child) {
    return NodePtr(
        new UnwindNode(ctx, std::move(projectionName), retainNonArrays, std::move(child)));
}

UnwindNode::UnwindNode(Context& ctx,
                       ProjectionName projectionName,
                       const bool retainNonArrays,
                       NodePtr child)
    : Node(ctx, std::move(child)),
      _projectionName(std::move(projectionName)),
      _retainNonArrays(retainNonArrays) {}

void UnwindNode::generateMemoInternal(std::ostringstream& os) {
    Node::generateMemoBase(os);
    os << "Unwind" << "\n";
}

void UnwindNode::visit(AbstractVisitor& visitor) {
    visitor.visit(*this);
}

NodePtr WindNode::create(Context& ctx, ProjectionName projectionName, NodePtr child) {
    return NodePtr(new WindNode(ctx, std::move(projectionName), std::move(child)));
}

WindNode::WindNode(Context& ctx, ProjectionName projectionName, NodePtr child)
    : Node(ctx, std::move(child)), _projectionName(std::move(projectionName)) {}

void WindNode::generateMemoInternal(std::ostringstream& os) {
    Node::generateMemoBase(os);
    os << "Wind" << "\n";
}

void WindNode::visit(AbstractVisitor& visitor) {
    visitor.visit(*this);
}

}  // namespace mongo::optimizer
