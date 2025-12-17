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

#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/fruits_test_stage.h"

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
        auto input = _getSource().getNext(execCtx.get());
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
            new sdk::ExtensionLogicalAggStage(std::make_unique<ValidateMultiSortKeyLogicalStage>(
                kMultiKeySortStageName, mongo::BSONObj()))});
        dpl.shardsPipeline = sdk::DPLArrayContainer(std::move(shardElements));

        dpl.sortPattern = BSON("$searchScore" << 1 << "$textScore" << 1);

        return dpl;
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
            "textScore", "searchScore", "score", "searchScoreDetails"};
        properties.setProvidedMetadataFields(providedMetadata);
        properties.serialize(&builder);
        return builder.obj();
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
