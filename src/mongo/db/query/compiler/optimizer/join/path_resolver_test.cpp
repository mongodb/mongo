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

#include "mongo/db/query/compiler/optimizer/join/path_resolver.h"

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {
TEST(PathResolverTests, OverridedEmbedPaths) {
    constexpr NodeId baseNodeId = 0;
    std::vector<ResolvedPath> resolvedPaths;
    PathResolver pathResolver{baseNodeId, resolvedPaths};

    // At this moment all paths are resolved to the base node.

    const auto pathA0 = pathResolver.resolve("a");
    ASSERT_TRUE(pathA0.has_value());
    ASSERT_EQ(resolvedPaths[*pathA0].nodeId, baseNodeId);

    const auto pathAB0 = pathResolver.resolve("a.b");
    ASSERT_TRUE(pathAB0.has_value());
    ASSERT_EQ(resolvedPaths[*pathAB0].nodeId, baseNodeId);

    const auto pathB0 = pathResolver.resolve("b");
    ASSERT_TRUE(pathB0.has_value());
    ASSERT_EQ(resolvedPaths[*pathB0].nodeId, baseNodeId);

    const auto pathBC0 = pathResolver.resolve("b.c");
    ASSERT_TRUE(pathBC0.has_value());
    ASSERT_EQ(resolvedPaths[*pathBC0].nodeId, baseNodeId);

    // Override path "b". Paths "b" and "b.c" from the base collection is not accessible any more.
    // Paths "a", "a.b" are still the same.
    constexpr NodeId firstNodeId = 10;  // The index of nodes does not really matter.
    pathResolver.addNode(firstNodeId, "b");

    const auto pathA1 = pathResolver.resolve("a");
    ASSERT_TRUE(pathA1.has_value());
    ASSERT_EQ(*pathA1, *pathA0);
    ASSERT_EQ(resolvedPaths[*pathA0].nodeId, baseNodeId);

    const auto pathAB1 = pathResolver.resolve("a.b");
    ASSERT_TRUE(pathAB1.has_value());
    ASSERT_EQ(*pathAB1, *pathAB0);
    ASSERT_EQ(resolvedPaths[*pathAB0].nodeId, baseNodeId);

    // Conflicts with the document prefix.
    ASSERT_EQ(pathResolver.resolve("b"), boost::none);

    const auto pathBC1 = pathResolver.resolve("b.c");
    ASSERT_TRUE(pathBC1.has_value());
    ASSERT_NE(*pathBC0, *pathBC1);
    ASSERT_EQ(resolvedPaths[*pathBC1].nodeId, firstNodeId);

    // Override path "a". Paths "a" and "a.b" from the base collection is not accessible any more.
    constexpr NodeId secondNodeId = 5;
    pathResolver.addNode(secondNodeId, "a");

    // Conflicts with the document prefix.
    ASSERT_EQ(pathResolver.resolve("a"), boost::none);

    const auto pathAB2 = pathResolver.resolve("a.b");
    ASSERT_TRUE(pathAB2.has_value());
    ASSERT_NE(*pathAB0, *pathAB2);
    ASSERT_EQ(resolvedPaths[*pathAB2].nodeId, secondNodeId);

    const auto pathBC2 = pathResolver.resolve("b.c");
    ASSERT_TRUE(pathBC2.has_value());
    ASSERT_EQ(*pathBC2, *pathBC1);
    ASSERT_EQ(resolvedPaths[*pathBC1].nodeId, firstNodeId);

    // Override path "b" again.
    constexpr NodeId thirdNodeId = 7;  // The index of nodes does not really matter.
    pathResolver.addNode(thirdNodeId, "b");

    const auto pathBC3 = pathResolver.resolve("b.c");
    ASSERT_TRUE(pathBC3.has_value());
    ASSERT_NE(*pathBC0, *pathBC3);
    ASSERT_NE(*pathBC1, *pathBC3);
    ASSERT_EQ(resolvedPaths[*pathBC3].nodeId, thirdNodeId);
}

TEST(PathResolverTests, OverridedLongerEmbedPaths) {
    constexpr NodeId baseNodeId = 0;
    std::vector<ResolvedPath> resolvedPaths;
    PathResolver pathResolver{baseNodeId, resolvedPaths};

    const NodeId nodeAB = 1;
    pathResolver.addNode(nodeAB, "a.b");

    auto pathABC0 = pathResolver.resolve("a.b.c");
    ASSERT_TRUE(pathABC0.has_value());
    ASSERT_EQ(nodeAB, resolvedPaths.at(*pathABC0).nodeId);

    const NodeId nodeA = 2;
    pathResolver.addNode(nodeA, "a");
    auto pathABC1 = pathResolver.resolve("a.b.c");
    ASSERT_TRUE(pathABC1.has_value());
    auto pathAB0 = pathResolver.resolve("a.b");
    ASSERT_TRUE(pathAB0.has_value());
    ASSERT_EQ(nodeA, resolvedPaths.at(*pathABC1).nodeId);
    ASSERT_EQ(nodeA, resolvedPaths.at(*pathAB0).nodeId);

    // Bring back a.b.
    const NodeId nodeAB1 = 3;
    pathResolver.addNode(nodeAB1, "a.b");
    auto pathABC2 = pathResolver.resolve("a.b.c");
    ASSERT_TRUE(pathABC2.has_value());
    auto pathAC0 = pathResolver.resolve("a.c");
    ASSERT_TRUE(pathAC0.has_value());
    ASSERT_EQ(nodeAB1, resolvedPaths.at(*pathABC2).nodeId);
    ASSERT_EQ(nodeA, resolvedPaths.at(*pathAC0).nodeId);

    // Bring back a.
    const NodeId nodeA1 = 4;
    pathResolver.addNode(nodeA1, "a");
    auto pathABC3 = pathResolver.resolve("a.b.c");
    ASSERT_TRUE(pathABC3.has_value());
    auto pathAB1 = pathResolver.resolve("a.b");
    ASSERT_TRUE(pathAB1.has_value());
    auto pathAC1 = pathResolver.resolve("a.c");
    ASSERT_TRUE(pathAC1.has_value());
    ASSERT_EQ(nodeA1, resolvedPaths.at(*pathABC3).nodeId);
    ASSERT_EQ(nodeA1, resolvedPaths.at(*pathAB1).nodeId);
    ASSERT_EQ(nodeA1, resolvedPaths.at(*pathAC1).nodeId);

    // a.b again
    const NodeId nodeAB2 = 5;
    pathResolver.addNode(nodeAB2, "a.b");
    auto pathABC4 = pathResolver.resolve("a.b.c");
    ASSERT_TRUE(pathABC4.has_value());
    auto pathAC2 = pathResolver.resolve("a.c");
    ASSERT_TRUE(pathAC2.has_value());
    ASSERT_EQ(nodeAB2, resolvedPaths.at(*pathABC4).nodeId);
    ASSERT_EQ(nodeA1, resolvedPaths.at(*pathAC2).nodeId);

    // Impossible a.b!
    const NodeId nodeAB3 = 6;
    pathResolver.addNode(nodeAB3, "a.b");
    auto pathABC5 = pathResolver.resolve("a.b.c");
    ASSERT_TRUE(pathABC5.has_value());
    auto pathAC3 = pathResolver.resolve("a.c");
    ASSERT_TRUE(pathAC3.has_value());
    ASSERT_EQ(nodeAB3, resolvedPaths.at(*pathABC5).nodeId);
    ASSERT_EQ(nodeA1, resolvedPaths.at(*pathAC3).nodeId);
}

#ifdef MONGO_CONFIG_DEBUG_BUILD
DEATH_TEST(PathResolverDeathTests, OverrideExistingNode, "This node has been already added") {
    constexpr NodeId baseNodeId = 0;
    std::vector<ResolvedPath> resolvedPaths;
    PathResolver pathResolver{baseNodeId, resolvedPaths};

    const NodeId node = 1;
    pathResolver.addNode(node, "a.b");

    pathResolver.addNode(node, "a.b");
}
#endif

TEST(PathResolverTests, AddPath) {
    constexpr NodeId baseNodeId = 11;
    std::vector<ResolvedPath> resolvedPaths;
    PathResolver pathResolver{baseNodeId, resolvedPaths};

    auto pathABC0 = pathResolver.addPath(baseNodeId, "a.b.c");
    ASSERT_EQ(pathABC0, pathResolver.addPath(baseNodeId, "a.b.c"));

    const NodeId nodeA0 = 7;
    pathResolver.addNode(nodeA0, "a");
    auto pathABC1 = pathResolver.addPath(nodeA0, "a.b.c");
    ASSERT_NE(pathABC0, pathABC1);
    ASSERT_EQ(pathABC1, pathResolver.addPath(nodeA0, "a.b.c"));
    auto pathAABC0 = pathResolver.resolve("a.a.b.c");
    ASSERT_TRUE(pathAABC0.has_value());
    ASSERT_EQ(pathABC1, pathResolver.resolve("a.a.b.c"));

    const NodeId nodeA1 = 10;
    pathResolver.addNode(nodeA1, "a");
    auto pathABC2 = pathResolver.addPath(nodeA1, "a.b.c");
    ASSERT_NE(pathABC1, pathABC2);
    ASSERT_NE(pathABC0, pathABC2);
    ASSERT_EQ(pathABC2, pathResolver.addPath(nodeA1, "a.b.c"));
    auto pathAABC1 = pathResolver.resolve("a.a.b.c");
    ASSERT_TRUE(pathAABC1.has_value());
    ASSERT_EQ(pathABC2, *pathAABC1);
}

TEST(PathResolverTests, OverlappingEmbedPaths) {
    constexpr NodeId baseNodeId = 0;
    std::vector<ResolvedPath> resolvedPaths;
    PathResolver pathResolver{baseNodeId, resolvedPaths};

    // At this moment all paths are resolved to the base node.

    const auto pathA0 = pathResolver.resolve("a");
    ASSERT_TRUE(pathA0.has_value());
    ASSERT_EQ(resolvedPaths[*pathA0].nodeId, baseNodeId);

    const auto pathAB0 = pathResolver.resolve("a.b");
    ASSERT_TRUE(pathAB0.has_value());
    ASSERT_EQ(resolvedPaths[*pathAB0].nodeId, baseNodeId);

    const auto pathABC0 = pathResolver.resolve("a.b.c");
    ASSERT_TRUE(pathABC0.has_value());
    ASSERT_EQ(resolvedPaths[*pathABC0].nodeId, baseNodeId);

    const auto pathAC0 = pathResolver.resolve("a.c");
    ASSERT_TRUE(pathAC0.has_value());
    ASSERT_EQ(resolvedPaths[*pathAC0].nodeId, baseNodeId);

    // Override path "a".
    constexpr NodeId firstNodeId = 1;  // The index of nodes does not really matter.
    pathResolver.addNode(firstNodeId, "a");

    // "b" still points to the base node
    const auto pathB0 = pathResolver.resolve("b");
    ASSERT_TRUE(pathB0.has_value());
    ASSERT_EQ(resolvedPaths[*pathB0].nodeId, baseNodeId);

    // "a.b" and "a.c" point to the first node
    const auto pathAB1 = pathResolver.resolve("a.b");
    ASSERT_TRUE(pathAB1.has_value());
    ASSERT_EQ(resolvedPaths[*pathAB1].nodeId, firstNodeId);
    const auto pathAC1 = pathResolver.resolve("a.c");
    ASSERT_TRUE(pathAC1.has_value());
    ASSERT_EQ(resolvedPaths[*pathAC1].nodeId, firstNodeId);

    // Override path "a.b"
    constexpr NodeId secondNodeId = 2;  // The index of nodes does not really matter.
    pathResolver.addNode(secondNodeId, "a.b");

    // "b" still points to the base node
    const auto pathB1 = pathResolver.resolve("b");
    ASSERT_TRUE(pathB1.has_value());
    ASSERT_EQ(resolvedPaths[*pathB1].nodeId, baseNodeId);

    // "a.b.c" point to the second node
    const auto pathABC1 = pathResolver.resolve("a.b.c");
    ASSERT_TRUE(pathABC1.has_value());
    ASSERT_EQ(resolvedPaths[*pathABC1].nodeId, secondNodeId);

    // "a.c" still points to the first node
    const auto pathAC2 = pathResolver.resolve("a.c");
    ASSERT_TRUE(pathAC2.has_value());
    ASSERT_EQ(resolvedPaths[*pathAC2].nodeId, firstNodeId);
}


TEST(PathResolverTests, ConflictingEmbedPaths) {
    // At this moment all paths are resolved to the base node.
    constexpr NodeId baseNodeId = 0;
    std::vector<ResolvedPath> resolvedPaths;
    PathResolver pathResolver{baseNodeId, resolvedPaths};

    // At this momment all paths are resolved to the base node.
    ASSERT_FALSE(pathResolver.pathResolvesToJoinNode("a", baseNodeId));
    ASSERT_FALSE(pathResolver.pathResolvesToJoinNode("a.b", baseNodeId));
    ASSERT_FALSE(pathResolver.pathResolvesToJoinNode("b", baseNodeId));
    ASSERT_FALSE(pathResolver.pathResolvesToJoinNode("b.c", baseNodeId));
    ASSERT_FALSE(pathResolver.pathResolvesToJoinNode("b.e", baseNodeId));

    constexpr NodeId firstNodeId = 10;  // The index of nodes does not really matter.
    pathResolver.addNode(firstNodeId, "b.c");

    // These paths will no longer resolve to the base node because we have added
    // the "b.c" prefix to the graph in firstNode.
    ASSERT_TRUE(pathResolver.pathResolvesToJoinNode("b", baseNodeId));
    ASSERT_TRUE(pathResolver.pathResolvesToJoinNode("b.c", baseNodeId));

    // These paths will still resolve to the base node.
    ASSERT_FALSE(pathResolver.pathResolvesToJoinNode("b.e", baseNodeId));
    ASSERT_FALSE(pathResolver.pathResolvesToJoinNode("a", baseNodeId));
    ASSERT_FALSE(pathResolver.pathResolvesToJoinNode("a.b", baseNodeId));

    constexpr NodeId secondNodeId = 100;  // The index of nodes does not really matter.
    pathResolver.addNode(secondNodeId, "a");
    ASSERT_TRUE(pathResolver.pathResolvesToJoinNode("a", baseNodeId));
    ASSERT_TRUE(pathResolver.pathResolvesToJoinNode("a.b", baseNodeId));

    // This path doesn't conflict with "a" or "b.e".
    ASSERT_FALSE(pathResolver.pathResolvesToJoinNode("b.e", baseNodeId));
}

TEST(PathResolverTests, EmbedPathShadowsResolvedPredicatePath) {
    constexpr NodeId baseNodeId = 0;
    std::vector<ResolvedPath> resolvedPaths;
    PathResolver pathResolver{baseNodeId, resolvedPaths};

    // Simulate the first $lookup: {localField: "a.x", foreignField: "k", as: "matched"}. The
    // localField "a.x" resolves to the base node and the foreignField "k" is added to the foreign
    // node, mimicking how AggJoinModel processes a $lookup with a local/foreign field join.
    auto localPath = pathResolver.resolve("a.x");
    ASSERT_TRUE(localPath.has_value());
    ASSERT_EQ(resolvedPaths[*localPath].nodeId, baseNodeId);

    constexpr NodeId matchedNodeId = 1;
    pathResolver.addNode(matchedNodeId, "matched");
    pathResolver.addPath(matchedNodeId, "k");

    // A second $lookup with as: "a" embeds its results at "a", which overwrites the base field
    // "a.x" that the first lookup uses as its localField. Reordering the second lookup ahead of
    // the first would corrupt that predicate, so this $lookup's "as" field must be rejected even
    // though "a" does not collide with any existing embed path.
    ASSERT_TRUE(pathResolver.pathResolvesToJoinNode("a", baseNodeId));
    // Overwriting the resolved path exactly is also a conflict.
    ASSERT_TRUE(pathResolver.pathResolvesToJoinNode("a.x", baseNodeId));
    // Overwriting a path nested under the resolved path is a conflict too.
    ASSERT_TRUE(pathResolver.pathResolvesToJoinNode("a.x.y", baseNodeId));

    // Sibling paths that don't overlap the resolved path "a.x" remain eligible.
    ASSERT_FALSE(pathResolver.pathResolvesToJoinNode("a.y", baseNodeId));
    ASSERT_FALSE(pathResolver.pathResolvesToJoinNode("b", baseNodeId));
}
}  // namespace mongo::join_ordering
