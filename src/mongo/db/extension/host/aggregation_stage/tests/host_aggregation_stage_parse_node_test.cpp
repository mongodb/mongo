// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/aggregation_stage/parse_node.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/query_shape_opts_handle.h"
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

    BSONObj getQueryShape(const sdk::QueryShapeOptsHandle&) const override {
        MONGO_UNIMPLEMENTED;
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<NoOpExtensionParseNode>();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<NoOpExtensionParseNode>();
    }
};

TEST(HostParseNodeTest, GetSpec) {
    auto spec = BSON("foo" << BSON("bar" << 0));

    // Get BSON spec directly.
    auto parseNode = host::AggStageParseNode{spec};
    ASSERT_TRUE(parseNode.getBsonSpec().binaryEqual(spec));

    // Get BSON spec through handle.
    auto noOpParseNode = new host::HostAggStageParseNodeAdapter(NoOpHostParseNode::make(spec));
    auto handle = AggStageParseNodeHandle{noOpParseNode};
    ASSERT_TRUE(static_cast<host::HostAggStageParseNodeAdapter*>(handle.get())
                    ->getBsonSpec()
                    .binaryEqual(spec));
}

TEST(HostParseNodeTest, IsHostAllocated) {
    auto noOpParseNode = new host::HostAggStageParseNodeAdapter(NoOpHostParseNode::make({}));
    auto handle = AggStageParseNodeHandle{noOpParseNode};

    ASSERT_TRUE(host::HostAggStageParseNodeAdapter::isHostAllocated(*handle.get()));
}

TEST(HostParseNodeTest, IsNotHostAllocated) {
    auto noOpExtensionParseNode =
        new sdk::ExtensionAggStageParseNodeAdapter(NoOpExtensionParseNode::make());
    auto handle = AggStageParseNodeHandle{noOpExtensionParseNode};

    ASSERT_FALSE(host::HostAggStageParseNodeAdapter::isHostAllocated(*handle.get()));
}

DEATH_TEST(HostParseNodeVTableDeathTest, InvalidParseNodeVTableFailsGetName, "517") {
    auto vtable = host::HostAggStageParseNodeAdapter::getVTable();
    vtable.get_name = nullptr;
    AggStageParseNodeAPI::assertVTableConstraints(vtable);
}

DEATH_TEST(HostParseNodeVTableDeathTest, InvalidParseNodeVTableFailsGetQueryShape, "517") {
    auto vtable = host::HostAggStageParseNodeAdapter::getVTable();
    vtable.get_query_shape = nullptr;
    AggStageParseNodeAPI::assertVTableConstraints(vtable);
}

DEATH_TEST(HostParseNodeVTableDeathTest, InvalidParseNodeVTableFailsExpand, "517") {
    auto vtable = host::HostAggStageParseNodeAdapter::getVTable();
    vtable.expand = nullptr;
    AggStageParseNodeAPI::assertVTableConstraints(vtable);
}

DEATH_TEST(HostParseNodeTestDeathTest, HostExpandUnimplemented, "10977801") {
    auto noOpParseNode = new host::HostAggStageParseNodeAdapter(NoOpHostParseNode::make({}));
    auto handle = AggStageParseNodeHandle{noOpParseNode};

    ::MongoExtensionExpandedArrayContainer* container = nullptr;
    handle.get()->vtable->expand(noOpParseNode, &container);
}

TEST(HostParseNodeCloneTest, CloneHostAllocatedParseNodePreservesSpec) {
    auto spec = BSON("$match" << BSON("x" << 1));

    auto parseNode = new host::HostAggStageParseNodeAdapter(NoOpHostParseNode::make(spec));
    auto handle = AggStageParseNodeHandle{parseNode};

    // Clone the parse node.
    auto clonedHandle = handle->clone();

    // Verify the clone has the same spec and name.
    ASSERT_TRUE(host::HostAggStageParseNodeAdapter::isHostAllocated(*clonedHandle.get()));
    ASSERT_TRUE(static_cast<host::HostAggStageParseNodeAdapter*>(clonedHandle.get())
                    ->getBsonSpec()
                    .binaryEqual(spec));
    ASSERT_EQ(handle->getName(), clonedHandle->getName());
}

TEST(HostParseNodeCloneTest, CloneHostAllocatedParseNodeIsIndependent) {
    auto spec = BSON("$skip" << 5);

    auto parseNode = new host::HostAggStageParseNodeAdapter(NoOpHostParseNode::make(spec));
    auto handle = AggStageParseNodeHandle{parseNode};

    // Clone the parse node.
    auto clonedHandle = handle->clone();

    // Verify they are different objects (different pointers).
    ASSERT_NE(handle.get(), clonedHandle.get());

    // Both should be valid handles.
    ASSERT_TRUE(handle.isValid());
    ASSERT_TRUE(clonedHandle.isValid());
}

TEST(HostParseNodeCloneTest, ClonedParseNodeSurvivesOriginalDestruction) {
    auto spec = BSON("$project" << BSON("_id" << 0));
    AggStageParseNodeHandle clonedHandle{nullptr};

    {
        auto parseNode = new host::HostAggStageParseNodeAdapter(NoOpHostParseNode::make(spec));
        auto handle = AggStageParseNodeHandle{parseNode};

        // Clone before original goes out of scope.
        clonedHandle = handle->clone();
    }

    // Cloned handle should still be valid and contain the correct spec.
    ASSERT_TRUE(clonedHandle.isValid());
    ASSERT_TRUE(host::HostAggStageParseNodeAdapter::isHostAllocated(*clonedHandle.get()));
    ASSERT_TRUE(static_cast<host::HostAggStageParseNodeAdapter*>(clonedHandle.get())
                    ->getBsonSpec()
                    .binaryEqual(spec));
}

TEST(HostParseNodeCloneTest, MultipleCloneAreIndependent) {
    auto spec = BSON("$match" << BSON("x" << 1));

    auto parseNode = new host::HostAggStageParseNodeAdapter(NoOpHostParseNode::make(spec));
    auto handle = AggStageParseNodeHandle{parseNode};

    // Create multiple clones.
    auto clone1 = handle->clone();
    auto clone2 = handle->clone();
    auto clone3 = clone1->clone();

    // All four should be different objects.
    ASSERT_NE(handle.get(), clone1.get());
    ASSERT_NE(handle.get(), clone2.get());
    ASSERT_NE(handle.get(), clone3.get());
    ASSERT_NE(clone1.get(), clone2.get());
    ASSERT_NE(clone1.get(), clone3.get());
    ASSERT_NE(clone2.get(), clone3.get());

    // All should have same name.
    ASSERT_EQ(handle->getName(), clone1->getName());
    ASSERT_EQ(handle->getName(), clone2->getName());
    ASSERT_EQ(handle->getName(), clone3->getName());
}

}  // namespace
}  // namespace mongo::extension
