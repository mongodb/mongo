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
#include "mongo/db/extension/host_connector/handle/aggregation_stage/ast_node.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::extension {

namespace {

class NoOpHostAstNode : public host::AggStageAstNode {
public:
    explicit NoOpHostAstNode(std::unique_ptr<mongo::LiteParsedDocumentSource> lp)
        : host::AggStageAstNode(std::move(lp)) {}

    static inline std::unique_ptr<host::AggStageAstNode> make(
        std::unique_ptr<mongo::LiteParsedDocumentSource> lp) {
        return std::make_unique<NoOpHostAstNode>(std::move(lp));
    }
};

class NoOpExtensionAstNode : public sdk::AggStageAstNode {
public:
    NoOpExtensionAstNode() : sdk::AggStageAstNode("$noOp") {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        MONGO_UNIMPLEMENTED;
    }
    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<NoOpExtensionAstNode>();
    }
};

class HostAstNodeVTableTest : public unittest::Test {
public:
    // This special handle class is only used within this fixture so that we can unit test the
    // assertVTableConstraints functionality of the handle.
    class TestHostAstNodeVTableHandle : public host_connector::AggStageAstNodeHandle {
    public:
        TestHostAstNodeVTableHandle(absl::Nonnull<::MongoExtensionAggStageAstNode*> astNode)
            : host_connector::AggStageAstNodeHandle(astNode) {};

        void assertVTableConstraints(const VTable_t& vtable) {
            _assertVTableConstraints(vtable);
        }
    };
};

TEST(HostAstNodeTest, GetSpec) {
    auto spec = BSON("$_internalSearchIdLookup" << BSONObj());

    // Get BSON spec directly, build a LiteParsed that holds the spec.
    auto astNode = host::AggStageAstNode{
        std::make_unique<mongo::DocumentSourceInternalSearchIdLookUp::LiteParsed>(
            "$_internalSearchIdLookup", spec.getOwned())};
    ASSERT_TRUE(astNode.getIdLookupSpec().binaryEqual(spec));

    // Get BSON spec through handle.
    auto noOpAstNode = new host::HostAggStageAstNode(NoOpHostAstNode::make(
        std::make_unique<mongo::DocumentSourceInternalSearchIdLookUp::LiteParsed>(
            "$_internalSearchIdLookup", spec.getOwned())));
    auto handle = host_connector::AggStageAstNodeHandle{noOpAstNode};
    ASSERT_TRUE(
        static_cast<host::HostAggStageAstNode*>(handle.get())->getIdLookupSpec().binaryEqual(spec));
}

TEST(HostAstNodeTest, IsHostAllocated) {
    auto noOpAstNode = new host::HostAggStageAstNode(NoOpHostAstNode::make(
        std::make_unique<mongo::DocumentSourceInternalSearchIdLookUp::LiteParsed>(
            "$_internalSearchIdLookup", BSONObj())));
    auto handle = host_connector::AggStageAstNodeHandle{noOpAstNode};

    ASSERT_TRUE(host::HostAggStageAstNode::isHostAllocated(*handle.get()));
}

TEST(HostAstNodeTest, IsNotHostAllocated) {
    auto noOpExtensionAstNode = new sdk::ExtensionAggStageAstNode(NoOpExtensionAstNode::make());
    auto handle = host_connector::AggStageAstNodeHandle{noOpExtensionAstNode};

    ASSERT_FALSE(host::HostAggStageAstNode::isHostAllocated(*handle.get()));
}

DEATH_TEST_F(HostAstNodeVTableTest, InvalidParseNodeVTableFailsGetName, "11217601") {
    auto noOpAstNode = std::make_unique<host::HostAggStageAstNode>(NoOpHostAstNode::make({}));
    auto handle = TestHostAstNodeVTableHandle{noOpAstNode.release()};

    auto vtable = handle.vtable();
    vtable.get_name = nullptr;
    handle.assertVTableConstraints(vtable);
}

DEATH_TEST_F(HostAstNodeVTableTest, InvalidParseNodeVTableFailsBind, "11113700") {
    auto noOpAstNode = new host::HostAggStageAstNode(NoOpHostAstNode::make(
        std::make_unique<mongo::DocumentSourceInternalSearchIdLookUp::LiteParsed>(
            "$_internalSearchIdLookup", BSONObj())));
    auto handle = TestHostAstNodeVTableHandle{noOpAstNode};

    auto vtable = handle.vtable();
    vtable.bind = nullptr;
    handle.assertVTableConstraints(vtable);
}

DEATH_TEST(HostAstNodeTest, HostBindUnimplemented, "11133600") {
    auto noOpAstNode = new host::HostAggStageAstNode(NoOpHostAstNode::make({}));
    auto handle = host_connector::AggStageAstNodeHandle{noOpAstNode};

    ::MongoExtensionLogicalAggStage** bind = nullptr;
    handle.vtable().bind(noOpAstNode, bind);
}

}  // namespace
}  // namespace mongo::extension
