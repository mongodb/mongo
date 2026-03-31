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
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/ast_node.h"
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

    std::unique_ptr<sdk::LogicalAggStage> bind(
        const ::MongoExtensionCatalogContext& catalogContext) const override {
        MONGO_UNIMPLEMENTED;
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        MONGO_UNIMPLEMENTED;
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<NoOpExtensionAstNode>();
    }
};

TEST(HostAstNodeTest, GetSpec) {
    DocumentSourceIdLookupSpec spec;
    auto expectedBSON = spec.toBSON();
    auto astNode =
        host::AggStageAstNode{std::make_unique<mongo::LiteParsedInternalSearchIdLookUp>(spec)};
    // getIdLookupSpec().toBSON() returns the inner spec, not the full stage BSON.
    ASSERT_TRUE(astNode.getIdLookupSpec().toBSON().binaryEqual(expectedBSON));

    // Get BSON spec through handle.
    DocumentSourceIdLookupSpec spec2;
    auto noOpAstNode = new host::HostAggStageAstNodeAdapter(
        NoOpHostAstNode::make(std::make_unique<mongo::LiteParsedInternalSearchIdLookUp>(spec2)));
    auto handle = AggStageAstNodeHandle{noOpAstNode};
    ASSERT_TRUE(static_cast<host::HostAggStageAstNodeAdapter*>(handle.get())
                    ->getIdLookupSpec()
                    .toBSON()
                    .binaryEqual(expectedBSON));
}

TEST(HostAstNodeTest, IsHostAllocated) {
    DocumentSourceIdLookupSpec spec;

    auto noOpAstNode = new host::HostAggStageAstNodeAdapter(
        NoOpHostAstNode::make(std::make_unique<mongo::LiteParsedInternalSearchIdLookUp>(spec)));
    auto handle = AggStageAstNodeHandle{noOpAstNode};

    ASSERT_TRUE(host::HostAggStageAstNodeAdapter::isHostAllocated(*handle.get()));
}

TEST(HostAstNodeTest, IsNotHostAllocated) {
    auto noOpExtensionAstNode =
        new sdk::ExtensionAggStageAstNodeAdapter(NoOpExtensionAstNode::make());
    auto handle = AggStageAstNodeHandle{noOpExtensionAstNode};

    ASSERT_FALSE(host::HostAggStageAstNodeAdapter::isHostAllocated(*handle.get()));
}

// TODO SERVER-123101: Move these death tests to host_aggregation_stage_death_test.cpp alongside the
// other host adapter death tests (see host_aggregation_stage_death_test.cpp).
DEATH_TEST(HostAstNodeVTableTestDeathTest, InvalidAstNodeVTableFailsGetName, "11217601") {
    auto vtable = host::HostAggStageAstNodeAdapter::getVTable();
    vtable.get_name = nullptr;
    AggStageAstNodeAPI::assertVTableConstraints(vtable);
}

DEATH_TEST(HostAstNodeVTableTestDeathTest, InvalidAstNodeVTableFailsGetProperties, "11347800") {
    auto vtable = host::HostAggStageAstNodeAdapter::getVTable();
    vtable.get_properties = nullptr;
    AggStageAstNodeAPI::assertVTableConstraints(vtable);
}

DEATH_TEST(HostAstNodeVTableTestDeathTest, InvalidAstNodeVTableFailsBind, "11113700") {
    auto vtable = host::HostAggStageAstNodeAdapter::getVTable();
    vtable.bind = nullptr;
    AggStageAstNodeAPI::assertVTableConstraints(vtable);
}

DEATH_TEST(HostAstNodeVTableTestDeathTest,
           InvalidAstNodeVTableFailsGetFirstStageViewApplicationPolicy,
           "11507400") {
    auto vtable = host::HostAggStageAstNodeAdapter::getVTable();
    vtable.get_first_stage_view_application_policy = nullptr;
    AggStageAstNodeAPI::assertVTableConstraints(vtable);
}

DEATH_TEST(HostAstNodeVTableTestDeathTest, InvalidAstNodeVTableFailsBindViewInfo, "11507500") {
    auto vtable = host::HostAggStageAstNodeAdapter::getVTable();
    vtable.bind_view_info = nullptr;
    AggStageAstNodeAPI::assertVTableConstraints(vtable);
}

DEATH_TEST(HostAstNodeTestDeathTest, HostGetPropertiesUnimplemented, "11347801") {
    auto noOpAstNode = new host::HostAggStageAstNodeAdapter(NoOpHostAstNode::make({}));
    auto handle = AggStageAstNodeHandle{noOpAstNode};

    ::MongoExtensionByteBuf** buf = nullptr;
    handle.get()->vtable->get_properties(noOpAstNode, buf);
}

DEATH_TEST(HostAstNodeTestDeathTest, HostBindUnimplemented, "11133600") {
    auto noOpAstNode = new host::HostAggStageAstNodeAdapter(NoOpHostAstNode::make({}));
    auto handle = AggStageAstNodeHandle{noOpAstNode};

    ::MongoExtensionLogicalAggStage** bind = nullptr;
    handle.get()->vtable->bind(noOpAstNode, nullptr, bind);
}

TEST(HostAstNodeCloneTest, CloneHostAllocatedAstNodePreservesSpec) {
    DocumentSourceIdLookupSpec spec;
    auto expectedBSON = spec.toBSON();

    auto astNode = new host::HostAggStageAstNodeAdapter(
        NoOpHostAstNode::make(std::make_unique<mongo::LiteParsedInternalSearchIdLookUp>(spec)));
    auto handle = AggStageAstNodeHandle{astNode};

    // Clone the AST node.
    auto clonedHandle = handle->clone();

    // Verify the clone has the same spec and name.
    ASSERT_TRUE(host::HostAggStageAstNodeAdapter::isHostAllocated(*clonedHandle.get()));
    // getIdLookupSpec().toBSON() returns the inner spec, not the full stage BSON.
    ASSERT_TRUE(static_cast<host::HostAggStageAstNodeAdapter*>(clonedHandle.get())
                    ->getIdLookupSpec()
                    .toBSON()
                    .binaryEqual(expectedBSON));
    ASSERT_EQ(handle->getName(), clonedHandle->getName());
}

TEST(HostAstNodeCloneTest, CloneHostAllocatedAstNodeIsIndependent) {
    DocumentSourceIdLookupSpec spec;

    auto astNode = new host::HostAggStageAstNodeAdapter(
        NoOpHostAstNode::make(std::make_unique<mongo::LiteParsedInternalSearchIdLookUp>(spec)));
    auto handle = AggStageAstNodeHandle{astNode};

    // Clone the AST node.
    auto clonedHandle = handle->clone();

    // Verify they are different objects (different pointers).
    ASSERT_NE(handle.get(), clonedHandle.get());

    // Both should be valid handles.
    ASSERT_TRUE(handle.isValid());
    ASSERT_TRUE(clonedHandle.isValid());
}

TEST(HostAstNodeCloneTest, ClonedAstNodeSurvivesOriginalDestruction) {
    DocumentSourceIdLookupSpec referenceSpec;
    auto expectedBSON = referenceSpec.toBSON();
    AggStageAstNodeHandle clonedHandle{nullptr};

    {
        DocumentSourceIdLookupSpec spec;

        auto astNode = new host::HostAggStageAstNodeAdapter(
            NoOpHostAstNode::make(std::make_unique<mongo::LiteParsedInternalSearchIdLookUp>(spec)));
        auto handle = AggStageAstNodeHandle{astNode};

        // Clone before original goes out of scope.
        clonedHandle = handle->clone();
    }

    // Cloned handle should still be valid and contain the correct spec.
    ASSERT_TRUE(clonedHandle.isValid());
    ASSERT_TRUE(host::HostAggStageAstNodeAdapter::isHostAllocated(*clonedHandle.get()));
    // getIdLookupSpec().toBSON() returns the inner spec, not the full stage BSON.
    ASSERT_TRUE(static_cast<host::HostAggStageAstNodeAdapter*>(clonedHandle.get())
                    ->getIdLookupSpec()
                    .toBSON()
                    .binaryEqual(expectedBSON));
}

TEST(HostAstNodeCloneTest, MultipleCloneAreIndependent) {
    DocumentSourceIdLookupSpec spec;

    auto astNode = new host::HostAggStageAstNodeAdapter(
        NoOpHostAstNode::make(std::make_unique<mongo::LiteParsedInternalSearchIdLookUp>(spec)));
    auto handle = AggStageAstNodeHandle{astNode};

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

DEATH_TEST(HostAstNodeViewPolicyTest,
           HostAstNodeCannotGetFirstStageViewApplicationPolicy,
           "11507401") {
    DocumentSourceIdLookupSpec spec;

    auto astNode = new host::HostAggStageAstNodeAdapter(
        NoOpHostAstNode::make(std::make_unique<mongo::LiteParsedInternalSearchIdLookUp>(spec)));
    auto handle = AggStageAstNodeHandle{astNode};
    handle->getFirstStageViewApplicationPolicy();
}

DEATH_TEST(HostAstNodeViewInfoTest, HostAstNodeCannotBindViewInfo, "11507501") {
    DocumentSourceIdLookupSpec spec;

    auto astNode = new host::HostAggStageAstNodeAdapter(
        NoOpHostAstNode::make(std::make_unique<mongo::LiteParsedInternalSearchIdLookUp>(spec)));
    auto handle = AggStageAstNodeHandle{astNode};

    std::string dbName = "testDbName";
    std::string viewName = "testViewName";
    ::MongoExtensionNamespaceString nss{stringViewAsByteView(dbName.c_str()),
                                        stringViewAsByteView(viewName.c_str())};
    ::MongoExtensionViewInfo viewInfo{nss, 0, nullptr};

    handle->bindViewInfo(viewInfo);
}

}  // namespace
}  // namespace mongo::extension
