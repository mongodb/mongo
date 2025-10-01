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

#include "mongo/db/query/compiler/optimizer/join/join_graph.h"

#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {

namespace {
NamespaceString makeNSS(StringData collName) {
    return NamespaceString::makeLocalCollection(collName);
}
}  // namespace


TEST(JoinGraphTests, AddNode) {
    JoinGraph graph{};

    auto first = graph.addNode(makeNSS("first"), nullptr, boost::none);
    auto second = graph.addNode(makeNSS("second"), nullptr, FieldPath("snd"));

    ASSERT_EQ(graph.numNodes(), 2);
    ASSERT_EQ(graph.numEdges(), 0);

    ASSERT_EQ(graph.getNode(first).collectionName, makeNSS("first"));
    ASSERT_EQ(graph.getNode(second).collectionName, makeNSS("second"));
}

TEST(JoinGraphTests, AddEdge) {
    JoinGraph graph{};

    auto first = graph.addNode(makeNSS("first"), nullptr, boost::none);
    auto second = graph.addNode(makeNSS("second"), nullptr, FieldPath("snd"));
    auto third = graph.addNode(makeNSS("third"), nullptr, FieldPath("trd"));


    auto firstSecond = graph.addSimpleEqualityEdge(first, second, 0, 1);
    auto secondThird = graph.addSimpleEqualityEdge(second, third, 2, 3);


    ASSERT_EQ(graph.numNodes(), 3);
    ASSERT_EQ(graph.numEdges(), 2);

    ASSERT_EQ(graph.getEdge(firstSecond).left, 1 << first);
    ASSERT_EQ(graph.getEdge(firstSecond).right, 1 << second);
    ASSERT_EQ(graph.getEdge(secondThird).left, 1 << second);
    ASSERT_EQ(graph.getEdge(secondThird).right, 1 << third);
}

TEST(JoinGraphTests, getJoinEdges) {
    JoinGraph graph{};

    auto a = graph.addNode(makeNSS("a"), nullptr, boost::none);
    auto b = graph.addNode(makeNSS("b"), nullptr, FieldPath("b"));
    auto c = graph.addNode(makeNSS("c"), nullptr, FieldPath("c"));
    auto d = graph.addNode(makeNSS("d"), nullptr, FieldPath("d"));

    auto ab = graph.addSimpleEqualityEdge(a, b, 0, 1);
    auto ac = graph.addSimpleEqualityEdge(a, c, 2, 3);
    auto cd = graph.addSimpleEqualityEdge(c, d, 4, 5);

    ASSERT_EQ(graph.getJoinEdges(NodeSet{"0001"}, NodeSet{"0010"}), std::vector<EdgeId>{ab});
    ASSERT_EQ(graph.getJoinEdges(NodeSet{"0001"}, NodeSet{"0100"}), std::vector<EdgeId>{ac});
    ASSERT_EQ(graph.getJoinEdges(NodeSet{"0100"}, NodeSet{"1000"}), std::vector<EdgeId>{cd});
    ASSERT_EQ(graph.getJoinEdges(NodeSet{"0001"}, NodeSet{"0110"}), (std::vector<EdgeId>{ab, ac}));
    ASSERT_EQ(graph.getJoinEdges(NodeSet{"1001"}, NodeSet{"0110"}),
              (std::vector<EdgeId>{ab, ac, cd}));
    ASSERT_EQ(graph.getJoinEdges(NodeSet{"0001"}, NodeSet{"1000"}), std::vector<EdgeId>{});
    ASSERT_EQ(graph.getJoinEdges(NodeSet{"1001"}, NodeSet{"1000"}), std::vector<EdgeId>{});
}
}  // namespace mongo::join_ordering
