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

#include "mongo/db/extension/host/document_source_extension_optimizable.h"

#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::extension {

class DocumentSourceExtensionOptimizableTest : public AggregationContextFixture {
public:
    DocumentSourceExtensionOptimizableTest() : DocumentSourceExtensionOptimizableTest(_nss) {}
    explicit DocumentSourceExtensionOptimizableTest(NamespaceString nsString)
        : AggregationContextFixture(std::move(nsString)) {};

protected:
    static inline NamespaceString _nss = NamespaceString::createNamespaceString_forTest(
        boost::none, "document_source_extension_optimizable_test");

    sdk::ExtensionAggStageDescriptor _transformAggregationStageDescriptor{
        sdk::shared_test_stages::TransformAggStageDescriptor::make()};
};

TEST_F(DocumentSourceExtensionOptimizableTest, transformConstructionSucceeds) {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::TransformAggStageAstNode::make());
    auto astHandle = AggStageAstNodeHandle(astNode);

    DepsTracker deps(QueryMetadataBitSet{});
    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(astHandle));

    ASSERT_EQ(std::string(optimizable->getSourceName()),
              sdk::shared_test_stages::TransformAggStageDescriptor::kStageName);
    ASSERT_DOES_NOT_THROW(optimizable->getDependencies(&deps));
}

TEST_F(DocumentSourceExtensionOptimizableTest, stageCanSerializeForQueryExecution) {
    using sdk::shared_test_stages::TransformAggStageAstNode;
    using sdk::shared_test_stages::TransformAggStageDescriptor;

    auto arguments = BSON("serializedForExecution" << true);
    auto astNode = new sdk::ExtensionAggStageAstNode(std::make_unique<TransformAggStageAstNode>(
        TransformAggStageDescriptor::kStageName, arguments));
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(astHandle));

    // Test that an extension can provide its own implementation of serialize, that might change
    // the raw spec provided.
    ASSERT_BSONOBJ_EQ(optimizable->serialize(SerializationOptions()).getDocument().toBson(),
                      BSON(TransformAggStageDescriptor::kStageName << arguments));
}

using DocumentSourceExtensionOptimizableTestDeathTest = DocumentSourceExtensionOptimizableTest;
DEATH_TEST_F(DocumentSourceExtensionOptimizableTestDeathTest,
             serializeWithWrongOptsFails,
             "11217800") {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::TransformAggStageAstNode::make());
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(astHandle));

    [[maybe_unused]] auto serialized =
        optimizable->serialize(SerializationOptions::kDebugQueryShapeSerializeOptions);
}

TEST_F(DocumentSourceExtensionOptimizableTest, stageWithDefaultStaticProperties) {
    // These should also be the default static properties for Transform stages.
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::TransformAggStageAstNode::make());
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(astHandle));

    const auto& staticProperties = optimizable->getStaticProperties();
    ASSERT_TRUE(staticProperties.getRequiresInputDocSource());
    ASSERT_EQ(staticProperties.getPosition(), MongoExtensionPositionRequirementEnum::kNone);
    ASSERT_EQ(staticProperties.getHostType(), MongoExtensionHostTypeRequirementEnum::kNone);
    ASSERT_TRUE(staticProperties.getPreservesUpstreamMetadata());
    ASSERT_FALSE(staticProperties.getRequiredMetadataFields().has_value());
    ASSERT_FALSE(staticProperties.getProvidedMetadataFields().has_value());

    auto constraints = optimizable->constraints(PipelineSplitState::kUnsplit);

    ASSERT_EQ(constraints.requiredPosition, StageConstraints::PositionRequirement::kNone);
    ASSERT_EQ(constraints.hostRequirement, StageConstraints::HostTypeRequirement::kNone);
    ASSERT_TRUE(constraints.requiresInputDocSource);
    ASSERT_TRUE(constraints.consumesLogicalCollectionData);
}

TEST_F(DocumentSourceExtensionOptimizableTest, searchLikeStageWithSourceStageStaticProperties) {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::SearchLikeSourceAggStageAstNode::make());
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(astHandle));

    const auto& staticProperties = optimizable->getStaticProperties();
    ASSERT_FALSE(staticProperties.getRequiresInputDocSource());
    ASSERT_EQ(staticProperties.getPosition(), MongoExtensionPositionRequirementEnum::kFirst);
    ASSERT_EQ(staticProperties.getHostType(), MongoExtensionHostTypeRequirementEnum::kAnyShard);
    ASSERT_TRUE(staticProperties.getProvidedMetadataFields().has_value());
    ASSERT_TRUE(staticProperties.getRequiredMetadataFields().has_value());

    auto constraints = optimizable->constraints(PipelineSplitState::kUnsplit);

    ASSERT_EQ(constraints.requiredPosition, StageConstraints::PositionRequirement::kFirst);
    ASSERT_EQ(constraints.hostRequirement, StageConstraints::HostTypeRequirement::kAnyShard);
    ASSERT_FALSE(constraints.requiresInputDocSource);
    ASSERT_FALSE(constraints.consumesLogicalCollectionData);
}

TEST_F(DocumentSourceExtensionOptimizableTest, searchLikeStageWithMetadataFields) {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::SearchLikeSourceAggStageAstNode::make());
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(astHandle));

    DepsTracker deps(QueryMetadataBitSet{});
    deps.setMetadataAvailable(DocumentMetadataFields::kScore);

    optimizable->getDependencies(&deps);

    ASSERT_FALSE(deps.getAvailableMetadata()[DocumentMetadataFields::kScore]);
    ASSERT_TRUE(deps.getAvailableMetadata()[DocumentMetadataFields::kSearchHighlights]);
}

TEST_F(DocumentSourceExtensionOptimizableTest,
       searchLikeStageWithMetadataFieldsWithPreserveUpstreamMetadata) {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::SearchLikeSourceWithPreserveUpstreamMetadataAstNode::make());
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(astHandle));

    DepsTracker deps(QueryMetadataBitSet{});
    deps.setMetadataAvailable(DocumentMetadataFields::kSearchScore);

    optimizable->getDependencies(&deps);

    ASSERT_TRUE(deps.getAvailableMetadata()[DocumentMetadataFields::kScore]);
    ASSERT_TRUE(deps.getAvailableMetadata()[DocumentMetadataFields::kSearchHighlights]);
}

TEST_F(DocumentSourceExtensionOptimizableTest, searchLikeStageWithNoSourceMetadata) {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::SearchLikeSourceAggStageAstNode::make());
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(astHandle));

    DepsTracker deps(DepsTracker::kNoMetadata);

    ASSERT_THROWS_CODE(optimizable->getDependencies(&deps), AssertionException, 40218);
}

TEST_F(DocumentSourceExtensionOptimizableTest, searchLikeStageWithNoSuitableSourceMetadata) {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::SearchLikeSourceAggStageAstNode::make());
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(astHandle));

    DepsTracker deps(DepsTracker::kAllGeoNearData);

    ASSERT_THROWS_CODE(optimizable->getDependencies(&deps), AssertionException, 40218);
}

TEST_F(DocumentSourceExtensionOptimizableTest,
       searchLikeStageWithMetadataFieldsWithInvalidRequiredMetadataField) {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::SearchLikeSourceWithInvalidRequiredMetadataFieldAstNode::make());
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(astHandle));

    DepsTracker deps(QueryMetadataBitSet{});
    deps.setMetadataAvailable(DocumentMetadataFields::kSearchScore);

    ASSERT_THROWS_CODE(optimizable->getDependencies(&deps), AssertionException, 17308);
}

TEST_F(DocumentSourceExtensionOptimizableTest,
       searchLikeStageWithMetadataFieldsWithInvalidProvidedMetadataField) {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::SearchLikeSourceWithInvalidProvidedMetadataFieldAstNode::make());
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(astHandle));

    DepsTracker deps(QueryMetadataBitSet{});
    deps.setMetadataAvailable(DocumentMetadataFields::kSearchScore);

    ASSERT_THROWS_CODE(optimizable->getDependencies(&deps), AssertionException, 17308);
}

TEST_F(DocumentSourceExtensionOptimizableTest, distributedPlanLogicReturnsNoneWhenNoDPL) {
    // TransformLogicalAggStage returns nullptr for getDistributedPlanLogic(), which should result
    // in boost::none being returned.
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::TransformAggStageAstNode::make());
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(astHandle));

    auto dpl = optimizable->distributedPlanLogic();
    ASSERT_FALSE(dpl.has_value());
}

class InvalidDPLLogicalStage : public sdk::shared_test_stages::TransformLogicalAggStage {
public:
    boost::optional<sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        sdk::DistributedPlanLogic dpl;

        {
            // Create a merging pipeline with a logical stage that is different from this one.
            std::vector<VariantDPLHandle> elements;
            elements.emplace_back(
                extension::LogicalAggStageHandle{new sdk::ExtensionLogicalAggStage(
                    sdk::shared_test_stages::MergeOnlyDPLLogicalStage::make())});
            dpl.mergingPipeline = sdk::DPLArrayContainer(std::move(elements));
        }

        return dpl;
    }
};

TEST_F(DocumentSourceExtensionOptimizableTest,
       distributedPlanLogicReturnsErrorWhenGivenMismatchedLogicalStage) {
    auto logicalStage =
        new sdk::ExtensionLogicalAggStage(std::make_unique<InvalidDPLLogicalStage>());
    auto logicalStageHandle = LogicalAggStageHandle(logicalStage);

    auto optimizable = host::DocumentSourceExtensionOptimizable::create(
        getExpCtx(), std::move(logicalStageHandle), MongoExtensionStaticProperties{});

    ASSERT_THROWS_CODE(optimizable->distributedPlanLogic(), AssertionException, 11513800);
}

TEST_F(DocumentSourceExtensionOptimizableTest, distributedPlanLogicWithMergeOnlyStage) {
    // Create a stage that returns DPL with merge-only stages containing all three types of
    // VariantDPLHandle.
    auto logicalStage = new sdk::ExtensionLogicalAggStage(
        sdk::shared_test_stages::MergeOnlyDPLLogicalStage::make());
    auto logicalStageHandle = LogicalAggStageHandle(logicalStage);

    // Initialize properties with a non-default value.
    auto properties =
        MongoExtensionStaticProperties::parse(BSON("requiresInputDocSource" << false));

    auto optimizable = host::DocumentSourceExtensionOptimizable::create(
        getExpCtx(), std::move(logicalStageHandle), properties);

    auto dpl = optimizable->distributedPlanLogic();
    ASSERT_TRUE(dpl.has_value());

    const auto& logic = dpl.get();

    // Verify shards pipeline is empty (nullptr).
    ASSERT_EQ(logic.shardsStage, nullptr);

    // Verify merging pipeline has three stages, one for each type of VariantDPLHandle.
    ASSERT_EQ(logic.mergingStages.size(), 3U);

    // Verify the first stage is from a host-defined parse node ($match).
    auto firstStageIt = logic.mergingStages.begin();
    ASSERT_NE(*firstStageIt, nullptr);
    ASSERT_EQ(std::string((*firstStageIt)->getSourceName()), "$match");

    // Verify the second stage is from an extension-defined parse node ($transformStage).
    auto secondStageIt = std::next(firstStageIt);
    ASSERT_NE(*secondStageIt, nullptr);
    ASSERT_EQ(std::string((*secondStageIt)->getSourceName()),
              sdk::shared_test_stages::TransformAggStageDescriptor::kStageName);

    // Verify the third stage is from a logical stage handle ($transformStage).
    auto thirdStageIt = std::next(secondStageIt);
    ASSERT_NE(*thirdStageIt, nullptr);
    ASSERT_EQ(std::string((*thirdStageIt)->getSourceName()),
              sdk::shared_test_stages::kMergeOnlyDPLStageName);
    // The new logical stage should inherit the original stage's static properties.
    ASSERT_EQ(static_cast<host::DocumentSourceExtensionOptimizable*>(thirdStageIt->get())
                  ->getStaticProperties()
                  .getRequiresInputDocSource(),
              properties.getRequiresInputDocSource());

    // Verify sort pattern is empty.
    ASSERT_FALSE(logic.mergeSortPattern.has_value());
}
}  // namespace mongo::extension
