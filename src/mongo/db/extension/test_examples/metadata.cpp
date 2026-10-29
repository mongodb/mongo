// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/fruits_test_stage.h"

#include <string_view>

namespace sdk = mongo::extension::sdk;

/**
 * ===================================================================
 * Transform stage that appends $sortKey metadata to source documents
 * ===================================================================
 */
static constexpr std::string_view kMultiKeySortStageName = "$validateMultiKeySort";

class ValidateMultiSortKeyExecStage : public mongo::extension::sdk::TestExecStage {
public:
    ValidateMultiSortKeyExecStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestExecStage(stageName, arguments) {}

    mongo::extension::ExtensionGetNextResult getNext(
        const sdk::QueryExecutionContextHandle& execCtx,
        ::MongoExtensionExecAggStage* execStage) override {
        auto input = _getSource()->getNext(execCtx.get());
        if (input.code == mongo::extension::GetNextCode::kPauseExecution) {
            return mongo::extension::ExtensionGetNextResult::pauseExecution();
        }
        if (input.code == mongo::extension::GetNextCode::kEOF) {
            return mongo::extension::ExtensionGetNextResult::eof();
        }
        // If we got here, we must have a document!
        sdk_tassert(11501001, "Failed to get an input document!", input.resultDocument.has_value());

        const auto& inputDoc = input.resultDocument->getUnownedBSONObj();
        auto docId = inputDoc["_id"].numberInt();

        auto documentResult = mongo::extension::ExtensionBSONObj::makeAsByteView(
            input.resultDocument->getUnownedBSONObj());

        return _metadataById.contains(docId)
            ? mongo::extension::ExtensionGetNextResult::advanced(
                  std::move(documentResult),
                  mongo::extension::ExtensionBSONObj::makeAsByteView(_metadataById.at(docId)))
            : mongo::extension::ExtensionGetNextResult::advanced(std::move(documentResult));
    }

private:
    static inline const std::unordered_map<int, mongo::BSONObj> _metadataById = [] {
        std::unordered_map<int, mongo::BSONObj> m;
        m.reserve(5);
        m.emplace(1,
                  BSON("$searchScoreDetails" << BSON("scoreDetails" << "foo") << "$sortKey"
                                             << BSON_ARRAY(0.0 << 0.0)));
        m.emplace(2,
                  BSON("$searchScore" << 0.0 << "$textScore" << 4.0 << "$sortKey"
                                      << BSON_ARRAY(0.0 << 4.0)));
        m.emplace(3,
                  BSON("$searchScore" << 0.0 << "$textScore" << 5.0 << "$sortKey"
                                      << BSON_ARRAY(0.0 << 5.0)));
        m.emplace(4,
                  BSON("$searchScore" << 1.5 << "$textScore" << 0.0 << "$sortKey"
                                      << BSON_ARRAY(1.5 << 0.0)));
        m.emplace(5,
                  BSON("$searchScore" << 2.0 << "$textScore" << 0.0 << "$sortKey"
                                      << BSON_ARRAY(2.0 << 0.0)));
        return m;
    }();
};

class ValidateMultiSortKeyLogicalStage
    : public sdk::TestLogicalStage<ValidateMultiSortKeyExecStage> {
public:
    ValidateMultiSortKeyLogicalStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestLogicalStage<ValidateMultiSortKeyExecStage>(stageName, arguments) {}

    boost::optional<sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        sdk::DistributedPlanLogic dpl;

        std::vector<mongo::extension::VariantDPLHandle> shardElements;
        shardElements.emplace_back(mongo::extension::LogicalAggStageHandle{
            new sdk::ExtensionLogicalAggStageAdapter(clone())});
        dpl.shardsPipeline = sdk::DPLArrayContainer(std::move(shardElements));

        dpl.sortPattern = BSON("$searchScore" << 1 << "$textScore" << 1);

        return dpl;
    }

    std::unique_ptr<sdk::LogicalAggStage> clone() const override {
        return std::make_unique<ValidateMultiSortKeyLogicalStage>(_name, _arguments);
    }
};

class ValidateMultiSortKeyAstNode : public sdk::TestAstNode<ValidateMultiSortKeyLogicalStage> {
public:
    ValidateMultiSortKeyAstNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestAstNode<ValidateMultiSortKeyLogicalStage>(stageName, arguments) {}

    mongo::BSONObj getProperties() const override {
        mongo::extension::MongoExtensionStaticProperties properties;
        mongo::BSONObjBuilder builder;
        // Report that this stage will provide metadata.
        std::vector<std::string> providedMetadata{
            "textScore", "searchScore", "score", "searchScoreDetails", "sortKey"};
        properties.setProvidedMetadataFields(providedMetadata);
        properties.serialize(&builder);
        return builder.obj();
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<ValidateMultiSortKeyAstNode>(getName(), _arguments);
    }
};

DEFAULT_PARSE_NODE(ValidateMultiSortKey);

using ValidateMultiSortKeyTransformStageDescriptor =
    sdk::TestStageDescriptor<"$validateMultiKeySort", ValidateMultiSortKeyParseNode, true>;

class FruitsExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<sdk::shared_test_stages::FruitsAsDocumentsSourceStageDescriptor>(portal);
        _registerStage<ValidateMultiSortKeyTransformStageDescriptor>(portal);
        _registerStage<sdk::shared_test_stages::AddFruitsToDocumentsTransformStageDescriptor>(
            portal);
        _registerStage<
            sdk::shared_test_stages::AddFruitsToDocumentsWithMetadataTransformStageDescriptor>(
            portal);
    }
};

REGISTER_EXTENSION(FruitsExtension)
DEFINE_GET_EXTENSION()
