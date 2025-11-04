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

#include "mongo/db/extension/host/aggregation_stage/parse_node.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::extension {

namespace {

class NoOpHostParseNode : public host::AggStageParseNode {
public:
    static inline std::unique_ptr<host::AggStageParseNode> make(BSONObj spec) {
        return std::make_unique<NoOpHostParseNode>(spec);
    }
};

class NoOpExtensionParseNode : public sdk::AggStageParseNode {
public:
    NoOpExtensionParseNode() : sdk::AggStageParseNode("$noOp") {}

    size_t getExpandedSize() const override {
        MONGO_UNIMPLEMENTED;
    }

    std::vector<VariantNodeHandle> expand() const override {
        MONGO_UNIMPLEMENTED;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        MONGO_UNIMPLEMENTED;
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<NoOpExtensionParseNode>();
    }
};

class HostParseNodeVTableTest : public unittest::Test {
public:
    // This special handle class is only used within this fixture so that we can unit test the
    // assertVTableConstraints functionality of the handle.
    class TestHostParseNodeVTableHandle : public AggStageParseNodeHandle {
    public:
        TestHostParseNodeVTableHandle(absl::Nonnull<::MongoExtensionAggStageParseNode*> parseNode)
            : AggStageParseNodeHandle(parseNode) {};

        void assertVTableConstraints(const VTable_t& vtable) {
            _assertVTableConstraints(vtable);
        }
    };
};

TEST(HostParseNodeTest, GetSpec) {
    auto spec = BSON("foo" << BSON("bar" << 0));

    // Get BSON spec directly.
    auto parseNode = host::AggStageParseNode{spec};
    ASSERT_TRUE(parseNode.getBsonSpec().binaryEqual(spec));

    // Get BSON spec through handle.
    auto noOpParseNode = new host::HostAggStageParseNode(NoOpHostParseNode::make(spec));
    auto handle = AggStageParseNodeHandle{noOpParseNode};
    ASSERT_TRUE(
        static_cast<host::HostAggStageParseNode*>(handle.get())->getBsonSpec().binaryEqual(spec));
}

TEST(HostParseNodeTest, IsHostAllocated) {
    auto noOpParseNode = new host::HostAggStageParseNode(NoOpHostParseNode::make({}));
    auto handle = AggStageParseNodeHandle{noOpParseNode};

    ASSERT_TRUE(host::HostAggStageParseNode::isHostAllocated(*handle.get()));
}

TEST(HostParseNodeTest, IsNotHostAllocated) {
    auto noOpExtensionParseNode =
        new sdk::ExtensionAggStageParseNode(NoOpExtensionParseNode::make());
    auto handle = AggStageParseNodeHandle{noOpExtensionParseNode};

    ASSERT_FALSE(host::HostAggStageParseNode::isHostAllocated(*handle.get()));
}

DEATH_TEST_F(HostParseNodeVTableTest, InvalidParseNodeVTableFailsGetName, "11217600") {
    auto noOpParseNode = std::make_unique<host::HostAggStageParseNode>(NoOpHostParseNode::make({}));
    auto handle = TestHostParseNodeVTableHandle{noOpParseNode.release()};

    auto vtable = handle.vtable();
    vtable.get_name = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(HostParseNodeVTableTest, InvalidParseNodeVTableFailsGetQueryShape, "10977600") {
    auto noOpParseNode = new host::HostAggStageParseNode(NoOpHostParseNode::make({}));
    auto handle = TestHostParseNodeVTableHandle{noOpParseNode};

    auto vtable = handle.vtable();
    vtable.get_query_shape = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(HostParseNodeVTableTest, InvalidParseNodeVTableFailsGetExpandedSize, "11113800") {
    auto noOpParseNode = new host::HostAggStageParseNode(NoOpHostParseNode::make({}));
    auto handle = TestHostParseNodeVTableHandle{noOpParseNode};

    auto vtable = handle.vtable();
    vtable.get_expanded_size = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(HostParseNodeVTableTest, InvalidParseNodeVTableFailsExpand, "10977601") {
    auto noOpParseNode = new host::HostAggStageParseNode(NoOpHostParseNode::make({}));
    auto handle = TestHostParseNodeVTableHandle{noOpParseNode};

    auto vtable = handle.vtable();
    vtable.expand = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST(HostParseNodeTest, HostGetQueryShapeUnimplemented, "10977800") {
    auto noOpParseNode = new host::HostAggStageParseNode(NoOpHostParseNode::make({}));
    auto handle = AggStageParseNodeHandle{noOpParseNode};

    ::MongoExtensionByteBuf* shape = {};
    handle.vtable().get_query_shape(noOpParseNode, nullptr, &shape);
}

DEATH_TEST(HostParseNodeTest, HostGetExpandedSizeUnimplemented, "11113803") {
    auto noOpParseNode = new host::HostAggStageParseNode(NoOpHostParseNode::make({}));
    auto handle = AggStageParseNodeHandle{noOpParseNode};

    ASSERT_EQ(handle.vtable().get_expanded_size(noOpParseNode), 0);

    // get_expanded_size cannot tassert because the return type is size_t, but the host
    // implementation of get_expanded_size still correctly fails because this expand call checks
    // that the return value of get_expanded_size is > 0.
    [[maybe_unused]] auto expanded = handle.expand();
}

DEATH_TEST(HostParseNodeTest, HostExpandUnimplemented, "10977801") {
    auto noOpParseNode = new host::HostAggStageParseNode(NoOpHostParseNode::make({}));
    auto handle = AggStageParseNodeHandle{noOpParseNode};

    ::MongoExtensionExpandedArray expanded = {};
    handle.vtable().expand(noOpParseNode, &expanded);
}
}  // namespace
}  // namespace mongo::extension
