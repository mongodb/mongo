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

#pragma once

#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/filter.h"
#include "mongo/db/query/optimizer/projection.h"
#include "mongo/db/query/optimizer/props.h"


namespace mongo::optimizer {

class Node;
using NodePtr = std::unique_ptr<Node>;

class AbstractVisitor;

class Node {
public:
    using ChildVector = std::vector<NodePtr>;

protected:
    explicit Node(Context& ctx);
    explicit Node(Context& ctx, NodePtr child);
    explicit Node(Context& ctx, ChildVector children);

    void generateMemoBase(std::ostringstream& os) const;

    virtual void visit(AbstractVisitor& visitor) const = 0;
    void visitPreOrder(AbstractVisitor& visitor) const;
    void visitPostOrder(AbstractVisitor& visitor) const;

    // clone
public:
    Node() = delete;

    std::string generateMemo() const;

    NodePtr clone(Context& ctx) const;

    int getChildCount() const;

private:
    const NodeIdType _nodeId;
    ChildVector _children;
};

class ScanNode : public Node {
public:
    static NodePtr create(Context& ctx, CollectionNameType collectionName);
    static NodePtr clone(Context& ctx, const ScanNode& other);

    void generateMemo(std::ostringstream& os) const;

protected:
    void visit(AbstractVisitor& visitor) const override;

private:
    explicit ScanNode(Context& ctx, CollectionNameType collectionName);

    const CollectionNameType _collectionName;
};

class MultiJoinNode : public Node {
public:
    using FilterSet = std::unordered_set<FilterType>;
    using ProjectionMap = std::unordered_map<ProjectionName, ProjectionType>;

    static NodePtr create(Context& ctx,
                          FilterSet filterSet,
                          ProjectionMap projectionMap,
                          ChildVector children);
    static NodePtr clone(Context& ctx, const MultiJoinNode& other, ChildVector newChildren);

    void generateMemo(std::ostringstream& os) const;

protected:
    void visit(AbstractVisitor& visitor) const override;

private:
    explicit MultiJoinNode(Context& ctx,
                           FilterSet filterSet,
                           ProjectionMap projectionMap,
                           ChildVector children);

    FilterSet _filterSet;
    ProjectionMap _projectionMap;
};

class UnionNode : public Node {
public:
    static NodePtr create(Context& ctx, ChildVector children);
    static NodePtr clone(Context& ctx, const UnionNode& other, ChildVector newChildren);

    void generateMemo(std::ostringstream& os) const;

protected:
    void visit(AbstractVisitor& visitor) const override;

private:
    explicit UnionNode(Context& ctx, ChildVector children);
};

class GroupByNode : public Node {
public:
    using GroupByVector = std::vector<ProjectionName>;
    using ProjectionMap = std::unordered_map<ProjectionName, ProjectionType>;

    static NodePtr create(Context& ctx,
                          GroupByVector groupByVector,
                          ProjectionMap projectionMap,
                          NodePtr child);
    static NodePtr clone(Context& ctx, const GroupByNode& other, NodePtr newChild);

    void generateMemo(std::ostringstream& os) const;

protected:
    void visit(AbstractVisitor& visitor) const override;

private:
    explicit GroupByNode(Context& ctx,
                         GroupByVector groupByVector,
                         ProjectionMap projectionMap,
                         NodePtr child);

    GroupByVector _groupByVector;
    ProjectionMap _projectionMap;
};

class UnwindNode : public Node {
public:
    static NodePtr create(Context& ctx,
                          ProjectionName projectionName,
                          bool retainNonArrays,
                          NodePtr child);
    static NodePtr clone(Context& ctx, const UnwindNode& other, NodePtr newChild);

    void generateMemo(std::ostringstream& os) const;

protected:
    void visit(AbstractVisitor& visitor) const override;

private:
    UnwindNode(Context& ctx, ProjectionName projectionName, bool retainNonArrays, NodePtr child);

    const ProjectionName _projectionName;
    const bool _retainNonArrays;
};

class WindNode : public Node {
public:
    static NodePtr create(Context& ctx, ProjectionName projectionName, NodePtr child);
    static NodePtr clone(Context& ctx, const WindNode& other, NodePtr newChild);

    void generateMemo(std::ostringstream& os) const;

protected:
    void visit(AbstractVisitor& visitor) const override;

private:
    WindNode(Context& ctx, ProjectionName projectionName, NodePtr child);

    const ProjectionName _projectionName;
};

}  // namespace mongo::optimizer
