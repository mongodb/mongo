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
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/distributed_plan_logic.h"
#include "mongo/db/extension/sdk/dpl_array_container.h"
#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"
#include "mongo/db/extension/shared/get_next_result.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/executable_agg_stage.h"
#include "mongo/util/modules.h"

namespace mongo::extension::sdk::shared_test_stages {
/**
 * =====================================================================
 * Source stage testing that generates documents and metadata with fruit
 * data from a static dataset.
 * =====================================================================
 */
static constexpr std::string_view kFruitsAsDocumentsName = "$fruitAsDocuments";
class FruitsAsDocumentsExecStage : public sdk::ExecAggStageSource {
public:
    FruitsAsDocumentsExecStage() : FruitsAsDocumentsExecStage(kFruitsAsDocumentsName) {}

    FruitsAsDocumentsExecStage(std::string_view stageName) : sdk::ExecAggStageSource(stageName) {}

    FruitsAsDocumentsExecStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : FruitsAsDocumentsExecStage(stageName) {}

    ExtensionGetNextResult getNext(const sdk::QueryExecutionContextHandle& execCtx,
                                   ::MongoExtensionExecAggStage* execStage) override {
        if (_currentIndex >= _documentsWithMetadata.size()) {
            return ExtensionGetNextResult::eof();
        }
        // Note, here we can create the result as a byte view, since this stage guarantees to keep
        // the results valid.
        auto& currentDocumentWithMetadata = _documentsWithMetadata[_currentIndex++];
        auto documentResult = ExtensionBSONObj::makeAsByteView(currentDocumentWithMetadata.first);
        auto metaDataResult = ExtensionBSONObj::makeAsByteView(currentDocumentWithMetadata.second);
        return ExtensionGetNextResult::advanced(std::move(documentResult),
                                                std::move(metaDataResult));
    }

    // Allow this to be public for visibility in unit tests.
    UnownedExecAggStageHandle& _getSource() override {
        return sdk::ExecAggStageSource::_getSource();
    }

    void open() override {}

    void reopen() override {}

    void close() override {}

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSONObj();
    }

    // Helper func that return input result for easy expected-vs-actual comparisons without
    // requiring a static test dataset.
    static inline auto getInputResults() {
        return _documentsWithMetadata;
    }

    static inline std::unique_ptr<FruitsAsDocumentsExecStage> make() {
        return std::make_unique<FruitsAsDocumentsExecStage>();
    }

private:
    // Every FruitsAsDocumentsExecAggStage object will have access to the same test document suite.
    static inline const std::vector<std::pair<BSONObj, BSONObj>> _documentsWithMetadata = {
        {BSON("_id" << 1 << "apples" << "red"), BSON("$textScore" << 5.0)},
        {BSON("_id" << 2 << "oranges" << 5), BSON("$searchScore" << 1.5)},
        {BSON("_id" << 3 << "bananas" << false), BSON("$searchScore" << 2.0)},
        {BSON("_id" << 4 << "tropical fruits"
                    << BSON_ARRAY("rambutan" << "durian"
                                             << "lychee")),
         BSON("$textScore" << 4.0)},
        {BSON("_id" << 5 << "pie" << 3.14159),
         BSON("$searchScoreDetails" << BSON("scoreDetails" << "foo"))}};
    size_t _currentIndex = 0;
};

DEFAULT_LOGICAL_STAGE(FruitsAsDocuments);

class FruitsAsDocumentsAstNode : public sdk::TestAstNode<FruitsAsDocumentsLogicalStage> {
public:
    FruitsAsDocumentsAstNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestAstNode<FruitsAsDocumentsLogicalStage>(stageName, arguments) {}

    mongo::BSONObj getProperties() const override {
        mongo::extension::MongoExtensionStaticProperties properties;
        mongo::BSONObjBuilder builder;
        properties.setPosition(mongo::extension::MongoExtensionPositionRequirementEnum::kFirst);
        properties.setHostType(
            mongo::extension::MongoExtensionHostTypeRequirementEnum::kRunOnceAnyNode);
        properties.setRequiresInputDocSource(false);
        // Report that this stage will provide metadata.
        std::vector<std::string> providedMetadata{
            "textScore", "searchScore", "score", "searchScoreDetails"};
        properties.setProvidedMetadataFields(providedMetadata);
        properties.serialize(&builder);
        return builder.obj();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<FruitsAsDocumentsAstNode>(
            static_cast<std::string_view>(kFruitsAsDocumentsName), BSONObj());
    }
};

DEFAULT_PARSE_NODE(FruitsAsDocuments);

using FruitsAsDocumentsSourceStageDescriptor =
    sdk::TestStageDescriptor<"$fruitAsDocuments", FruitsAsDocumentsParseNode, true>;

/**
 * ===================================================================
 * Transform stage testing that transforms documents and metadata by
 * appending fruit data from a static dataset and additional metadata.
 * ===================================================================
 */
class AddFruitsToDocumentsWithMetadataExecStage : public TestExecStage {
public:
    AddFruitsToDocumentsWithMetadataExecStage(std::string_view stageName,
                                              const mongo::BSONObj& arguments)
        : sdk::TestExecStage(stageName, arguments) {}

    ExtensionGetNextResult getNext(const sdk::QueryExecutionContextHandle& execCtx,
                                   MongoExtensionExecAggStage* execStag) override {
        auto input = _getSource()->getNext(execCtx.get());
        if (input.code == GetNextCode::kPauseExecution) {
            return ExtensionGetNextResult::pauseExecution();
        }
        if (input.code == GetNextCode::kEOF) {
            return ExtensionGetNextResult::eof();
        }
        if (_currentIndex >= _documents.size()) {
            return ExtensionGetNextResult::eof();
        }
        // If we got here, we must have a document!
        sdk_tassert(11357803, "Failed to get an input document!", input.resultDocument.has_value());
        BSONObjBuilder bob;
        bob.append("existingDoc", input.resultDocument->getUnownedBSONObj());
        // Transform the returned input document by adding a new field.
        bob.append("addedFields", _documents[_currentIndex++]);
        // We need to preserve metadata from source stage if present and return the result as a
        // ByteBuf, since we are returning a temporary.
        BSONObjBuilder metaBob;
        if (input.resultMetadata.has_value()) {
            auto obj = input.resultMetadata->getUnownedBSONObj();
            for (const auto& elem : obj) {
                if (elem.type() == BSONType::numberDouble) {
                    // For simplicity, Overwrite each metadata field value with 10.0
                    metaBob.append(elem.fieldNameStringData(), 10.0);
                } else {
                    metaBob.append(elem);
                }
            }
        }
        metaBob.append("$vectorSearchScore", 50.0);
        return ExtensionGetNextResult::advanced(ExtensionBSONObj::makeAsByteBuf(bob.done()),
                                                ExtensionBSONObj::makeAsByteBuf(metaBob.done()));
    }

private:
    // Every AddFruitsToDocumentsExecStage object will have access to the same test document
    // suite.
    static inline const std::vector<BSONObj> _documents = {
        BSON("_id" << 1 << "apples" << "red"),
        BSON("_id" << 2 << "oranges" << 5),
        BSON("_id" << 3 << "bananas" << false),
        BSON("_id" << 4 << "tropical fruits" << BSON_ARRAY("rambutan" << "durian" << "lychee")),
        BSON("_id" << 5 << "pie" << 3.14159)};
    size_t _currentIndex = 0;
};

DEFAULT_LOGICAL_STAGE(AddFruitsToDocumentsWithMetadata);

class AddFruitsToDocumentsWithMetadataAstNode
    : public sdk::TestAstNode<AddFruitsToDocumentsWithMetadataLogicalStage> {
public:
    AddFruitsToDocumentsWithMetadataAstNode(std::string_view stageName,
                                            const mongo::BSONObj& arguments)
        : sdk::TestAstNode<AddFruitsToDocumentsWithMetadataLogicalStage>(stageName, arguments),
          _preserveUpstreamMetadata(true) {
        if (arguments.hasField("preserveUpstreamMetadata")) {
            _preserveUpstreamMetadata =
                arguments.getField("preserveUpstreamMetadata").booleanSafe();
        }
    }

    mongo::BSONObj getProperties() const override {
        mongo::extension::MongoExtensionStaticProperties properties;
        mongo::BSONObjBuilder builder;
        std::vector<std::string> providedMetadata{"vectorSearchScore"};
        properties.setProvidedMetadataFields(providedMetadata);
        properties.setPreservesUpstreamMetadata(_preserveUpstreamMetadata);
        properties.serialize(&builder);
        return builder.obj();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<AddFruitsToDocumentsWithMetadataAstNode>(
            "$addFruitsToDocumentsWithMetadata", BSONObj());
    }

private:
    bool _preserveUpstreamMetadata;
};

DEFAULT_PARSE_NODE(AddFruitsToDocumentsWithMetadata);

using AddFruitsToDocumentsWithMetadataTransformStageDescriptor =
    sdk::TestStageDescriptor<"$addFruitsToDocumentsWithMetadata",
                             AddFruitsToDocumentsWithMetadataParseNode,
                             false>;

/**
 * =========================================================
 * Transform stage testing that transforms documents by
 * appending fruit data from a static dataset.
 * =========================================================
 */
class AddFruitsToDocumentsExecStage : public TestExecStage {
public:
    AddFruitsToDocumentsExecStage() : AddFruitsToDocumentsExecStage(kTransformName, BSONObj()) {}

    AddFruitsToDocumentsExecStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestExecStage(stageName, BSONObj()) {}

    // Loosely modeled on the behavior of the existing transform stage:
    // exec::agg::SingleDocumentTransformationStage::doGetNext(), more specifically, the
    // $addFields behavior.
    ExtensionGetNextResult getNext(const sdk::QueryExecutionContextHandle& execCtx,
                                   MongoExtensionExecAggStage* execStage) override {
        auto input = _getSource()->getNext(execCtx.get());
        if (input.code == GetNextCode::kPauseExecution) {
            return ExtensionGetNextResult::pauseExecution();
        }
        if (input.code == GetNextCode::kEOF) {
            return ExtensionGetNextResult::eof();
        }
        if (_currentIndex >= _documents.size()) {
            return ExtensionGetNextResult::eof();
        }
        // If we got here, we must have a document!
        sdk_tassert(11357802, "Failed to get an input document!", input.resultDocument.has_value());
        BSONObjBuilder bob;
        bob.append("existingDoc", input.resultDocument->getUnownedBSONObj());
        // Transform the returned input document by adding a new field.
        bob.append("addedFields", _documents[_currentIndex++]);
        // We need to preserve metadata from source stage if present and return the result as a
        // ByteBuf, since we are returning a temporary.
        return input.resultMetadata.has_value()
            ? ExtensionGetNextResult::advanced(
                  ExtensionBSONObj::makeAsByteBuf(bob.done()),
                  ExtensionBSONObj::makeAsByteBuf(input.resultMetadata->getUnownedBSONObj()))
            : ExtensionGetNextResult::advanced(ExtensionBSONObj::makeAsByteBuf(bob.done()));
    }

    void open() override {}

    void reopen() override {}

    void close() override {}

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::ExecAggStageTransform> make() {
        return std::make_unique<AddFruitsToDocumentsExecStage>();
    }

private:
    // Every AddFruitsToDocumentsExecStage object will have access to the same test document
    // suite.
    static inline const std::vector<BSONObj> _documents = {
        BSON("_id" << 1 << "apples" << "red"),
        BSON("_id" << 2 << "oranges" << 5),
        BSON("_id" << 3 << "bananas" << false),
        BSON("_id" << 4 << "tropical fruits" << BSON_ARRAY("rambutan" << "durian" << "lychee")),
        BSON("_id" << 5 << "pie" << 3.14159)};
    size_t _currentIndex = 0;
};

DEFAULT_LOGICAL_STAGE(AddFruitsToDocuments);

class AddFruitsToDocumentsAstNode : public sdk::TestAstNode<AddFruitsToDocumentsLogicalStage> {
public:
    AddFruitsToDocumentsAstNode()
        : sdk::TestAstNode<AddFruitsToDocumentsLogicalStage>(kTransformName, BSONObj()) {};

    AddFruitsToDocumentsAstNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestAstNode<AddFruitsToDocumentsLogicalStage>(stageName, arguments) {}

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<AddFruitsToDocumentsAstNode>();
    }
};

DEFAULT_PARSE_NODE(AddFruitsToDocuments);

using AddFruitsToDocumentsTransformStageDescriptor =
    sdk::TestStageDescriptor<"$addFruitsToDocuments", AddFruitsToDocumentsParseNode, false>;
}  // namespace mongo::extension::sdk::shared_test_stages
