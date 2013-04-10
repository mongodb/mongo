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

/**
 * Unit tests of the InitializerDependencyGraph type.
 */

#include "mongo/base/init.h"
#include "mongo/base/initializer_dependency_graph.h"
#include "mongo/base/make_string_vector.h"
#include "mongo/unittest/unittest.h"

#define ADD_INITIALIZER(GRAPH, NAME, FN, PREREQS, DEPS)                 \
    (GRAPH).addInitializer(                                             \
            (NAME),                                                     \
            (FN),                                                       \
            MONGO_MAKE_STRING_VECTOR PREREQS,                           \
            MONGO_MAKE_STRING_VECTOR DEPS)

#define ASSERT_ADD_INITIALIZER(GRAPH, NAME, FN, PREREQS, DEPS)  \
    ASSERT_EQUALS(Status::OK(), ADD_INITIALIZER(GRAPH, NAME, FN, PREREQS, DEPS))

#define ASSERT_EXACTLY_N_IN_CONTAINER(N, CONTAINER, THING)               \
    ASSERT_EQUALS(N, std::count((CONTAINER).begin(), (CONTAINER).end(), (THING)))

#define ASSERT_AT_LEAST_N_IN_CONTAINER(N, CONTAINER, THING)             \
    ASSERT_LESS_THAN_OR_EQUALS(N, std::count((CONTAINER).begin(), (CONTAINER).end(), (THING)))

#define ASSERT_EXACTLY_ONE_IN_CONTAINER(CONTAINER, THING)               \
    ASSERT_EXACTLY_N_IN_CONTAINER(1, CONTAINER, THING)

namespace mongo {
namespace {

    Status doNothing(InitializerContext*) { return Status::OK(); }

    TEST(InitializerDependencyGraphTest, InsertNullFunctionFails) {
        InitializerDependencyGraph graph;
        ASSERT_EQUALS(ErrorCodes::BadValue, ADD_INITIALIZER(
                          graph, "A", InitializerFunction(),
                          MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS));
    }

    TEST(InitializerDependencyGraphTest, InsertSameNameTwiceFails) {
        InitializerDependencyGraph graph;
        ASSERT_ADD_INITIALIZER(graph, "A", doNothing, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS);
        ASSERT_EQUALS(ErrorCodes::DuplicateKey, ADD_INITIALIZER(
                          graph, "A", doNothing,
                          MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS));
    }

    TEST(InitializerDependencyGraphTest, TopSortEmptyGraph) {
        InitializerDependencyGraph graph;
        std::vector<std::string> nodeNames;
        ASSERT_EQUALS(Status::OK(), graph.topSort(&nodeNames));
        ASSERT_EQUALS(0U, nodeNames.size());
    }

    TEST(InitializerDependencyGraphTest, TopSortGraphNoDeps) {
        InitializerDependencyGraph graph;
        std::vector<std::string> nodeNames;
        ASSERT_ADD_INITIALIZER(graph, "A", doNothing, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS);
        ASSERT_ADD_INITIALIZER(graph, "B", doNothing, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS);
        ASSERT_ADD_INITIALIZER(graph, "C", doNothing, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS);
        ASSERT_EQUALS(Status::OK(), graph.topSort(&nodeNames));
        ASSERT_EQUALS(3U, nodeNames.size());
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "A");
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "B");
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "C");
    }

    TEST(InitializerDependencyGraphTest, TopSortWithDiamondPrerequisites) {
        /*
         * This tests top-sorting a simple diamond, specified using prerequisites:
         *
         *     B
         *   /  ^
         *  v    \ 
         * A      D
         *  ^   /
         *   \ v
         *    C
         */
        InitializerDependencyGraph graph;
        std::vector<std::string> nodeNames;
        ASSERT_ADD_INITIALIZER(graph, "A", doNothing, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS);
        ASSERT_ADD_INITIALIZER(graph, "D", doNothing, ("B", "C"), MONGO_NO_DEPENDENTS);
        ASSERT_ADD_INITIALIZER(graph, "B", doNothing, ("A"), MONGO_NO_DEPENDENTS);
        ASSERT_ADD_INITIALIZER(graph, "C", doNothing, ("A"), MONGO_NO_DEPENDENTS);
        ASSERT_EQUALS(Status::OK(), graph.topSort(&nodeNames));
        ASSERT_EQUALS(4U, nodeNames.size());
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "A");
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "B");
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "C");
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "D");
        ASSERT_EQUALS("A", nodeNames.front());
        ASSERT_EQUALS("D", nodeNames.back());
    }

    TEST(InitializerDependencyGraphTest, TopSortWithDiamondDependents) {
        /*
         * This tests top-sorting a simple diamond, specified using dependents:
         *
         *     B
         *   /  ^
         *  v    \ 
         * A      D
         *  ^   /
         *   \ v
         *    C
         */
        InitializerDependencyGraph graph;
        std::vector<std::string> nodeNames;
        ASSERT_ADD_INITIALIZER(graph, "A", doNothing, MONGO_NO_PREREQUISITES, ("B", "C"));
        ASSERT_ADD_INITIALIZER(graph, "D", doNothing, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS);
        ASSERT_ADD_INITIALIZER(graph, "B", doNothing, MONGO_NO_PREREQUISITES, ("D"));
        ASSERT_ADD_INITIALIZER(graph, "C", doNothing, MONGO_NO_PREREQUISITES, ("D"));
        ASSERT_EQUALS(Status::OK(), graph.topSort(&nodeNames));
        ASSERT_EQUALS(4U, nodeNames.size());
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "A");
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "B");
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "C");
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "D");
        ASSERT_EQUALS("A", nodeNames.front());
        ASSERT_EQUALS("D", nodeNames.back());
    }

    TEST(InitializerDependencyGraphTest, TopSortWithDiamondGeneral1) {
        /*
         * This tests top-sorting a simple diamond, where B and C specify all prerequisites and
         * dependents.
         *
         *     B
         *   /  ^
         *  v    \ 
         * A      D
         *  ^   /
         *   \ v
         *    C
         */
        InitializerDependencyGraph graph;
        std::vector<std::string> nodeNames;
        ASSERT_ADD_INITIALIZER(graph, "A", doNothing, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS);
        ASSERT_ADD_INITIALIZER(graph, "D", doNothing, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS);
        ASSERT_ADD_INITIALIZER(graph, "B", doNothing, ("A"), ("D"));
        ASSERT_ADD_INITIALIZER(graph, "C", doNothing, ("A"), ("D"));
        ASSERT_EQUALS(Status::OK(), graph.topSort(&nodeNames));
        ASSERT_EQUALS(4U, nodeNames.size());
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "A");
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "B");
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "C");
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "D");
        ASSERT_EQUALS("A", nodeNames.front());
        ASSERT_EQUALS("D", nodeNames.back());
    }

    TEST(InitializerDependencyGraphTest, TopSortWithDiamondGeneral2) {
        /*
         * This tests top-sorting a simple diamond, where A and D specify all prerequisites and
         * dependents.
         *
         *     B
         *   /  ^
         *  v    \ 
         * A      D
         *  ^   /
         *   \ v
         *    C
         */
        InitializerDependencyGraph graph;
        std::vector<std::string> nodeNames;
        ASSERT_ADD_INITIALIZER(graph, "A", doNothing, MONGO_NO_PREREQUISITES, ("B", "C"));
        ASSERT_ADD_INITIALIZER(graph, "D", doNothing, ("C", "B"), MONGO_NO_DEPENDENTS);
        ASSERT_ADD_INITIALIZER(graph, "B", doNothing, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS);
        ASSERT_ADD_INITIALIZER(graph, "C", doNothing, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS);
        ASSERT_EQUALS(Status::OK(), graph.topSort(&nodeNames));
        ASSERT_EQUALS(4U, nodeNames.size());
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "A");
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "B");
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "C");
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "D");
        ASSERT_EQUALS("A", nodeNames.front());
        ASSERT_EQUALS("D", nodeNames.back());
    }

    TEST(InitializerDependencyGraphTest, TopSortWithDiamondGeneral3) {
        /*
         * This tests top-sorting a simple diamond, where A and D specify all prerequisites and
         * dependents, but so do B and C.
         *
         *     B
         *   /  ^
         *  v    \ 
         * A      D
         *  ^   /
         *   \ v
         *    C
         */
        InitializerDependencyGraph graph;
        std::vector<std::string> nodeNames;
        ASSERT_ADD_INITIALIZER(graph, "A", doNothing, MONGO_NO_PREREQUISITES, ("B", "C"));
        ASSERT_ADD_INITIALIZER(graph, "D", doNothing, ("C", "B"), MONGO_NO_DEPENDENTS);
        ASSERT_ADD_INITIALIZER(graph, "B", doNothing, ("A"), ("D"));
        ASSERT_ADD_INITIALIZER(graph, "C", doNothing, ("A"), ("D"));
        ASSERT_EQUALS(Status::OK(), graph.topSort(&nodeNames));
        ASSERT_EQUALS(4U, nodeNames.size());
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "A");
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "B");
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "C");
        ASSERT_EXACTLY_ONE_IN_CONTAINER(nodeNames, "D");
        ASSERT_EQUALS("A", nodeNames.front());
        ASSERT_EQUALS("D", nodeNames.back());
    }

    TEST(InitializerDependencyGraphTest, TopSortWithDiamondAndCycle) {
        /*
         * This tests top-sorting a graph with a cycle, which should fail..
         *
         *     B <- E
         *   /  ^   ^
         *  v    \ /
         * A      D
         *  ^   /
         *   \ v
         *    C
         */
        InitializerDependencyGraph graph;
        std::vector<std::string> nodeNames;
        ASSERT_ADD_INITIALIZER(graph, "A", doNothing, MONGO_NO_PREREQUISITES, ("B", "C"));
        ASSERT_ADD_INITIALIZER(graph, "D", doNothing, ("C", "B"), MONGO_NO_DEPENDENTS);
        ASSERT_ADD_INITIALIZER(graph, "B", doNothing, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS);
        ASSERT_ADD_INITIALIZER(graph, "C", doNothing, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS);
        ASSERT_ADD_INITIALIZER(graph, "E", doNothing, ("D"), ("B"));
        ASSERT_EQUALS(ErrorCodes::GraphContainsCycle, graph.topSort(&nodeNames));
        ASSERT_EQUALS(4U, nodeNames.size());
        ASSERT_EQUALS(nodeNames.front(), nodeNames.back());
        ASSERT_AT_LEAST_N_IN_CONTAINER(1, nodeNames, "D");
        ASSERT_AT_LEAST_N_IN_CONTAINER(1, nodeNames, "E");
        ASSERT_AT_LEAST_N_IN_CONTAINER(1, nodeNames, "B");
        ASSERT_EXACTLY_N_IN_CONTAINER(0, nodeNames, "A");
        ASSERT_EXACTLY_N_IN_CONTAINER(0, nodeNames, "C");
    }

    TEST(InitializerDependencyGraphTest, TopSortFailsWhenMissingPrerequisite) {
        /*
         * If a node names a never-declared prerequisite, topSort should fail.
         */
        InitializerDependencyGraph graph;
        std::vector<std::string> nodeNames;
        ASSERT_ADD_INITIALIZER(graph, "B", doNothing, ("A"), MONGO_NO_DEPENDENTS);
        ASSERT_EQUALS(ErrorCodes::BadValue, graph.topSort(&nodeNames));
    }

    TEST(InitializerDependencyGraphTest, TopSortFailsWhenMissingDependent) {
        /*
         * If a node names a never-declared dependent, topSort should fail.
         */
        InitializerDependencyGraph graph;
        std::vector<std::string> nodeNames;
        ASSERT_ADD_INITIALIZER(graph, "A", doNothing, MONGO_NO_PREREQUISITES, ("B"));
        ASSERT_EQUALS(ErrorCodes::BadValue, graph.topSort(&nodeNames));
    }

}  // namespace
}  // namespace mongo

