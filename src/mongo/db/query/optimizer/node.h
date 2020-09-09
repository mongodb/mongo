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


class Node {
protected:
    Node() : _children() {}

    // pass context
    explicit Node(NodePtr child);

    explicit Node(std::vector<NodePtr> children);

    virtual void generateMemoInternal(std::ostringstream& os) = 0;

    // clone
public:
    virtual std::string generateMemo() final;

private:
    // id
    // properties
    std::vector<NodePtr> _children;
};

class ScanNode : public Node {
public:
    static NodePtr create(CollectionNameType collectionName);

protected:
    void generateMemoInternal(std::ostringstream& os) override;

private:
    explicit ScanNode(CollectionNameType collectionName);

    const CollectionNameType _collectionName;
};

class MultiJoinNode : public Node {
public:
    using FilterSet = std::unordered_set<FilterType>;
    using ProjectionMap = std::unordered_map<ProjectionName, ProjectionType>;

    static NodePtr create(FilterSet filterSet,
                          ProjectionMap projectionMap,
                          std::vector<NodePtr> children);

protected:
    void generateMemoInternal(std::ostringstream& os) override;

private:
    explicit MultiJoinNode(FilterSet filterSet,
                           ProjectionMap projectionMap,
                           std::vector<NodePtr> children);

    FilterSet _filterSet;
    ProjectionMap _projectionMap;
};

class UnionNode : public Node {
public:
    static NodePtr create(std::vector<NodePtr> children);

protected:
    void generateMemoInternal(std::ostringstream& os) override;

private:
    explicit UnionNode(std::vector<NodePtr> children);
};

class GroupByNode : public Node {
public:
    using GroupByVector = std::vector<ProjectionName>;
    using ProjectionMap = std::unordered_map<ProjectionName, ProjectionType>;

    static NodePtr create(GroupByVector groupByVector, ProjectionMap projectionMap, NodePtr child);

protected:
    void generateMemoInternal(std::ostringstream& os) override;

private:
    explicit GroupByNode(GroupByVector groupByVector, ProjectionMap projectionMap, NodePtr child);

    GroupByVector _groupByVector;
    ProjectionMap _projectionMap;
};

class UnwindNode : public Node {
public:
    static NodePtr create(ProjectionName projectionName, bool retainNonArrays, NodePtr child);

protected:
    void generateMemoInternal(std::ostringstream& os) override;

private:
    UnwindNode(ProjectionName projectionName, bool retainNonArrays, NodePtr child);

    const ProjectionName _projectionName;
    const bool _retainNonArrays;
};

class WindNode : public Node {
public:
    static NodePtr create(ProjectionName projectionName, NodePtr child);

protected:
    void generateMemoInternal(std::ostringstream& os) override;

private:
    WindNode(ProjectionName projectionName, NodePtr child);

    const ProjectionName _projectionName;
};


}  // namespace mongo::optimizer
