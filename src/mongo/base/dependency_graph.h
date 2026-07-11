// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mongo::initializer_details {

/**
 * To separate concerns, this just captures the setup and execution of a
 * topological sort without regard to higher-level semantics like process
 * initialization. Each node is mapped to an abstract Data payload.
 */
class DependencyGraph {
public:
    class Payload {
    public:
        virtual ~Payload() = default;
    };

    /**
     * Add a new initializer node, named `name`, to the dependency graph,
     * having the given `prerequisites` and `dependents`, which are the names
     * of other nodes which will be in the graph when `topSort` is called.
     *
     * The new node can be mapped to an abstract `payload`, which can
     * be retrieved with `find`.
     *
     * Note that cycles in the dependency graph are not discovered by this function.
     * Rather, they're discovered by `topSort`, below.
     */
    void addNode(const std::string& name,
                 const std::vector<std::string>& prerequisites,
                 const std::vector<std::string>& dependents,
                 std::unique_ptr<Payload> payload = nullptr);

    /**
     * Returns a topological sort of the dependency graph, represented
     * as an ordered vector of node names.
     *
     * Throws with `ErrorCodes::GraphContainsCycle` if the graph contains a cycle.
     * If a `cycle` is given, it will be overwritten with the node sequence involved.
     *
     * Throws with `ErrorCodes::BadValue` if any node in the graph names a
     * prerequisite that is missing from the graph.
     */
    std::vector<std::string> topSort(unsigned randomSeed,
                                     std::vector<std::string>* cycle = nullptr) const;

    Payload* find(const std::string& name);

private:
    struct Node {
        std::set<std::string> prerequisites;
        std::unique_ptr<Payload> payload;
    };

    /**
     * Map of all named nodes.  Nodes named as prerequisites or dependents but not explicitly
     * added via addInitializer will either be absent from this map or be present with
     * NodeData::fn set to a false-ish value.
     */
    std::map<std::string, Node> _nodes;
};

}  // namespace mongo::initializer_details
