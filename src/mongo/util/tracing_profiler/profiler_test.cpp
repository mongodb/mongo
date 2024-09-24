/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include <algorithm>

#include "mongo/bson/json.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tracing_profiler/profiler.h"

namespace mongo::tracing_profiler {
namespace {
using namespace mongo::tracing_profiler::internal;

unittest::GoldenTestConfig goldenTestConfig{"src/mongo/util/tracing_profiler/test_output"};

CallTree::ChildrenHashMap getChildren(const CallTree::Node& node) {
    return std::visit(OverloadedVisitor{[](const CallTree::ChildrenInlinedMap& map) {
                                            CallTree::ChildrenHashMap r;
                                            for (size_t i = 0; i < map.size; i++) {
                                                r.insert({map.data[i].tagId, map.data[i].nodeId});
                                            }
                                            return r;
                                        },
                                        [](const CallTree::ChildrenHashMap& map) {
                                            return map;
                                        }},
                      node.children);
}

TEST(CallTreeTest, CallTree_Flat) {
    CallTree t;
    ASSERT_EQ(t.getOrInsertChildNode(0, 5), 1);
    ASSERT_EQ(t.getOrInsertChildNode(0, 7), 2);
    ASSERT_EQ(t.getOrInsertChildNode(0, 7), 2);
    ASSERT_EQ(t.getOrInsertChildNode(0, 5), 1);
}

TEST(CallTreeTest, ProfilerStack_Nested) {
    CallTree t;
    ASSERT_EQ(t.getOrInsertChildNode(0, 3), 1);
    ASSERT_EQ(t.getOrInsertChildNode(1, 5), 2);
    ASSERT_EQ(t.getOrInsertChildNode(2, 7), 3);
    ASSERT_EQ(t.getOrInsertChildNode(0, 3), 1);
    ASSERT_EQ(t.getOrInsertChildNode(1, 5), 2);
    ASSERT_EQ(t.getOrInsertChildNode(2, 7), 3);
}

TEST(CallTreeTest, ProfilerStack_NodesNarrow) {
    CallTree t;
    ASSERT_EQ(t.getOrInsertChildNode(0, 3), 1);

    std::vector<std::pair<TagId, NodeId>> node1Children{{5, 2}, {7, 3}};
    CallTree::ChildrenHashMap expectedNode1Children(node1Children.begin(), node1Children.end());
    for (auto& p : node1Children) {
        ASSERT_EQ(t.getOrInsertChildNode(1, p.first), p.second);
    }

    auto& nodes = t.nodes();
    ASSERT_EQ(nodes.size(), 4);

    ASSERT_EQ(nodes[1].tagId, 3);
    ASSERT_EQ(nodes[1].parentId, 0);
    ASSERT_EQ(getChildren(nodes[1]), expectedNode1Children);

    for (auto& p : node1Children) {
        ASSERT_EQ(nodes[p.second].tagId, p.first);
        ASSERT_EQ(nodes[p.second].parentId, 1);
        CallTree::ChildrenHashMap emptyChildren;
        ASSERT_EQ(getChildren(nodes[p.second]), emptyChildren);
    }
}

TEST(CallTreeTest, ProfilerStack_NodesWide) {
    CallTree t;
    ASSERT_EQ(t.getOrInsertChildNode(0, 3), 1);

    std::vector<std::pair<TagId, NodeId>> node1Children{{5, 2}, {7, 3}, {9, 4}, {11, 5}, {13, 6}};
    CallTree::ChildrenHashMap expectedNode1Children(node1Children.begin(), node1Children.end());

    for (auto& p : node1Children) {
        ASSERT_EQ(t.getOrInsertChildNode(1, p.first), p.second);
    }

    auto& nodes = t.nodes();
    ASSERT_EQ(nodes.size(), 7);

    ASSERT_EQ(nodes[1].tagId, 3);
    ASSERT_EQ(nodes[1].parentId, 0);
    ASSERT_EQ(getChildren(nodes[1]), expectedNode1Children);

    for (auto& p : node1Children) {
        ASSERT_EQ(nodes[p.second].tagId, p.first);
        ASSERT_EQ(nodes[p.second].parentId, 1);
        CallTree::ChildrenHashMap emptyChildren;
        ASSERT_EQ(getChildren(nodes[p.second]), emptyChildren);
    }
}
class MockCycleClock : public CycleClockIface {
public:
    int64_t now() final {
        current += 10;
        return current;
    }

    double frequency() final {
        return 1000;
    }

    int64_t current{0};
};

class ProfilerTestFixture : public unittest::Test {
public:
    ProfilerTestFixture()
        : profilerTags(), profiler(&profilerTags, {5, 5}), shard(profiler.createShard()) {}

    template <FixedString name>
    MONGO_COMPILER_ALWAYS_INLINE Profiler::SpanState enterSpan() {
        return shard->enterSpan(profilerTags.getOrInsertTag(name).id, &clock);
    }

    MONGO_COMPILER_ALWAYS_INLINE void leaveSpan(const Profiler::SpanState& state) {
        shard->leaveSpan(state, &clock);
    }

    int doX() {
        auto s = enterSpan<"doX">();
        leaveSpan(s);
        return 5;
    }

    int doY() {
        auto s1 = enterSpan<"doY1">();
        auto x1 = doX();
        leaveSpan(s1);

        auto s2 = enterSpan<"doY2">();
        auto x2 = doX();
        leaveSpan(s2);

        return x1 + x2;
    }

    int doZ() {
        auto s1 = enterSpan<"doZ1">();
        auto y1 = doY();
        leaveSpan(s1);

        auto s2 = enterSpan<"doZ2">();
        auto y2 = doY();
        leaveSpan(s2);

        return y1 + y2;
    }

    ProfilerTags profilerTags;
    Profiler profiler;
    Profiler::ShardUniquePtr shard;
    MockCycleClock clock;
};

TEST_F(ProfilerTestFixture, ProfilerService_Simple) {
    mongo::unittest::GoldenTestContext ctx(&goldenTestConfig);
    auto& os = ctx.outStream();

    for (int i = 0; i < 100; i++)
        doZ();

    auto metrics = profiler.getMetrics(&clock);

    BSONObjBuilder builder;
    metrics.toBson(&builder);
    BSONObj bson = builder.obj();

    os << "Profiler metrics:" << std::endl;
    os << tojson(bson, ExtendedRelaxedV2_0_0, true) << std::endl;
}

}  // namespace
}  // namespace mongo::tracing_profiler
