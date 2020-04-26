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
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"

namespace mongo {

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
    void addNode(std::string name,
                 std::vector<std::string> prerequisites,
                 std::vector<std::string> dependents,
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
    std::vector<std::string> topSort(std::vector<std::string>* cycle = nullptr) const;

    Payload* find(const std::string& name);

private:
    struct Node {
        stdx::unordered_set<std::string> prerequisites;
        std::unique_ptr<Payload> payload;
    };

    /**
     * Map of all named nodes.  Nodes named as prerequisites or dependents but not explicitly
     * added via addInitializer will either be absent from this map or be present with
     * NodeData::fn set to a false-ish value.
     */
    stdx::unordered_map<std::string, Node> _nodes;
};

}  // namespace mongo
