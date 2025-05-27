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

/**
 * Unit tests of the DependencyGraph type.
 */

#include <algorithm>
#include <cstddef>
#include <random>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>  // IWYU pragma: keep
// IWYU pragma: no_include "format.h"

#include "mongo/base/dependency_graph.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/string_data.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

template <typename C, typename T>
size_t count(const C& c, const T& value) {
    return std::count(c.begin(), c.end(), value);
}

class DependencyGraphTest : public unittest::Test {
public:
    void setUp() override {
        unsigned seed = std::random_device{}();
        LOGV2(8991202, "Running test with seed", "seed"_attr = seed);
        _gen.seed(seed);
    }

    std::mt19937& prng() {
        return _gen;
    }

private:
    std::mt19937 _gen;
};

TEST_F(DependencyGraphTest, InsertNullPayloadOkay) {
    DependencyGraph graph;
    graph.addNode("A", {}, {});
}

TEST_F(DependencyGraphTest, InsertSameNameTwiceFails) {
    DependencyGraph graph;
    graph.addNode("A", {}, {});
    ASSERT_THROWS_CODE(graph.addNode("A", {}, {}), DBException, 50999);
}

TEST_F(DependencyGraphTest, TopSortEmptyGraph) {
    DependencyGraph graph;
    std::vector<std::string> nodeNames = graph.topSort(prng()());
    ASSERT_EQUALS(nodeNames.size(), 0u);
}

TEST_F(DependencyGraphTest, TopSortGraphNoDeps) {
    DependencyGraph graph;
    graph.addNode("A", {}, {});
    graph.addNode("B", {}, {});
    graph.addNode("C", {}, {});
    auto nodeNames = graph.topSort(prng()());
    ASSERT_EQ(nodeNames.size(), 3);
    ASSERT_EQ(count(nodeNames, "A"), 1);
    ASSERT_EQ(count(nodeNames, "B"), 1);
    ASSERT_EQ(count(nodeNames, "C"), 1);
}

TEST_F(DependencyGraphTest, TopSortGraphSameShuffle) {
    auto makeGraph = [] {
        DependencyGraph graph;
        graph.addNode("1", {}, {});
        graph.addNode("A", {"1"}, {});
        graph.addNode("B", {"1"}, {});
        graph.addNode("C", {"1"}, {});
        graph.addNode("D", {"1"}, {});
        graph.addNode("E", {"1"}, {});
        graph.addNode("F", {"1"}, {});
        return graph.topSort(123456);
    };
    ASSERT_EQ(makeGraph(), makeGraph());
}

/**
 * Verify a node order for the diamond topology used in several tests.
 * Specifically, this graph:
 *
 * A
 * |
 * +->B
 * |  |
 * +---->C
 *    |  |
 *    +--+->D
 *
 * `B` and `C` have no order relative to each other, but both must
 * happen after `A` and before `D`.
 */
void checkDiamondTopology(const std::vector<std::string>& nodeNames) {
    ASSERT_STRING_SEARCH_REGEX(
        fmt::format("{}", fmt::join(nodeNames.begin(), nodeNames.end(), " ")), "^A (B C|C B) D$");
}

TEST_F(DependencyGraphTest, TopSortWithDiamondPrerequisites) {
    /*
     * Top-sorting a simple diamond specified using prerequisites.
     * See checkDiamondTopology for topology.
     */
    DependencyGraph graph;
    graph.addNode("A", {}, {});
    graph.addNode("B", {"A"}, {});
    graph.addNode("C", {"A"}, {});
    graph.addNode("D", {"B", "C"}, {});
    checkDiamondTopology(graph.topSort(prng()()));
}

TEST_F(DependencyGraphTest, TopSortWithDiamondDependents) {
    /*
     * Same diamond topology as preceding test, but specified using dependents.
     */
    DependencyGraph graph;
    graph.addNode("A", {}, {"B", "C"});
    graph.addNode("B", {}, {"D"});
    graph.addNode("C", {}, {"D"});
    graph.addNode("D", {}, {});
    checkDiamondTopology(graph.topSort(prng()()));
}

TEST_F(DependencyGraphTest, TopSortWithDiamondGeneral1) {
    /*
     * Same diamond topology, specified completely by prerequisites and
     * dependents declared on B and C.
     */
    DependencyGraph graph;
    graph.addNode("A", {}, {});
    graph.addNode("B", {"A"}, {"D"});
    graph.addNode("C", {"A"}, {"D"});
    graph.addNode("D", {}, {});
    checkDiamondTopology(graph.topSort(prng()()));
}

TEST_F(DependencyGraphTest, TopSortWithDiamondGeneral2) {
    /*
     * Same diamond topology, specified completely by prerequisites and
     * dependents declared on A and D.
     */
    DependencyGraph graph;
    graph.addNode("A", {}, {"B", "C"});
    graph.addNode("B", {}, {});
    graph.addNode("C", {}, {});
    graph.addNode("D", {"C", "B"}, {});
    checkDiamondTopology(graph.topSort(prng()()));
}

TEST_F(DependencyGraphTest, TopSortWithDiamondGeneral3) {
    /*
     * Same diamond topology, specified by redundant but coherent constraints.
     */
    DependencyGraph graph;
    graph.addNode("A", {}, {"B", "C"});
    graph.addNode("B", {"A"}, {"D"});
    graph.addNode("C", {"A"}, {"D"});
    graph.addNode("D", {"C", "B"}, {});
    checkDiamondTopology(graph.topSort(prng()()));
}

TEST_F(DependencyGraphTest, TopSortWithDiamondAndCycle) {
    /*
     * Cyclic graph. Should fail.
     *
     * A
     * |
     * +->B<-------+
     * |  |        |
     * +---->C     |
     *    |  |     |
     *    +--+->D  |
     *          |  |
     *          +->E
     */
    DependencyGraph graph;
    graph.addNode("A", {}, {"B", "C"});
    graph.addNode("B", {}, {});
    graph.addNode("C", {}, {});
    graph.addNode("D", {"C", "B"}, {});
    graph.addNode("E", {"D"}, {"B"});

    std::vector<std::string> cycle;
    auto check = [](auto&& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::GraphContainsCycle) << ex.toString();
    };
    ASSERT_THROWS_WITH_CHECK(graph.topSort(prng()(), &cycle), DBException, check);
    ASSERT_EQ(cycle.size(), 3);
    ASSERT_EQ(count(cycle, "B"), 1);
    ASSERT_EQ(count(cycle, "D"), 1);
    ASSERT_EQ(count(cycle, "E"), 1);
}

TEST_F(DependencyGraphTest, TopSortFailsWhenMissingPrerequisite) {
    /*
     * If a node names a never-declared prerequisite, topSort should fail.
     */
    DependencyGraph graph;
    graph.addNode("B", {"A"}, {});
    auto check = [&](const DBException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::BadValue) << ex.toString();
        ASSERT_STRING_CONTAINS(ex.reason(), "node B depends on missing node A");
    };
    ASSERT_THROWS_WITH_CHECK(graph.topSort(prng()()), DBException, check);
}

TEST_F(DependencyGraphTest, TopSortFailsWhenMissingDependent) {
    /*
     * If a node names a never-declared dependent, topSort should fail.
     */
    DependencyGraph graph;
    graph.addNode("A", {}, {"B"});
    auto check = [](const DBException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::BadValue) << ex.toString();
        ASSERT_STRING_CONTAINS(ex.reason(), "node B was mentioned but never added");
    };
    ASSERT_THROWS_WITH_CHECK(graph.topSort(prng()()), DBException, check);
}

std::vector<std::vector<std::string>> allPermutations(std::vector<std::string> vec,
                                                      size_t first,
                                                      size_t last) {
    std::vector<std::vector<std::string>> out;
    auto i1 = vec.begin() + first;
    auto i2 = vec.begin() + last;
    std::sort(i1, i2);
    do {
        out.push_back(vec);
    } while (std::next_permutation(i1, i2));
    return out;
}

template <typename Expectations, typename F>
void doUntilAllSeen(const Expectations& expected, F&& f) {
    std::vector<int> seen(expected.size(), 0);
    while (std::find(seen.begin(), seen.end(), 0) != seen.end()) {
        auto found = std::find(expected.begin(), expected.end(), f());
        ASSERT_TRUE(found != expected.end());
        ++seen[found - expected.begin()];
    }
}

TEST_F(DependencyGraphTest, TopSortShufflesNodes) {
    /*
     * Make sure all node orderings can appear as outputs.
     */
    DependencyGraph graph;
    std::vector<std::string> graphNodes;
    for (int i = 0; i < 5; ++i) {
        std::string s = "Node" + std::to_string(i);
        graphNodes.push_back(s);
        graph.addNode(s, {}, {});
    }
    doUntilAllSeen(allPermutations(graphNodes, 0, graphNodes.size()),
                   [&] { return graph.topSort(prng()()); });
}

TEST_F(DependencyGraphTest, TopSortShufflesChildren) {
    /*
     * Make sure all child orderings can appear as outputs.
     */
    DependencyGraph graph;
    std::vector<std::string> graphNodes;
    graphNodes.push_back("Parent");
    graph.addNode("Parent", {}, {});
    for (int i = 0; i < 5; ++i) {
        std::string s = "Child" + std::to_string(i);
        graphNodes.push_back(s);
        graph.addNode(s, {"Parent"}, {});
    }
    // Permute only the children.
    doUntilAllSeen(allPermutations(graphNodes, 1, graphNodes.size()),
                   [&] { return graph.topSort(prng()()); });
}

}  // namespace
}  // namespace mongo
