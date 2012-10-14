/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <string>
#include <vector>
#include <utility>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/initializer_function.h"
#include "mongo/base/status.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

    /**
     * Representation of a dependency graph of "initialization operations."
     *
     * Each operation has a unique name, a function object implementing the operation's behavior,
     * and a set of prerequisite operations, which may be empty.  A legal graph contains no cycles.
     *
     * Instances of this class are used in two phases.  In the first phase, the graph is constructed
     * by repeated calls to addInitializer().  In the second phase, a user calls the topSort()
     * method to produce an initialization order that respects the dependencies among operations, and
     * then uses the getInitializerFunction() to get the behavior function for each operation, in
     * turn.
     *
     * Concurrency Notes: The user is responsible for synchronization.  Multiple threads may
     * simultaneously call the const functions, getInitializerFunction and topSort, on the same
     * instance of InitializerDependencyGraph.  However, no thread may call addInitializer while any
     * thread is executing those functions or addInitializer on the same instance.
     */
    class InitializerDependencyGraph {
        MONGO_DISALLOW_COPYING(InitializerDependencyGraph);

    public:
        InitializerDependencyGraph();
        ~InitializerDependencyGraph();

        /**
         * Add a new initializer node, named "name", to the dependency graph, with the given
         * behavior, "fn", and the given "prerequisites" (input dependencies) and "dependents"
         * (output dependencies).
         *
         * If "!fn" (fn is NULL in function pointer parlance), returns status with code
         * ErrorCodes::badValue.  If "name" is a duplicate of a name already present in the graph,
         * returns "ErrorCodes::duplicateKey".  Otherwise, returns Status::OK() and adds the new node
         * to the graph.  Note that cycles in the dependency graph are not discovered in this phase.
         * Rather, they're discovered by topSort, below.
         */
        Status addInitializer(const std::string& name,
                              const InitializerFunction& fn,
                              const std::vector<std::string>& prerequisites,
                              const std::vector<std::string>& dependents);

        /**
         * Given a dependency operation node named "name", return its behavior function.  Returns
         * a value that evaluates to "false" in boolean context, otherwise.
         */
        InitializerFunction getInitializerFunction(const std::string& name) const;

        /**
         * Construct a topological sort of the dependency graph, and store that order into
         * "sortedNames".  Returns Status::OK() on success.
         *
         * If the graph contains a cycle, returns ErrorCodes::graphContainsCycle, and "sortedNames"
         * is an ordered sequence of nodes involved in a cycle.  In this case, the first and last
         * element of "sortedNames" will be equal.
         *
         * If any node in the graph names a prerequisite that was never added to the graph via
         * addInitializer, this function will return ErrorCodes::badValue.
         *
         * Any other return value indicates an internal error, and should not occur.
         */
        Status topSort(std::vector<std::string>* sortedNames) const;

    private:
        struct NodeData {
            InitializerFunction fn;
            unordered_set<std::string> prerequisites;
        };

        typedef unordered_map<std::string, NodeData> NodeMap;
        typedef NodeMap::value_type Node;

        /**
         * Helper function to recursively top-sort a graph.  Used by topSort().
         */
        static Status recursiveTopSort(
                const NodeMap& nodeMap,
                const Node& currentNode,
                std::vector<std::string>* inProgressNodeNames,
                unordered_set<std::string>* visitedNodeNames,
                std::vector<std::string>* sortedNames);

        /**
         * Map of all named nodes.  Nodes named as prerequisites or dependents but not explicitly
         * added via addInitializer will either be absent from this map or be present with
         * NodeData::fn set to a false-ish value.
         */
        NodeMap _nodes;
    };

}  // namespace mongo
