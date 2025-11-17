/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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


#include "mongo/db/query/compiler/optimizer/join/adjacency_matrix.h"

#include "mongo/unittest/unittest.h"


namespace mongo::join_ordering {

namespace {
AdjacencyMatrix makeMatrix(std::vector<std::string_view> rows) {
    AdjacencyMatrix matrix;
    for (size_t i = 0; i < rows.size(); ++i) {
        matrix[i] = NodeSet{rows[i].data(), rows[i].size()};
    }
    return matrix;
}

NamespaceString makeNSS(StringData collName) {
    return NamespaceString::makeLocalCollection(collName);
}

NodeId addNode(JoinGraph& graph) {
    return graph.addNode(makeNSS("test"), nullptr, boost::none);
}

EdgeId addEdge(JoinGraph& graph, NodeId left, NodeId right) {
    return graph.addSimpleEqualityEdge(left, right, 0, 0);
}
}  // namespace

TEST(AdjacencyMatrixBuilderTest, Smoke) {
    JoinGraph graph;
    auto node0 = addNode(graph);
    auto node1 = addNode(graph);
    auto node2 = addNode(graph);
    auto node3 = addNode(graph);
    auto node4 = addNode(graph);

    auto edge01 = addEdge(graph, node0, node1);
    auto edge12 = addEdge(graph, node1, node2);
    auto edge23 = addEdge(graph, node2, node3);
    auto edge34 = addEdge(graph, node3, node4);
    auto edge40 = addEdge(graph, node4, node0);

    // No edges case
    {
        auto expectedMatrix = makeMatrix({
            "00000",
            "00000",
            "00000",
            "00000",
            "00000",
        });
        ASSERT_EQ(makeAdjacencyMatrix(graph, {}), expectedMatrix);
    }

    // All edges case
    {
        auto expectedMatrix = makeMatrix({
            "10010",
            "00101",
            "01010",
            "10100",
            "01001",
        });
        ASSERT_EQ(makeAdjacencyMatrix(graph, {edge01, edge12, edge23, edge34, edge40}),
                  expectedMatrix);
    }

    // 2 edges case
    {
        auto expectedMatrix = makeMatrix({
            "00010",
            "00101",
            "00010",
            "00000",
            "00000",
        });
        ASSERT_EQ(makeAdjacencyMatrix(graph, {edge01, edge12}), expectedMatrix);
    }

    // 1 edge case
    {
        auto expectedMatrix = makeMatrix({
            "00000",
            "00100",
            "00010",
            "00000",
            "00000",
        });
        ASSERT_EQ(makeAdjacencyMatrix(graph, {edge12}), expectedMatrix);
    }
}

TEST(AdjacencyMatrixBuilderTest, OneToAllEdge) {
    JoinGraph graph;
    auto node0 = addNode(graph);
    auto node1 = addNode(graph);
    auto node2 = addNode(graph);
    auto node3 = addNode(graph);
    auto node4 = addNode(graph);

    NodeSet left{};
    left.set(node0);

    NodeSet right;
    right.set(node1);
    right.set(node2);
    right.set(node3);
    right.set(node4);

    auto edge = graph.addEdge(left, right, {});
    auto expectedMatrix = makeMatrix({
        "11110",
        "00001",
        "00001",
        "00001",
        "00001",
    });
    ASSERT_EQ(makeAdjacencyMatrix(graph, {edge}), expectedMatrix);
}

TEST(AdjacencyMatrixBuilderTest, DoubleEdges) {
    JoinGraph graph;
    auto node0 = addNode(graph);
    auto node1 = addNode(graph);
    auto node2 = addNode(graph);
    auto node3 = addNode(graph);
    auto node4 = addNode(graph);
    auto node5 = addNode(graph);

    NodeSet left{};
    NodeSet right{};

    left.set(node0);
    left.set(node2);
    right.set(node1);
    right.set(node3);

    const auto edge0213 = graph.addEdge(left, right, {});

    left.reset();
    right.reset();
    left.set(node2);
    left.set(node4);
    right.set(node3);
    right.set(node5);
    const auto edge2435 = graph.addEdge(left, right, {});

    left.reset();
    right.reset();
    left.set(node3);
    left.set(node5);
    right.set(node0);
    right.set(node2);
    const auto edge3502 = graph.addEdge(left, right, {});

    // No edges case
    {
        auto expectedMatrix = makeMatrix({
            "000000",
            "000000",
            "000000",
            "000000",
            "000000",
            "000000",
        });
        ASSERT_EQ(makeAdjacencyMatrix(graph, {}), expectedMatrix);
    }

    // Single edge case
    {
        auto expectedMatrix = makeMatrix({
            "001010",
            "000101",
            "001010",
            "000101",
            "000000",
            "000000",
        });
        ASSERT_EQ(makeAdjacencyMatrix(graph, {edge0213}), expectedMatrix);
    }

    // two edges case
    {
        auto expectedMatrix = makeMatrix({
            "001010",
            "000101",
            "101010",
            "010101",
            "101000",
            "010100",
        });
        ASSERT_EQ(makeAdjacencyMatrix(graph, {edge0213, edge2435}), expectedMatrix);
    }

    // three edges case
    {
        auto expectedMatrix = makeMatrix({
            "101010",
            "000101",
            "101010",
            "010101",
            "101000",
            "010101",
        });
        ASSERT_EQ(makeAdjacencyMatrix(graph, {edge0213, edge2435, edge3502}), expectedMatrix);
    }
}

TEST(AdjacencyMatrixBuilderTest, FullyConnectedGraph) {
    JoinGraph graph;
    auto node0 = addNode(graph);
    auto node1 = addNode(graph);
    auto node2 = addNode(graph);
    auto node3 = addNode(graph);

    auto edge01 = addEdge(graph, node0, node1);
    auto edge02 = addEdge(graph, node0, node2);
    auto edge03 = addEdge(graph, node0, node3);
    auto edge12 = addEdge(graph, node1, node2);
    auto edge13 = addEdge(graph, node1, node3);
    auto edge23 = addEdge(graph, node2, node3);

    auto expectedMatrix = makeMatrix({
        "1110",
        "1101",
        "1011",
        "0111",
    });

    ASSERT_EQ(makeAdjacencyMatrix(graph, {edge01, edge02, edge03, edge12, edge13, edge23}),
              expectedMatrix);
}
}  // namespace mongo::join_ordering
