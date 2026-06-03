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
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/search/lite_parsed_internal_search_id_lookup.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::extension {

namespace {

/**
 * Helper that builds an IdLookupAstNode wrapping a $_internalSearchIdLookup LiteParsed.
 */
std::unique_ptr<host::AggStageAstNode> makeIdLookupAstNode() {
    DocumentSourceIdLookupSpec spec;
    return std::make_unique<host::IdLookupAstNode>(
        std::make_unique<mongo::LiteParsedInternalSearchIdLookUp>(spec));
}

/**
 * Helper that builds a DocumentResultsAndMetadataAstNode from a DRM stage BSON whose 'source' is an
 * initial source stage ($collStats), so that it can be re-parsed/expanded.
 */
std::unique_ptr<host::AggStageAstNode> makeDRMAstNode() {
    auto stageBson = BSON(DocumentSourceInternalDocumentResultsAndMetadata::kStageName
                          << BSON("source" << BSON("$collStats" << BSONObj())))
                         .getOwned();
    return std::make_unique<host::DocumentResultsAndMetadataAstNode>(stageBson);
}

class NoOpExtensionAstNode : public sdk::AggStageAstNode {
public:
    NoOpExtensionAstNode() : sdk::AggStageAstNode("$noOp") {}

    std::unique_ptr<sdk::LogicalAggStage> promote(
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

TEST(HostAstNodeTest, IdLookupGetName) {
    auto astNode = makeIdLookupAstNode();
    ASSERT_EQ(astNode->getName(), std::string(mongo::LiteParsedInternalSearchIdLookUp::kStageName));
}

TEST(HostAstNodeTest, DRMGetName) {
    auto astNode = makeDRMAstNode();
    ASSERT_EQ(astNode->getName(),
              std::string(DocumentSourceInternalDocumentResultsAndMetadata::kStageName));
}

TEST(HostAstNodeTest, IsHostAllocated) {
    auto noOpAstNode = new host::HostAggStageAstNodeAdapter(makeIdLookupAstNode());
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

DEATH_TEST(HostAstNodeVTableTestDeathTest, InvalidAstNodeVTableFailsPromote, "11113700") {
    auto vtable = host::HostAggStageAstNodeAdapter::getVTable();
    vtable.promote = nullptr;
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
    auto noOpAstNode = new host::HostAggStageAstNodeAdapter(makeIdLookupAstNode());
    auto handle = AggStageAstNodeHandle{noOpAstNode};

    ::MongoExtensionByteBuf** buf = nullptr;
    handle.get()->vtable->get_properties(noOpAstNode, buf);
}

DEATH_TEST(HostAstNodeTestDeathTest, HostPromoteUnimplemented, "11133600") {
    auto noOpAstNode = new host::HostAggStageAstNodeAdapter(makeIdLookupAstNode());
    auto handle = AggStageAstNodeHandle{noOpAstNode};

    ::MongoExtensionLogicalAggStage** bind = nullptr;
    handle.get()->vtable->promote(noOpAstNode, nullptr, bind);
}

TEST(HostAstNodeCloneTest, CloneIdLookupPreservesNameAndType) {
    auto astNode = makeIdLookupAstNode();
    auto cloned = astNode->clone();

    // The clone is an independent IdLookupAstNode with the same name.
    ASSERT_NE(astNode.get(), cloned.get());
    ASSERT_TRUE(dynamic_cast<host::IdLookupAstNode*>(cloned.get()) != nullptr);
    ASSERT_EQ(astNode->getName(), cloned->getName());
}

TEST(HostAstNodeCloneTest, CloneDRMPreservesNameAndType) {
    auto astNode = makeDRMAstNode();
    auto cloned = astNode->clone();

    // The clone is an independent DocumentResultsAndMetadataAstNode with the same name.
    ASSERT_NE(astNode.get(), cloned.get());
    ASSERT_TRUE(dynamic_cast<host::DocumentResultsAndMetadataAstNode*>(cloned.get()) != nullptr);
    ASSERT_EQ(astNode->getName(), cloned->getName());
}

TEST(HostAstNodeCloneTest, CloneHostAllocatedAstNodeIsIndependent) {
    auto astNode = new host::HostAggStageAstNodeAdapter(makeIdLookupAstNode());
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
    AggStageAstNodeHandle clonedHandle{nullptr};

    {
        auto astNode = new host::HostAggStageAstNodeAdapter(makeIdLookupAstNode());
        auto handle = AggStageAstNodeHandle{astNode};

        // Clone before original goes out of scope.
        clonedHandle = handle->clone();
    }

    // Cloned handle should still be valid and carry the correct name.
    ASSERT_TRUE(clonedHandle.isValid());
    ASSERT_TRUE(host::HostAggStageAstNodeAdapter::isHostAllocated(*clonedHandle.get()));
    ASSERT_EQ(clonedHandle->getName(),
              std::string(mongo::LiteParsedInternalSearchIdLookUp::kStageName));
}

TEST(HostAstNodeCloneTest, MultipleCloneAreIndependent) {
    auto astNode = new host::HostAggStageAstNodeAdapter(makeIdLookupAstNode());
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

TEST(HostAstNodeCloneTest, CloneDRMNodeViaAdapterPreservesName) {
    auto astNode = new host::HostAggStageAstNodeAdapter(makeDRMAstNode());
    auto handle = AggStageAstNodeHandle{astNode};

    auto clonedHandle = handle->clone();

    ASSERT_TRUE(host::HostAggStageAstNodeAdapter::isHostAllocated(*clonedHandle.get()));
    ASSERT_NE(handle.get(), clonedHandle.get());
    ASSERT_EQ(clonedHandle->getName(),
              std::string(DocumentSourceInternalDocumentResultsAndMetadata::kStageName));
}

TEST(HostAstNodeExpandTest, DRMNodeExpandsToStageAfterClone) {
    auto drm = makeDRMAstNode();
    auto cloned = drm->clone();
    auto* clonedDrm = dynamic_cast<host::DocumentResultsAndMetadataAstNode*>(cloned.get());
    ASSERT_TRUE(clonedDrm != nullptr);

    // The stored stage BSON is preserved on the clone: expanding it produces a DRM stage.
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto stages = clonedDrm->expandToDocumentSource(expCtx);
    ASSERT_EQ(stages.size(), 1u);
    ASSERT_EQ(stages.front()->getSourceName(),
              DocumentSourceInternalDocumentResultsAndMetadata::kStageName);
}

TEST(HostAstNodeExpandTest, DRMNodeExpandsToLiteParsedAfterClone) {
    auto drm = makeDRMAstNode();
    auto cloned = drm->clone();
    auto* clonedDrm = dynamic_cast<host::DocumentResultsAndMetadataAstNode*>(cloned.get());
    ASSERT_TRUE(clonedDrm != nullptr);

    // The stored stage BSON is preserved on the clone: re-parsing it with the real nss/options
    // produces a DRM LiteParsedDocumentSource.
    auto nss = NamespaceString::createNamespaceString_forTest("testDb.testColl");
    auto liteParsed = clonedDrm->expandToLiteParsed(nss, LiteParserOptions{});
    ASSERT_TRUE(liteParsed != nullptr);
    ASSERT_EQ(liteParsed->getParseTimeName(),
              std::string(DocumentSourceInternalDocumentResultsAndMetadata::kStageName));
}

DEATH_TEST(HostAstNodeViewPolicyTest,
           HostAstNodeCannotGetFirstStageViewApplicationPolicy,
           "11507401") {
    auto astNode = new host::HostAggStageAstNodeAdapter(makeIdLookupAstNode());
    auto handle = AggStageAstNodeHandle{astNode};
    handle->getFirstStageViewApplicationPolicy();
}

DEATH_TEST(HostAstNodeViewInfoTest, HostAstNodeCannotBindViewInfo, "11507501") {
    auto astNode = new host::HostAggStageAstNodeAdapter(makeIdLookupAstNode());
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
