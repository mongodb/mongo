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

#include "mongo/base/initializer_dependency_graph.h"

#include <algorithm>
#include <iterator>
#include <sstream>

namespace mongo {

    InitializerDependencyGraph::InitializerDependencyGraph() {}
    InitializerDependencyGraph::~InitializerDependencyGraph() {}

    Status InitializerDependencyGraph::addInitializer(const std::string& name,
                                                      const InitializerFunction& fn,
                                                      const std::vector<std::string>& prerequisites,
                                                      const std::vector<std::string>& dependents) {
        if (!fn)
            return Status(ErrorCodes::BadValue, "Illegal to supply a NULL function");

        NodeData& newNode = _nodes[name];
        if (newNode.fn) {
            return Status(ErrorCodes::DuplicateKey, name);
        }

        newNode.fn = fn;

        for (size_t i = 0; i < prerequisites.size(); ++i) {
            newNode.prerequisites.insert(prerequisites[i]);
        }

        for (size_t i = 0; i < dependents.size(); ++i) {
            _nodes[dependents[i]].prerequisites.insert(name);
        }

        return Status::OK();
    }

    InitializerFunction InitializerDependencyGraph::getInitializerFunction(
            const std::string& name) const {

        NodeMap::const_iterator iter = _nodes.find(name);
        if (iter == _nodes.end())
            return InitializerFunction();
        return iter->second.fn;
    }

    Status InitializerDependencyGraph::topSort(std::vector<std::string>* sortedNames) const {
        /*
         * This top-sort is implemented by performing a depth-first traversal of the dependency
         * graph, once for each node.  "visitedNodeNames" tracks the set of node names ever visited,
         * and it is used to prune each DFS.  A node that has been visited once on any DFS is never
         * visited again.  Complexity of this implementation is O(n+m) where "n" is the number of
         * nodes and "m" is the number of prerequisite edges.  Space complexity is O(n), in both
         * stack space and size of the "visitedNodeNames" set.
         *
         * "inProgressNodeNames" is used to detect and report cycles.
         */

        std::vector<std::string> inProgressNodeNames;
        unordered_set<std::string> visitedNodeNames;

        sortedNames->clear();
        for (NodeMap::const_iterator iter = _nodes.begin(), end = _nodes.end();
             iter != end; ++iter) {

            Status status = recursiveTopSort(_nodes,
                                             *iter,
                                             &inProgressNodeNames,
                                             &visitedNodeNames,
                                             sortedNames);
            if (Status::OK() != status)
                return status;
        }
        return Status::OK();
    }

    Status InitializerDependencyGraph::recursiveTopSort(
            const NodeMap& nodeMap,
            const Node& currentNode,
            std::vector<std::string>* inProgressNodeNames,
            unordered_set<std::string>* visitedNodeNames,
            std::vector<std::string>* sortedNames) {

        /*
         * The top sort is performed by depth-first traversal starting at each node in the
         * dependency graph, short-circuited any time a node is seen that has already been visited
         * in any traversal.  "visitedNodeNames" is the set of nodes that have been successfully
         * visited, while "inProgressNodeNames" are nodes currently in the exploration chain.  This
         * structure is kept explicitly to facilitate cycle detection.
         *
         * This function implements a depth-first traversal, and is called once for each node in the
         * graph by topSort(), above.
         */

        if ((*visitedNodeNames).count(currentNode.first))
            return Status::OK();

        if (!currentNode.second.fn)
            return Status(ErrorCodes::BadValue, currentNode.first);

        inProgressNodeNames->push_back(currentNode.first);

        std::vector<std::string>::iterator firstOccurence = std::find(
                inProgressNodeNames->begin(), inProgressNodeNames->end(), currentNode.first);
        if (firstOccurence + 1 != inProgressNodeNames->end()) {
            sortedNames->clear();
            std::copy(firstOccurence, inProgressNodeNames->end(), std::back_inserter(*sortedNames));
            std::ostringstream os;
            os << "Cycle in dependendcy graph: " << sortedNames->at(0);
            for (size_t i = 1; i < sortedNames->size(); ++i)
                os << " -> " << sortedNames->at(i);
            return Status(ErrorCodes::GraphContainsCycle, os.str());
        }

        for (unordered_set<std::string>::const_iterator
                 iter = currentNode.second.prerequisites.begin(),
                 end = currentNode.second.prerequisites.end();
             iter != end; ++iter) {

            NodeMap::const_iterator nextNode = nodeMap.find(*iter);
            if (nextNode == nodeMap.end())
                return Status(ErrorCodes::BadValue, *iter);

            Status status = recursiveTopSort(nodeMap,
                                             *nextNode,
                                             inProgressNodeNames,
                                             visitedNodeNames,
                                             sortedNames);
            if (Status::OK() != status)
                return status;
        }
        sortedNames->push_back(currentNode.first);
        if (inProgressNodeNames->back() != currentNode.first)
            return Status(ErrorCodes::InternalError, "inProgressNodeNames stack corrupt");
        inProgressNodeNames->pop_back();
        visitedNodeNames->insert(currentNode.first);
        return Status::OK();
    }

}  // namespace mongo
