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

#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {
TEST(PathResolverTests, OverridedEmbedPaths) {
    constexpr NodeId baseNodeId = 0;
    std::vector<ResolvedPath> resolvedPaths;
    PathResolver pathResolver{baseNodeId, resolvedPaths};

    // At this momment all paths are resolved to the base node.

    const auto pathA0 = pathResolver.resolve("a");
    ASSERT_EQ(resolvedPaths[pathA0].nodeId, baseNodeId);

    const auto pathAB0 = pathResolver.resolve("a.b");
    ASSERT_EQ(resolvedPaths[pathAB0].nodeId, baseNodeId);

    const auto pathB0 = pathResolver.resolve("b");
    ASSERT_EQ(resolvedPaths[pathB0].nodeId, baseNodeId);

    const auto pathBC0 = pathResolver.resolve("b.c");
    ASSERT_EQ(resolvedPaths[pathBC0].nodeId, baseNodeId);

    // Override path "b". Paths "b" and "b.c" from the base collection is not accessible any more.
    // Paths "a", "a.b" are still the same.
    constexpr NodeId firstNodeId = 10;  // The index of nodes does not really matter.
    pathResolver.addNode(firstNodeId, "b");

    ASSERT_EQ(pathResolver.resolve("a"), pathA0);
    ASSERT_EQ(resolvedPaths[pathA0].nodeId, baseNodeId);

    ASSERT_EQ(pathResolver.resolve("a.b"), pathAB0);
    ASSERT_EQ(resolvedPaths[pathAB0].nodeId, baseNodeId);

    // Conflicts with the document prefix.
    ASSERT_THROWS_CODE(pathResolver.resolve("b"), DBException, 10985001);

    const auto pathBC1 = pathResolver.resolve("b.c");
    ASSERT_NE(pathBC0, pathBC1);
    ASSERT_EQ(resolvedPaths[pathBC1].nodeId, firstNodeId);

    // Override path "a". Paths "a" and "a.b" from the base collection is not accessible any more.
    constexpr NodeId secondNodeId = 5;
    pathResolver.addNode(secondNodeId, "a");

    // Conflicts with the document prefix.
    ASSERT_THROWS_CODE(pathResolver.resolve("a"), DBException, 10985001);

    const auto pathAB2 = pathResolver.resolve("a.b");
    ASSERT_NE(pathAB0, pathAB2);
    ASSERT_EQ(resolvedPaths[pathAB2].nodeId, secondNodeId);

    ASSERT_EQ(pathResolver.resolve("b.c"), pathBC1);
    ASSERT_EQ(resolvedPaths[pathBC1].nodeId, firstNodeId);

    // Override path "b" again.
    constexpr NodeId thirdNodeId = 7;  // The index of nodes does not really matter.
    pathResolver.addNode(thirdNodeId, "b");

    const auto pathBC3 = pathResolver.resolve("b.c");
    ASSERT_NE(pathBC0, pathBC3);
    ASSERT_NE(pathBC1, pathBC3);
    ASSERT_EQ(resolvedPaths[pathBC3].nodeId, thirdNodeId);
}

TEST(PathResolverTests, OverlappingEmbedPaths) {
    constexpr NodeId baseNodeId = 0;
    std::vector<ResolvedPath> resolvedPaths;
    PathResolver pathResolver{baseNodeId, resolvedPaths};

    // At this momment all paths are resolved to the base node.

    const auto pathA0 = pathResolver.resolve("a");
    ASSERT_EQ(resolvedPaths[pathA0].nodeId, baseNodeId);

    const auto pathAB0 = pathResolver.resolve("a.b");
    ASSERT_EQ(resolvedPaths[pathAB0].nodeId, baseNodeId);

    const auto pathABC0 = pathResolver.resolve("a.b.c");
    ASSERT_EQ(resolvedPaths[pathABC0].nodeId, baseNodeId);

    const auto pathAC0 = pathResolver.resolve("a.c");
    ASSERT_EQ(resolvedPaths[pathAC0].nodeId, baseNodeId);

    // Override path "a".
    constexpr NodeId firstNodeId = 1;  // The index of nodes does not really matter.
    pathResolver.addNode(firstNodeId, "a");

    // "b" still points to the base node
    ASSERT_EQ(resolvedPaths[pathResolver.resolve("b")].nodeId, baseNodeId);

    // "a.b" and "a.c" point to the first node
    ASSERT_EQ(resolvedPaths[pathResolver.resolve("a.b")].nodeId, firstNodeId);
    ASSERT_EQ(resolvedPaths[pathResolver.resolve("a.c")].nodeId, firstNodeId);

    // Override path "a.b"
    constexpr NodeId secondNodeId = 2;  // The index of nodes does not really matter.
    pathResolver.addNode(secondNodeId, "a.b");

    // "b" still points to the base node
    ASSERT_EQ(resolvedPaths[pathResolver.resolve("b")].nodeId, baseNodeId);

    // "a.b.c" point to the second node
    ASSERT_EQ(resolvedPaths[pathResolver.resolve("a.b.c")].nodeId, secondNodeId);

    // "a.c" still points to the first node
    ASSERT_EQ(resolvedPaths[pathResolver.resolve("a.c")].nodeId, firstNodeId);
}
}  // namespace mongo::join_ordering
