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
#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/extension_status.h"
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
 * Helper that builds a DocumentResultsAndMetadataAstNode with an optionally-populated DPL callback
 * owner.
 */
std::unique_ptr<host::AggStageAstNode> makeDRMAstNode(host::DPLCallbackOwner dplOwner = {}) {
    auto stageBson = BSON(DocumentSourceInternalDocumentResultsAndMetadata::kStageName
                          << BSON("source" << BSON("$collStats" << BSONObj())))
                         .getOwned();
    return std::make_unique<host::DocumentResultsAndMetadataAstNode>(stageBson,
                                                                     std::move(dplOwner));
}

// Minimal DPL callback: emits a {score: -1} merge-sort pattern and no metadata merge pipeline.
::MongoExtensionStatus* scoreSortNoMergeDplCallback(void*,
                                                    ::MongoExtensionQueryExecutionContext*,
                                                    ::MongoExtensionByteBuf** sortOut,
                                                    ::MongoExtensionByteBuf** mergeOut) {
    *sortOut = new ByteBuf(BSON("score" << -1));
    *mergeOut = nullptr;
    return &ExtensionStatusOK::getInstance();
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

TEST(HostAstNodeCloneTest, CloneDRMNodePreservesDPLCallback) {
    // The DPL callback owner is single-use: its destroy hook must run exactly once across all
    // shared copies. clone() must SHARE the owner with the clone - neither dropping it (which would
    // run destroy when the original alone is destroyed) nor giving the clone an independent owner
    // (which would run destroy a second time). 'userData' points at a counter the destroy hook
    // increments, so we can observe both failure modes through a clone.
    int destroyCount = 0;
    ::MongoExtensionDocResultsDPLCallback callback =
        [](void*,
           ::MongoExtensionQueryExecutionContext*,
           ::MongoExtensionByteBuf**,
           ::MongoExtensionByteBuf**) -> ::MongoExtensionStatus* {
        return nullptr;
    };
    void (*destroyFn)(void*) = [](void* userData) {
        ++*static_cast<int*>(userData);
    };

    {
        host::DPLCallbackOwner dplOwner{callback, &destroyCount, destroyFn};
        ASSERT_TRUE(dplOwner.hasCallback());

        auto drm = makeDRMAstNode(std::move(dplOwner));
        auto cloned = drm->clone();
        ASSERT_TRUE(dynamic_cast<host::DocumentResultsAndMetadataAstNode*>(cloned.get()) !=
                    nullptr);

        // Destroying the original must NOT run the destroy hook while the clone still shares the
        // owner. If clone() had dropped the callback, destroyCount would already be 1 here.
        drm.reset();
        ASSERT_EQ(destroyCount, 0);
        // 'cloned' goes out of scope at the end of this block.
    }
    // The shared destroy hook ran exactly once across the original and the clone.
    ASSERT_EQ(destroyCount, 1);
}

// getOrInvoke() is the bridge logic: it invokes the extension's single-use C callback, takes
// ownership of the returned buffers, and parses them into a ShardedPlanSpec.
TEST(HostAstNodeDplTest, DplCallbackGetOrInvokeParsesSortAndMergePipeline) {
    auto callback = [](void*,
                       ::MongoExtensionQueryExecutionContext*,
                       ::MongoExtensionByteBuf** sortOut,
                       ::MongoExtensionByteBuf** mergeOut) -> ::MongoExtensionStatus* {
        *sortOut = new ByteBuf(BSON("score" << -1));
        *mergeOut = new ByteBuf(BSON_ARRAY(BSON("$limit" << 1) << BSON("$skip" << 2)));
        return &ExtensionStatusOK::getInstance();
    };
    host::DPLCallbackOwner owner{callback, /*userData*/ nullptr, /*destroyFn*/ nullptr};
    ASSERT_TRUE(owner.hasCallback());

    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const auto& result = owner.getOrInvoke(expCtx.get());
    ASSERT_EQ(result.resultsSortPattern.woCompare(BSON("score" << -1)), 0);
    ASSERT_EQ(result.metaMergePipeline.size(), 2u);
    ASSERT_EQ(result.metaMergePipeline[0].woCompare(BSON("$limit" << 1)), 0);
    ASSERT_EQ(result.metaMergePipeline[1].woCompare(BSON("$skip" << 2)), 0);
}

TEST(HostAstNodeDplTest, DplCallbackGetOrInvokeHandlesNullMergePipeline) {
    host::DPLCallbackOwner owner{scoreSortNoMergeDplCallback, nullptr, nullptr};

    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const auto& result = owner.getOrInvoke(expCtx.get());
    ASSERT_EQ(result.resultsSortPattern.woCompare(BSON("score" << -1)), 0);
    ASSERT_TRUE(result.metaMergePipeline.empty());
}

// The extension callback consumes single-use output buffers, but the planner queries
// distributedPlanLogic() multiple times, so getOrInvoke() must invoke the callback at most once and
// return the cached result thereafter.
TEST(HostAstNodeDplTest, DplCallbackGetOrInvokeInvokesAtMostOnce) {
    int callCount = 0;
    auto callback = [](void* userData,
                       ::MongoExtensionQueryExecutionContext*,
                       ::MongoExtensionByteBuf** sortOut,
                       ::MongoExtensionByteBuf** mergeOut) -> ::MongoExtensionStatus* {
        ++*static_cast<int*>(userData);
        *sortOut = new ByteBuf(BSON("score" << -1));
        *mergeOut = nullptr;
        return &ExtensionStatusOK::getInstance();
    };
    host::DPLCallbackOwner owner{callback, &callCount, nullptr};

    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const auto& first = owner.getOrInvoke(expCtx.get());
    const auto& second = owner.getOrInvoke(expCtx.get());
    ASSERT_EQ(callCount, 1);
    // Same cached object returned on every call.
    ASSERT_EQ(&first, &second);
}

// End-to-end: expandToDocumentSource() wraps the AST node's DPL callback into the stage's
// sharded-plan provider, so the resulting DocumentSource's distributedPlanLogic() (owned by
// SERVER-126255) produces the merge sort pattern from the callback.
TEST(HostAstNodeDplBridgeTest, ExpandWiresCallbackIntoDistributedPlanLogic) {
    auto drmNode =
        makeDRMAstNode(host::DPLCallbackOwner{scoreSortNoMergeDplCallback, nullptr, nullptr});

    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto stages = drmNode->expandToDocumentSource(expCtx);
    ASSERT_EQ(stages.size(), 1u);
    auto* drm =
        dynamic_cast<DocumentSourceInternalDocumentResultsAndMetadata*>(stages.front().get());
    ASSERT_TRUE(drm != nullptr);

    auto dpl = drm->distributedPlanLogic(nullptr);
    ASSERT_TRUE(dpl.has_value());
    ASSERT_TRUE(dpl->mergeSortPattern.has_value());
    ASSERT_EQ(dpl->mergeSortPattern->woCompare(BSON("score" << -1)), 0);
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
