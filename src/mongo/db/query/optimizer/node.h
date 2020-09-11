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

#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/filter.h"
#include "mongo/db/query/optimizer/projection.h"
#include "mongo/db/query/optimizer/props.h"


namespace mongo::optimizer {


class ScanNode;
class MultiJoinNode;
class UnionNode;
class GroupByNode;
class UnwindNode;
class WindNode;

using PolymorphicNode =
    algebra::PolyValue<ScanNode, MultiJoinNode, UnionNode, GroupByNode, UnwindNode, WindNode>;

template <typename Derived, size_t Arity>
using Operator = algebra::OpSpecificArity<PolymorphicNode, Derived, Arity>;

template <typename Derived, size_t Arity>
using OperatorDynamic = algebra::OpSpecificDynamicArity<PolymorphicNode, Derived, Arity>;

template <typename Derived>
using OperatorDynamicHomogenous = OperatorDynamic<Derived, 0>;

using PolymorphicNodeVector = std::vector<PolymorphicNode>;

template <typename T, typename... Args>
inline auto make(Args&&... args) {
    return PolymorphicNode::make<T>(std::forward<Args>(args)...);
}

template <typename... Args>
inline auto makeSeq(Args&&... args) {
    PolymorphicNodeVector seq;
    (seq.emplace_back(std::forward<Args>(args)), ...);
    return seq;
}

class Node {
protected:
    explicit Node(Context& ctx);

    void generateMemoBase(std::ostringstream& os) const;

public:
    Node() = delete;

private:
    const NodeIdType _nodeId;
};

class ScanNode final : public Operator<ScanNode, 0>, public Node {
    using Base = Operator<ScanNode, 0>;

public:
    explicit ScanNode(Context& ctx, CollectionNameType collectionName);

    void generateMemo(std::ostringstream& os) const;

private:
    const CollectionNameType _collectionName;
};

class MultiJoinNode final : public OperatorDynamicHomogenous<MultiJoinNode>, public Node {
    using Base = OperatorDynamicHomogenous<MultiJoinNode>;

public:
    using FilterSet = std::unordered_set<FilterType>;
    using ProjectionMap = std::unordered_map<ProjectionName, ProjectionType>;

    explicit MultiJoinNode(Context& ctx,
                           FilterSet filterSet,
                           ProjectionMap projectionMap,
                           PolymorphicNodeVector children);

    void generateMemo(std::ostringstream& os) const;

private:
    FilterSet _filterSet;
    ProjectionMap _projectionMap;
};

class UnionNode final : public OperatorDynamicHomogenous<UnionNode>, public Node {
    using Base = OperatorDynamicHomogenous<UnionNode>;

public:
    explicit UnionNode(Context& ctx, PolymorphicNodeVector children);

    void generateMemo(std::ostringstream& os) const;
};

class GroupByNode : public Operator<GroupByNode, 1>, public Node {
    using Base = Operator<GroupByNode, 1>;

public:
    using GroupByVector = std::vector<ProjectionName>;
    using ProjectionMap = std::unordered_map<ProjectionName, ProjectionType>;

    explicit GroupByNode(Context& ctx,
                         GroupByVector groupByVector,
                         ProjectionMap projectionMap,
                         PolymorphicNode child);

    void generateMemo(std::ostringstream& os) const;

private:
    GroupByVector _groupByVector;
    ProjectionMap _projectionMap;
};

class UnwindNode final : public Operator<UnwindNode, 1>, public Node {
    using Base = Operator<UnwindNode, 1>;

public:
    explicit UnwindNode(Context& ctx,
                        ProjectionName projectionName,
                        bool retainNonArrays,
                        PolymorphicNode child);

    void generateMemo(std::ostringstream& os) const;

private:
    const ProjectionName _projectionName;
    const bool _retainNonArrays;
};

class WindNode final : public Operator<WindNode, 1>, public Node {
    using Base = Operator<WindNode, 1>;

public:
    explicit WindNode(Context& ctx, ProjectionName projectionName, PolymorphicNode child);

    void generateMemo(std::ostringstream& os) const;

private:
    const ProjectionName _projectionName;
};
}  // namespace mongo::optimizer
