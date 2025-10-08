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

#include "mongo/db/extension/host/aggregation_stage/ast_node.h"
#include "mongo/db/extension/host_adapter/handle/aggregation_stage/ast_node.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::extension {

namespace {

class NoOpHostAstNode : public host::AggregationStageAstNode {
public:
    static inline std::unique_ptr<host::AggregationStageAstNode> make(BSONObj spec) {
        return std::make_unique<NoOpHostAstNode>(spec);
    }
};

class NoOpExtensionAstNode : public sdk::AggregationStageAstNode {
public:
    std::unique_ptr<sdk::LogicalAggregationStage> bind() const override {
        MONGO_UNIMPLEMENTED;
    }
    static inline std::unique_ptr<sdk::AggregationStageAstNode> make() {
        return std::make_unique<NoOpExtensionAstNode>();
    }
};

class HostAstNodeVTableTest : public unittest::Test {
public:
    // This special handle class is only used within this fixture so that we can unit test the
    // assertVTableConstraints functionality of the handle.
    class TestHostAstNodeVTableHandle : public host_adapter::AggregationStageAstNodeHandle {
    public:
        TestHostAstNodeVTableHandle(absl::Nonnull<::MongoExtensionAggregationStageAstNode*> astNode)
            : host_adapter::AggregationStageAstNodeHandle(astNode) {};

        void assertVTableConstraints(const VTable_t& vtable) {
            _assertVTableConstraints(vtable);
        }
    };
};

TEST(HostAstNodeTest, GetSpec) {
    auto spec = BSON("$_internalSearchIdLookup" << BSONObj());

    // Get BSON spec directly.
    auto astNode = host::AggregationStageAstNode{spec};
    ASSERT_TRUE(astNode.getIdLookupSpec().binaryEqual(spec));

    // Get BSON spec through handle.
    auto noOpAstNode =
        std::make_unique<host::HostAggregationStageAstNode>(NoOpHostAstNode::make(spec));
    auto handle = host_adapter::AggregationStageAstNodeHandle{noOpAstNode.release()};
    ASSERT_TRUE(static_cast<host::HostAggregationStageAstNode*>(handle.get())
                    ->getIdLookupSpec()
                    .binaryEqual(spec));
}

TEST(HostAstNodeTest, IsHostAllocated) {
    auto noOpAstNode =
        std::make_unique<host::HostAggregationStageAstNode>(NoOpHostAstNode::make({}));
    auto handle = host_adapter::AggregationStageAstNodeHandle{noOpAstNode.release()};

    ASSERT_TRUE(host::HostAggregationStageAstNode::isHostAllocated(*handle.get()));
}

TEST(HostAstNodeTest, IsNotHostAllocated) {
    auto noOpExtensionAstNode =
        std::make_unique<sdk::ExtensionAggregationStageAstNode>(NoOpExtensionAstNode::make());
    auto handle = host_adapter::AggregationStageAstNodeHandle{noOpExtensionAstNode.release()};

    ASSERT_FALSE(host::HostAggregationStageAstNode::isHostAllocated(*handle.get()));
}

DEATH_TEST_F(HostAstNodeVTableTest, InvalidParseNodeVTableFailsBind, "11113700") {
    auto noOpAstNode =
        std::make_unique<host::HostAggregationStageAstNode>(NoOpHostAstNode::make({}));
    auto handle = TestHostAstNodeVTableHandle{noOpAstNode.release()};

    auto vtable = handle.vtable();
    vtable.bind = nullptr;
    handle.assertVTableConstraints(vtable);
}

DEATH_TEST(HostAstNodeTest, HostBindUnimplemented, "11133600") {
    auto noOpAstNode =
        std::make_unique<host::HostAggregationStageAstNode>(NoOpHostAstNode::make({}));
    auto handle = host_adapter::AggregationStageAstNodeHandle{noOpAstNode.release()};

    ::MongoExtensionLogicalAggregationStage** bind = nullptr;
    handle.vtable().bind(noOpAstNode.get(), bind);
}

}  // namespace
}  // namespace mongo::extension
