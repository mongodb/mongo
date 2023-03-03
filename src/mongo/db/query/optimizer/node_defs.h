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

#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/props.h"

namespace mongo::optimizer {

// Used for physical rewrites. For each child we optimize, we specify physical properties.
using ChildPropsType = std::vector<std::pair<ABT*, properties::PhysProps>>;

// Use for physical rewrites. For each physical node we implement, we set CE to use when costing.
using NodeCEMap = opt::unordered_map<const Node*, CEType>;

struct NodeProps {
    // Used to tie to a corresponding SBE stage.
    int32_t _planNodeId;

    // Which is the corresponding memo group, and its properties.
    MemoPhysicalNodeId _groupId;
    properties::LogicalProps _logicalProps;
    properties::PhysProps _physicalProps;

    // Set if we have an RID projection name.
    boost::optional<ProjectionName> _ridProjName;

    // Total cost of the best plan (includes the subtree).
    CostType _cost;
    // Local cost (excludes subtree).
    CostType _localCost;

    // For display purposes, adjusted cardinality based on physical properties (e.g. Repetition and
    // Limit-Skip).
    CEType _adjustedCE;
};

// Map from node to various properties, including logical and physical. Used to determine for
// example which of the available projections are used for exchanges.
using NodeToGroupPropsMap = opt::unordered_map<const Node*, NodeProps>;

/**
 * Utility used to copy an ABT and an associated annotation map which points into the ABT in order
 * to retain the map references into the newly copied ABT. The map can only point to nodes in the
 * ABT.
 */
template <class MapType>
class NodeAnnotationCopier {
    // Used to keep track of the node pointers in the annotation map according to traversal order.
    using PtrPosVector = std::vector<std::pair<size_t, const Node*>>;

public:
    template <typename T, typename... Ts>
    void transport(
        const T& node, const MapType& mapInput, PtrPosVector& ptrPos, size_t& nodeIndex, Ts&&...) {
        if constexpr (std::is_base_of_v<Node, T>) {
            if (mapInput.count(&node) > 0) {
                // Step 1: collect pointers from old map.
                ptrPos.emplace_back(nodeIndex, &node);
            }
            nodeIndex++;
        }
    }

    template <typename T, typename... Ts>
    void transport(const T& node,
                   const MapType& mapInput,
                   PtrPosVector& ptrPos,
                   size_t& nodeIndex,
                   size_t& ptrIndex,
                   MapType& mapCopy,
                   Ts&&...) {
        if constexpr (std::is_base_of_v<Node, T>) {
            // Step 2: copy to new map using previously collected pointers.
            if (ptrIndex < ptrPos.size()) {
                if (const auto [pos, ptr] = ptrPos.at(ptrIndex); nodeIndex == pos) {
                    ptrIndex++;
                    mapCopy.emplace(&node, mapInput.at(ptr));
                }
            }
            nodeIndex++;
        }
    }

    std::pair<ABT, MapType> copy(const ABT& abtInput, const MapType& mapInput) {
        PtrPosVector ptrPos;
        size_t nodeIndex = 0;
        algebra::transport<false>(abtInput, *this, mapInput, ptrPos, nodeIndex);

        ABT abtCopy = abtInput;
        MapType mapCopy;
        nodeIndex = 0;
        size_t ptrIndex = 0;
        algebra::transport<false>(abtCopy, *this, mapInput, ptrPos, nodeIndex, ptrIndex, mapCopy);

        return {std::move(abtCopy), std::move(mapCopy)};
    }
};

/**
 * Structure which can safely copy a plan and associated annotation map (map from node pointer to
 * some value type).
 */
template <class MapType>
struct CopySafeNodeAnnotation {
    CopySafeNodeAnnotation(ABT node, MapType map) : _node(std::move(node)), _map(std::move(map)) {}
    CopySafeNodeAnnotation(CopySafeNodeAnnotation&& other)
        : _node(std::move(other._node)), _map(std::move(other._map)) {}
    CopySafeNodeAnnotation(const CopySafeNodeAnnotation& other)
        : CopySafeNodeAnnotation(make<Blackhole>(), {}) {
        *this = other;
    }

    CopySafeNodeAnnotation& operator=(const CopySafeNodeAnnotation& other) {
        std::tie(_node, _map) = NodeAnnotationCopier<MapType>{}.copy(other._node, other._map);
        return *this;
    }
    CopySafeNodeAnnotation& operator=(CopySafeNodeAnnotation&& other) {
        _node = std::move(other._node);
        _map = std::move(other._map);
        return *this;
    }

    const auto& getRootAnnotation() const {
        return _map.at(_node.cast<Node>());
    }
    auto& getRootAnnotation() {
        return _map.at(_node.cast<Node>());
    }

    template <class T>
    void setRootAnnotation(T value) {
        _map.emplace(_node.cast<Node>(), std::move(value));
    }

    ABT _node;
    MapType _map;
};

using PlanAndProps = CopySafeNodeAnnotation<NodeToGroupPropsMap>;

}  // namespace mongo::optimizer
