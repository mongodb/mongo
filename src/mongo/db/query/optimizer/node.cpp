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

#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/visitor.h"
#include "mongo/util/assert_util.h"

namespace mongo::optimizer {

Node::Node(Context& ctx) : _nodeId(ctx.getNextNodeId()), _children() {}

Node::Node(Context& ctx, NodePtr child) : _nodeId(ctx.getNextNodeId()) {
    _children.push_back(std::move(child));
}

Node::Node(Context& ctx, ChildVector children)
    : _nodeId(ctx.getNextNodeId()), _children(std::move(children)) {}

void Node::generateMemoBase(std::ostringstream& os) const {
    os << "NodeId: " << _nodeId << "\n";
}

void Node::visitPreOrder(AbstractVisitor& visitor) const {
    visit(visitor);
    for (const NodePtr& ptr : _children) {
        ptr->visitPreOrder(visitor);
    }
}

void Node::visitPostOrder(AbstractVisitor& visitor) const {
    for (const NodePtr& ptr : _children) {
        ptr->visitPostOrder(visitor);
    }
    visit(visitor);
}

std::string Node::generateMemo() const {
    class MemoVisitor : public AbstractVisitor {
    protected:
        void visit(const ScanNode& node) override {
            node.generateMemo(_os);
        }
        void visit(const MultiJoinNode& node) override {
            node.generateMemo(_os);
        }
        void visit(const UnionNode& node) override {
            node.generateMemo(_os);
        }
        void visit(const GroupByNode& node) override {
            node.generateMemo(_os);
        }
        void visit(const UnwindNode& node) override {
            node.generateMemo(_os);
        }
        void visit(const WindNode& node) override {
            node.generateMemo(_os);
        }

    public:
        std::ostringstream _os;
    };

    MemoVisitor visitor;
    visitPreOrder(visitor);
    return visitor._os.str();
}

NodePtr Node::clone(Context& ctx) const {
    class CloneVisitor : public AbstractVisitor {
    public:
        explicit CloneVisitor(Context& ctx) : _ctx(ctx), _childStack() {}

    protected:
        void visit(const ScanNode& node) override {
            doClone(node, [&](ChildVector v){ return ScanNode::clone(_ctx, node); });
        }
        void visit(const MultiJoinNode& node) override {
            doClone(node, [&](ChildVector v){ return MultiJoinNode::clone(_ctx, node, std::move(v)); });
        }
        void visit(const UnionNode& node) override {
            doClone(node, [&](ChildVector v){ return UnionNode::clone(_ctx, node, std::move(v)); });
        }
        void visit(const GroupByNode& node) override {
            doClone(node, [&](ChildVector v){ return GroupByNode::clone(_ctx, node, std::move(v.at(0))); });
        }
        void visit(const UnwindNode& node) override {
            doClone(node, [&](ChildVector v){ return UnwindNode::clone(_ctx, node, std::move(v.at(0))); });
        }
        void visit(const WindNode& node) override {
            doClone(node, [&](ChildVector v){ return WindNode::clone(_ctx, node, std::move(v.at(0))); });
        }

    private:
        void doClone(const Node& node, const std::function<NodePtr(ChildVector newChildren)>& cloneFn) {
            ChildVector newChildren;
            for (int i = 0; i < node.getChildCount(); i++) {
                newChildren.push_back(std::move(_childStack.top()));
                _childStack.pop();
            }
            _childStack.push(cloneFn(std::move(newChildren)));
        }

    public:
        Context& _ctx;
        std::stack<NodePtr> _childStack;
    };

    CloneVisitor visitor(ctx);
    visitPostOrder(visitor);
    invariant(visitor._childStack.size() == 1);
    return std::move(visitor._childStack.top());
}

int Node::getChildCount() const {
    return _children.size();
}

NodePtr ScanNode::create(Context& ctx, CollectionNameType collectionName) {
    return NodePtr(new ScanNode(ctx, std::move(collectionName)));
}

NodePtr ScanNode::clone(Context& ctx, const ScanNode& other) {
    return create(ctx, other._collectionName);
}

ScanNode::ScanNode(Context& ctx, CollectionNameType collectionName)
    : Node(ctx), _collectionName(std::move(collectionName)) {}

void ScanNode::generateMemo(std::ostringstream& os) const {
    Node::generateMemoBase(os);
    os << "Scan"
       << "\n";
}

void ScanNode::visit(AbstractVisitor& visitor) const {
    visitor.visit(*this);
}

NodePtr MultiJoinNode::create(Context& ctx,
                              FilterSet filterSet,
                              ProjectionMap projectionMap,
                              ChildVector children) {
    return NodePtr(new MultiJoinNode(
        ctx, std::move(filterSet), std::move(projectionMap), std::move(children)));
}

NodePtr MultiJoinNode::clone(Context& ctx, const MultiJoinNode& other, ChildVector newChildren) {
    return create(ctx, other._filterSet, other._projectionMap, std::move(newChildren));
}

MultiJoinNode::MultiJoinNode(Context& ctx,
                             FilterSet filterSet,
                             ProjectionMap projectionMap,
                             ChildVector children)
    : Node(ctx, std::move(children)),
      _filterSet(std::move(filterSet)),
      _projectionMap(std::move(projectionMap)) {}

void MultiJoinNode::generateMemo(std::ostringstream& os) const {
    Node::generateMemoBase(os);
    os << "MultiJoin"
       << "\n";
}

void MultiJoinNode::visit(AbstractVisitor& visitor) const {
    visitor.visit(*this);
}

NodePtr UnionNode::create(Context& ctx, ChildVector children) {
    return NodePtr(new UnionNode(ctx, std::move(children)));
}

NodePtr UnionNode::clone(Context& ctx, const UnionNode& other, ChildVector newChildren) {
    return create(ctx, std::move(newChildren));
}

UnionNode::UnionNode(Context& ctx, ChildVector children)
    : Node(ctx, std::move(children)) {}

void UnionNode::generateMemo(std::ostringstream& os) const {
    Node::generateMemoBase(os);
    os << "Union"
       << "\n";
}

void UnionNode::visit(AbstractVisitor& visitor) const {
    visitor.visit(*this);
}

NodePtr GroupByNode::create(Context& ctx,
                            GroupByNode::GroupByVector groupByVector,
                            GroupByNode::ProjectionMap projectionMap,
                            NodePtr child) {
    return NodePtr(
        new GroupByNode(ctx, std::move(groupByVector), std::move(projectionMap), std::move(child)));
}

NodePtr GroupByNode::clone(Context& ctx, const GroupByNode& other, NodePtr newChild) {
    return create(ctx, other._groupByVector, other._projectionMap, std::move(newChild));
}

GroupByNode::GroupByNode(Context& ctx,
                         GroupByNode::GroupByVector groupByVector,
                         GroupByNode::ProjectionMap projectionMap,
                         NodePtr child)
    : Node(ctx, std::move(child)),
      _groupByVector(std::move(groupByVector)),
      _projectionMap(std::move(projectionMap)) {}

void GroupByNode::generateMemo(std::ostringstream& os) const {
    Node::generateMemoBase(os);
    os << "GroupBy"
       << "\n";
}

void GroupByNode::visit(AbstractVisitor& visitor) const {
    visitor.visit(*this);
}

NodePtr UnwindNode::create(Context& ctx,
                           ProjectionName projectionName,
                           const bool retainNonArrays,
                           NodePtr child) {
    return NodePtr(
        new UnwindNode(ctx, std::move(projectionName), retainNonArrays, std::move(child)));
}

NodePtr UnwindNode::clone(Context& ctx, const UnwindNode& other, NodePtr newChild) {
    return create(ctx, other._projectionName, other._retainNonArrays, std::move(newChild));
}

UnwindNode::UnwindNode(Context& ctx,
                       ProjectionName projectionName,
                       const bool retainNonArrays,
                       NodePtr child)
    : Node(ctx, std::move(child)),
      _projectionName(std::move(projectionName)),
      _retainNonArrays(retainNonArrays) {}

void UnwindNode::generateMemo(std::ostringstream& os) const {
    Node::generateMemoBase(os);
    os << "Unwind"
       << "\n";
}

void UnwindNode::visit(AbstractVisitor& visitor) const {
    visitor.visit(*this);
}

NodePtr WindNode::create(Context& ctx, ProjectionName projectionName, NodePtr child) {
    return NodePtr(new WindNode(ctx, std::move(projectionName), std::move(child)));
}

NodePtr WindNode::clone(Context& ctx, const WindNode& other, NodePtr newChild) {
    return create(ctx, other._projectionName, std::move(newChild));
}

WindNode::WindNode(Context& ctx, ProjectionName projectionName, NodePtr child)
    : Node(ctx, std::move(child)), _projectionName(std::move(projectionName)) {}

void WindNode::generateMemo(std::ostringstream& os) const {
    Node::generateMemoBase(os);
    os << "Wind"
       << "\n";
}

void WindNode::visit(AbstractVisitor& visitor) const {
    visitor.visit(*this);
}

}  // namespace mongo::optimizer
